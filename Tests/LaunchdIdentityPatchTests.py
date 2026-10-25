#!/usr/bin/env python3
"""Offline patch validation; supply --source with original or unpatched re-signed 23A343 launchd_sim."""

import argparse
import importlib.util
import json
import struct
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('launchd_identity_patch', ROOT / 'Tools/patch-ios26-launchd-identity.py')
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)
SOURCE = None


def decode_branch(word, pc):
    value = word & 0x3FFFFFF
    if value & 0x2000000:
        value -= 0x4000000
    return pc + value * 4


def imports(data):
    offset, size = struct.unpack_from('<2I', data, PATCH.FIXUPS_COMMAND + 8)
    blob = data[offset:offset + size]
    version, starts, table, strings, count, format_number, compression = struct.unpack_from('<7I', blob)
    assert version == 0 and format_number == 1 and compression == 0
    result = []
    for index in range(count):
        word = struct.unpack_from('<I', blob, table + index * 4)[0]
        name = strings + (word >> 9)
        end = blob.index(b'\0', name)
        result.append((word & 255, (word >> 8) & 1, blob[name:end]))
    return result


def fixups(data):
    """Independent complete walk of DYLD_CHAINED_PTR_64_OFFSET page chains, preserving semantic values."""
    offset, size = struct.unpack_from('<2I', data, PATCH.FIXUPS_COMMAND + 8)
    blob = data[offset:offset + size]
    starts = struct.unpack_from('<I', blob, 4)[0]
    count = struct.unpack_from('<I', blob, starts)[0]
    result = {}
    for segment_index in range(count):
        relative = struct.unpack_from('<I', blob, starts + 4 + segment_index * 4)[0]
        if not relative:
            continue
        begin = starts + relative
        record_size, page_size, pointer_format, segment, max_pointer, pages = struct.unpack_from('<IHHQIH', blob, begin)
        assert pointer_format == 6 and max_pointer == 0 and record_size == 22 + pages * 2
        for page_index in range(pages):
            start = struct.unpack_from('<H', blob, begin + 22 + page_index * 2)[0]
            if start == 0xFFFF:
                continue
            assert not start & 0x8000
            page = segment + page_index * page_size
            location = page + start
            while True:
                assert page <= location <= page + page_size - 8 and location not in result
                word = struct.unpack_from('<Q', data, location)[0]
                next_offset = ((word >> 51) & 0xFFF) * 4
                result[location] = word & ~(0xFFF << 51)
                if not next_offset:
                    break
                location += next_offset
    return result


class EncodingTests(unittest.TestCase):
    def test_branches_keep_lr_and_resume_exactly(self):
        for index, (offset, _, _, _) in enumerate(PATCH.SITES):
            veneer = PATCH.CAVE_BEGIN + index * PATCH.VENEER_STRIDE
            instruction = PATCH.branch(offset, veneer)
            self.assertEqual(instruction >> 26, 0b000101)  # B, never BL.
            self.assertEqual(decode_branch(instruction, offset), veneer)
            code, source = PATCH.build_veneer(index, external_labels=True)
            last = struct.unpack_from('<I', code, len(code) - 4)[0]
            self.assertEqual(decode_branch(last, veneer + len(code) - 4), offset + 4)
            self.assertIn(f'b _LSBTestResume{index}', source)
            self.assertEqual(len(code) % 4, 0)
            self.assertLessEqual(len(code), PATCH.VENEER_STRIDE)

    def test_saved_registers_flags_and_fail_closed_result(self):
        for index, (_, _, token, destination) in enumerate(PATCH.SITES):
            _, source = PATCH.build_veneer(index, external_labels=True)
            self.assertIn('sub sp, sp, #800', source)
            self.assertIn('add sp, sp, #800', source)
            for register in range(0, 32, 2):
                second = 'xzr' if register == 30 else f'x{register + 1}'
                self.assertIn(f'stp x{register}, {second}, [sp, #{register * 8}]', source)
                self.assertIn(f'ldp x{register}, {second}, [sp, #{register * 8}]', source)
                self.assertIn(f'stp q{register}, q{register + 1}, [sp, #{0x120 + register * 16}]', source)
                self.assertIn(f'ldp q{register}, q{register + 1}, [sp, #{0x120 + register * 16}]', source)
            for name in ('nzcv', 'fpcr', 'fpsr'):
                self.assertIn(f'mrs x16, {name}', source)
                self.assertIn(f'msr {name}, x16', source)
            self.assertIn('mov w0, #-1', source)
            self.assertIn(f'str x0, [sp, #{destination * 8}]', source)
            self.assertIn('add x19, sp, #800' if token is None else f'ldr x19, [sp, #{token * 8}]', source)

    def test_branch_range_rejected(self):
        for target in (3, 1 << 27, -(1 << 27) - 4):
            with self.assertRaises(ValueError):
                PATCH.branch(0, target)


class PinnedBinaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if SOURCE is None:
            raise unittest.SkipTest('Supply --source to test the pinned runtime binary')
        cls.original = SOURCE.read_bytes()
        if PATCH.is_patched(cls.original):
            raise ValueError('--source must be the original or unpatched re-signed binary')
        cls.patched = PATCH.patched_bytes(cls.original)

    def test_all_original_fixups_and_imports_preserved(self):
        before, after = fixups(self.original), fixups(self.patched)
        self.assertEqual(len(after), len(before) + 1)
        self.assertEqual(after.pop(PATCH.GOT_SLOT), (1 << 63) | 436)
        self.assertEqual(before, after)
        old_imports, new_imports = imports(self.original), imports(self.patched)
        self.assertEqual(new_imports[:-1], old_imports)
        self.assertEqual(new_imports[-1], (1, 0, b'_dlsym'))

    def test_signature_moved_metadata_bounded_and_idempotent(self):
        old_start, old_size = PATCH.inspect_header(self.original)
        new_start, new_size = PATCH.inspect_header(self.patched)
        self.assertEqual(old_size, new_size)
        self.assertEqual(self.original[old_start:], self.patched[new_start:])
        fixup_offset, fixup_size = struct.unpack_from('<2I', self.patched, PATCH.FIXUPS_COMMAND + 8)
        self.assertEqual(fixup_offset, old_start)
        self.assertLessEqual(fixup_offset + fixup_size, new_start)
        self.assertEqual(new_start % 16, 0)
        self.assertEqual(PATCH.patched_bytes(self.patched), self.patched)

    def test_resigning_size_changes_supported(self):
        # No signing is performed: simulate a larger signature allocation, just as codesign may do.
        changed = bytearray(self.patched + bytes(8192))
        signature, capacity = PATCH.inspect_header(self.patched)
        struct.pack_into('<I', changed, PATCH.SIGNATURE_COMMAND + 12, capacity + 8192)
        struct.pack_into('<Q', changed, PATCH.LINKEDIT_COMMAND + 48, len(changed) - 0x60000)
        struct.pack_into('<Q', changed, PATCH.LINKEDIT_COMMAND + 32, PATCH.align(len(changed) - 0x60000, 0x4000))
        self.assertTrue(PATCH.is_patched(changed))
        self.assertEqual(PATCH.patched_bytes(changed), changed)
        self.assertEqual(struct.unpack_from('<I', changed, PATCH.SIGNATURE_COMMAND + 8)[0], signature)

    def test_reject_bad_uuid_original_instruction_and_unrelated_change(self):
        for offset in (0xAF8, PATCH.SITES[0][0], 0x1234, PATCH.CAVE_BEGIN, PATCH.GOT_SLOT, PATCH.CHAIN_TAIL):
            with self.subTest(offset=hex(offset)):
                changed = bytearray(self.original)
                changed[offset] ^= 1
                with self.assertRaises(ValueError):
                    PATCH.patched_bytes(changed)

    def test_reject_partial_or_tampered_patch(self):
        for offset in (PATCH.SITES[1][0], PATCH.CAVE_BEGIN + 72, PATCH.SYMBOL_OFFSET, PATCH.GOT_SLOT,
                       PATCH.CHAIN_TAIL, PATCH.ORIGINAL_SIGNATURE + 200, 0x4321, PATCH.FIXUPS_COMMAND + 8):
            with self.subTest(offset=hex(offset)):
                changed = bytearray(self.patched)
                changed[offset] ^= 1
                with self.assertRaises(ValueError):
                    PATCH.patched_bytes(changed)

    def test_file_safety_manifest_and_already_patched_copy(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            source, output, again = work / 'original', work / 'patched', work / 'again'
            source.write_bytes(self.original)
            source.chmod(0o751)
            report = PATCH.patch_file(source, output)
            self.assertFalse(report['already_patched'])
            self.assertTrue(report['requires_separate_signing'])
            self.assertEqual(output.read_bytes(), self.patched)
            self.assertEqual(output.stat().st_mode & 0o777, 0o751)
            self.assertEqual(source.read_bytes(), self.original)
            self.assertEqual(json.loads((work / 'patched.identity-patch.json').read_text())['missing_symbol_pid'], -1)
            self.assertTrue(PATCH.patch_file(output, again)['already_patched'])
            self.assertEqual(again.read_bytes(), self.patched)
            for destination in (source, output):
                with self.assertRaises(ValueError):
                    PATCH.patch_file(source, destination)
            link = work / 'symlink'
            link.symlink_to(work / 'nonexistent')
            with self.assertRaises(ValueError):
                PATCH.patch_file(source, link)
            self.assertEqual(source.read_bytes(), self.original)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--source', type=Path)
    options, rest = parser.parse_known_args()
    SOURCE = options.source
    unittest.main(argv=[__file__] + rest)
