#!/usr/bin/env python3
import hashlib
import importlib.util
from pathlib import Path
import plistlib
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('device_stager', Path(__file__).resolve().parents[1] / 'Tools/setup-ios26-device-types.py')
stager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stager)


class DeviceStagingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        self.root = self.base / 'DeviceTypes'
        self.root.mkdir()
        self.output = self.base / 'output'
        for device in stager.CATALOG:
            contents = self.root / (device['name'] + '.simdevicetype') / 'Contents'
            (contents / 'Resources').mkdir(parents=True)
            info = {'CFBundleIdentifier': 'com.apple.CoreSimulator.SimDeviceType.' + device['bundle'], 'CFBundleVersion': 'fixture'}
            profile = {'modelIdentifier': device['model'], 'productClass': device['product'], 'supportedArchs': ['arm64'], 'minRuntimeVersion': '11.0'}
            if device['experimental']:
                profile['maxRuntimeVersion'] = '16.9'
            capabilities = {'capabilities': {'modelIdentifier': device['model'], 'idiom': device['idiom'], 'ipad': device['idiom'] == 'pad',
                'HomeButtonType': device['home'], 'DeviceSupportsDynamicIsland': device['island'],
                'ArtworkTraits': {'ArtworkDeviceIdiom': device['idiom'], 'ArtworkDeviceScaleFactor': device['scale'],
                                  'ArtworkDeviceSubType': device['subtype']},
                'displays': [{'deviceName': 'primary', 'width': device['width'], 'height': device['height'], 'scale': device['scale'],
                              'hdpi': device['pitch'], 'vdpi': device['pitch']}], 'opaque-original-data': b'preserve\x00exactly'}}
            if device['identifier'] == 'iphone-8-plus':
                capabilities['capabilities']['ScreenDimensionsCapability'] = {'main-screen-width': 1080, 'main-screen-height': 1920}
            for name, document in [('Info.plist', info), ('Resources/profile.plist', profile), ('Resources/capabilities.plist', capabilities)]:
                (contents / name).write_bytes(plistlib.dumps(document, fmt=plistlib.FMT_BINARY))

    def tearDown(self):
        self.temporary.cleanup()

    def source(self, name='capabilities.plist'):
        return self.root / (stager.CATALOG[0]['name'] + '.simdevicetype') / 'Contents/Resources' / name

    def mutate(self, relative, callback):
        path = self.source(relative)
        document = plistlib.loads(path.read_bytes())
        callback(document)
        path.write_bytes(plistlib.dumps(document))

    def fails(self):
        with self.assertRaises((stager.ValidationError, OSError)):
            stager.stage(self.root, self.output)
        self.assertFalse(self.output.exists())

    def test_stage_preserves_originals_and_records_primary_display(self):
        before = {p: p.read_bytes() for p in self.root.rglob('*.plist')}
        manifest = stager.stage(self.root, self.output)
        self.assertEqual(len(manifest['device_types']), 7)
        self.assertEqual(manifest['default_identifier'], 'iphone-15-pro-max')
        self.assertEqual(manifest['experimental_identifiers'], ['iphone-8', 'iphone-8-plus'])
        for row in manifest['device_types']:
            original = self.root / (row['name'] + '.simdevicetype') / 'Contents/Resources/capabilities.plist'
            staged = self.output / row['destination']
            self.assertEqual(staged.read_bytes(), original.read_bytes())
            self.assertEqual(row['sha256'], hashlib.sha256(staged.read_bytes()).hexdigest())
            self.assertEqual(row['bytes'], staged.stat().st_size)
        plus = manifest['device_types'][1]
        self.assertEqual((plus['width'], plus['height'], plus['pitch']), (1242, 2208, 461))
        self.assertEqual(before, {p: p.read_bytes() for p in self.root.rglob('*.plist')})
        self.assertEqual(len(list(self.output.rglob('capabilities.plist'))), 7)
        self.assertTrue((self.output / 'manifest.json').exists())

    def test_dry_run_does_not_create_output(self):
        self.assertEqual(len(stager.stage(self.root, self.output, True)['device_types']), 7)
        self.assertFalse(self.output.exists())

    def test_no_overwrite_or_source_output(self):
        self.output.mkdir()
        (self.output / 'sentinel').write_bytes(b'keep')
        with self.assertRaises(stager.ValidationError): stager.stage(self.root, self.output)
        self.assertEqual((self.output / 'sentinel').read_bytes(), b'keep')
        with self.assertRaises(stager.ValidationError): stager.stage(self.root, self.root / 'output')
        link = self.base / 'output-link'
        link.symlink_to(self.base / 'missing')
        with self.assertRaises(stager.ValidationError): stager.stage(self.root, link)

    def test_wrong_profile_or_runtime_boundary(self):
        for key, value in [('modelIdentifier', 'wrong'), ('productClass', 'wrong'), ('supportedArchs', ['arm64e']),
                           ('minRuntimeVersion', '27.0'), ('maxRuntimeVersion', '26.9')]:
            original = self.source('profile.plist').read_bytes()
            self.mutate('profile.plist', lambda data: data.__setitem__(key, value))
            self.fails()
            self.source('profile.plist').write_bytes(original)

    def test_malformed_identity_primary_and_artwork(self):
        original = self.source().read_bytes()
        changes = [lambda c: c.__setitem__('modelIdentifier', 'wrong'), lambda c: c.__setitem__('idiom', 'pad'),
                   lambda c: c.__setitem__('ipad', 0), lambda c: c.__setitem__('HomeButtonType', True),
                   lambda c: c['ArtworkTraits'].__setitem__('ArtworkDeviceScaleFactor', 3),
                   lambda c: c['displays'].append(dict(c['displays'][0])), lambda c: c.__setitem__('displays', []),
                   lambda c: c['displays'][0].__setitem__('width', True), lambda c: c['displays'][0].__setitem__('vdpi', 1)]
        for change in changes:
            self.mutate('capabilities.plist', lambda data: change(data['capabilities']))
            self.fails()
            self.source().write_bytes(original)

    def test_sources_symlinks_malformed_and_size(self):
        source = self.source()
        data = source.read_bytes()
        source.unlink()
        elsewhere = self.base / 'elsewhere.plist'
        elsewhere.write_bytes(data)
        source.symlink_to(elsewhere)
        self.fails()
        source.unlink()
        for data in (b'not-a-plist', plistlib.dumps([]), b'x' * (stager.MAX_PLIST_BYTES + 1)):
            source.write_bytes(data)
            self.fails()

    def test_failed_write_removes_only_new_output(self):
        original_open = Path.open
        def failing_open(path, *args, **kwargs):
            if path == self.output / 'manifest.json':
                raise OSError('simulated publication failure')
            return original_open(path, *args, **kwargs)
        before = self.source().read_bytes()
        with patch.object(Path, 'open', failing_open):
            self.fails()
        self.assertEqual(self.source().read_bytes(), before)


if __name__ == '__main__':
    unittest.main()
