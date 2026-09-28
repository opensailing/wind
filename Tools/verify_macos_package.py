#!/usr/bin/env python3
"""Verify native document access in the signed macOS application."""
import argparse
import configparser
import hashlib
import json
from pathlib import Path
import plistlib
import subprocess


def configured_bundle_identifier(project):
    config = configparser.ConfigParser(interpolation=None, strict=False)
    config.read(project/'Config/DefaultEngine.ini')
    identifier = config.get('/Script/MacTargetPlatform.XcodeProjectSettings',
                            'BundleIdentifier', fallback='').strip()
    if not identifier or '$(' in identifier:
        raise ValueError('Configure an explicit BundleIdentifier in XcodeProjectSettings.')
    return identifier


def check_bundle_identifier(info, expected):
    actual = info.get('CFBundleIdentifier')
    return [] if actual == expected else [f'Bundle identifier mismatch: expected {expected}, got {actual}.']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('application', type=Path)
    parser.add_argument('--expected-bundle-id')
    args = parser.parse_args()
    app = args.application.resolve()
    signature = subprocess.run(
        ['/usr/bin/codesign', '--verify', '--strict', str(app)],
        capture_output=True, text=True)
    result = subprocess.run(
        ['/usr/bin/codesign', '-d', '--entitlements', ':-', str(app)],
        capture_output=True)
    errors = []
    try:
        expected = args.expected_bundle_id or configured_bundle_identifier(Path(__file__).resolve().parents[1])
        with (app/'Contents/Info.plist').open('rb') as stream:
            info = plistlib.load(stream)
        errors.extend(check_bundle_identifier(info, expected))
    except (OSError, ValueError, configparser.Error, plistlib.InvalidFileException) as error:
        expected, info = args.expected_bundle_id, {}
        errors.append(str(error))
    if signature.returncode:
        errors.append(signature.stderr.strip())
    if result.returncode:
        errors.append(result.stderr.decode(errors='replace').strip())
    try:
        entitlements = plistlib.loads(result.stdout)
    except (plistlib.InvalidFileException, ValueError):
        entitlements = {}
        errors.append('Cannot read signed entitlements.')
    for key in ('com.apple.security.app-sandbox',
                'com.apple.security.files.user-selected.read-write',
                'com.apple.security.files.bookmarks.app-scope'):
        if entitlements.get(key) is not True:
            errors.append(f'Required signed entitlement is missing: {key}')
    executable = app/'Contents/MacOS/LBMStudio'
    digest = hashlib.sha256()
    with executable.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    print(json.dumps({'application': str(app), 'binary_sha256': digest.hexdigest(),
                      'bundle_identifier': info.get('CFBundleIdentifier'),
                      'expected_bundle_identifier': expected,
                      'entitlements': entitlements, 'errors': errors,
                      'passed': not errors}, indent=2))
    return int(bool(errors))


if __name__ == '__main__':
    raise SystemExit(main())
