#!/usr/bin/env python3
"""Prepare the isolated native Metal backend from original iOS 26 build 23A343.

Adapt platform/install-name metadata, expose a verified existing peer handler,
and compile this repository's native probe/service. Never sign or deploy. All
original file-backed section payloads are preserved. Output must be a new folder.
"""
import argparse
import hashlib
import json
import pathlib
import struct
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
ORIGINAL_SHA256 = "321e0b3a7379eb258afe5f30e5b71944fbcac0877f95951154e903506b8e31bf"
ORIGINAL_HASHES = {"MTLSimImplementation": ORIGINAL_SHA256,
                   "MetalSerializer": "c691826dd9794fecd4cb4917069f99c098a8e0b9441b0acdf671d6276ac932f5"}
EXPECTED_UUID = bytes.fromhex("04f76bd9b7bb3b5ea562c260f8d3d1cf")
ALIAS = b"_LSBMTLSimulatorAcceptPeer"
LOCAL = b"__Z23MTLSimulatorHandleEventPU24objcproto13OS_xpc_object8NSObject"
ADDRESS = 0x2AE4
BODY_SIZE = 196
BODY_SHA256 = "2d7059957d1f7aec23f5ab627f840e3b0b3d5dedb13ae778b6ad75c6fc5d6fa3"
INIT_SHA256 = "8d6fe059e3b2e530c20cf8e9a486e25b91b461de9cc0e38f54c16dc816c02cfa"
BLOCK_SHA256 = "a9f5c9504d658fda494ebfbea66aa87777d81c7eb077481b3971e13767b152db"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def uleb(value):
    require(0 <= value < 1 << 64, "ULEB value out of range")
    result = bytearray()
    while value >= 0x80:
        result.append((value & 0x7F) | 0x80)
        value >>= 7
    result.append(value)
    return bytes(result)


def read_uleb(data, offset):
    result = 0
    for shift in range(0, 70, 7):
        require(offset < len(data), "Truncated export ULEB")
        value = data[offset]
        offset += 1
        require(shift != 63 or value <= 1, "Export ULEB exceeds 64 bits")
        result |= (value & 0x7F) << shift
        if not value & 0x80:
            return result, offset
    raise ValueError("Unterminated export ULEB")


def decode_exports(data):
    result = {}
    visited_count = 0

    def visit(offset, prefix, ancestors):
        nonlocal visited_count
        require(offset not in ancestors, "Cyclic export trie")
        require(len(prefix) <= 4096, "Export name too long")
        visited_count += 1
        require(visited_count <= 16384, "Export trie too large")
        length, cursor = read_uleb(data, offset)
        end = cursor + length
        require(end < len(data), "Truncated export terminal")
        if length:
            require(prefix and prefix not in result, "Invalid or duplicate export")
            result[prefix] = bytes(data[cursor:end])
        count = data[end]
        cursor = end + 1
        edges = set()
        for _ in range(count):
            terminator = data.find(b"\0", cursor)
            require(terminator >= cursor, "Unterminated export edge")
            edge = bytes(data[cursor:terminator])
            require(edge and edge[0] not in edges, "Invalid or overlapping export edges")
            edges.add(edge[0])
            child, cursor = read_uleb(data, terminator + 1)
            visit(child, prefix + edge, ancestors | {offset})

    visit(0, b"", set())
    return result


