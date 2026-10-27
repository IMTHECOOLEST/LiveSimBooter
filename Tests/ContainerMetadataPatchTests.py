#!/usr/bin/env python3
"""Small synthetic-cache tests; no Apple runtime, signing identity, or device access is required."""

import hashlib
import importlib.util
import json
import os
import struct
import tempfile
import unittest
import uuid
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('container_metadata_patch', ROOT / 'Tools/patch-ios26-container-metadata.py')
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)


class ContainerMetadataPatchTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name).resolve()
        self.source = self.directory / 'source-cache'
        self.output = self.directory / 'patched-cache'
        self.manifest = Path(str(self.output) + '.patch-manifest.json')
        self.data = bytearray(0x10080)
        self.data[:16] = b'dyld_v1   arm64\0'
        struct.pack_into('<II', self.data, 16, 0x228, 1)
        struct.pack_into('<QQ', self.data, 40, 0x10000, 0x80)
        self.data[88:104] = uuid.UUID(PATCH.PROFILE['uuid']).bytes
        struct.pack_into('<II', self.data, 216, 7, 0x200)
        struct.pack_into('<QQ', self.data, 224, PATCH.PROFILE['base'], PATCH.PROFILE['span'])
        struct.pack_into('<II', self.data, 448, 0x500, 5)
        struct.pack_into('<QQQII', self.data, 0x228, PATCH.PROFILE['base'], 0x10000, 0, 5, 5)
        self.profile = dict(PATCH.PROFILE, image_count=5, owners=[])
        self.image_headers, self.section_records = [], []
        sites = []
        for index, original in enumerate(PATCH.PROFILE['owners']):
            header, section, name_offset = 0x4000 + index * 0x400, 0xC000 + index * 0x200, 0x1000 + index * 0x180
            spec = dict(original, size=0x100, relative=0x40)
            self.data[section + spec['relative']:section + spec['relative'] + 51] = PATCH.ORIGINAL + b'\0'
            spec['section_sha256'] = hashlib.sha256(self.data[section:section + spec['size']]).hexdigest()
            self.profile['owners'].append(spec)
            self.image_headers.append(header)
            self.section_records.append(self.make_image(header, section, spec))
            sites.append(section + spec['relative'])
            path = spec['path'].encode() + b'\0'
            self.data[name_offset:name_offset + len(path)] = path
            struct.pack_into('<QQQII', self.data, 0x500 + index * 32, self.profile['base'] + header, 0, 0, name_offset, 0)
        self.profile['sites'] = tuple(sites)
        self.write_source()

    def make_image(self, header, section, spec, executable=False):
        base = 0x100000000 if executable else PATCH.PROFILE['base']
        struct.pack_into('<8I', self.data, header, 0xFEEDFACF, 0x100000C, 0, 2 if executable else 6, 3, 200, 0, 0)
        struct.pack_into('<II16s', self.data, header + 32, 0x1B, 24, uuid.UUID(spec['uuid']).bytes)
        struct.pack_into('<6I', self.data, header + 56, 0x32, 24, 7, 0x1A0000, 0x1A0000, 0)
        position = header + 80
        extent = section + spec['size'] - header
        struct.pack_into('<II16sQQQQIIII', self.data, position, 0x19, 152, b'__TEXT', base + header, extent, header, extent, 5, 5, 1, 0)
        struct.pack_into('<16s16sQQ8I', self.data, position + 72, b'__cstring', b'__TEXT', base + section, spec['size'],
                         section, 0, 0, 0, 2, 0, 0, 0)
        return position + 72

    def write_source(self, update_hash=True):
        self.source.write_bytes(self.data)
        if update_hash:
            self.profile['sha256'] = hashlib.sha256(self.data).hexdigest()

    def patch(self):
        return PATCH.patch_cache(self.source, self.output, profile=self.profile)

    def assert_rejected(self, message):
        with self.assertRaisesRegex(ValueError, message):
            self.patch()
        self.assertFalse(self.output.exists())
        self.assertFalse(self.manifest.exists())

    def test_exact_replacements_preserve_every_other_byte_and_source(self):
        before = bytes(self.data)
        report = self.patch()
        expected = bytearray(before)
        for offset in self.profile['sites']:
            expected[offset:offset + len(PATCH.REPLACEMENT)] = PATCH.REPLACEMENT
        self.assertEqual(self.source.read_bytes(), before)
        self.assertEqual(self.output.read_bytes(), expected)
        self.assertEqual(report, json.loads(self.manifest.read_text()))
        self.assertEqual(report['source_sha256'], hashlib.sha256(before).hexdigest())
        self.assertEqual(report['output_sha256'], hashlib.sha256(expected).hexdigest())
        self.assertEqual(report['changed_bytes'], sum(a != b for a, b in zip(before, expected)))
        self.assertEqual(len(report['sites']), 5)
        self.assertTrue(report['signature_invalidated'] and report['requires_separate_signing'])
        self.assertEqual(self.output.stat().st_mode & 0o777, self.source.stat().st_mode & 0o777)

    def test_patch_crossing_stream_chunk_boundary(self):
        with mock.patch.object(PATCH, 'CHUNK_SIZE', 37):
            report = self.patch()
        self.assertEqual(report['output_bytes'], len(self.data))
        for offset in self.profile['sites']:
            self.assertEqual(self.output.read_bytes()[offset:offset + 51], PATCH.REPLACEMENT + b'\0')

    def test_check_only_writes_nothing(self):
        report = PATCH.patch_cache(self.source, profile=self.profile)
        self.assertTrue(report['validation_only'])
        self.assertEqual(sorted(path.name for path in self.directory.iterdir()), ['source-cache'])

    def test_unknown_bytes_rejected_even_when_sites_match(self):
        self.data[0xC001] ^= 1
        self.write_source(update_hash=False)
        self.assert_rejected('SHA-256')

    def test_changed_literal_or_missing_terminator_rejected(self):
        original = bytes(self.data)
        for position in (self.profile['sites'][0], self.profile['sites'][-1] + len(PATCH.ORIGINAL)):
            with self.subTest(position=position):
                self.data[:] = original
                self.data[position] ^= 1
                self.write_source()
                self.assert_rejected('cstring section SHA-256')

    def test_extra_occurrence_or_partial_patch_rejected(self):
        for value, message in ((PATCH.ORIGINAL, 'extra'), (PATCH.REPLACEMENT, 'already exists')):
            with self.subTest(value=value):
                self.data[0xD000:0xD000 + len(value)] = value
                self.write_source()
                self.assert_rejected(message)

    def test_invalid_header_metadata_rejected_independently_of_hash(self):
        original = bytes(self.data)
        changes = [(88, '<16s', (bytes(16),), 'UUID'), (224, '<Q', (0x120000000,), 'reservation'),
                   (232, '<Q', (0x8000,), 'reservation'), (216, '<I', (2,), 'simulator'),
                   (220, '<I', (0,), 'simulator'), (396, '<I', (1,), 'single-file'),
                   (452, '<I', (1352,), 'image table'), (448, '<I', (0xFFF0,), 'image table'),
                   (40, '<Q', (0x8000,), 'signature extent'), (48, '<Q', (1,), 'signature extent'),
                   (16, '<I', (0xFFFF,), 'mapping table'), (20, '<I', (65,), 'mapping table')]
        for offset, format_string, values, message in changes:
            with self.subTest(offset=offset):
                self.data[:] = original
                struct.pack_into(format_string, self.data, offset, *values)
                self.write_source()
                self.assert_rejected(message)

    def test_writable_mapping_or_unmapped_literal_rejected(self):
        original = bytes(self.data)
        for size, maximum, initial, message in ((0x10000, 3, 1, 'RX/RO'), (0x10000, 7, 5, 'RX/RO'),
                                                 (0xC120, 5, 5, 'cache mapping'), (0x20000, 5, 5, 'signed payload')):
            with self.subTest(size=size, maximum=maximum):
                self.data[:] = original
                struct.pack_into('<QQQII', self.data, 0x228, self.profile['base'], size, 0, maximum, initial)
                self.write_source()
                self.assert_rejected(message)

    def test_in_place_existing_output_and_manifest_are_untouched(self):
        with self.assertRaisesRegex(ValueError, 'in-place'):
            PATCH.patch_cache(self.source, self.source, profile=self.profile)
        self.output.write_bytes(b'existing')
        with self.assertRaisesRegex(ValueError, 'existing output'):
            self.patch()
        self.assertEqual(self.output.read_bytes(), b'existing')
        self.output.unlink()
        self.manifest.write_text('existing manifest')
        with self.assertRaisesRegex(ValueError, 'existing output'):
            self.patch()
        self.assertEqual(self.manifest.read_text(), 'existing manifest')
        self.assertFalse(self.output.exists())

    def test_symlinks_and_hardlink_alias_refused(self):
        link = self.directory / 'linked-source'
        link.symlink_to(self.source)
        with self.assertRaisesRegex(ValueError, 'nonsymlink'):
            PATCH.patch_cache(link, self.output, profile=self.profile)
        self.output.symlink_to(self.directory / 'missing')
        with self.assertRaisesRegex(ValueError, 'existing output'):
            self.patch()
        self.output.unlink()
        os.link(self.source, self.output)
        with self.assertRaisesRegex(ValueError, 'existing output'):
            self.patch()
        self.assertEqual(self.source.read_bytes(), self.data)

    def test_failed_copy_removes_only_new_files(self):
        original_hash = PATCH.hash_stream
        calls = 0

        def bad_verification(stream):
            nonlocal calls
            calls += 1
            return original_hash(stream) if calls == 1 else 'bad-output'

        with mock.patch.object(PATCH, 'hash_stream', side_effect=bad_verification):
            self.assert_rejected('verification')
        self.assertEqual(self.source.read_bytes(), self.data)

    def test_cli_profile_does_not_accept_synthetic_input(self):
        with self.assertRaises(ValueError):
            PATCH.patch_cache(self.source, self.output)
        self.assertFalse(self.output.exists())

    def test_rebuilt_uuid_and_section_location_are_supported(self):
        self.data[88:104] = uuid.UUID('AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE').bytes
        self.data[0xB000] = 42  # Unrelated rebuilt payload is not pinned to a previous full-file signature/hash.
        old, new, size = 0xC400, 0xD400, self.profile['owners'][2]['size']
        self.data[new:new + size] = self.data[old:old + size]
        self.data[old:old + size] = bytes(size)
        self.make_image(self.image_headers[2], new, self.profile['owners'][2])
        self.write_source()
        report = self.patch()
        self.assertFalse(report['known_signed_cache_variant'])
        self.assertEqual(report['cache_uuid'], 'AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE')
        self.assertIn(hex(new + 0x40), [site['file_offset'] for site in report['sites']])
        self.assertEqual(self.output.read_bytes()[0xB000], 42)

    def test_unsigned_cache_with_pending_signature_is_supported(self):
        self.data = self.data[:0x10000]
        struct.pack_into('<Q', self.data, 48, 0)
        self.write_source()
        report = self.patch()
        self.assertEqual(report['signature_size'], 0)
        self.assertTrue(report['requires_separate_signing'])

    def test_image_identity_section_and_name_constraints(self):
        original = bytes(self.data)
        mutations = [
            (self.image_headers[0] + 40, b'\0', 'image UUID'),
            (self.section_records[0], b'__wrong\0', 'Missing pinned cstring'),
            (0x1000, b'/not-the-owner\0', 'Missing pinned image'),
            (0x500 + 32 + 24, struct.pack('<I', 0x1000), 'duplicate image'),
            (self.image_headers[0] + 20, struct.pack('<I', 199), 'command'),
        ]
        for offset, replacement, message in mutations:
            with self.subTest(offset=offset):
                self.data[:] = original
                self.data[offset:offset + len(replacement)] = replacement
                self.write_source()
                self.assert_rejected(message)

    def make_helper(self):
        self.data = bytearray(0x1000)
        spec = dict(PATCH.HELPER, size=0x100, relative=0x40)
        section = 0x400
        self.data[section + 0x40:section + 0x40 + 51] = PATCH.ORIGINAL + b'\0'
        spec['section_sha256'] = hashlib.sha256(self.data[section:section + spec['size']]).hexdigest()
        self.make_image(0, section, spec, executable=True)
        self.source.write_bytes(self.data)
        return spec

    def test_exact_loose_helper_and_separate_signature_requirement(self):
        spec = self.make_helper()
        before = bytes(self.data)
        report = PATCH.patch_cache(self.source, self.output, profile=spec, mach_o=True)
        expected = bytearray(before)
        expected[0x440:0x440 + 50] = PATCH.REPLACEMENT
        self.assertEqual(self.output.read_bytes(), expected)
        self.assertEqual(self.source.read_bytes(), before)
        self.assertEqual(report['kind'], 'mach-o')
        self.assertEqual(report['image'], PATCH.HELPER['path'])
        self.assertEqual(len(report['sites']), 1)
        self.assertTrue(report['requires_separate_signing'])

    def test_loose_helper_rejects_unknown_platform_uuid_or_extra_literal(self):
        spec = self.make_helper()
        original = bytes(self.data)
        for offset, replacement, message in [(64, struct.pack('<I', 2), 'simulator'), (40, b'\0', 'UUID'),
                                               (8, struct.pack('<I', 2), 'arm64'), (0x800, PATCH.ORIGINAL, 'extra')]:
            with self.subTest(offset=offset):
                self.data[:] = original
                self.data[offset:offset + len(replacement)] = replacement
                self.source.write_bytes(self.data)
                with self.assertRaisesRegex(ValueError, message):
                    PATCH.patch_cache(self.source, self.output, profile=spec, mach_o=True)
                self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
