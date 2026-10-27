#!/usr/bin/env python3
"""Rename container-metadata filename literals in the validated 23A343 simulator cache or helper.

Cache mode requires the 1,353-image arm64 simulator layout at 0x110000000 and the five exact image
paths, UUIDs and original __TEXT,__cstring sections. Layout offsets, cache UUID and signature may
change across rebuilds. --mach-o supports only the matching MobileInstallationHelperService image.
The source remains unchanged. Equal-length replacements invalidate signatures: sign the new cache
with Tools/sign-dyld-cache.py, or the helper with Tools/sign-sim-executable.py, before deployment.
No device actions are performed. This changes filenames, not the sandbox policy or container checks.
"""

import argparse
import hashlib
import json
import mmap
import os
import stat
import struct
import sys
import uuid
from pathlib import Path


ORIGINAL = b'.com.apple.mobile_container_manager.metadata.plist'
REPLACEMENT = b'.com.kdt.LiveSimBooter.container-metadata.v1.plist'
CHUNK_SIZE = 4 * 1024 * 1024
PROFILE = {
    'sha256': 'ad3b1eb360f5f1883d8eb49efec351116084e26b1009b73b521e91c2acbebd09',
    'uuid': '73ED978A-FD25-3166-A57A-34E21BB7E5B8',
    'base': 0x110000000,
    'span': 0x7E194000,
    'image_count': 1353,
    'sites': (0x216C5EF2, 0x2565779E, 0x35F6D6AA, 0x42D5B9B8, 0x5A9A24C7),
}
PROFILE['owners'] = [
    dict(path='/System/Library/PrivateFrameworks/ContainerManagerCommon.framework/ContainerManagerCommon',
         uuid='DDD62F2B-454B-334B-886E-E4FBFBAE523A', size=0x8BA7, relative=0x1442,
         section_sha256='111da250c1a7655e793debb3557568f780e5e5bc2ee7041e3858eaf644b031bb'),
    dict(path='/System/Library/PrivateFrameworks/DMCUtilities.framework/DMCUtilities',
         uuid='FADC31EF-4790-3D7B-A895-56C8B5C6AE4D', size=0x31F4, relative=0x26E2,
         section_sha256='74c4d60e6404014db18914c13322bd7a32f07871a69c062c73fcb9345e7a3cb5'),
    dict(path='/System/Library/PrivateFrameworks/MobileInstallation.framework/MobileInstallation',
         uuid='7617ADA2-8257-3C9A-8333-E9DA188A0728', size=0x7B9F, relative=0x5052,
         section_sha256='0d94eaa5af664a3ccd4d47cf5abc3e7ed3e03948bfaf9a3d1dbb2cc7d4a99525'),
    dict(path='/System/Library/PrivateFrameworks/SafariCore.framework/SafariCore',
         uuid='4FFA3634-B727-3303-9246-7C714CD66639', size=0x119D5, relative=0x93D8,
         section_sha256='e9bceff7197d323eb0928830d4ef64dab748af57ed6932bc3dd2d8fc301c3642'),
    dict(path='/usr/lib/system/libsystem_containermanager.dylib', uuid='1F6AA4B4-5480-329B-A016-668D816C5353',
         size=0x3899, relative=0x1237, section_sha256='df25c844a319b2a5b0b0b1c535f0b8e51da5ae3658c77a8cafe998f7800c846b'),
]
HELPER = dict(path='/System/Library/PrivateFrameworks/MobileInstallation.framework/XPCServices/'
                   'com.apple.MobileInstallationHelperService.xpc/com.apple.MobileInstallationHelperService',
              uuid='3F7ABD1C-04D6-3A63-9C61-731176B12263', size=0x3784, relative=0x1EA1,
              section_sha256='b79ffa957ea55a428f6cadb5df1b2c78eb6925fe82c7c70abb123cceb234efcb')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def file_identity(metadata):
    return (metadata.st_dev, metadata.st_ino, metadata.st_size, metadata.st_mtime_ns, metadata.st_ctime_ns)


