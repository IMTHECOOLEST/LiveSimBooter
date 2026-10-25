#!/usr/bin/env python3
"""Patch exact iOS 26 build 23A343 tools to build/map a simulator cache at 0x120000000.

Use the patched builder only with -arch arm64 and an iOS simulator RuntimeRoot. The builder patch replaces
the nonsimulator layout selection; it is not a general-purpose cache builder. The dyld_sim patch uses the
cache header's base and bounds its failure cleanup to the cache span. Its original executable-overlap
heuristic remains unchanged; the current df test needs no environment override. Do not set
DYLD_SHARED_REGION=private in the app launch environment: native dyld sees it before main and its private
cache mapping conflicts with retained native mappings. If ever needed, add it only to the simulator's
environment after native startup. Check the destination VM range is free before mapping. Outputs retain
invalidated signatures and must be signed separately.
"""

import argparse
import hashlib
import json
import stat
import struct
import sys
from pathlib import Path

BASE = 0x120000000
BINARY_SPECS = {
    'builder': {
        'name': 'update_dyld_sim_shared_cache',
        'source_sha256': '79e721958b8d733bd943cc550dee7336b9fcafa2e90619f16c36dd2931f396a3',
        'patched_sha256': '79685c6bdf494d3a776e48e5469e49a0ebf718376b2e6fba7b371a574b164af9',
        'patches': (
            (0x5EA5C, 0xB26107E8, 0xD2C00028, 'mov x8, #0x100000000'),
            (0x5EA60, 0xD2C00029, 0xB2630108, 'orr x8, x8, #0x20000000'),
            (0x5EA64, 0x9A881129, 0xD2C00029, 'mov x9, #0x100000000'),
        ),
    },
    'dyld_sim': {
        'name': 'dyld_sim',
        'source_sha256': '47a6f5e9b44ae48182ec085b748f708c35254b894c4bbc51e3fe47bfe18c528a',
        'patched_sha256': 'd20fcb59e37833db9f96049d617cc6380977c3604ff9d8f0e1523b4f3d92bf82',
        'patches': (
            (0x1C1B4, 0xB26107E8, 0xAA0A03E8, 'mov x8, x10; use first mapping base'),
            (0x1C47C, 0xB26107E0, 0xF94903E0, 'ldr x0, [sp, #0x1200]; cleanup cache base'),
            (0x1C480, 0xB26107E1, 0xF94907E1, 'ldr x1, [sp, #0x1208]; cleanup cache size'),
        ),
    },
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def validate_paths(sources, output_dir):
    require(not output_dir.exists() or output_dir.is_dir(), f'Output directory is not a directory: {output_dir}')
    for source in sources.values():
        require(source.is_file(), f'Input is not a regular file: {source}')
    require(not sources['builder'].samefile(sources['dyld_sim']), 'Builder and dyld_sim inputs must be different files')
    outputs = [output_dir / spec['name'] for spec in BINARY_SPECS.values()] + [output_dir / 'patch-manifest.json']
    for output in outputs:
        for source in sources.values():
            same_file = output.resolve() == source or (output.exists() and output.samefile(source))
            require(not same_file, f'Output would overwrite an input: {output}')
        require(not output.exists() and not output.is_symlink(), f'Refusing to overwrite existing output: {output}')


def validate_original(source, spec):
    original = source.read_bytes()
    digest = sha256(original)
    require(digest == spec['source_sha256'], f'Unsupported 23A343 source SHA-256 for {source}: {digest}')
    require(struct.unpack_from('<II', original) == (0xFEEDFACF, 0x0100000C), f'Expected a thin arm64 Mach-O: {source}')
    for offset, before, _, _ in spec['patches']:
        require(struct.unpack_from('<I', original, offset)[0] == before, f'Unexpected instruction at {source}+{offset:#x}')
    return original


def validate_patched(original, patched, spec):
    require(len(original) == len(patched), f"Patched length changed: {spec['name']}")
    cursor, changed_bytes = 0, 0
    for offset, _, after, _ in spec['patches']:
        require(patched[cursor:offset] == original[cursor:offset], f'Unexpected change before {offset:#x}')
        require(struct.unpack_from('<I', patched, offset)[0] == after, f'Wrong replacement instruction at {offset:#x}')
        changed_bytes += sum(before != replacement for before, replacement in zip(original[offset:offset + 4], patched[offset:offset + 4]))
        cursor = offset + 4
    require(patched[cursor:] == original[cursor:], f"Unexpected change after patches: {spec['name']}")
    require(sha256(patched) == spec['patched_sha256'], f"Unexpected patched SHA-256: {spec['name']}")
    return changed_bytes


def patch_binaries(sources, output_dir):
    validate_paths(sources, output_dir)
    prepared, records = [], []
    # Validate both originals and all intended changes before creating any output.
    for kind, spec in BINARY_SPECS.items():
        source = sources[kind]
        original = validate_original(source, spec)
        patched = bytearray(original)
        for offset, _, after, _ in spec['patches']:
            struct.pack_into('<I', patched, offset, after)
        changed_bytes = validate_patched(original, patched, spec)
        output = output_dir / spec['name']
        prepared.append((source, output, original, patched, spec))
        records.append(dict(source=str(source), output=str(output), source_sha256=spec['source_sha256'],
                            patched_sha256=spec['patched_sha256'], changed_bytes=changed_bytes,
                            patches=[dict(file_offset=hex(o), before=hex(b), after=hex(a), instruction=instruction)
                                     for o, b, a, instruction in spec['patches']]))
    report = dict(base=hex(BASE), runtime_build='23A343', simulator_only=True, builder_arch='arm64',
                  overlap_heuristic='Unchanged; the current df test needs no environment override.',
                  environment_warning='Do not set DYLD_SHARED_REGION=private at native app launch; only add it after native startup if needed.',
                  requires_separate_signing=True, binaries=records)
    output_dir.mkdir(parents=True, exist_ok=True)
    created = []
    try:
        for source, output, original, patched, spec in prepared:
            with output.open('xb') as stream:
                created.append(output)
                stream.write(patched)
            output.chmod(stat.S_IMODE(source.stat().st_mode))
            validate_patched(original, output.read_bytes(), spec)
        for source, _, original, _, _ in prepared:
            require(source.read_bytes() == original, f'Input changed during patching: {source}')
        manifest = output_dir / 'patch-manifest.json'
        with manifest.open('x') as stream:
            created.append(manifest)
            stream.write(json.dumps(report, indent=2) + '\n')
    except Exception:
        for output in reversed(created):
            output.unlink()
        raise
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--builder', required=True, type=Path, help='Original 23A343 update_dyld_sim_shared_cache')
    parser.add_argument('--dyld-sim', required=True, type=Path, help='Original 23A343 RuntimeRoot/usr/lib/dyld_sim')
    parser.add_argument('--output-dir', required=True, type=Path, help='Directory for new binaries and patch-manifest.json; outputs must not exist')
    args = parser.parse_args()
    sources = {'builder': args.builder.resolve(), 'dyld_sim': args.dyld_sim.resolve()}
    try:
        report = patch_binaries(sources, args.output_dir.resolve())
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f'error: {error}\n')
    print('Use only -arch arm64 simulator roots; do not set DYLD_SHARED_REGION at native app launch. Sign outputs separately.', file=sys.stderr)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
