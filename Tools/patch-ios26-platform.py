#!/usr/bin/env python3
"""Let the exact iOS 26 (23A343) dyld_sim run plain arm64 iOS images with its Simulator runtime.

Creates a new dyld_sim and JSON manifest; never writes an input or overwrites an output. Sign the output
separately. Original Mach-O platform commands in executables/dylibs do not need to change. This patch
does not provide missing APIs, arm64e ABI support, decryption, or compatibility with arbitrary runtimes.
"""

import argparse
import hashlib
import json
import stat
import struct
from pathlib import Path


SUPPORTED_INPUTS = {
    '47a6f5e9b44ae48182ec085b748f708c35254b894c4bbc51e3fe47bfe18c528a': 'original 23A343',
    'd20fcb59e37833db9f96049d617cc6380977c3604ff9d8f0e1523b4f3d92bf82': 'relocated-cache patch, unsigned',
    'a5eac19614450ce9b496b7fdb7720a2812d471c6c487cc24058fd102dad0406b': 'relocated-cache patch, existing project signature',
}

# Process::getMainPlatform(): keep minOS/SDK values, but map the iOS main process to iOS Simulator.
# The replaced DYLD_FORCE_PLATFORM branch only applied to macOS main images, which our loader rejects.
# Header::loadableIntoProcess(): reuse the otherwise unreachable macCatalyst->macOS fallback for
# iOS Simulator->iOS. Existing Simulator->visionOS Simulator and host-library exceptions stay intact.
# No platform structures or Mach-O headers are mutated. Every offset is both VM and file offset in
# this exact dyld_sim's __TEXT segment; the complete input hash pins its symbols/layout too.
PATCHES = (
    (0x3520, 0x9131A14A, 0x9131E14A, 'Compare main platform with Platform::iOS instead of macOS'),
    (0x3538, 0xF9405E60, 0x90000288, 'adrp x8, Platform::iOS_simulator@PAGE'),
    (0x353C, 0x90000201, 0x91332108, 'add x8, x8, #0xcc8'),
    (0x3540, 0x912F3021, 0xF9400109, 'ldr x9, [x8]'),
    (0x3544, 0x9400C317, 0xF9000289, 'str x9, [x20]'),
    (0x3548, 0xB4000660, 0xB9400908, 'ldr w8, [x8, #8]'),
    (0x354C, 0xAA0003F5, 0xB9000A88, 'str w8, [x20, #8]'),
    (0x3550, 0xF9400660, 0x14000031, 'b 0x3614; preserve original SDK/minOS and compute version sets'),
    (0x3B1C0, 0xF9465D29, 0xF9466529, 'Load iOS Simulator platform singleton for final dependency fallback'),
    (0x3B1D4, 0xB94CC129, 0xB94CD129, 'Load iOS Simulator platform discriminator for final dependency fallback'),
    (0x3B1E4, 0x9131A108, 0x9131E108, 'Compare dependency platform with iOS instead of macOS'),
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def patched_bytes(original):
    digest = sha256(original)
    require(digest in SUPPORTED_INPUTS, f'Unsupported 23A343 dyld_sim SHA-256: {digest}')
    require(len(original) >= 32 and struct.unpack_from('<IIII', original) == (0xFEEDFACF, 0x0100000C, 0, 7),
            'Expected a thin arm64 MH_DYLINKER')
    result = bytearray(original)
    for offset, before, after, _ in PATCHES:
        require(offset + 4 <= len(original), f'Truncated patch site at {offset:#x}')
        require(struct.unpack_from('<I', original, offset)[0] == before, f'Unexpected instruction at {offset:#x}')
        struct.pack_into('<I', result, offset, after)
    return bytes(result)


def patch_file(source, output):
    require(source.is_file(), f'Input is not a regular file: {source}')
    manifest = output.with_name(output.name + '.platform-patch.json')
    for destination in (output, manifest):
        require(destination.resolve() != source.resolve(), f'Output would overwrite input: {destination}')
        require(not destination.exists() and not destination.is_symlink(), f'Refusing to overwrite: {destination}')
    original = source.read_bytes()
    patched = patched_bytes(original)
    digest = sha256(original)
    report = dict(runtime_build='23A343', source=str(source), source_sha256=digest, input_variant=SUPPORTED_INPUTS[digest],
                  output=str(output), output_sha256=sha256(patched), requires_separate_signing=True,
                  main_platform='iOS -> iOS Simulator; original minOS/SDK preserved',
                  dependency_platforms='iOS and existing supported Simulator platforms; all other checks unchanged',
                  unsupported_main_platforms='macOS/macCatalyst remain rejected by LiveSimBooter',
                  patches=[dict(offset=hex(offset), before=hex(before), after=hex(after), description=description)
                           for offset, before, after, description in PATCHES])
    output.parent.mkdir(parents=True, exist_ok=True)
    created = []
    try:
        with output.open('xb') as stream:
            created.append(output)
            stream.write(patched)
        output.chmod(stat.S_IMODE(source.stat().st_mode))
        require(output.read_bytes() == patched, 'Output verification failed')
        require(source.read_bytes() == original, 'Input changed during patching')
        with manifest.open('x') as stream:
            created.append(manifest)
            stream.write(json.dumps(report, indent=2) + '\n')
    except Exception:
        for destination in reversed(created):
            destination.unlink()
        raise
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    try:
        report = patch_file(args.source.resolve(), args.output.absolute())
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f'error: {error}\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
