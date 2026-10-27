#!/usr/bin/env python3
"""Sign a single-file arm64 iOS simulator cache with an exact keychain identity.

Requires macOS, Xcode command-line tools, and a development signing identity. The source is never edited.
Ordinary codesign supplies the identifier, team, requirements, and executable-segment metadata. Its DSC
writer emits scatter entries rejected by TXM, so rehash contiguous 16KB pages and sign the new CodeDirectory
with Security.framework. No entitlements or executable-segment privileges are added.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

PAGE_SIZE = 16384
SUPERBLOB = 0xFADE0CC0
SLOT_MAGICS = {0: 0xFADE0C02, 2: 0xFADE0C01, 0x10000: 0xFADE0B01}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def run(command):
    subprocess.run([str(value) for value in command], check=True, stdout=sys.stderr)


def cache_info(path):
    with path.open('rb') as stream:
        header = stream.read(400)
        require(len(header) == 400 and header[:16] == b'dyld_v1   arm64\0', 'Expected an arm64 dyld cache header')
        mapping_offset, mapping_count = struct.unpack_from('<II', header, 16)
        signature_offset, signature_size = struct.unpack_from('<QQ', header, 40)
        platform, flags = struct.unpack_from('<II', header, 216)
        require(platform == 7 and flags & 0x200, 'Expected an iOS simulator cache')
        require(mapping_offset >= 400 and struct.unpack_from('<I', header, 396)[0] == 0, 'Only single-file caches are supported')
        require(signature_offset % PAGE_SIZE == 0 and 0 < signature_offset < 0xFFFFFFFF, 'Unsupported signature offset')
        require(signature_offset <= path.stat().st_size, 'Signature offset exceeds file length')
        require(0 < mapping_count <= 64 and mapping_offset + mapping_count * 32 <= signature_offset, 'Invalid cache mapping table')
        stream.seek(mapping_offset)
        mappings = []
        for _ in range(mapping_count):
            address, size, offset, maximum, initial = struct.unpack('<QQQII', stream.read(32))
            require(size > 0 and offset + size <= signature_offset, 'Cache mapping exceeds signed payload')
            mappings.append(dict(address=address, size=size, file_offset=offset, max_protection=maximum, protection=initial))
    executable = [mapping for mapping in mappings if mapping['protection'] & 4]
    require(len(executable) == 1 and executable[0]['file_offset'] == 0, 'Expected one executable mapping at file offset zero')
    return signature_offset, signature_size, mappings


def signature_slots(path, signature_offset):
    with path.open('rb') as stream:
        stream.seek(signature_offset)
        header = stream.read(12)
        require(len(header) == 12, 'Missing embedded signature')
        magic, length, count = struct.unpack('>III', header)
        require(magic == SUPERBLOB and count == 3, 'Expected one CodeDirectory, requirements, and CMS signature')
        require(36 <= length <= path.stat().st_size - signature_offset, 'Invalid signature superblob length')
        signature = header + stream.read(length - 12)
    slots, ranges = {}, []
    for index in range(count):
        kind, offset = struct.unpack_from('>II', signature, 12 + index * 8)
        require(kind in SLOT_MAGICS and kind not in slots, 'Unsupported or duplicate signature slot (entitlements are not supported)')
        require(36 <= offset <= length - 8, 'Invalid signature blob offset')
        magic, size = struct.unpack_from('>II', signature, offset)
        require(magic == SLOT_MAGICS[kind] and size >= 8 and offset + size <= length, 'Invalid signature blob')
        require(all(offset + size <= start or offset >= end for start, end in ranges), 'Overlapping signature blobs')
        ranges.append((offset, offset + size))
        slots[kind] = signature[offset:offset + size]
    return slots, length


def cd_info(cd, args, signature_offset, mappings):
    require(len(cd) >= 88, 'Truncated CodeDirectory')
    version, flags, hash_offset, id_offset, special_count, page_count, code_limit = struct.unpack_from('>7I', cd, 8)
    require(version == 0x20400 and flags == 0, 'Expected CodeDirectory v0x20400 without signing flags')
    require(cd[36:39] == bytes([32, 2, 0]) and cd[39] in (12, 14), 'Expected SHA-256 code hashes with 4KB or 16KB pages')
    require(special_count == 2 and code_limit == signature_offset, 'Unexpected special slots or signed range')
    require(page_count == (code_limit + (1 << cd[39]) - 1) >> cd[39], 'Incorrect code page count')
    require(88 + special_count * 32 <= hash_offset and hash_offset + page_count * 32 == len(cd), 'Invalid hash table bounds')
    scatter_offset, team_offset = struct.unpack_from('>II', cd, 44)
    for offset, expected, label in ((id_offset, args.identifier, 'identifier'), (team_offset, args.team, 'team')):
        limit = hash_offset - special_count * 32
        require(88 <= offset < limit, f'Invalid {label} offset')
        end = cd.find(b'\0', offset, limit)
        require(end >= 0 and cd[offset:end].decode('utf-8') == expected, f'Unexpected signing {label}')
    executable = next(mapping for mapping in mappings if mapping['protection'] & 4)
    require(struct.unpack_from('>QQQ', cd, 64) == (0, executable['size'], 0), 'Unexpected executable-segment metadata')
    require(cd[hash_offset - 32:hash_offset] == bytes(32), 'Unexpected Info.plist hash')
    return hash_offset, page_count, 1 << cd[39], scatter_offset


def verify_hashes(path, cd, requirements, args, signature_offset, mappings):
    hash_offset, page_count, page_size, scatter = cd_info(cd, args, signature_offset, mappings)
    require(scatter == 0 and page_size == PAGE_SIZE, 'Expected contiguous 16KB code hashes')
    require(hashlib.sha256(requirements).digest() == cd[hash_offset - 64:hash_offset - 32], 'Requirements hash mismatch')
    with path.open('rb') as stream:
        for index in range(page_count):
            data = stream.read(min(page_size, signature_offset - index * page_size))
            require(hashlib.sha256(data).digest() == cd[hash_offset + index * 32:hash_offset + (index + 1) * 32],
                    f'Code page hash mismatch: {index}')
    return page_count


def cms_helper(args, work):
    supplied = getattr(args, 'cms_helper', None)
    if supplied is not None:
        helper = Path(supplied).resolve(strict=True)
        require(helper.is_file() and os.access(helper, os.X_OK), 'CMS helper must be an executable regular file')
        return helper
    helper = work / 'sign-code-directory'
    helper_source = Path(__file__).resolve().with_name('sign-code-directory.m')
    run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-framework', 'Foundation', '-framework', 'Security', helper_source, '-o', helper])
    return helper


def sign_cache(args):
    signature_offset, _, mappings = cache_info(args.source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.cache-sign-', dir=args.output.parent) as directory:
        work = Path(directory)
        output = work / 'cache'
        helper = cms_helper(args, work)
        command = ['/usr/bin/codesign', '--force', '--sign', args.identity, '--identifier', args.identifier, '--timestamp=none', output]
        shutil.copy2(args.source, output)
        run(command)
        _, blob_size = signature_slots(output, signature_offset)
        # codesign does not update the cache's codeSignatureSize. Reserve space before hashing the final header.
        capacity = (blob_size + 1024 + PAGE_SIZE - 1) & -PAGE_SIZE
        shutil.copy2(args.source, output)
        with output.open('r+b') as stream:
            stream.seek(48)
            stream.write(struct.pack('<Q', capacity))
            stream.truncate(signature_offset + capacity)
        run(command)
        run(['/usr/bin/codesign', '--verify', '--verbose=4', output])
        slots, _ = signature_slots(output, signature_offset)
        old_cd = slots[0]
        hash_offset, _, _, _ = cd_info(old_cd, args, signature_offset, mappings)
        cd = bytearray(old_cd[:hash_offset])
        cd[39] = 14
        struct.pack_into('>I', cd, 44, 0)
        struct.pack_into('>I', cd, 28, signature_offset // PAGE_SIZE)
        with output.open('rb') as stream:
            for _ in range(signature_offset // PAGE_SIZE):
                cd.extend(hashlib.sha256(stream.read(PAGE_SIZE)).digest())
        struct.pack_into('>I', cd, 4, len(cd))
        cd_path, cms_path = work / 'CodeDirectory.bin', work / 'CMS.der'
        cd_path.write_bytes(cd)
        run([helper, args.identity, cd_path, cms_path])
        # Verify CMS mathematics separately; codesign below also validates the signing identity and requirements.
        run(['/usr/bin/openssl', 'cms', '-verify', '-binary', '-inform', 'DER', '-in', cms_path,
             '-content', cd_path, '-noverify', '-out', '/dev/null'])
        cms = cms_path.read_bytes()
        slots[0] = bytes(cd)
        slots[0x10000] = struct.pack('>II', SLOT_MAGICS[0x10000], len(cms) + 8) + cms
        blob_size = 36 + sum(len(blob) for blob in slots.values())
        require(blob_size <= capacity, 'CMS signature exceeds reserved capacity')
        signature = bytearray(struct.pack('>III', SUPERBLOB, blob_size, 3))
        offset = 36
        for kind, blob in sorted(slots.items()):
            signature.extend(struct.pack('>II', kind, offset))
            offset += len(blob)
        for _, blob in sorted(slots.items()):
            signature.extend(blob)
        with output.open('r+b') as stream:
            stream.seek(signature_offset)
            stream.write(signature)
            stream.write(bytes(capacity - blob_size))
            stream.truncate(signature_offset + capacity)
        run(['/usr/bin/codesign', '--verify', '--verbose=4', output])
        final_offset, final_capacity, final_mappings = cache_info(output)
        require((final_offset, final_capacity, final_mappings) == (signature_offset, capacity, mappings), 'Cache metadata changed unexpectedly')
        final_slots, _ = signature_slots(output, signature_offset)
        require(final_slots == slots, 'Embedded signature differs from verified signature')
        pages = verify_hashes(output, final_slots[0], final_slots[2], args, signature_offset, mappings)
        with args.source.open('rb') as original, output.open('rb') as signed:
            offset = 0
            while offset < signature_offset:
                size = min(1024 * 1024, signature_offset - offset)
                before, after = original.read(size), signed.read(size)
                if offset == 0:
                    before = before[:48] + after[48:56] + before[56:]
                require(before == after, f'Cache payload changed at offset {offset}')
                offset += size
        digest = hashlib.sha256()
        with output.open('rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(block)
        record = dict(source=str(args.source), output=str(args.output), identity=args.identity, identifier=args.identifier, team=args.team,
                      size=output.stat().st_size, sha256=digest.hexdigest(), cdhash=hashlib.sha256(cd).hexdigest()[:40],
                      signature_offset=signature_offset, signature_capacity=capacity, signature_blob_size=blob_size,
                      code_directory_version='0x20400', code_page_size=PAGE_SIZE, verified_code_pages=pages, scatter_offset=0,
                      exec_segment_flags=0, entitlements=None, payload_unchanged_except_signature_size=True, mappings=mappings)
        output.replace(args.output)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--identity', required=True, help='Exact certificate SHA-1 (40 hexadecimal characters)')
    parser.add_argument('--identifier', required=True)
    parser.add_argument('--team', required=True)
    parser.add_argument('--cms-helper', type=Path, help='Reuse a prebuilt sign-code-directory helper for a batch (avoids repeated keychain prompts)')
    args = parser.parse_args()
    args.source, args.output = args.source.resolve(), args.output.resolve()
    require(re.fullmatch(r'[0-9a-fA-F]{40}', args.identity), 'Identity must be an exact certificate SHA-1')
    require(args.identifier and args.team and '\0' not in args.identifier + args.team, 'Identifier and team must be nonempty strings')
    require(args.source != args.output and not (args.output.exists() and args.source.samefile(args.output)), 'Source and output must be different files')
    print(json.dumps(sign_cache(args), indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(f'sign-dyld-cache: {error}')
