"""Keep local package validity separate from portable distribution acceptance."""
import contextlib
import io
import json
from pathlib import Path
import plistlib
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import verify_macos_package as verifier


AD_HOC = '''Identifier=com.YourCompany.LBMStudio
CodeDirectory v=20400 flags=0x2(adhoc) hashes=12+7
Signature=adhoc
TeamIdentifier=not set
'''
DEVELOPER_ID = '''Identifier=com.YourCompany.LBMStudio
CodeDirectory v=20500 flags=0x10000(runtime) hashes=12+7
Authority=Developer ID Application: Fixture (TEAM123456)
Authority=Developer ID Certification Authority
Authority=Apple Root CA
TeamIdentifier=TEAM123456
'''


class MacPackageVerificationTests(unittest.TestCase):
    def setUp(self):
        scratch = Path(__file__).resolve().parents[1]/'tmp'
        scratch.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix='package-verifier-', dir=scratch)
        self.addCleanup(self.temporary.cleanup)
        self.app = Path(self.temporary.name)/'Fixture.app'
        contents = self.app/'Contents'
        (contents/'MacOS').mkdir(parents=True)
        (contents/'MacOS/LBMStudio').write_bytes(b'explicit non-executable test fixture')
        (contents/'Info.plist').write_bytes(plistlib.dumps(
            {'CFBundleIdentifier': 'com.YourCompany.LBMStudio'}))
        self.entitlements = {key: True for key in (
            'com.apple.security.app-sandbox',
            'com.apple.security.files.user-selected.read-write',
            'com.apple.security.files.bookmarks.app-scope')}
        self.metadata = AD_HOC
        self.signature_code = self.gatekeeper_code = self.ticket_code = 0

    def command(self, argv, **_):
        if '--entitlements' in argv:
            return subprocess.CompletedProcess(argv, 0, plistlib.dumps(self.entitlements), b'')
        if '--verify' in argv:
            return subprocess.CompletedProcess(argv, self.signature_code, '', 'invalid seal' if self.signature_code else '')
        if '-dv' in argv:
            return subprocess.CompletedProcess(argv, 0, '', self.metadata)
        if '--assess' in argv:
            return subprocess.CompletedProcess(argv, self.gatekeeper_code, '', 'Gatekeeper fixture')
        if 'stapler' in argv:
            return subprocess.CompletedProcess(argv, self.ticket_code, 'Ticket fixture', '')
        raise AssertionError(f'Unexpected command: {argv}')

    def verify(self, distribution=False):
        argv = ['verify_macos_package.py', str(self.app), '--expected-bundle-id', 'com.YourCompany.LBMStudio']
        if distribution:
            argv.append('--distribution')
        output = io.StringIO()
        with patch('sys.argv', argv), patch.object(verifier.subprocess, 'run', side_effect=self.command) as commands:
            with contextlib.redirect_stdout(output):
                code = verifier.main()
        return code, json.loads(output.getvalue()), [call.args[0] for call in commands.call_args_list]

    def test_local_ad_hoc_pass_is_explicitly_not_distribution(self):
        code, report, commands = self.verify()
        self.assertEqual(code, 0)
        self.assertTrue(report['passed'])
        self.assertFalse(report['distribution_ready'])
        self.assertEqual(report['scope'], 'local-package')
        self.assertIsNone(report['gatekeeper'])
        self.assertIsNone(report['notarization_ticket'])
        self.assertTrue(report['notes'])
        self.assertFalse(any('--assess' in command or 'stapler' in command for command in commands))

    def test_received_ad_hoc_debug_signature_cannot_pass_distribution(self):
        self.entitlements['com.apple.security.get-task-allow'] = True
        code, report, commands = self.verify(True)
        self.assertEqual(code, 1)
        self.assertFalse(report['passed'])
        self.assertFalse(report['distribution_ready'])
        self.assertEqual(len(report['errors']), 5)
        self.assertFalse(any('--assess' in command for command in commands))

    def test_distribution_requires_both_gatekeeper_and_offline_ticket(self):
        self.metadata = DEVELOPER_ID
        for gatekeeper, ticket in [(1, 0), (0, 1), (1, 1)]:
            with self.subTest(gatekeeper=gatekeeper, ticket=ticket):
                self.gatekeeper_code, self.ticket_code = gatekeeper, ticket
                code, report, _ = self.verify(True)
                self.assertEqual(code, 1)
                self.assertFalse(report['distribution_ready'])
                self.assertEqual(report['gatekeeper']['passed'], gatekeeper == 0)
                self.assertEqual(report['notarization_ticket']['passed'], ticket == 0)

    def test_distribution_pass_requires_real_identity_runtime_and_services(self):
        self.metadata = DEVELOPER_ID
        code, report, commands = self.verify(True)
        self.assertEqual(code, 0)
        self.assertTrue(report['distribution_ready'])
        self.assertEqual(report['scope'], 'distribution')
        self.assertEqual(report['signing']['team_identifier'], 'TEAM123456')
        self.assertTrue(any('--assess' in command for command in commands))
        self.assertTrue(any('stapler' in command for command in commands))
        self.assertTrue(any('--verify' in command and '--deep' in command for command in commands))
        self.assertFalse(any('--sign' in command or '--remove-signature' in command for command in commands))

    def test_development_certificate_is_not_a_distribution_certificate(self):
        self.metadata = DEVELOPER_ID.replace('Developer ID Application:', 'Apple Development:')
        code, report, _ = self.verify(True)
        self.assertEqual(code, 1)
        self.assertTrue(any('Developer ID Application' in message for message in report['errors']))

    def test_runtime_and_debug_entitlements_are_required_independently(self):
        for metadata, debug in [(DEVELOPER_ID.replace('0x10000(runtime)', '0x0(none)'), False),
                                (DEVELOPER_ID, True)]:
            with self.subTest(debug=debug):
                self.metadata = metadata
                self.entitlements['com.apple.security.get-task-allow'] = debug
                code, report, _ = self.verify(True)
                self.assertEqual(code, 1)
                self.assertEqual(len(report['errors']), 1)

    def test_invalid_seal_or_missing_document_entitlements_still_fail_locally(self):
        self.signature_code = 1
        code, report, _ = self.verify()
        self.assertEqual(code, 1)
        self.assertIn('invalid seal', report['errors'])
        self.signature_code = 0
        del self.entitlements['com.apple.security.files.bookmarks.app-scope']
        code, report, _ = self.verify()
        self.assertEqual(code, 1)
        self.assertTrue(any('bookmarks.app-scope' in error for error in report['errors']))

    def test_missing_binary_produces_report_instead_of_crashing(self):
        (self.app/'Contents/MacOS/LBMStudio').unlink()
        code, report, _ = self.verify()
        self.assertEqual(code, 1)
        self.assertIsNone(report['binary_sha256'])

    def test_malformed_entitlement_root_is_rejected(self):
        self.entitlements = ['not a plist dictionary']
        code, report, _ = self.verify()
        self.assertEqual(code, 1)
        self.assertIn('Cannot read signed entitlements.', report['errors'])

    def test_numeric_ad_hoc_flag_is_detected_without_signature_label(self):
        metadata = DEVELOPER_ID.replace('0x10000(runtime)', '0x10002(adhoc,runtime)')
        self.assertTrue(verifier.signing_details(metadata)['ad_hoc'])


if __name__ == '__main__':
    unittest.main()
