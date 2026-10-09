#!/usr/bin/env python3
"""Verify a local package, or explicitly assess a Mac distribution artifact."""
import argparse
import configparser
import hashlib
import json
from pathlib import Path
import plistlib
import re
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


def signing_details(output):
    """Read public signing metadata; do not treat an ad hoc seal as identity."""
    values = {}
    authorities = []
    for line in output.splitlines():
        key, separator, value = line.partition('=')
        if not separator:
            continue
        if key == 'Authority':
            authorities.append(value)
        else:
            values[key] = value
    match = re.search(r'\bflags=0x([0-9a-fA-F]+)', output)
    flags = int(match.group(1), 16) if match else 0
    team = values.get('TeamIdentifier')
    return {'ad_hoc': values.get('Signature') == 'adhoc' or bool(flags & 2),
            'team_identifier': None if team in (None, '', 'not set') else team,
            'authorities': authorities, 'hardened_runtime': bool(flags & 0x10000)}


def distribution_errors(signing, entitlements):
    errors = []
    if signing['ad_hoc']:
        errors.append('Ad hoc signing is for local development; use Developer ID Application signing for distribution.')
    if not signing['authorities'] or not signing['authorities'][0].startswith('Developer ID Application:'):
        errors.append('A Developer ID Application signing identity is required for distribution.')
    if not signing['team_identifier']:
        errors.append('The distribution signature has no signing team identifier.')
    if not signing['hardened_runtime']:
        errors.append('Hardened runtime is required for notarized distribution.')
    if entitlements.get('com.apple.security.get-task-allow') not in (None, False):
        errors.append('Remove the get-task-allow debugging entitlement from the distribution signature.')
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('application', type=Path)
    parser.add_argument('--expected-bundle-id')
    parser.add_argument('--distribution', action='store_true',
                        help='Require Developer ID, hardened runtime, Gatekeeper acceptance and a valid stapled notarization ticket.')
    args = parser.parse_args()
    app = args.application.resolve()
    signature = subprocess.run(
        ['/usr/bin/codesign', '--verify', '--strict', '--deep', str(app)],
        capture_output=True, text=True)
    result = subprocess.run(
        ['/usr/bin/codesign', '-d', '--entitlements', '-', '--xml', str(app)],
        capture_output=True)
    metadata = subprocess.run(
        ['/usr/bin/codesign', '-dv', '--verbose=4', str(app)],
        capture_output=True, text=True)
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
    if metadata.returncode:
        errors.append(metadata.stderr.strip())
    signing = signing_details(metadata.stdout + metadata.stderr)
    try:
        entitlements = plistlib.loads(result.stdout)
        if not isinstance(entitlements, dict):
            raise ValueError('Entitlements must be a dictionary.')
    except (plistlib.InvalidFileException, ValueError):
        entitlements = {}
        errors.append('Cannot read signed entitlements.')
    for key in ('com.apple.security.app-sandbox',
                'com.apple.security.files.user-selected.read-write',
                'com.apple.security.files.bookmarks.app-scope'):
        if entitlements.get(key) is not True:
            errors.append(f'Required signed entitlement is missing: {key}')
    assessment = None
    ticket = None
    if args.distribution:
        errors.extend(distribution_errors(signing, entitlements))
        # Reject unsuitable signatures before invoking the distribution services.
        if not errors:
            gatekeeper = subprocess.run(
                ['/usr/sbin/spctl', '--assess', '--type', 'execute', '--verbose=4', str(app)],
                capture_output=True, text=True)
            assessment = {'passed': gatekeeper.returncode == 0,
                          'output': (gatekeeper.stdout + gatekeeper.stderr).strip()}
            stapler = subprocess.run(
                ['xcrun', 'stapler', 'validate', str(app)], capture_output=True, text=True)
            ticket = {'passed': stapler.returncode == 0,
                      'output': (stapler.stdout + stapler.stderr).strip()}
            if not assessment['passed']:
                errors.append('Gatekeeper rejected the distribution artifact: ' + assessment['output'])
            if not ticket['passed']:
                errors.append('A valid stapled notarization ticket is required: ' + ticket['output'])
    executable = app/'Contents/MacOS/LBMStudio'
    digest = None
    try:
        hasher = hashlib.sha256()
        with executable.open('rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                hasher.update(block)
        digest = hasher.hexdigest()
    except OSError as error:
        errors.append(str(error))
    print(json.dumps({'application': str(app), 'binary_sha256': digest,
                      'bundle_identifier': info.get('CFBundleIdentifier'),
                      'expected_bundle_identifier': expected,
                      'entitlements': entitlements, 'errors': errors,
                      'scope': 'distribution' if args.distribution else 'local-package',
                      'signing': signing, 'gatekeeper': assessment, 'notarization_ticket': ticket,
                      'distribution_ready': args.distribution and not errors,
                      'notes': [] if args.distribution else [
                          'A local-package pass does not verify distribution signing, notarization or startup on another Mac.'],
                      'passed': not errors}, indent=2))
    return int(bool(errors))


if __name__ == '__main__':
    raise SystemExit(main())
