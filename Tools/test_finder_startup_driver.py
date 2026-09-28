"""Ownership checks for LaunchServices startup acceptance; does not launch UE."""
from pathlib import Path
import unittest

from test_finder_startup import find_launched_process


class LaunchServicesIdentityTests(unittest.TestCase):
    binary = Path('/Applications/LBMStudio.app/Contents/MacOS/LBMStudio')
    token = '-StudioLaunchToken=unique'

    def row(self, path=None):
        return {'command': str(path or self.binary), 'started': 'start', 'parent': 1}

    def test_identifies_reexec_without_launchservices_pid(self):
        current = {73: self.row()}
        self.assertEqual(find_launched_process({}, current, self.binary, self.token,
                         lambda pid: f'{self.binary} -windowed {self.token} -LLM'), (73, current[73]))

    def test_rejects_preexisting_wrong_binary_and_partial_token(self):
        before = {73: self.row()}
        current = {**before, 74: self.row('/Applications/Other'), 75: self.row()}
        self.assertIsNone(find_launched_process(before, current, self.binary, self.token,
                          lambda pid: self.token + ('-suffix' if pid == 75 else '')))

    def test_ambiguous_or_missing_identity_never_adopted(self):
        self.assertIsNone(find_launched_process({}, {}, self.binary, self.token))
        with self.assertRaises(ValueError):
            find_launched_process({}, {73: self.row(), 74: self.row()}, self.binary,
                                  self.token, lambda pid: self.token)


if __name__ == '__main__':
    unittest.main()
