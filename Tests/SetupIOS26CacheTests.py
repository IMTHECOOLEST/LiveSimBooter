#!/usr/bin/env python3
"""Cache setup rejects incompatible inputs and unsafe layouts before publication; no Apple binaries required."""

import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('cache_setup', ROOT / 'Tools/setup-ios26-cache.py')
setup = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(setup)


class CacheSetupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name).resolve()
        self.path = self.directory / 'cache'
        self.inventory = dict(expected_image_count=1, images=[dict(install_name='/usr/lib/test.dylib')],
                              automatically_supplied_host_libraries=[])
        self.data = bytearray(4096)
        self.data[:16] = b'dyld_v1   arm64\0'
        struct.pack_into('<II', self.data, 16, 512, 1)
        struct.pack_into('<I', self.data, 216, 7)
        struct.pack_into('<QQ', self.data, 224, setup.BASE, 4096)
        struct.pack_into('<II', self.data, 448, 544, 1)
        struct.pack_into('<QQQII', self.data, 512, setup.BASE, 4096, 0, 5, 5)
        struct.pack_into('<QQQII', self.data, 544, setup.BASE + 1024, 0, 0, 640, 0)
        name = b'/usr/lib/test.dylib\0'
        self.data[640:640 + len(name)] = name
        struct.pack_into('<8I', self.data, 1024, 0xFEEDFACF, 0x100000C, 0, 6, 3, 112, 0x80000000, 0)
        struct.pack_into('<2I', self.data, 1056, 0xB, 80)
        struct.pack_into('<4I', self.data, 1136, 0x80000033, 16, 2048, 1)
        struct.pack_into('<4I', self.data, 1152, 0x26, 16, 2056, 1)

    def check(self):
        self.path.write_bytes(self.data)
        return setup.check_cache(self.path, self.inventory)

    def test_exact_image_and_metadata(self):
        result = self.check()
        self.assertEqual(result['images'], 1)
        self.assertEqual(result['base'], '0x110000000')
        self.assertEqual(result['export_tries'], 1)
        self.assertEqual(result['function_starts'], 1)
        self.assertEqual(result['signature_validation'], 'not_performed')

    def test_wrong_image_even_when_count_matches(self):
        self.inventory['images'][0]['install_name'] = '/usr/lib/other.dylib'
        with self.assertRaisesRegex(ValueError, 'inventory mismatch'):
            self.check()

    def test_mapping_permissions_and_limits(self):
        original = bytes(self.data)
        for maximum, initial in ((5, 7), (8, 0)):
            with self.subTest(maximum=maximum, initial=initial):
                self.data[:] = original
                struct.pack_into('<II', self.data, 536, maximum, initial)
                with self.assertRaisesRegex(ValueError, 'protection'):
                    self.check()
        for maximum in (2, 4, 6):
            with self.subTest(crossing_maximum=maximum):
                self.data[:] = original
                span = setup.SHARED_REGION - setup.BASE + 4096
                struct.pack_into('<Q', self.data, 232, span)
                struct.pack_into('<QQQII', self.data, 512, setup.SHARED_REGION - 2048, 4096, 0, maximum, 0)
                with self.assertRaisesRegex(ValueError, 'crosses native shared region'):
                    self.check()

    def test_read_only_metadata_can_cross_boundary(self):
        span = setup.SHARED_REGION - setup.BASE + 4096
        struct.pack_into('<Q', self.data, 232, span)
        struct.pack_into('<I', self.data, 20, 2)
        # Move image-table entry out of the second mapping-table record.
        self.data[576:608] = self.data[544:576]
        struct.pack_into('<I', self.data, 448, 576)
        struct.pack_into('<QQQII', self.data, 544, setup.SHARED_REGION, 4096, 0, 1, 1)
        self.assertEqual(len(self.check()['mappings']), 2)

    def test_corrupt_tables_and_symbols(self):
        original = bytes(self.data)
        changes = [(16, 4094, 'outside file'), (20, 0, 'mapping count'), (396, 1, 'single-file'),
                   (448, 4094, 'outside file'), (452, 2, 'image count'), (1060, 4, 'load-command'),
                   (1068, 1, 'metadata'), (1148, 0, 'metadata'), (1164, 0, 'metadata')]
        for offset, value, reason in changes:
            with self.subTest(offset=offset):
                self.data[:] = original
                struct.pack_into('<I', self.data, offset, value)
                with self.assertRaisesRegex(ValueError, reason):
                    self.check()

    def test_pinned_patch_changes_only_verified_sites(self):
        original = bytearray(0x60000)
        patched = bytearray(original)
        for offset, before, after, _ in setup.PATCHES:
            struct.pack_into('<I', original, offset, before)
            struct.pack_into('<I', patched, offset, after)
        manifest = dict(builder_sha256=hashlib.sha256(original).hexdigest(),
                        patched_builder_sha256_before_signing=hashlib.sha256(patched).hexdigest())
        self.assertEqual(setup.patch_builder(original, manifest), patched)
        original[100] = 1
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            setup.patch_builder(original, manifest)

    def test_refuses_overwrite_and_source_mutation(self):
        root = self.directory / 'RuntimeRoot'
        root.mkdir()
        existing = self.directory / 'existing'
        existing.mkdir()
        with self.assertRaisesRegex(ValueError, 'already exists'):
            setup.build(root, existing, {}, {})
        with self.assertRaisesRegex(ValueError, 'inside the original'):
            setup.build(root, root / 'new' / 'cache', {}, {})
        self.assertEqual(list(root.iterdir()), [])
        for invalid in ('', '/absolute', '../escape', 'x/../escape', 'x//y', 'x/./y'):
            with self.subTest(path=invalid), self.assertRaises(ValueError):
                setup.relative_path(invalid)

    def test_repository_inventory_has_no_machine_paths(self):
        manifest = setup.read_inventory()
        self.assertEqual(len(manifest['images']), 1350)
        for record in manifest['images']:
            self.assertFalse(record['source'].startswith('/'))
            self.assertEqual(len(record['sha256']), 64)
        self.assertEqual(manifest['runtime_build'], '23A343')


if __name__ == '__main__':
    unittest.main()