def hash_stream(stream):
    stream.seek(0)
    digest = hashlib.sha256()
    while True:
        block = stream.read(CHUNK_SIZE)
        if not block:
            return digest.hexdigest()
        digest.update(block)


def literal_occurrences(data, sites):
    require(len(ORIGINAL) == len(REPLACEMENT) == 50, 'Filename patch must preserve literal length')
    found, cursor = [], 0
    while True:
        cursor = data.find(ORIGINAL, cursor)
        if cursor < 0:
            break
        found.append(cursor)
        require(len(found) <= len(sites), 'Unexpected extra metadata filename occurrence')
        cursor += len(ORIGINAL)
    require(found == sorted(sites), 'Metadata filename occurrences do not match validated image sections')
    require(data.find(REPLACEMENT) == -1, 'Replacement filename already exists; refusing a partial or repeated patch')


def inspect_image(data, start, spec, file_offset, payload_end, executable=False):
    require(0 <= start <= payload_end - 32, 'Image header outside payload')
    magic, cpu, subtype, kind, count, size, _, _ = struct.unpack_from('<8I', data, start)
    require(magic == 0xFEEDFACF and cpu == 0x100000C and subtype & 0xFFFFFF == 0 and kind == (2 if executable else 6),
            'Expected matching thin arm64 Mach-O type')
    end = start + 32 + size
    require(0 < count <= 2048 and size >= count * 8 and end <= payload_end, 'Invalid image load-command bounds')
    cursor, image_uuid, section, platform = start + 32, None, None, None
    signature = None
    for _ in range(count):
        require(cursor <= end - 8, 'Truncated image load command')
        command, length = struct.unpack_from('<II', data, cursor)
        require(length >= 8 and length % 8 == 0 and cursor + length <= end, 'Invalid image load command')
        if command == 0x1B:
            require(length == 24 and image_uuid is None, 'Invalid or duplicate image UUID')
            image_uuid = str(uuid.UUID(bytes=data[cursor + 8:cursor + 24])).upper()
        elif command == 0x32:
            require(length >= 24 and platform is None, 'Invalid or duplicate build-version command')
            platform = struct.unpack_from('<I', data, cursor + 8)[0]
        elif command == 0x1D and executable:
            require(length == 16 and signature is None, 'Invalid or duplicate signature command')
            signature = struct.unpack_from('<II', data, cursor + 8)
            require(signature[0] > 0 and signature[0] + signature[1] <= len(data), 'Invalid Mach-O signature extent')
        elif command == 0x19:
            require(length >= 72, 'Truncated segment command')
            segment_name = data[cursor + 8:cursor + 24].rstrip(b'\0')
            address, vm_size, segment_offset, file_size, maximum, initial, sections, _ = struct.unpack_from('<QQQQIIII', data, cursor + 24)
            require(sections <= 4096 and length == 72 + sections * 80, 'Invalid segment section table')
            for index in range(sections):
                position = cursor + 72 + index * 80
                name, owner = data[position:position + 16].rstrip(b'\0'), data[position + 16:position + 32].rstrip(b'\0')
                if name != b'__cstring' or owner != b'__TEXT':
                    continue
                require(segment_name == b'__TEXT' and section is None, 'Ambiguous cstring ownership')
                section_address, section_size, direct_offset = struct.unpack_from('<QQI', data, position + 32)
                flags = struct.unpack_from('<I', data, position + 64)[0]
                require(flags & 0xFF == 2 and section_size == spec['size'] and
                        address <= section_address < section_address + section_size <= address + vm_size,
                        'Unexpected cstring section extent/type')
                offset = direct_offset if executable else file_offset(section_address, section_size)
                require(maximum in (1, 5) and initial in (1, 5), 'CString segment must be RX/RO')
                require(0 <= offset < offset + section_size <= payload_end, 'CString section exceeds payload')
                if executable:
                    require(segment_offset <= offset and offset + section_size <= segment_offset + file_size and
                            offset - segment_offset == section_address - address, 'CString file/VM coordinates disagree')
                content = data[offset:offset + section_size]
                require(hashlib.sha256(content).hexdigest() == spec['section_sha256'], 'Unsupported cstring section SHA-256')
                relative = spec['relative']
                require(0 < relative <= section_size - 51 and content[relative - 1:relative + 51] == b'\0' + ORIGINAL + b'\0',
                        'Unexpected literal bytes or boundary inside cstring section')
                section = dict(image=spec['path'], image_uuid=spec['uuid'], section='__TEXT,__cstring',
                               section_sha256=spec['section_sha256'], section_bytes=section_size, section_relative_offset=hex(relative),
                               file_offset=hex(offset + relative), address=hex(section_address + relative),
                               max_protection=maximum, protection=initial, literal_bytes=50)
        cursor += length
    require(cursor == end and image_uuid == spec['uuid'], 'Unexpected image UUID or command count')
    require(section is not None, 'Missing pinned cstring section')
    if executable:
        require(platform == 7, 'Expected an iOS simulator helper')
        if signature:
            require(int(section['file_offset'], 16) + 51 <= signature[0], 'Literal overlaps Mach-O signature')
    return section