def encode_exports(exports):
    # A byte radix trie avoids ambiguous shared edge prefixes, including names
    # that are complete exports and prefixes of another export.
    root = {"terminal": b"", "children": {}, "offset": 0}
    for name, terminal in sorted(exports.items()):
        require(name and b"\0" not in name and terminal, "Invalid export record")
        node = root
        for char in name:
            node = node["children"].setdefault(char, {"terminal": b"", "children": {}, "offset": 0})
        node["terminal"] = terminal
    nodes = []

    def collect(node):
        nodes.append(node)
        for _, child in sorted(node["children"].items()):
            collect(child)

    collect(root)

    def encode_node(node):
        result = bytearray(uleb(len(node["terminal"])) + node["terminal"])
        result.append(len(node["children"]))
        for char, child in sorted(node["children"].items()):
            result.extend(bytes((char, 0)) + uleb(child["offset"]))
        return bytes(result)

    for _ in range(100):
        chunks = [encode_node(node) for node in nodes]
        offset = 0
        changed = False
        for node, chunk in zip(nodes, chunks):
            changed |= node["offset"] != offset
            node["offset"] = offset
            offset += len(chunk)
        if not changed:
            result = b"".join(chunks)
            require(decode_exports(result) == exports, "Export trie roundtrip failed")
            return result
    raise ValueError("Export trie offset convergence failed")


def macho(data):
    require(len(data) >= 32, "Truncated Mach-O")
    magic, cpu, subtype, filetype, count, command_size = struct.unpack_from("<6I", data)
    require((magic, cpu, filetype) == (0xFEEDFACF, 0x0100000C, 6), "Expected thin arm64 MH_DYLIB")
    require(32 + command_size <= len(data), "Invalid load command span")
    commands, sections = {}, {}
    offset = 32
    for _ in range(count):
        command, size = struct.unpack_from("<II", data, offset)
        require(size >= 8 and size % 8 == 0 and offset + size <= 32 + command_size, "Invalid load command")
        commands.setdefault(command, []).append(offset)
        if command == 0x19:
            require(size >= 72, "Truncated segment command")
            section_count = struct.unpack_from("<I", data, offset + 64)[0]
            require(72 + section_count * 80 <= size, "Truncated section commands")
            for index in range(section_count):
                entry = offset + 72 + index * 80
                section, segment, address, length, file_offset = struct.unpack_from("<16s16sQQI", data, entry)
                flags = struct.unpack_from("<I", data, entry + 64)[0]
                if flags & 0xFF in (1, 12, 18):
                    continue
                require(file_offset + length <= len(data), "Section outside file")
                key = (segment.rstrip(b"\0").decode(), section.rstrip(b"\0").decode())
                require(key not in sections, "Duplicate section")
                sections[key] = (file_offset, length, address, sha256(data[file_offset:file_offset + length]))
        offset += size
    require(offset == 32 + command_size, "Load command size mismatch")
    return commands, sections


def one(commands, kind):
    require(len(commands.get(kind, [])) == 1, f"Expected one load command {kind:#x}")
    return commands[kind][0]


def verify_function_bodies(data):
    for address, size, digest in ((ADDRESS, BODY_SIZE, BODY_SHA256), (0x3198, 60, INIT_SHA256), (0x31D4, 8, BLOCK_SHA256)):
        require(sha256(data[address:address + size]) == digest, f"Function body mismatch at {address:#x}")


