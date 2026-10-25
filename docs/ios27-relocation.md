# iOS 27 cache relocation feasibility

Read-only audit on 2026-10-24 of the stock iOS 27 simulator runtime, build **24A434**, and its two arm64 cache files.
No runtime files were changed, signed, copied, or tested on the phone during this audit.

The full cache cannot be uniformly relocated below `0x180000000` in the current jailed arm64 app. Its address span is
larger than the entire permitted low-address window. A smaller selection or a different layout would also require
reconstructing relocation information that the shipped simulator cache no longer contains.

## Address-space fit

| Quantity | Exact value | Size |
| --- | --- | --- |
| Original base | `0x1c0000000` | |
| Declared end, exclusive | `0x2abccc000` | |
| Complete VM span | `0xebccc000` | 3.684372 GiB |
| Sum of all 11 mapping sizes | `0xe3cc8000` | 3.559357 GiB |
| Internal unmapped gaps | `0x8000000` | 128 MiB |
| Tail reservation beyond last mapping | `0x4000` | 16 KiB |
| Main executable mapping | `0x62270000` | 1,570.4375 MiB |
| `.01` executable mapping | `0x23f48000` | 575.28125 MiB |
| Both executable mappings | `0x861b8000` | 2,145.71875 MiB |
| Theoretical low window | `[0x100000000, 0x180000000)` | 2 GiB |

The highest possible base that would put the entire span below the host shared region is:

```text
0x180000000 - 0xebccc000 = 0x94334000
```

