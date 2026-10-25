# Minimum stock iOS 26 cache: comparing two bases

Built on 2026-10-24 from original iOS 26 simulator build **23A343** libraries at `0x120000000` and `0x180000000`.
Both are new builder outputs from the same inputs. No source-library dependencies were changed, and neither cache was
installed on the phone. They retain the builder's signatures; they were not signed with the development identity.

## Why the minimum is 217 libraries

A ten-library cache is not possible with the unmodified libraries and this builder's dependency rules. The scan covered
all **3,332 arm64 dylibs** in the mounted runtime. The smallest strong dependency closure is **217 images**: 214 simulator
libraries in one strongly connected component, plus the three host support libraries added by the builder (`libsystem_kernel`,
`libsystem_platform`, and `libsystem_pthread`). `dyld_sim` is staged for platform detection but is not included as a cache image.

The cycle includes libSystem, libobjc, SwiftCore, Foundation, and CoreFoundation. Delayed-initialization dependency attributes
do not make these edges weak, so the builder still requires their targets. This differs from the smaller set actually initialized
for `df`. Removing dependency commands or using synthetic libraries would be a different experiment.

`closure.json` records the exact 217-image set; both built image tables match it exactly. `dependency-analysis.json` records
the minimum and component counts; `all-runtime-dependencies.json` retains the full inventory. Corresponding builder rules:
[dependency verification](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/cache_builder/NewSharedCacheBuilder.cpp#L933)
and [simulator main-image selection](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/cache_builder/NewSharedCacheBuilder.cpp#L6005-L6020).

## Build outputs

Paths are relative to the repository root and ignored by Git.

| Property | Low cache | High cache |
| --- | --- | --- |
| File | `tmp/minimal-cache-compare/base-120/dyld_sim_shared_cache_arm64` | `tmp/minimal-cache-compare/base-180/dyld_sim_shared_cache_arm64` |
| Base | `0x120000000` | `0x180000000` |
| Declared end | `0x1354ec000` | `0x1954ec000` |
| VM span | `0x154ec000` / 340.921875 MiB | Same |
| File size | 290,930,688 bytes / 277.453125 MiB | Same |
| Images / mappings | 217 / 6 | 217 / 6 |
| UUID | `d8d177d4-b773-38e7-a772-c469d71faf3c` | `5259e1c0-b7b6-3b0a-bf0d-f9493e1976a5` |

Relative layout, file offsets, mapping sizes, and protections are identical. The translation is **high = low + `0x60000000`**.
The high build uses the original 23A343 builder. The low build uses the three-instruction base patch implemented by
`Tools/patch-ios26-cache-base.py`. No fake-main or dependency-verification patches were used.

Low SHA-256: `0bc6e5d5aa0e6e491d93a47334a98f9c40eda585a1187f0f57492fa72566ccbf`.
High SHA-256: `f76ab5567444e87cf2da7610893a5e279b7ecfa713b6d07d736f9fa219080a9b`.

## Assembly comparison

All **546 nonempty instruction-marked sections**, totaling **145,265,276 bytes (138.536 MiB)**, are byte-identical.
There are **zero changed bytes or four-byte words** in those sections. Their sizes include declared padding or embedded data;
36,316,319 four-byte words must not be described as a verified instruction count.

Every recognized PC-relative encoding's target also translates by the base delta, without exceptions. Those encoding counts
are secondary evidence: the caches have no `LC_DATA_IN_CODE` entries, and encoding-pattern scanning is not a complete
code/data classifier. The exact byte comparison does not depend on decoding.

LLDB disassembly of exported `malloc`, `free`, `printf`, `read`, and `write` confirms identical encodings. Their starting
symbols match `LC_FUNCTION_STARTS` entries. Disassembly at cache-relative PCs is exactly identical. Complete `malloc` entry:

```text
Low address   High address  Word       Assembly
12b20676c     18b20676c     aa0003e1   mov  x1, x0
12b206770     18b206770     d001f9c8   adrp x8, <same relative page displacement>
12b206774     18b206774     f940b900   ldr  x0, [x8, #0x170]
12b206778     18b206778     52800022   mov  w2, #1
12b20677c     18b20677c     17fffc72   b    12b205944 / 18b205944
```

Absolute PCs and displayed branch targets change, while instruction bytes do not. The same result holds for ADRP/ADD pairs
and BL instructions in `printf`. Full excerpts are retained in `function-assembly.txt` and `function-assembly-normalized.txt`.

## Pointer, metadata, and full-file comparison

The structural verifier checked exact base translation for:

| Structure | Checked count |
| --- | ---: |
| Mapping / mapping-with-slide entries | 6 / 6 |
| Image / image-text addresses | 217 / 217 |
| Nonzero selected absolute cache-header fields | 6 |
| Mach-O segments | 856 |
| Mach-O sections | 4,542 |
| Address-valued section symbols | 764,963 |
| Actual slots in typed pointer sections | 63,377 |
| Non-cache values in those pointer sections, unchanged | 375 |

A GOT example in libobjc changes from `0x12b2dc3e0` to `0x18b2dc3e0`. The verifier checks tags as well as the address delta.
Typed-slot checks are separate from heuristic classification of arbitrary words.

Across file-backed sections, 2,169 noninstruction sections containing 41,186,482 bytes are unchanged. Another 1,532
noninstruction sections change; every differing aligned 64-bit word in them matches the cache-address delta.

A separate scan covers **every aligned 64-bit word before the code signature**, including metadata outside Mach-O sections:

- 33,392,880 words are unchanged.
- 2,901,774 changed words match an in-range cache address plus `0x60000000`, preserving any high-byte tag.
- The remaining two changed words are the cache UUID; no other changed word remains unexplained by these categories.
- The signature regions have 91,696 differing bytes.

This classifies the complete raw difference for this pair, but does not provide a general relocation table. In particular,
**2,201 unchanged words also look like cache addresses after masking the top byte**. A blind range-based rebaser would
incorrectly change them. This is not confined to instruction bytes: CoreGraphics `__TEXT,__const` contains an unchanged
`0x1219e3600` at cache file offset `0x3869b98`, although the value falls inside the low cache address interval.

The [follow-up classification audit](cache-pointer-classification.md) explains these candidates: 1,851 instruction pairs,
317 VideoToolbox scalar fields, 26 constant-data words, six DOF field windows, and one unwind pair. A classifier using
actual `__TEXT` sections and the two typed VideoToolbox record layouts rejects all 2,201 without losing a measured
relocation. The initial rules left 173 false positives when scanning the high cache's address interval. The extended
audit resolves them using ImageIO scalar headers and parsed export, function-start, and cache-path trie fields: both
directions now have zero false positives or false negatives. This remains a result for the measured pair, not a general
pointer detector for other runtimes.

The pair demonstrates that this uniform base change requires pointer/metadata updates but no instruction-byte changes.
It also demonstrates that address-range scanning cannot determine which words require relocation. It does not prove the
same result for a different input set, runtime, or nonuniform layout.

## Reproduction and retained evidence

Work directory: `tmp/minimal-cache-compare/`.

- `stage.py`, `stage-manifest.json`: hash-verified unchanged runtime inputs, hard-linked into an isolated root; dyld_sim copied separately.
- `tool-patch.json`: exact checked builder patch; patched host tool ad-hoc signed for execution on the Mac.
- `build-120.log`, `build-180.log`, `build-manifest.json`: build output, options, cache ranges, UUIDs, hashes, and sizes.
- `metadata-comparison.json`: result of `tmp/dsc-relocated/verify-rebuild.py` on this pair.
- `audit-sections.py`, `section-audit.json`: every cached section and instruction-byte comparison.
- `disassemble-functions.py`, `function-assembly.txt`, `function-assembly-normalized.txt`: paired real-address and normalized assembly.
- `disassemble-samples.py`, `section-audit-assembly.txt`: additional PC-relative encoding examples.
- `classify-file-diff.py`, `raw-comparison.json`: complete aligned-word classification and unchanged address-shaped examples.

Both build commands use the same staged root with `-iOS -arch arm64 -force -verbose`, changing only builder and output path.
The stock builder warning that dyld_sim is not included is expected. Input binaries and the current device runtime were not
changed. This offline comparison adds no execution claim for either new cache.