def prepare(original, data):
    require(sha256(original) == ORIGINAL_SHA256, "Original 23A343 binary hash mismatch")
    validate_adaptation("MTLSimImplementation", original, data)
    verify_function_bodies(original)
    verify_function_bodies(data)
    commands, sections = macho(data)
    _, original_sections = macho(original)
    require({k: v[3] for k, v in sections.items()} == {k: v[3] for k, v in original_sections.items()}, "Adapted section payload mismatch")
    require(data[one(commands, 0x1B) + 8:one(commands, 0x1B) + 24] == EXPECTED_UUID, "UUID mismatch")
    require(struct.unpack_from("<I", data, one(commands, 0x32) + 8)[0] == 2, "Input is not native iOS platform")
    text = sections[("__TEXT", "__text")]
    require(text[0] <= ADDRESS and ADDRESS + BODY_SIZE <= text[0] + text[1] and text[0] == text[2], "Handler is not original text")

    # Check the original local symbol, independently of the function fingerprint.
    symoff, symbol_count, stroff, strsize = struct.unpack_from("<4I", data, one(commands, 0x2) + 8)
    require(symoff + symbol_count * 16 <= len(data) and stroff + strsize <= len(data), "Invalid symbol table")
    matches = []
    for index in range(symbol_count):
        string_index, kind, section, desc, value = struct.unpack_from("<IBBHQ", data, symoff + index * 16)
        require(string_index < strsize, "Invalid symbol name offset")
        end = data.find(b"\0", stroff + string_index, stroff + strsize)
        require(end >= 0, "Unterminated symbol name")
        if not kind & 0xE0 and data[stroff + string_index:end] == LOCAL:
            matches.append((kind, value))
    require(len(matches) == 1 and matches[0][0] & 0xE == 0xE and matches[0][1] == ADDRESS, "Local peer handler symbol mismatch")

    trie_command = one(commands, 0x80000033)
    old_trie_offset, old_trie_size = struct.unpack_from("<II", data, trie_command + 8)
    require(old_trie_offset + old_trie_size <= len(data), "Invalid original exports")
    exports = decode_exports(data[old_trie_offset:old_trie_offset + old_trie_size])
    require(exports.get(b"_init_with_xpc_connection") == uleb(0) + uleb(0x3198), "Initializer export mismatch")
    require(ALIAS not in exports, "Alias is already present in input")
    patched_exports = {**exports, ALIAS: uleb(0) + uleb(ADDRESS)}
    trie = encode_exports(patched_exports)

    signature_command = one(commands, 0x1D)
    signature_offset, signature_size = struct.unpack_from("<II", data, signature_command + 8)
    require(signature_size and signature_offset % 16 == 0 and signature_offset + signature_size == len(data), "Signature is not final aligned blob")
    linkedit = [offset for offset in commands[0x19] if data[offset + 8:offset + 24].rstrip(b"\0") == b"__LINKEDIT"]
    require(len(linkedit) == 1, "Expected one __LINKEDIT")
    segment = linkedit[0]
    vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<4Q", data, segment + 24)
    require(fileoff + filesize == len(data) and fileoff <= signature_offset, "Invalid __LINKEDIT extent")
    require(all(value[0] + value[1] <= signature_offset for value in sections.values()), "Signature overlaps a section")
    require(symoff + symbol_count * 16 <= signature_offset and stroff + strsize <= signature_offset, "Signature overlaps symbol data")
    # All other linkedit_data_command ranges are retained at their original offsets.
    for kind in (0x80000034, 0x80000033, 0x26, 0x29):
        for offset in commands.get(kind, []):
            start, size = struct.unpack_from("<II", data, offset + 8)
            require(start + size <= signature_offset, f"Signature overlaps linkedit command {kind:#x}")

    result = bytearray(data[:signature_offset])
    new_trie_offset = len(result)
    result.extend(trie)
    result.extend(b"\0" * ((-len(result)) % 16))
    struct.pack_into("<II", result, trie_command + 8, new_trie_offset, len(trie))
    struct.pack_into("<II", result, signature_command + 8, 0, 0)
    new_filesize = len(result) - fileoff
    struct.pack_into("<Q", result, segment + 32, (new_filesize + 0x3FFF) & ~0x3FFF)
    struct.pack_into("<Q", result, segment + 48, new_filesize)
    result = bytes(result)
    require(macho(result)[1] == sections, "Output changed a section payload or its metadata")
    require(decode_exports(result[new_trie_offset:new_trie_offset + len(trie)]) == patched_exports, "Output exports changed")
    # Every pre-signature byte except these four metadata fields must be untouched.
    allowed = set()
    for offset, size in ((trie_command + 8, 8), (signature_command + 8, 8), (segment + 32, 8), (segment + 48, 8)):
        allowed.update(range(offset, offset + size))
    require(all(a == b or index in allowed for index, (a, b) in enumerate(zip(data[:signature_offset], result))), "Unexpected original payload edit")
    result = remove_signature_command(result)
    report = {
        "runtime_build": "23A343", "uuid": EXPECTED_UUID.hex(), "original_sha256": ORIGINAL_SHA256,
        "input_sha256": sha256(data), "output_sha256": sha256(result), "input_size": len(data), "output_size": len(result),
        "signature": "Removed final signature blob and LC_CODE_SIGNATURE load command. Output MUST be signed before loading.",
        "alias": {"dlsym_name": ALIAS.decode()[1:], "vm_offset": hex(ADDRESS), "body_size": BODY_SIZE, "body_sha256": BODY_SHA256,
                  "prototype": "void LSBMTLSimulatorAcceptPeer(xpc_connection_t peer)", "original_local_symbol": LOCAL.decode()},
        "original_exports_preserved": {name.decode(): value.hex() for name, value in exports.items()},
        "exports": {name.decode(): value.hex() for name, value in patched_exports.items()},
        "old_trie": {"offset": old_trie_offset, "size": old_trie_size}, "new_trie": {"offset": new_trie_offset, "size": len(trie)},
        "unchanged_sections": {','.join(key): value[3] for key, value in sections.items()},
        "text_edited": False, "signed": False, "runtime_tested": False,
    }
    return result, report


