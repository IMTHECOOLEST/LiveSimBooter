# Distinguishing address-shaped constants from cache pointers

Investigated on 2026-10-24 using the two [iOS 26 build 23A343 caches](ios26-base-comparison.md), rebuilt from identical
inputs at `0x120000000` and `0x180000000`. This is an offline analysis; neither cache nor the device runtime was modified.

The extended structural classifier separates all **2,201 low-range false positives** and all **3,709 high-range false
positives**, preserving all **2,901,774 relocated address words** in both directions. The initial classifier left 173
high-range residuals; the follow-up below resolves them using parsed metadata and typed ImageIO records. These are
measured results for this cache pair, not proof of a complete pointer detector for another runtime.

## Ground truth and scope

The test scans eight-byte-aligned words before the code signature. A candidate's low 56 bits must fall inside the low
cache's declared address interval, `[0x120000000, 0x1354ec000)`. The second cache supplies the label:

- **Relocated address word:** changes by `0x60000000`, preserving the high byte. There are **2,901,774**.
- **False positive:** remains byte-identical despite satisfying the range test. There are **2,201**.

The positive set includes cache headers, Mach-O metadata, symbol values, and payload pointers. It is not a count of
runtime pointer slots alone. This scan does not establish completeness for unaligned pointers, other encodings, or
addresses outside the cache. The two cache UUID words are separately explained differences, outside the candidate set.

For classification, only the first cache's sections, symbols, and known record layouts may be consulted. The second
cache's bytes are used to score predictions, not to decide them. Layout rules recovered using this pair are still
specific to this corpus; this is not an independent validation on another runtime.

## What the 2,201 false positives actually contain

| Source | Count | Interpretation |
| --- | ---: | --- |
| Instruction sections | 1,851 | Two adjacent ARM64 instructions interpreted as one tagged 64-bit address |
| VideoToolbox `__DATA_CONST,__const` | 317 | Pixel-format FourCC followed by a 32-bit scalar equal to one |
| `__TEXT,__const` | 26 | Numeric and encoded constant data, detailed below |
| DTrace DOF sections | 6 | Windows spanning integer, string-index, and relative-offset fields |
| Compact unwind section | 1 | Compressed unwind entry next to an unwind encoding |
| **Total** | **2,201** | All are fully contained in Mach-O sections |

No false positive occurs in `__LINKEDIT` or outside sections.

### Instruction pairs

All 1,851 instruction candidates decode as two valid instructions. Of these, **1,748** are a `CBZ` or `CBNZ` followed
by `B +4`. The second instruction's word is `0x14000001`. When it becomes the upper half of a 64-bit word, clearing
the top byte leaves an apparent address in the 4 GiB range.

For example, CoreGraphics file offset `0x34f5b28` contains:

```text
0x1234f5b28: 34000108    cbz w8, 0x1234f5b48
0x1234f5b2c: 14000001    b   0x1234f5b30

Combined little-endian u64: 0x1400000134000108
After clearing top byte:   0x0000000134000108
```

The result falls inside the cache address interval, but it is not an address-valued data slot. All 546 instruction-marked
sections are byte-identical across the pair. Excluding those sections removes these 1,851 candidates without rejecting
any measured relocation. Valid disassembly alone would not prove this for arbitrary mixed code and literal pools.

### VideoToolbox records contain both scalars and real pointers

The 317 candidates are in two tables. A typical value is `0x0000000132767579`: the lower 32-bit integer is the FourCC
`'2vuy'`, and the upper 32-bit integer is `1`. It is two scalar fields, not a pointer.

| Table | Record size | Records | False positives, all at record `+0` |
| --- | ---: | ---: | ---: |
| `_VTPixelGraphBlitterTable` | 48 bytes | 4,965 | 309 |
| `_VTPixelGraphVImageBlitterTable` | 32 bytes | 344 | 8 |

