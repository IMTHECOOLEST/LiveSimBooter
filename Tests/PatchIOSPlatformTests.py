#!/usr/bin/env python3
"""Test refusal/preservation rules for the version-pinned dyld_sim platform patcher."""

import importlib.util
import json
import os
import stat
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('platform_patch', Path(__file__).resolve().parents[1] / 'Tools/patch-ios26-platform.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PlatformPatchTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.source = self.root / 'source'
        self.output = self.root / 'output'
        self.manifest = self.root / 'output.platform-patch.json'
        data = bytearray(max(item[0] for item in module.PATCHES) + 128)
        struct.pack_into('<IIII', data, 0, 0xFEEDFACF, 0x0100000C, 0, 7)
        for offset, before, _, _ in module.PATCHES:
            struct.pack_into('<I', data, offset, before)
        self.original = bytes(data)
        self.source.write_bytes(self.original)
        self.source.chmod(0o751)
        self.allowed = patch.dict(module.SUPPORTED_INPUTS, {module.sha256(self.original): 'test fixture'}, clear=True)
        self.allowed.start()
        self.addCleanup(self.allowed.stop)

    def assertNoOutput(self):
        self.assertFalse(self.output.exists())
        self.assertFalse(self.manifest.exists())
        self.assertEqual(self.source.read_bytes(), self.original)

    def test_changes_only_exact_instruction_bytes(self):
        result = module.patch_file(self.source, self.output)
        after = self.output.read_bytes()
        changed = {i for i, pair in enumerate(zip(self.original, after)) if pair[0] != pair[1]}
        permitted = {i for offset, _, _, _ in module.PATCHES for i in range(offset, offset + 4)}
        self.assertTrue(changed)
        self.assertLessEqual(changed, permitted)
        self.assertEqual(len(after), len(self.original))
        for offset, _, instruction, _ in module.PATCHES:
            self.assertEqual(struct.unpack_from('<I', after, offset)[0], instruction)
        self.assertEqual(self.source.read_bytes(), self.original)
        self.assertEqual(stat.S_IMODE(self.output.stat().st_mode), 0o751)
        self.assertEqual(result['source_sha256'], module.sha256(self.original))
        self.assertEqual(result['output_sha256'], module.sha256(after))
        self.assertEqual(json.loads(self.manifest.read_text()), result)
        self.assertTrue(result['requires_separate_signing'])

    def test_unknown_hash(self):
        self.source.write_bytes(self.original + b'changed')
        with self.assertRaisesRegex(ValueError, 'Unsupported'):
            module.patch_file(self.source, self.output)
        self.assertFalse(self.output.exists())
        self.assertFalse(self.manifest.exists())

    def test_bad_instruction_even_with_accepted_hash(self):
        data = bytearray(self.original)
        data[module.PATCHES[0][0]] ^= 1
        with patch.dict(module.SUPPORTED_INPUTS, {module.sha256(data): 'invalid fixture'}):
            with self.assertRaisesRegex(ValueError, 'Unexpected instruction'):
                module.patched_bytes(data)

    def test_wrong_header_or_arm64e_even_with_accepted_hash(self):
        for offset, value in ((0, 0), (4, 0x01000007), (8, 2), (12, 6)):
            data = bytearray(self.original)
            struct.pack_into('<I', data, offset, value)
            with patch.dict(module.SUPPORTED_INPUTS, {module.sha256(data): 'invalid fixture'}):
                with self.assertRaisesRegex(ValueError, 'thin arm64 MH_DYLINKER'):
                    module.patched_bytes(data)

    def test_truncated_patch_site(self):
        data = self.original[:module.PATCHES[-1][0] + 3]
        with patch.dict(module.SUPPORTED_INPUTS, {module.sha256(data): 'truncated fixture'}):
            with self.assertRaisesRegex(ValueError, 'Truncated patch site'):
                module.patched_bytes(data)

    def test_never_overwrites_source(self):
        with self.assertRaisesRegex(ValueError, 'overwrite input'):
            module.patch_file(self.source, self.source)
        self.assertNoOutput()

    def test_never_overwrites_output(self):
        self.output.write_bytes(b'keep this')
        with self.assertRaisesRegex(ValueError, 'Refusing to overwrite'):
            module.patch_file(self.source, self.output)
        self.assertEqual(self.output.read_bytes(), b'keep this')
        self.assertFalse(self.manifest.exists())

    def test_never_overwrites_manifest(self):
        self.manifest.write_bytes(b'keep this')
        with self.assertRaisesRegex(ValueError, 'Refusing to overwrite'):
            module.patch_file(self.source, self.output)
        self.assertFalse(self.output.exists())
        self.assertEqual(self.manifest.read_bytes(), b'keep this')

    def test_rejects_dangling_output_symlink(self):
        self.output.symlink_to(self.root / 'missing')
        with self.assertRaisesRegex(ValueError, 'Refusing to overwrite'):
            module.patch_file(self.source, self.output)
        self.assertTrue(self.output.is_symlink())
        self.assertFalse((self.root / 'missing').exists())

    def test_rejects_existing_hardlink(self):
        os.link(self.source, self.output)
        with self.assertRaisesRegex(ValueError, 'Refusing to overwrite'):
            module.patch_file(self.source, self.output)
        self.assertEqual(self.source.read_bytes(), self.original)

    def test_cleans_partial_outputs_on_write_failure(self):
        original_chmod = Path.chmod

        def fail_output_chmod(path, mode):
            if path == self.output:
                raise OSError('test write failure')
            return original_chmod(path, mode)

        with patch.object(Path, 'chmod', fail_output_chmod):
            with self.assertRaisesRegex(OSError, 'test write failure'):
                module.patch_file(self.source, self.output)
        self.assertNoOutput()


if __name__ == '__main__':
    unittest.main()