def self_test(original, data):
    cases = [
        {b"_a": b"\0\1", b"_ab": b"\0\2", b"_b": b"\0\3"},
        {f"_symbol_{n:03d}".encode(): uleb(0) + uleb(n * 4096) for n in range(100)},
        {b"_reexport": uleb(8) + uleb(2) + b"_target\0"},
    ]
    for exports in cases:
        require(decode_exports(encode_exports(exports)) == exports, "Synthetic trie roundtrip failed")
    for malformed in (b"", b"\x80", b"\0\1x\0\0", b"\0\1x\0\xff\x7f"):
        try:
            decode_exports(malformed)
        except ValueError:
            continue
        raise ValueError("Malformed trie was accepted")
    output, _ = prepare(original, data)
    changed_signature = bytearray(data)
    changed_signature[-1] ^= 1
    require(prepare(original, bytes(changed_signature))[0] == output, "Equivalent input with a different signature was rejected")
    commands, _ = macho(data)
    changed_platform = bytearray(data)
    struct.pack_into("<I", changed_platform, one(commands, 0x32) + 8, 7)
    for bad_original, bad_input in ((original[:-1], data), (original, data[:ADDRESS] + bytes((data[ADDRESS] ^ 1,)) + data[ADDRESS + 1:]),
                                    (original, bytes(changed_platform)), (original, output)):
        try:
            prepare(bad_original, bad_input)
        except ValueError:
            continue
        raise ValueError("Nonmatching baseline was accepted")
    print("Self-test passed: trie roundtrips, malformed tries, baseline rejection, original payload/section preservation.")


def dylib_names(data, commands):
    result = []
    for kind in (0xC, 0xD, 0x18 | 0x80000000, 0x1F | 0x80000000, 0x20, 0x23 | 0x80000000):
        for offset in commands.get(kind, []):
            size, relative = struct.unpack_from("<II", data, offset + 4)
            end = data.find(b"\0", offset + relative, offset + size)
            require(24 <= relative < size and end >= 0, "Invalid dylib name")
            result.append((kind, data[offset + relative:end].decode()))
    return sorted(result)