def inspect_cache(data, profile):
    require(len(ORIGINAL) == len(REPLACEMENT) == 50, 'Filename patch must preserve literal length')
    require(len(data) >= 456 and data[:16] == b'dyld_v1   arm64\0', 'Expected a complete arm64 cache header')
    mapping_offset, mapping_count = struct.unpack_from('<II', data, 16)
    signature_offset, signature_size = struct.unpack_from('<QQ', data, 40)
    cache_uuid = str(uuid.UUID(bytes=data[88:104])).upper()
    platform, flags = struct.unpack_from('<II', data, 216)
    base, span = struct.unpack_from('<QQ', data, 224)
    images_offset, image_count = struct.unpack_from('<II', data, 448)
    require(uuid.UUID(cache_uuid).int != 0, 'Invalid cache UUID')
    require(base == profile['base'] and 0 < span < (1 << 32) and base + span < (1 << 64), 'Unexpected cache reservation')
    require(platform == 7 and flags & 0x200, 'Expected an iOS simulator cache')
    require(struct.unpack_from('<I', data, 396)[0] == 0, 'Only the pinned single-file cache is supported')
    require(signature_offset > 0 and signature_offset % 16384 == 0 and
            signature_offset + signature_size == len(data), 'Invalid signature extent')
    require(mapping_offset >= 456 and 0 < mapping_count <= 64 and mapping_offset + mapping_count * 32 <= signature_offset,
            'Invalid mapping table')
    require(image_count == profile['image_count'] and images_offset >= mapping_offset + mapping_count * 32 and
            images_offset + image_count * 32 <= signature_offset, 'Invalid image table or image count')
    mappings = []
    for index in range(mapping_count):
        address, size, offset, maximum, initial = struct.unpack_from('<QQQII', data, mapping_offset + index * 32)
        require(size > 0 and base <= address < address + size <= base + span and offset + size <= signature_offset,
                'Mapping exceeds cache reservation or signed payload')
        require(maximum & ~7 == 0 and initial & ~maximum == 0, 'Invalid mapping protection')
        for prior in mappings:
            require(address + size <= prior['address'] or address >= prior['address'] + prior['size'], 'Overlapping VM mappings')
            require(offset + size <= prior['offset'] or offset >= prior['offset'] + prior['size'], 'Overlapping file mappings')
        mappings.append(dict(address=address, size=size, offset=offset, maximum=maximum, initial=initial))
    require(mappings[0]['address'] == base and mappings[0]['offset'] == 0, 'First mapping does not match cache base')
    def file_offset(address, size=1):
        matches = [region for region in mappings if region['address'] <= address and address + size <= region['address'] + region['size']]
        require(len(matches) == 1, 'Image address lies outside one cache mapping')
        return matches[0]['offset'] + address - matches[0]['address']

    owners = {owner['path']: owner for owner in profile['owners']}
    require(len(owners) == 5, 'Expected five pinned image owners')
    images = {}
    for index in range(image_count):
        address, _, _, name_offset, _ = struct.unpack_from('<QQQII', data, images_offset + index * 32)
        require(0 < name_offset < signature_offset, 'Invalid image path offset')
        name_end = data.find(b'\0', name_offset, min(signature_offset, name_offset + 4096))
        require(name_end > name_offset, 'Unterminated image path')
        path = data[name_offset:name_end].decode('utf-8')
        require(path.startswith('/') and path not in images, 'Invalid or duplicate image path')
        images[path] = address
    require(all(path in images for path in owners), 'Missing pinned image owner')
    records = []
    for path, spec in owners.items():
        header_offset = file_offset(images[path], 32)
        command_bytes = struct.unpack_from('<I', data, header_offset + 20)[0]
        file_offset(images[path], 32 + command_bytes)  # Header and complete command table must stay in one mapped payload extent.
        records.append(inspect_image(data, header_offset, spec, file_offset, signature_offset))
    sites = tuple(sorted(int(record['file_offset'], 16) for record in records))
    require(len(set(sites)) == 5, 'Aliased metadata filename sites')
    for offset in sites:
        require(0 < offset <= signature_offset - len(ORIGINAL) - 1 and
                data[offset:offset + len(ORIGINAL) + 1] == ORIGINAL + b'\0', 'Unexpected literal bytes at %#x' % offset)
        matches = [region for region in mappings if region['offset'] <= offset and
                   offset + len(ORIGINAL) + 1 <= region['offset'] + region['size']]
        require(len(matches) == 1 and matches[0]['maximum'] in (1, 5) and matches[0]['initial'] in (1, 5),
                'Literal must lie entirely in an RX/RO signed mapping at %#x' % offset)
    literal_occurrences(data, sites)
    return dict(cache_uuid=cache_uuid, cache_base=hex(base), cache_span=hex(span), image_count=image_count,
                signature_offset=signature_offset, signature_size=signature_size, sites=records)