That base is below 4 GiB. XNU 12377.1.9 rejects arm64 executables without a hard page-zero reservation of at least 4 GiB.
The restored LiveSimBooter binary has `__PAGEZERO = [0, 0x100000000)` and preferred `__TEXT = 0x100000000`, as expected.
The kernel implements this reservation by raising the VM map's minimum address; the PIE slide can raise it further.
`MAP_FIXED` does not override that minimum, and `vm_map_raise_min_offset()` cannot lower it. See
[the arm64 load requirement](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/bsd/kern/mach_loader.c#L897-L903),
[fixed-map bounds](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/osfmk/vm/vm_map.c#L2549-L2556), and
[minimum-address updates](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.1.9/osfmk/vm/vm_map.c#L22030-L22040).

Thus the full cache exceeds even the theoretical low window by **1.684372 GiB**, before accounting for the host app,
dyld_sim, df, thread storage, heap, or other live allocations. Removing all four 32 MiB gaps still does not make it fit.
Even putting only the existing executable mappings low would exceed the window by **97.71875 MiB**, before host allocations.
Simply placing the data mappings elsewhere is therefore insufficient for the unchanged executable mappings.
These kernel restrictions are source findings, not a new lower-address mapping experiment on the live iOS 27 kernel.

## Relocation information actually present

The direct cache-header and embedded-image audits found:

| Metadata | Observed value |
| --- | --- |
| `maxSlide` | `0` in both members |
| Mapping slide-info offset/size | Zero for all 11 mappings |
| Unique cached Mach-O images | 1,826 |
| `LC_DYLD_CHAINED_FIXUPS` | Absent in all images |
| `LC_SEGMENT_SPLIT_INFO` | Absent in all images |
| `LC_DYLD_INFO` / `LC_DYLD_INFO_ONLY` | Absent in all images |
| Section relocations | Zero in all sections |
| `LC_DYSYMTAB` external/local relocations | Zero in all images |
| `LC_DYLD_EXPORTS_TRIE`, `LC_SYMTAB`, `LC_DYSYMTAB` | Present in all 1,826 images |

This matches the pinned dyld builder's behavior: simulator configuration omits slide information; the builder materializes
tracked fixups as absolute VM addresses, preserves high-byte tags, and strips chained-fixup and split-segment commands.
Some optimized GOT sections also become ordinary `S_REGULAR` sections. Export/symbol tables and cache patch tables do not
replace the missing complete list of pointer locations. See
[simulator configuration](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/cache_builder/BuilderConfig.cpp#L170-L174),
[pointer materialization](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/cache_builder/SubCache.cpp#L2271-L2285), and
[command removal](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/cache_builder/NewAdjustDylibSegments.cpp#L499-L546).

Changing `maxSlide` or applying a negative slide cannot recover absent relocation records. The examined simulator loader
uses slide zero. A static rebase could retain that contract, but it would still need a reliable pointer inventory.

## Fixups for a uniform, page-aligned move

For a common 16 KiB-aligned delta, the following absolute values need relocation where present:

- Both mapping-address tables and cache base fields; absolute header addresses such as the dylib trie, PBL sets, and patch info.
- Image and image-text load addresses; the two TPRO unslid addresses; absolute addresses inside nested cache metadata,
  including patch-table headers and function-variant tables.
- Mach-O segment `vmaddr`, section `addr`, absolute entry/routine addresses, and address-valued `N_SECT` symbols.
- Actual pointer slots in GOTs, initializer/TLV arrays, vtables, function pointers, ObjC structures, blocks, CFStrings,
  and arbitrary library data. Nulls, external addresses, high-byte tags, low-bit flags, and addends must be treated correctly.
- Any absolute cache address synthesized by instructions, if present; identifying these requires inspecting instruction
  semantics, not changing every immediate that happens to fall in the cache range.

Cache-relative offsets, subcache offsets, file offsets, trie indices, ordinary image-relative export values, and relative
Swift/ObjC references remain unchanged when both their base and target move by the same delta. Header fields must be
classified by the exact runtime's consumer; their names alone do not establish whether a value is absolute or relative.
The pinned public header is shorter than the 24A434 header, so its trailing fields need version-specific validation.

Two fields were checked against the actual 24A434 dyld_sim disassembly. `functionVariantInfoAddr` at header offset `0x208`
contains the absolute address `0x26f9a1a78`; its consumer adds the cache slide. In contrast, `prewarmingDataOffset` at `0x218`
contains `0xaae68000`, and its consumer adds the mapped cache base, making it cache-relative despite the older header's
file-offset comment. The latter must stay unchanged for a uniform move. The TPRO addresses are `0x22e474000` and
`0x266b24000`, each covering `0x4000` bytes.

Internal PC-relative ARM64 instructions generally do **not** need rewriting for a common page-aligned move:

```text
(target + delta) - (instruction + delta) = target - instruction
```

This preserves `B`/`BL`, `ADR`, literal loads, and conditional branch displacements. It also preserves `ADRP` page deltas
and the paired `ADD`/`LDR`/`STR` page offsets. References to fixed external targets and embedded absolute literals are
exceptions. The audit did not exhaustively disassemble every code page, so this is a relocation rule, not a claim that
every TEXT byte can remain untouched. Apple's adjustor uses these same
[page-difference](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/cache_builder/NewAdjustDylibSegments.cpp#L1062-L1156)
and [branch-difference](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/cache_builder/NewAdjustDylibSegments.cpp#L1234-L1253) calculations.

The stock cache is arm64 simulator code, not an arm64e cache; this audit does not introduce a blanket PAC-regeneration
requirement. Tagged pointer representations still require preservation. Both modified cache files would need signing again
after all offline changes, and the loader's mapping, cleanup, and debugger address reporting would need to match the result.

## What changes when packing or pruning

Moving images or segments by different deltas loses the common-slide property. In addition to absolute pointers, that work
must update cross-segment/cross-image `ADRP` pairs, direct branches and stubs, relative data references, cache/image offsets,
exports as affected by segment movement, optimized ObjC/Swift metadata, patch tables, and prebuilt-loader metadata.
Instruction and relative-pointer ranges must be checked; a new layout can require branch islands or other transformations.
The original split-segment metadata used by Apple's builder would be particularly useful, but is absent here.

A scan for 64-bit values within the original cache address interval is only a heuristic. It can modify integers that are
not pointers, miss tagged or unaligned pointers, and does not recover instruction or relative-reference relationships.
Likewise, an extractor does not automatically restore the complete original relocation metadata.

For the current jailed app, the next feasible scope would be a substantially smaller dependency set with a verified layout
and reconstructed fixups, or a rebuild from original standalone inputs if those become available. This audit establishes
neither a working iOS 27 subset nor a complete rebasing algorithm. The iOS 26 rebuilt cache remains the tested runtime.

## Evidence

The stock input directory is the 24A434 runtime's `RuntimeRoot/System/Library/Caches/com.apple.dyld`.
Main UUID: `5827bb56-52e1-3d0b-ac02-7a3201246563`; `.01` UUID: `d944612d-f32d-3c56-a003-d678ee4f6076`.
Read-only reports and reproduction scripts are under ignored `tmp/ios27-relocation-audit/` and `tmp/ios27-pointer-audit/`.
The latter's `inspect.py` walks every image's bounded load-command region and checks relocation commands and section tables.
The earlier high-mapping trial's stale-trie fault is documented in [Shared cache](shared-cache.md#first-forced-base-trial-inside-the-native-reservation).