def validate_adaptation(name, original, data):
    require(sha256(original) == ORIGINAL_HASHES[name], f"Original {name} 23A343 hash mismatch")
    commands, sections = macho(data)
    original_commands, original_sections = macho(original)
    require(sections == original_sections, f"{name} section payloads or metadata changed")
    uuid = one(commands, 0x1B)
    original_uuid = one(original_commands, 0x1B)
    require(data[uuid + 8:uuid + 24] == original[original_uuid + 8:original_uuid + 24], "UUID changed")
    require(struct.unpack_from("<3I", data, one(commands, 0x32) + 8) == (2, 26 << 16, 26 << 16), "Expected iOS 26 build metadata")
    expected = []
    for kind, install_name in dylib_names(original, original_commands):
        if kind == 0xD:
            install_name = f"@loader_path/{name}.dylib"
        elif install_name == "/System/Library/PrivateFrameworks/MetalSerializer.framework/MetalSerializer":
            install_name = "@loader_path/MetalSerializer.dylib"
        expected.append((kind, install_name))
    require(dylib_names(data, commands) == sorted(expected), f"Unexpected {name} dependency/install name")
    return {"uuid": data[uuid + 8:uuid + 24].hex(), "unchanged_sections": len(sections),
            "original_sha256": sha256(original), "adapted_sha256": sha256(data)}


def remove_signature_command(data):
    commands, sections = macho(data)
    offset = one(commands, 0x1D)
    command_size = struct.unpack_from("<I", data, offset + 4)[0]
    require(command_size == 16, "Unexpected signature load command size")
    count, total = struct.unpack_from("<II", data, 16)
    end = 32 + total
    result = bytearray(data)
    result[offset:end - command_size] = data[offset + command_size:end]
    result[end - command_size:end] = bytes(command_size)
    struct.pack_into("<II", result, 16, count - 1, total - command_size)
    new_commands, new_sections = macho(result)
    require(0x1D not in new_commands and sections == new_sections, "Signature command removal changed sections")
    return bytes(result)


def strip_signature(data):
    commands, sections = macho(data)
    command = one(commands, 0x1D)
    start, size = struct.unpack_from("<II", data, command + 8)
    require(size and start + size == len(data), "Expected final signature blob")
    require(all(offset + length <= start for offset, length, _, _ in sections.values()), "Signature overlaps a section")
    result = bytearray(data[:start])
    segment = [offset for offset in commands[0x19] if data[offset + 8:offset + 24].rstrip(b"\0") == b"__LINKEDIT"]
    require(len(segment) == 1, "Expected one __LINKEDIT")
    fileoff = struct.unpack_from("<Q", data, segment[0] + 40)[0]
    size = len(result) - fileoff
    require(size >= 0, "Signature before __LINKEDIT")
    struct.pack_into("<II", result, command + 8, 0, 0)
    struct.pack_into("<Q", result, segment[0] + 32, (size + 0x3FFF) & ~0x3FFF)
    struct.pack_into("<Q", result, segment[0] + 48, size)
    require(macho(result)[1] == sections, "Signature removal changed sections")
    return remove_signature_command(bytes(result))


def run(command, commands):
    commands.append([str(part) for part in command])
    subprocess.run(command, check=True)