The original standalone VideoToolbox dylib provides an independent field-type check. `xcrun dyld_info -fixups` decodes
its fixup metadata; source and cache locations are matched by offset within `__DATA_CONST,__const`. Across that entire
274,288-byte section:

- All **11,843 source fixup locations** exactly match the **11,843 relocated words** in the rebuilt caches.
- All **317 unchanged candidates** are outside those fixup locations and retain their original source-file values.
- There are no other changed words in the section.

The two tables contain **10,274 genuine pointer slots** alongside the scalar fields:

| Table | Pointer field | Populated fixup slots |
| --- | ---: | ---: |
| Blitter | `+16` | 696 |
| Blitter | `+24` | 4,269 |
| Blitter | `+40` | 4,965 |
| VImageBlitter | `+16` | 344 |

Rejecting an entire table, or the entire `__DATA_CONST,__const` section, would discard genuine relocations. The useful
rule identifies the first scalar field of each record. A global rule based on a value resembling a FourCC is unsound.

### The remaining 33 words in `__TEXT`

The 26 `__TEXT,__const` cases consist of:

- **11 CoreGraphics words** within the encoded `_cmap_Adobe_Japan1_UCS2` character-map blob.
- **6 vImage words** in reciprocal/unpremultiply numeric tables.
- **6 libc++ words** in `__POW10_SPLIT` and `__POW10_SPLIT_2`, used by floating-point decimal conversion. These are
  integer limbs; for example, a limb value `4,943,102,544` is also the apparent address `0x126a1c250`.
  [Upstream Ryu table](https://github.com/ulfjack/ryu/blob/master/ryu/d2fixed_full_table.h#L431).
- **2 ICU words** in a byte-indexed joining-group table and a serialized UnicodeSet. Consumers use byte loads and
  16-bit range boundaries, respectively. Nearby RTTI symbol names do not describe these objects. See ICU's
  [joining-group lookup](https://github.com/unicode-org/icu/blob/main/icu4c/source/common/ubidi_props.cpp#L188-L200)
  and [serialized set construction](https://github.com/unicode-org/icu/blob/main/icu4c/source/i18n/collationdatareader.cpp#L249-L250).
- **1 Foundation word** near `__NSSharedKeySetS_g`. Its exact field type remains unresolved; the paired build proves
  that this word remains unchanged under this base translation. It also matches the original standalone dylib, which
  has no fixup at that location or elsewhere in its `__TEXT,__const` section.

The six DOF cases arise because the DOF structures are not aligned to the cache scan's global eight-byte grid. Some
candidates cross field boundaries. For example, Foundation file offset `0x549fb98` reads as `0x120000000`, but consists
of the final three bytes of `dofr_type = 1` and the first five bytes of `dofr_offset = 0x120`. This is not a relocation
target pointer. DOF also has address-valued fields, so excluding all DOF data is not a universal rule.

CoreVideo file offset `0x444d7d8` combines compressed unwind entry `0x34038bc0` with encoding `0x54000001`. The former
contains encoding index 52 and an image-relative function offset of `0x3a008`; neither word is an absolute address.

## Initial classifier and reverse check

The initial classifier used these rules, consulting only the cache being classified:

1. Exclude candidates fully contained in declared `__TEXT` sections.
2. Resolve the two VideoToolbox table starts and the following boundary from that image's symbols. Require the exact
   record counts and strides above. Exclude only the first eight-byte scalar field of each record.
3. Retain the other address-shaped candidates, including tagged values and pointers to unaligned targets.

There are no overlapping `__TEXT` section ranges in these inputs. Every exclusion requires complete eight-byte
containment; segment permissions and the other cache's values do not inform the prediction.

| Evaluation | Candidates | True addresses retained | Constants rejected | False positives | False negatives |
| --- | ---: | ---: | ---: | ---: | ---: |
| Classify `0x120000000`, label using `0x180000000` | 2,903,975 | 2,901,774 | 2,201 | **0** | **0** |
| Classify `0x180000000`, label using `0x120000000` | 2,905,483 | 2,901,774 | 3,536 | **173** | **0** |

The forward exclusions are 1,884 `__TEXT` words, 309 Blitter scalar fields, and eight VImageBlitter scalar fields.
Both runs have zero unlabelled candidates. SHA-256 checks before and after confirm that both caches and the original
VideoToolbox dylib are unchanged.

The initial reverse run deliberately kept the same rules without adjustment. It changes the address range used for candidate
selection, exposing a different set of unchanged numbers. Its 173 remaining false positives demonstrate why a zero-error
fit on the original set cannot be treated as a general relocation algorithm.

Those 173 consist of eight ImageIO `__DATA_CONST,__const` words, 159 words in `LC_DYLD_EXPORTS_TRIE` data, four in
`LC_FUNCTION_STARTS` data, and two words outside image sections. The follow-up identifies the last two as cache dylib-path
trie records and structurally resolves all four groups. They are a separate candidate set; none is one of the original
2,201 low-range false positives.

For the iOS 27 cache, the reusable method is to recover field roles from format metadata, original fixup records where
available, and the instructions that consume the data. The exact iOS 26 table schema and blanket `__TEXT` exclusion are
not established for that runtime. Original fixup metadata would give a stronger pointer inventory than value heuristics;
the VideoToolbox audit demonstrates this for one full section. Cache-generated metadata also needs its own format parsers.

## Resolving the 173 reverse-check residuals

The extended classifier retains the original rules and adds three input-only parsers. None of their predictions uses
the reference cache, a list of false-positive offsets, or a blocklist of numeric values. Paired bytes are consulted only
after prediction, to score the result. The named ImageIO record schema was recovered and validated for build 23A343.

### Eight ImageIO scalar headers

These candidates are the first eight bytes of 56-byte TIFF/EXIF tag-definition records. Consumer disassembly confirms
halfword loads of the tag at `+0`, type selector at `+2`, and expected item count at `+4`, plus record advancement by
`0x38`. Bytes `+6..+7` are zero padding/reserved bytes in every record; their more specific purpose is not assumed.

For example, the apparent pointer `0x180030142` consists of:

```text
Bytes:         42 01 | 03 80 | 01 00 | 00 00
16-bit fields: 0142    8003    0001    0000
Role:          tag    type    count   padding/reserved
```

The classifier resolves the table and boundary symbols from ImageIO's own symbol table, requires section containment,
checks the record counts and 56-byte stride, and validates the terminal `0xffff` sentinel and reserved bytes. It excludes
only each record's first eight bytes.

| Table | Records, including sentinel | False positives removed | Real pointer slots retained |
| --- | ---: | ---: | ---: |
| `tagdefsIFD0` | 34 | 4 | 114 |
| `tagdefsExif` | 77 | 2 | 316 |
| `tagdefsGPS` | 33 | 0 | 168 |
| `tagdefsIFD1` | 12 | 2 | 44 |
| **Total** | **156** | **8** | **642** |

The original standalone ImageIO dylib provides a separate check: all **46,761 fixup locations** in its 555,896-byte
`__DATA_CONST,__const` section exactly match the **46,761 relocated words** in the two caches. All eight scalar candidates
retain their source values and have no fixup; there are no other changed words in the section. Excluding the three whole
tables containing false positives would instead lose 474 genuine pointers.

### 159 exported-symbol trie windows

All 159 candidates cross ordinary export flags, ULEB128 image-relative offsets, and neighboring trie node fields. For
example, file offset `0xdb90c20` contains bytes `00 f0 90 82 01 00 00 03`, which decode as flags zero, image-relative
offset `0x208870`, a leaf child count of zero, and the next node's empty payload and three-child count. Treating the
eight bytes as a tagged integer invents the apparent address `0x030000018290f000`.

The parser locates every `LC_DYLD_EXPORTS_TRIE` payload through load commands and parses all nodes and terminals. It
validates bounds, ULEBs, flags, strings, node ownership, terminal lengths, and zero padding. It accounts for **168,495
exports in 217 tries**, including 707 reexports and 22 resolver exports. Only windows wholly covered by non-absolute
serialized fields are excluded.

There are also **five absolute exports**, occupying 29 encoded value bytes. Those bytes are deliberately left outside
the exclusion intervals. They are ULEB128 values, not raw 64-bit pointer slots, and would require semantic handling in
a general relocator. Their presence is why blanket exclusion of all export data is too broad. Apple dyld distinguishes
absolute, image-relative, thread-local, reexport, and resolver terminals in
[ExportsTrie.cpp](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/mach_o/ExportsTrie.cpp#L281-L337).

### Four function-start stream windows

These candidates combine consecutive ULEB128 function-address deltas, the zero terminator, and padding. For example,
file offset `0xdafc1e8` contains `10 84 01 84 01 00 00 00`: deltas 16, 132, and 132, followed by termination and padding.
The parser validates all **217 function-start streams**, cumulative offsets against the image's TEXT extent, and zero
termination/padding before excluding their serialized contents. See Apple's
[FunctionStarts.cpp](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/mach_o/FunctionStarts.cpp#L40-L87).

### Two cache dylib-path trie windows

The previously untyped words at `0xd4555d0` and `0xd455670` belong to the cache's path-to-image-index trie. Its bounds
come from `dyld_cache_header.dylibsTrieAddr` and `dylibsTrieSize`, at header offsets `0x108` and `0x110`, not from a
hardcoded payload address. The resulting file interval is `[0xd454000, 0xd455ca8)` in this pair.

The parser accounts for **327 nodes, 217 terminals, 7,334 encoded bytes, and two zero padding bytes**. Each cached
image path resolves to its matching image-array index. The two false positives cross ULEB128 image indices 136/137 and
144/145, respectively, together with terminal lengths and child counts. These fields contain no absolute addresses.
This matches the lookup consumer in
[DyldSharedCache.cpp](https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/common/DyldSharedCache.cpp#L326-L341).

Only the validated payload is excluded. **The header's `dylibsTrieAddr` is still retained for relocation.**

### Extended evaluation

| Input cache | Candidates | True address words retained | Constants rejected | False positives | False negatives |
| --- | ---: | ---: | ---: | ---: | ---: |
| `0x120000000` | 2,903,975 | 2,901,774 | 2,201 | **0** | **0** |
| `0x180000000` | 2,905,483 | 2,901,774 | 3,709 | **0** | **0** |

All 173 additional exclusions are verified unchanged words: eight ImageIO headers, 159 export windows, four
function-start windows, and two cache-path trie windows. There are zero unlabelled candidates in either direction.
Both caches and the original VideoToolbox/ImageIO inputs retain identical SHA-256 hashes before and after evaluation.
The original classifier and its 173-residual report are preserved separately for comparison.

This resolves every observed false positive in both candidate sets. The evaluation still covers aligned, in-range,
pre-signature words; it does not prove completeness for other pointer encodings or unaligned slots. The parsers describe
real formats, but the VideoToolbox and ImageIO schemas and original `__TEXT` rule still need validation on another build.

## Why simple value filters fail

Each row below applies independently to the same 2,903,975 candidates. "Lost addresses" includes metadata and symbol
values, as well as runtime pointers.

| Filter | False positives retained | True address words wrongly rejected |
| --- | ---: | ---: |
| Range alone | 2,201 | 0 |
| Require high byte zero | 336 | 14,673 |
| Require target alignment of 8 | 1,853 | 963,287 |
| Require target alignment of 4 | 1,857 | 442,829 |
| Require target inside an actual mapping | 2,195 | 0 |
| Require target inside a Mach-O section | 41 | 82,264 |
| Require source to be a pointer-typed section | 0 | 2,838,397 |
| Reject printable FourCC-like low words | 1,883 | 121,412 |
| Exclude instruction-marked sections | 350 | 0 |
| Exclude actual `__TEXT` sections | 317 | 0 |

All 14,673 tagged positives use high byte `0x80`. A libBNNS example is `0x8000000120debdce`, pointing at a C++ RTTI
name; the original source fixup also retains the high bit. Target alignment is not a sound requirement because genuine
pointers may address individual string bytes. A pointer-section whitelist misses structures such as vtables and CFStrings,
along with cache and Mach-O metadata outside sections.

"Exclude `__TEXT` sections" means only declared section contents. It does **not** mean excluding the executable mapping
or the entire Mach-O segment: those also contain headers and other address-valued metadata that must be preserved.

## Evidence and reproduction

Analysis artifacts remain under ignored `tmp/pointer-ground-truth/`:

- `false-positives.json`: every false positive, its exact file offset/value, and its owning section.
- `code/instructions.json`: actual disassembly of all 1,851 instruction candidates.
- `code/structured.json`: decoded fields for all seven DOF/unwind candidates.
- `semantics/const.json`, `semantics/icu-consumers.txt`: constant-data locations and consumer disassembly.
- `semantics/findings.md`: per-group explanations, primary source links, and the unresolved Foundation field.
- `review/analyze.py`, `review/filters.json`: full-corpus single-filter statistics.
- `review/source-fixups.py`, `review/source-fixups.json`: original VideoToolbox fixup comparison and record-field audit.
- `review/classify-low-cache.py`: standalone parser and structural classifier; the other cache is used only for scoring.
- `review/classify-low-cache.json`, `review/classify-high-cache.json`: forward/reverse confusion counts, symbols, and hashes.
- `review/reverse-fp-owners.json`: all 173 reverse-check residuals and their section/load-command ownership.
- `review/Foundation-negative-source.json`: source-value and fixup checks for the remaining untyped Foundation field.
- `review/classify-structured-cache.py`, `review/structured-{low,high}.json`: extended classifier and complete two-direction evaluation.
- `reverse-imageio/rules.py`, `reverse-imageio/audit.json`, `reverse-imageio/consumers.asm`: typed headers, source fixups, and field consumers.
- `reverse-linkedit/rules.py`, `reverse-linkedit/linkedit-report.json`: parsed exports/function starts and every residual's field ownership.
- `reverse-untyped/rules.py`, `reverse-untyped/audit.json`: cache-path trie parser, terminal checks, and both decoded windows.

Run the combined classifier from the repository root:

```sh
python3 tmp/pointer-ground-truth/review/classify-low-cache.py
python3 tmp/pointer-ground-truth/review/classify-low-cache.py \
  --low tmp/minimal-cache-compare/base-180/dyld_sim_shared_cache_arm64 \
  --high tmp/minimal-cache-compare/base-120/dyld_sim_shared_cache_arm64 \
  --output tmp/pointer-ground-truth/review/classify-high-cache.json
python3 tmp/pointer-ground-truth/review/source-fixups.py
```

Run the extended classifier, retaining the initial reports:

```sh
python3 tmp/pointer-ground-truth/review/classify-structured-cache.py
python3 tmp/pointer-ground-truth/review/classify-structured-cache.py \
  --input tmp/minimal-cache-compare/base-180/dyld_sim_shared_cache_arm64 \
  --reference tmp/minimal-cache-compare/base-120/dyld_sim_shared_cache_arm64 \
  --output tmp/pointer-ground-truth/review/structured-high.json
```

The other audit scripts use the cache parser at `tmp/dsc-relocated/verify-rebuild.py`. All need the retained caches under
`tmp/minimal-cache-compare/`; the combined classifier also hashes the staged original VideoToolbox dylib. The source-fixup
audit needs that dylib and Xcode's `dyld_info`. None of these scripts signs, modifies, or installs runtime binaries.
The extended classifier imports the original classifier and the three new `rules.py` modules; it also hashes the staged
ImageIO dylib. Parsing or exact-schema failures abort the audit rather than silently creating exclusions.
