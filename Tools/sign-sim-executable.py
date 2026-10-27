#!/usr/bin/env python3
"""Sign a thin arm64 runtime executable without CS_EXECSEG_MAIN_BINARY.

The original MH_EXECUTE file is never edited. codesign supplies a SHA-256 signature with
the selected identifier, then the existing CMS helper signs the same CodeDirectory after
clearing its main-executable bit. Explicit 16KB signing pages also handle legacy iOS binaries
without modifying their platform commands. Output must be a new file. No entitlements are added.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def run(command):
    subprocess.run([str(value) for value in command], check=True, stdout=sys.stderr)


def signature_range(binary):
    require(len(binary) >= 32, 'Truncated Mach-O header')
    magic, cpu, _, filetype, count, command_bytes, _, _ = struct.unpack_from('<8I', binary)
    require(magic == 0xFEEDFACF and cpu == 0x100000C and filetype == 2, 'Expected a thin arm64 MH_EXECUTE')
    require(32 + command_bytes <= len(binary), 'Load commands exceed file')
    offset, signature = 32, None
    for _ in range(count):
        require(offset + 8 <= 32 + command_bytes, 'Truncated load command')
        command, size = struct.unpack_from('<2I', binary, offset)
        require(size >= 8 and size % 8 == 0 and offset + size <= 32 + command_bytes, 'Invalid load command size')
        if command == 0x1D:
            require(size == 16 and signature is None, 'Invalid or duplicate LC_CODE_SIGNATURE')
            signature = struct.unpack_from('<2I', binary, offset + 8)
        offset += size
    require(offset == 32 + command_bytes, 'Load-command count does not match size')
    if signature:
        start, capacity = signature
        require(start >= offset and capacity >= 12 and start + capacity <= len(binary), 'Invalid signature allocation')
    return signature


def inspect_directory(cd, binary, signature_offset, slots, identifier):
    require(len(cd) >= 88, 'CodeDirectory lacks executable segment fields')
    magic, size, version, flags, hash_offset, identifier_offset, special_count, code_count, limit32 = struct.unpack_from('>9I', cd)
    hash_size, hash_type, _, page_shift = struct.unpack_from('4B', cd, 36)
    require(magic == 0xFADE0C02 and size == len(cd) and version == 0x20400, 'Expected CodeDirectory v0x20400')
    require(hash_size == 32 and hash_type == 2 and not flags & 2, 'Expected a non-ad-hoc SHA-256 CodeDirectory')
    require(page_shift == 14 and struct.unpack_from('>I', cd, 44)[0] == 0, 'CMS helper requires contiguous 16KB pages')
    limit64 = struct.unpack_from('>Q', cd, 56)[0]
    code_limit = limit64 if limit32 == 0xFFFFFFFF else limit32
    require(code_limit == signature_offset, 'CodeDirectory does not cover all bytes before the signature')
    require(code_count == (code_limit + 16383) // 16384, 'Invalid code slot count')
    require(hash_offset >= 88 + special_count * hash_size and hash_offset + code_count * hash_size <= len(cd),
            'Hash table exceeds CodeDirectory')
    metadata_end = hash_offset - special_count * hash_size
    require(88 <= identifier_offset < metadata_end, 'Invalid signing identifier offset')
    end = cd.find(b'\0', identifier_offset, metadata_end)
    require(end >= 0 and cd[identifier_offset:end] == identifier.encode(), 'Unexpected signing identifier')
    for index in range(code_count):
        begin, end = index * 16384, min((index + 1) * 16384, code_limit)
        require(hashlib.sha256(binary[begin:end]).digest() == cd[hash_offset + index * 32:hash_offset + (index + 1) * 32],
                f'Code page {index} hash mismatch')
    for slot in slots:
        if 0 < slot['type'] <= special_count:
            begin = hash_offset - slot['type'] * hash_size
            require(hashlib.sha256(slot['blob']).digest() == cd[begin:begin + hash_size], f"Special slot {slot['type']} hash mismatch")
    return code_count


def parse_signature(binary, identifier):
    signature = signature_range(binary)
    require(signature is not None, 'Missing LC_CODE_SIGNATURE after codesign')
    start, capacity = signature
    magic, length, count = struct.unpack_from('>3I', binary, start)
    require(magic == 0xFADE0CC0 and 12 + count * 8 <= length <= capacity, 'Invalid signature superblob')
    slots, seen = [], set()
    for index in range(count):
        kind, relative = struct.unpack_from('>2I', binary, start + 12 + index * 8)
        require(kind not in seen and 12 + count * 8 <= relative <= length - 8, 'Invalid or duplicate signature slot')
        require(kind in (0, 2, 65536), 'Expected CodeDirectory, requirements and CMS only; entitlements/alternate directories are unsupported')
        seen.add(kind)
        blob_magic, blob_size = struct.unpack_from('>2I', binary, start + relative)
        require(blob_size >= 8 and relative + blob_size <= length, 'Signature blob exceeds superblob')
        require(all(relative + blob_size <= slot['offset'] or relative >= slot['offset'] + len(slot['blob']) for slot in slots),
                'Overlapping signature blobs')
        expected_magic = {0: 0xFADE0C02, 2: 0xFADE0C01, 65536: 0xFADE0B01}[kind]
        require(blob_magic == expected_magic, 'Unexpected signature blob magic')
        slots.append({'type': kind, 'offset': relative, 'blob': binary[start + relative:start + relative + blob_size]})
    require(seen == {0, 2, 65536}, 'Missing CodeDirectory, requirements or CMS')
    cd = next(slot['blob'] for slot in slots if slot['type'] == 0)
    pages = inspect_directory(cd, binary, start, slots, identifier)
    return dict(offset=start, capacity=capacity, length=length, slots=slots, cd=cd, pages=pages)


def repack(binary, parsed, cd, cms, identifier):
    require(cms and cms[0] == 0x30, 'Expected a CMS ASN.1 sequence')
    count = len(parsed['slots'])
    header_size = 12 + count * 8
    indices, blobs = bytearray(), bytearray()
    for slot in parsed['slots']:
        if slot['type'] == 0:
            blob = cd
        elif slot['type'] == 65536:
            blob = struct.pack('>2I', 0xFADE0B01, len(cms) + 8) + cms
        else:
            blob = slot['blob']
        indices.extend(struct.pack('>2I', slot['type'], header_size + len(blobs)))
        blobs.extend(blob)
    length = header_size + len(blobs)
    require(length <= parsed['capacity'], f"New signature needs {length} bytes; reserved capacity is {parsed['capacity']}")
    signature = struct.pack('>3I', 0xFADE0CC0, length, count) + indices + blobs + bytes(parsed['capacity'] - length)
    begin, end = parsed['offset'], parsed['offset'] + parsed['capacity']
    result = binary[:begin] + signature + binary[end:]
    verified = parse_signature(result, identifier)
    require(len(result) == len(binary) and result[:begin] == binary[:begin] and result[end:] == binary[end:], 'Mach-O bytes changed while repacking')
    require(verified['cd'] == cd, 'Final CodeDirectory differs from the signed directory')
    for before, after in zip(parsed['slots'], verified['slots']):
        require(before['type'] == after['type'], 'Signature slot order changed')
        if before['type'] not in (0, 65536):
            require(before['blob'] == after['blob'], 'Unrelated signature blob changed')
    return result, verified


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


def sign_executable(args):
    source_bytes = args.source.read_bytes()
    signature_range(source_bytes)
    require(not os.path.lexists(args.output), 'Output already exists; choose a new output file')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.sim-sign-', dir=args.output.parent) as directory:
        work = Path(directory)
        executable = work / 'executable'
        shutil.copy2(args.source, executable)
        run(['/usr/bin/codesign', '--force', '--sign', args.identity, '--identifier', args.identifier,
             '--digest-algorithm=sha256', '--pagesize', '16384', '--timestamp=none', executable])
        run(['/usr/bin/codesign', '--verify', '--verbose=4', executable])
        signed_bytes = executable.read_bytes()
        parsed = parse_signature(signed_bytes, args.identifier)
        cd = bytearray(parsed['cd'])
        old_flags = struct.unpack_from('>Q', cd, 80)[0]
        require(old_flags & 1 and old_flags & ~1 == 0, 'Expected only CS_EXECSEG_MAIN_BINARY before modification')
        struct.pack_into('>Q', cd, 80, old_flags & ~1)
        require(bytes(cd[:80]) == parsed['cd'][:80] and bytes(cd[88:]) == parsed['cd'][88:], 'Unexpected CodeDirectory change')
        cd_path, cms_path = work / 'CodeDirectory.bin', work / 'CMS.der'
        cd_path.write_bytes(cd)
        helper = cms_helper(args, work)
        print('Signing modified CodeDirectory; macOS may request keychain access for sign-code-directory.', file=sys.stderr, flush=True)
        run([helper, args.identity, cd_path, cms_path])
        run(['/usr/bin/openssl', 'cms', '-verify', '-binary', '-inform', 'DER', '-in', cms_path,
             '-content', cd_path, '-noverify', '-out', '/dev/null'])
        cms = cms_path.read_bytes()
        result, verified = repack(signed_bytes, parsed, bytes(cd), cms, args.identifier)
        executable.write_bytes(result)
        run(['/usr/bin/codesign', '--verify', '--verbose=4', executable])
        require(args.source.read_bytes() == source_bytes, 'Source changed during signing')
        record = dict(source=str(args.source), output=str(args.output), identity=args.identity, identifier=args.identifier,
                      source_sha256=digest(source_bytes), sha256=digest(result), cdhash=digest(cd)[:40], size=len(result),
                      filetype='MH_EXECUTE', exec_segment_flags_before=old_flags, exec_segment_flags_after=0,
                      signature_offset=verified['offset'], signature_capacity=verified['capacity'], cms_size=len(cms),
                      verified_code_pages=verified['pages'], code_page_size=16384, entitlements=None,
                      source_unchanged=True, macho_unchanged_after_codesign=True, cms_verified=True)
        # Link atomically rather than replacing: an output created by another process must not be overwritten.
        os.link(executable, args.output)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--identity', required=True, help='Exact signing certificate SHA-1 (40 hexadecimal characters)')
    parser.add_argument('--identifier', required=True)
    parser.add_argument('--cms-helper', type=Path, help='Reuse a prebuilt sign-code-directory helper for a batch (avoids repeated keychain prompts)')
    args = parser.parse_args()
    args.source = args.source.resolve(strict=True)
    args.output = args.output.absolute()
    require(re.fullmatch(r'[0-9a-fA-F]{40}', args.identity), 'Identity must be an exact certificate SHA-1')
    require(args.identifier and '\0' not in args.identifier, 'Identifier must be a nonempty string without NUL')
    require(args.source != args.output.resolve(), 'Source and output must be different files')
    print(json.dumps(sign_executable(args), indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, struct.error, subprocess.CalledProcessError) as error:
        sys.exit(f'sign-sim-executable: {error}')
