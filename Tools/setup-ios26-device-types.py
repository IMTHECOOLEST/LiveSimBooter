#!/usr/bin/env python3
"""Stage original CoreSimulator device capabilities for LiveSimBooter's fixed device catalog.

Apple device profile resources are user-supplied inputs, never repository assets. This does not
modify a runtime, sign code, deploy files, or establish that an experimental device boots iOS 26.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import plistlib
import shutil
import stat


CATALOG = (
    dict(identifier='iphone-8', name='iPhone 8', bundle='iPhone-8', model='iPhone10,4', product='D20',
         width=750, height=1334, scale=2, pitch=326, idiom='phone', subtype=569, home=1, experimental=True, island=False),
    dict(identifier='iphone-8-plus', name='iPhone 8 Plus', bundle='iPhone-8-Plus', model='iPhone10,5', product='D21',
         width=1242, height=2208, scale=3, pitch=461, idiom='phone', subtype=570, home=1, experimental=True, island=False),
    dict(identifier='iphone-se-2', name='iPhone SE (2nd generation)', bundle='iPhone-SE--2nd-generation-', model='iPhone12,8', product='D79',
         width=750, height=1334, scale=2, pitch=326, idiom='phone', subtype=569, home=1, experimental=False, island=False),
    dict(identifier='iphone-11', name='iPhone 11', bundle='iPhone-11', model='iPhone12,1', product='N104',
         width=828, height=1792, scale=2, pitch=326, idiom='phone', subtype=1792, home=2, experimental=False, island=False),
    dict(identifier='iphone-15-pro-max', name='iPhone 15 Pro Max', bundle='iPhone-15-Pro-Max', model='iPhone16,2', product='D84',
         width=1290, height=2796, scale=3, pitch=460, idiom='phone', subtype=2796, home=2, experimental=False, island=True),
    dict(identifier='ipad-9', name='iPad (9th generation)', bundle='iPad-9th-generation', model='iPad12,2', product='J182',
         width=1620, height=2160, scale=2, pitch=264, idiom='pad', subtype=2160, home=0, experimental=False, island=False),
    dict(identifier='ipad-a16', name='iPad (A16)', bundle='iPad-A16', model='iPad15,7', product='J481',
         width=1640, height=2360, scale=2, pitch=264, idiom='pad', subtype=2360, home=2, experimental=False, island=False),
)
CAPABILITIES_DIRECTORY = Path('RuntimeRoot/Library/Developer/CoreSimulator/DeviceTypes')
MAX_PLIST_BYTES = 2 * 1024 * 1024


class ValidationError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ValidationError(message)


def number(value, expected):
    return type(value) in (int, float) and math.isfinite(value) and value == expected


def read_original(root, relative):
    path = root / relative
    for component in (path, *path.parents):
        if component == root:
            break
        require(not component.is_symlink(), f'Symlinked source is not accepted: {component}')
    metadata = path.stat()
    require(stat.S_ISREG(metadata.st_mode) and 0 < metadata.st_size <= MAX_PLIST_BYTES, f'Invalid source plist: {path}')
    data = path.read_bytes()
    require(len(data) == metadata.st_size, f'Source changed while reading: {path}')
    try:
        document = plistlib.loads(data)
    except (ValueError, TypeError, OverflowError, plistlib.InvalidFileException) as error:
        raise ValidationError(f'Invalid plist {path}: {error}') from error
    require(isinstance(document, dict), f'Expected plist dictionary: {path}')
    return data, document


def runtime_version(value):
    require(isinstance(value, str) and value and all(part.isdecimal() for part in value.split('.')), 'Invalid runtime version')
    parts = tuple(int(part) for part in value.split('.'))
    require(len(parts) <= 3, 'Invalid runtime version')
    return parts + (0,) * (3 - len(parts))


def inspect_device(root, device):
    bundle = Path(device['name'] + '.simdevicetype') / 'Contents'
    info_bytes, info = read_original(root, bundle / 'Info.plist')
    profile_bytes, profile = read_original(root, bundle / 'Resources/profile.plist')
    capabilities_bytes, document = read_original(root, bundle / 'Resources/capabilities.plist')
    require(info.get('CFBundleIdentifier') == 'com.apple.CoreSimulator.SimDeviceType.' + device['bundle'], 'Wrong device bundle identifier')
    require(profile.get('modelIdentifier') == device['model'] and profile.get('productClass') == device['product'], 'Wrong device identity')
    require(isinstance(profile.get('supportedArchs'), list) and 'arm64' in profile['supportedArchs'], 'Profile does not support arm64')
    require(runtime_version(profile.get('minRuntimeVersion')) <= (26, 0, 0), 'Profile requires a newer runtime')
    maximum = profile.get('maxRuntimeVersion')
    excluded = maximum is not None and runtime_version(maximum) < (26, 0, 0)
    require(excluded == device['experimental'], 'Runtime support boundary differs from the catalog')
    capabilities = document.get('capabilities')
    require(isinstance(capabilities, dict), 'Missing capabilities dictionary')
    for key, expected in (('modelIdentifier', device['model']), ('idiom', device['idiom'])):
        require(capabilities.get(key) == expected, f'Wrong capabilities {key}')
    require('productClass' not in capabilities or capabilities['productClass'] == device['product'], 'Wrong capabilities productClass')
    require(type(capabilities.get('ipad')) is bool and capabilities['ipad'] == (device['idiom'] == 'pad'), 'Wrong iPad capability')
    require(number(capabilities.get('HomeButtonType'), device['home']), 'Wrong home-button type')
    if 'DeviceSupportsDynamicIsland' in capabilities:
        require(type(capabilities['DeviceSupportsDynamicIsland']) is bool and capabilities['DeviceSupportsDynamicIsland'] == device['island'],
                'Wrong Dynamic Island capability')
    artwork = capabilities.get('ArtworkTraits')
    require(isinstance(artwork, dict) and artwork.get('ArtworkDeviceIdiom') == device['idiom'], 'Wrong artwork idiom')
    require(number(artwork.get('ArtworkDeviceScaleFactor'), device['scale']) and number(artwork.get('ArtworkDeviceSubType'), device['subtype']),
            'Wrong artwork scale/subtype')
    displays = capabilities.get('displays')
    require(isinstance(displays, list) and len(displays) <= 32 and all(isinstance(item, dict) for item in displays), 'Invalid displays')
    primary = [item for item in displays if item.get('deviceName') == 'primary']
    require(len(primary) == 1, 'Expected exactly one primary display')
    for key, catalog_key in (('width', 'width'), ('height', 'height'), ('scale', 'scale'), ('hdpi', 'pitch'), ('vdpi', 'pitch')):
        require(number(primary[0].get(key), device[catalog_key]), f'Wrong primary display {key}')
    # In particular, do not substitute iPhone 8 Plus's physical 1080x1920 ScreenDimensionsCapability for its 1242x2208 simulator display.
    destination = CAPABILITIES_DIRECTORY / device['identifier'] / 'capabilities.plist'
    receipt = dict(device, destination=destination.as_posix(), original_bundle=str(root / bundle.parent),
                   device_type_version=info.get('CFBundleVersion'), min_runtime=profile['minRuntimeVersion'], max_runtime=maximum,
                   bytes=len(capabilities_bytes), sha256=hashlib.sha256(capabilities_bytes).hexdigest(),
                   profile_sha256=hashlib.sha256(profile_bytes).hexdigest(), info_sha256=hashlib.sha256(info_bytes).hexdigest())
    return receipt, capabilities_bytes


def stage(device_types_root, output_dir, dry_run=False):
    root = Path(device_types_root).resolve(strict=True)
    require(root.is_dir(), 'Device types root must be a directory')
    output = Path(output_dir).absolute()
    require(not os.path.lexists(output), f'Output already exists: {output}')
    try:
        output.resolve().relative_to(root)
    except ValueError:
        pass
    else:
        raise ValidationError('Output must be outside the original device types root')
    inspected = [inspect_device(root, device) for device in CATALOG]
    manifest = dict(version=1, runtime_version='26.0', runtime_build='23A343', default_identifier='iphone-15-pro-max',
                    device_types=[receipt for receipt, _ in inspected], experimental_identifiers=[x['identifier'] for x in CATALOG if x['experimental']])
    if dry_run:
        return manifest
    output.parent.mkdir(parents=True, exist_ok=True)
    output.mkdir()  # Claim only this new directory; never clean another caller's existing output on failure.
    try:
        for receipt, data in inspected:
            destination = output / receipt['destination']
            destination.parent.mkdir(parents=True, exist_ok=True)
            with destination.open('xb') as stream:
                stream.write(data)
            destination.chmod(0o644)
        with (output / 'manifest.json').open('x') as stream:
            json.dump(manifest, stream, indent=2, sort_keys=True)
            stream.write('\n')
    except BaseException:
        shutil.rmtree(output)
        raise
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device-types-root', type=Path, required=True, help='Original CoreSimulator Profiles/DeviceTypes directory')
    parser.add_argument('--output-dir', type=Path, required=True, help='New staging directory; existing paths are refused')
    parser.add_argument('--dry-run', action='store_true', help='Validate all seven profiles and print the inventory without writing files')
    options = parser.parse_args()
    try:
        manifest = stage(options.device_types_root, options.output_dir, options.dry_run)
    except (OSError, ValidationError) as error:
        parser.exit(1, f'{error}\n')
    print(json.dumps(manifest, indent=2, sort_keys=True))


if __name__ == '__main__':
    main()