def inspect_helper(data, spec):
    record = inspect_image(data, 0, spec, None, len(data), executable=True)
    literal_occurrences(data, [int(record['file_offset'], 16)])
    return dict(image=spec['path'], sites=[record])


def validate_paths(source, output, manifest):
    metadata = source.lstat()
    require(stat.S_ISREG(metadata.st_mode) and not source.is_symlink(), 'Source must be a regular, nonsymlink file')
    if output is None:
        require(manifest is None, 'A manifest requires an output file')
        return metadata
    require(output.resolve() != source.resolve() and manifest.resolve() != source.resolve(), 'Refusing in-place patching')
    require(output.resolve() != manifest.resolve(), 'Output and manifest must be different paths')
    for path in (output, manifest):
        require(not path.exists() and not path.is_symlink(), 'Refusing existing output: %s' % path)
        require(path.parent.is_dir(), 'Output parent directory must already exist: %s' % path.parent)
    return metadata


def patch_cache(source, output=None, manifest=None, profile=None, mach_o=False):
    profile = (HELPER if mach_o else PROFILE) if profile is None else profile  # Only offline tests supply a synthetic profile.
    source = Path(source).absolute()
    output = Path(output).absolute() if output is not None else None
    manifest = (Path(manifest).absolute() if manifest is not None else Path(str(output) + '.patch-manifest.json')) if output else None
    before = validate_paths(source, output, manifest)
    created = []
    try:
        descriptor = os.open(str(source), os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(descriptor, 'rb') as stream:
            require(file_identity(os.fstat(stream.fileno())) == file_identity(before), 'Source changed before opening')
            require(before.st_size > 0, 'Empty cache input')
            with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
                details = inspect_helper(data, profile) if mach_o else inspect_cache(data, profile)
            digest = hash_stream(stream)
            sites = tuple(sorted(int(record['file_offset'], 16) for record in details['sites']))
            report = dict(version=1, runtime='iOS Simulator 26.0', runtime_build='23A343', source=str(source),
                          source_sha256=digest, source_bytes=before.st_size, original_filename=ORIGINAL.decode(),
                          replacement_filename=REPLACEMENT.decode(), requires_separate_signing=True,
                          kind='mach-o' if mach_o else 'cache', known_signed_cache_variant=not mach_o and digest == PROFILE['sha256'], **details)
            require(file_identity(os.fstat(stream.fileno())) == file_identity(before), 'Source changed during validation')
            if output is None:
                report['validation_only'] = True
                return report
            original_hash, patched_hash = hashlib.sha256(), hashlib.sha256()
            position = 0
            stream.seek(0)
            with output.open('xb') as destination:
                created.append((output, os.fstat(destination.fileno())))
                while True:
                    block = stream.read(CHUNK_SIZE)
                    if not block:
                        break
                    original_hash.update(block)
                    changed = bytearray(block)
                    for offset in sites:
                        begin, end = max(position, offset), min(position + len(block), offset + len(REPLACEMENT))
                        if begin < end:
                            changed[begin - position:end - position] = REPLACEMENT[begin - offset:end - offset]
                    destination.write(changed)
                    patched_hash.update(changed)
                    position += len(block)
                require(position == before.st_size and original_hash.hexdigest() == digest, 'Source changed during copy')
                require(file_identity(os.fstat(stream.fileno())) == file_identity(before), 'Source metadata changed during copy')
                os.fchmod(destination.fileno(), stat.S_IMODE(before.st_mode) & 0o777)
                destination.flush()
                os.fsync(destination.fileno())
            require(file_identity(source.lstat()) == file_identity(before), 'Source path changed during patching')
            with output.open('rb') as verification:
                require(hash_stream(verification) == patched_hash.hexdigest(), 'Output verification failed')
            changed_per_site = sum(a != b for a, b in zip(ORIGINAL, REPLACEMENT))
            report.update(output=str(output), output_sha256=patched_hash.hexdigest(), output_bytes=position,
                          changed_bytes=changed_per_site * len(sites), signature_invalidated=True,
                          verification='Complete output SHA-256 matches the streamed source with only the validated literal replacements')
            with manifest.open('x') as record:
                created.append((manifest, os.fstat(record.fileno())))
                json.dump(report, record, indent=2)
                record.write('\n')
            return report
    except Exception:
        for path, metadata in reversed(created):
            try:
                current = path.lstat()
                if (current.st_dev, current.st_ino) == (metadata.st_dev, metadata.st_ino):
                    path.unlink()
            except FileNotFoundError:
                pass
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path, help='Original matching cache or helper; never modified')
    parser.add_argument('--mach-o', action='store_true', help='Patch only the matching 23A343 MobileInstallationHelperService executable')
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--output', type=Path, help='New patched-file path in an existing directory')
    action.add_argument('--check-only', action='store_true', help='Validate the pinned input without writing files')
    parser.add_argument('--manifest', type=Path, help='New JSON path; default OUTPUT.patch-manifest.json')
    args = parser.parse_args()
    if args.check_only and args.manifest:
        parser.error('--manifest requires --output')
    try:
        report = patch_cache(args.source, args.output, args.manifest, mach_o=args.mach_o)
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, 'error: %s\n' % error)
    if args.output:
        print('Output signature is invalidated; sign the new file separately before deployment.', file=sys.stderr)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