def verify_exports(path):
    listing = subprocess.check_output(["xcrun", "dyld_info", "-exports", str(path)], text=True)
    parsed = {}
    for line in listing.splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0].startswith("0x"):
            parsed[fields[1]] = int(fields[0], 16)
    require(parsed.get(ALIAS.decode()) == ADDRESS and parsed.get("_init_with_xpc_connection") == 0x3198, "dyld_info export verification failed")
    return listing


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-root", required=True, type=pathlib.Path, help="Original iOS 26.0 23A343 RuntimeRoot")
    parser.add_argument("--output-dir", required=True, type=pathlib.Path, help="New folder for three unsigned native dylibs and manifest")
    parser.add_argument("--self-test", action="store_true", help="Exercise export codec and baseline rejection before publishing")
    args = parser.parse_args()
    source, output = args.runtime_root.resolve(), args.output_dir.resolve()
    require(source.is_dir(), "RuntimeRoot does not exist")
    require(not output.exists(), "Output folder already exists; refusing to overwrite artifacts or signatures")
    require(output.parent.is_dir(), "Output parent directory must exist")
    originals = {}
    for name, digest in ORIGINAL_HASHES.items():
        path = source / f"System/Library/PrivateFrameworks/{name}.framework/{name}"
        original = path.read_bytes()
        require(sha256(original) == digest, f"Original {name} hash mismatch; only 23A343 is supported")
        originals[name] = (path, original)
    commands, records = [], {}
    with tempfile.TemporaryDirectory(prefix="native-metal-", dir=output.parent) as temporary:
        stage = pathlib.Path(temporary) / "NativeMetal"
        stage.mkdir()
        for name, (path, original) in originals.items():
            destination = stage / f"{name}.dylib"
            run(["xcrun", "vtool", "-set-build-version", "ios", "26.0", "26.0", "-replace", "-output", destination, path], commands)
            run(["xcrun", "install_name_tool", "-id", f"@loader_path/{name}.dylib", destination], commands)
            if name == "MTLSimImplementation":
                run(["xcrun", "install_name_tool", "-change", "/System/Library/PrivateFrameworks/MetalSerializer.framework/MetalSerializer",
                     "@loader_path/MetalSerializer.dylib", destination], commands)
            adapted = destination.read_bytes()
            records[name] = validate_adaptation(name, original, adapted)
            if name == "MTLSimImplementation":
                if args.self_test:
                    self_test(original, adapted)
                result, report = prepare(original, adapted)
                records[name]["peer_alias"] = report
            else:
                result = strip_signature(adapted)
            destination.write_bytes(result)
            validate_adaptation(name, original, result)
            records[name].update({"source": str(path), "output_sha256": sha256(result), "output_size": len(result)})
        records["MTLSimImplementation"]["dyld_info_exports"] = verify_exports(stage / "MTLSimImplementation.dylib")
        sdk = subprocess.check_output(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], text=True).strip()
        native_sources = [ROOT / "NativeMetal/MetalNativeProbe.m", ROOT / "NativeMetal/MetalNativeService.m",
                          ROOT / "ProcessSupport/NativeMetalControl.c"]
        native_output = stage / "MetalNativeProbe.dylib"
        run(["xcrun", "--sdk", "iphoneos", "clang", "-target", "arm64-apple-ios26.0", "-isysroot", sdk, "-fobjc-arc", "-fblocks",
             "-Wall", "-Wextra", "-Werror", "-dynamiclib", "-fvisibility=hidden", *native_sources, "-framework", "Foundation",
             "-framework", "Metal", "-Wl,-install_name,@loader_path/MetalNativeProbe.dylib", "-Wl,-no_adhoc_codesign", "-o", native_output], commands)
        exports = subprocess.check_output(["xcrun", "dyld_info", "-exports", str(native_output)], text=True)
        for symbol in ("_LSBMetalNativeProbe", "_LSBMetalNativeServicePrepare", "_LSBMetalNativeServiceRun"):
            require(any(line.split()[-1:] == [symbol] for line in exports.splitlines()), f"Missing native backend export {symbol}")
        headers = [ROOT / "NativeMetal/Backend.h", ROOT / "NativeMetal/PrivateMetalProbe.h", ROOT / "ProcessSupport/NativeMetalControl.h"]
        records["MetalNativeProbe"] = {"output_sha256": sha256(native_output.read_bytes()), "output_size": native_output.stat().st_size,
                                      "sources": {str(path.relative_to(ROOT)): sha256(path.read_bytes()) for path in native_sources + headers},
                                      "dyld_info_exports": exports}
        manifest = {"runtime_build": "23A343", "runtime_root": str(source), "target": "arm64-apple-ios26.0", "sdk": sdk,
                    "signed": False, "deployed": False, "files": records, "commands": commands,
                    "signing_note": "Sign all three dylibs with the app's identity and identifier, then embed in LiveProcess.appex/Frameworks/NativeMetal."}
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        stage.rename(output)
    print(f"Prepared three unsigned native dylibs in {output}. Original sections unchanged; export alias verified by dyld_info.")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"prepare-native-metal: {error}") from error
