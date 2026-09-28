"""Crash-reporter ownership checks; no Unreal process is launched or signalled."""
import datetime
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import studio_processes
from test_stability import inventory


class ReporterOwnershipTests(unittest.TestCase):
    def setUp(self):
        self.started = time.time()
        self.item = {'parent': 1, 'started': datetime.datetime.fromtimestamp(self.started).strftime('%a %b %d %H:%M:%S %Y'),
                     'command': '/Engine/CrashReportClientEditor'}
        self.uuid = '1234567890abcdef1234567890abcdef'
        self.args = f'CrashReportClient /home/Library/Application Support/Epic/Crashes/CrashReport-UE-LBMStudio-pid-123-{self.uuid}/ -NoAnalytics'

    def classify(self, args=None, before=None, item=None):
        with patch.object(studio_processes, 'process_arguments', return_value=self.args if args is None else args):
            return studio_processes.detached_reporters({456: item or self.item}, before or {}, 123, self.started)

    def test_exact_detached_reporter_and_ensure_identity(self):
        owned = self.classify()
        self.assertEqual(owned[456]['crash_report_owner_pid'], 123)
        self.assertEqual(owned[456]['crash_report_run_uuid'], self.uuid)
        self.assertIn(456, self.classify(self.args.replace('CrashReport-UE-', 'EnsureReport-UE-')))

    def test_other_launch_and_project_are_not_owned(self):
        for wrong in ('pid-1234-', 'pid-12-', 'pid-124-'):
            self.assertEqual(self.classify(self.args.replace('pid-123-', wrong)), {})
        self.assertEqual(self.classify(self.args.replace('UE-LBMStudio-', 'UE-AnotherProject-')), {})
        self.assertEqual(self.classify(self.args.replace(self.uuid, self.uuid+'a')), {})
        self.assertEqual(self.classify(self.args.replace(self.uuid, 'not-a-run-uuid')), {})

    def test_preexisting_old_unidentified_and_unrelated_processes_are_retained(self):
        self.assertEqual(self.classify(before={456: self.item}), {})
        old = {**self.item, 'started': datetime.datetime.fromtimestamp(self.started-5).strftime('%a %b %d %H:%M:%S %Y')}
        self.assertEqual(self.classify(item=old), {})
        self.assertEqual(self.classify(item={**self.item, 'started': 'unknown'}), {})
        self.assertEqual(self.classify(item={**self.item, 'command': '/usr/bin/OtherApp'}), {})
        self.assertEqual(self.classify(args=''), {})

    def test_exited_process_during_argument_query_is_ignored(self):
        with patch.object(studio_processes.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, 'ps')):
            self.assertEqual(studio_processes.process_arguments(456), '')

    def test_reused_tracked_pid_cannot_adopt_unrelated_descendants(self):
        tracked = {456: self.item}
        current = {456: {**self.item, 'command': '/unrelated/new-process'},
                   789: {**self.item, 'command': '/unrelated/child', 'parent': 456}}
        with patch.object(studio_processes.os, 'getpgid', return_value=999):
            studio_processes.track_processes(SimpleNamespace(pid=123, returncode=0), {}, tracked, current, self.started)
        self.assertNotIn(789, tracked)
        self.assertFalse(studio_processes.same_process(456, tracked[456], current))

    @unittest.skipUnless(sys.platform == 'darwin', 'Exercises macOS ps and detached process semantics')
    def test_real_reparented_separate_group_is_identified_without_name_only_cleanup(self):
        debug = Path(__file__).resolve().parents[1]/'tmp/debug'
        debug.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='detached-reporter-', dir=debug) as temporary:
            root = Path(temporary)
            # A tiny owned POSIX process stands in for the reporter executable;
            # no engine, window, upload or system executable copy is used.
            executable = root/'CrashReportClientEditor'
            subprocess.run(['xcrun', 'clang', '-x', 'c', '-', '-o', str(executable)],
                           input='#include <unistd.h>\nint main(void) { for (;;) pause(); }\n',
                           check=True, text=True, capture_output=True)
            script = '''import os, pathlib, subprocess, sys
folder = pathlib.Path(sys.argv[2])
report = folder / ('CrashReport-UE-LBMStudio-pid-%d-' % os.getpid() + '1234567890abcdef1234567890abcdef')
report.touch()
child = subprocess.Popen([sys.argv[1], '-f', str(report)], stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
print(child.pid, flush=True)
'''
            before = inventory(); started = time.time()
            parent = subprocess.Popen([sys.executable, '-c', script, str(executable), str(root)],
                                      stdout=subprocess.PIPE, text=True, start_new_session=True)
            child_pid = None
            try:
                output, _ = parent.communicate(timeout=5)
                self.assertEqual(parent.returncode, 0)
                child_pid = int(output.strip())
                current = inventory()
                self.assertTrue(child_pid in current, 'Fixture reporter must be live')
                self.assertNotEqual(current[child_pid]['parent'], parent.pid)
                self.assertNotEqual(os.getpgid(child_pid), parent.pid)
                owned = studio_processes.detached_reporters(current, before, parent.pid, started)
                self.assertEqual(set(owned), {child_pid})
                # An unrelated launch cannot adopt this same reporter.
                self.assertEqual(studio_processes.detached_reporters(current, before, parent.pid+1, started), {})
            finally:
                if parent.poll() is None:
                    parent.terminate(); parent.wait(timeout=5)
                if child_pid is not None:
                    current = inventory()
                    if current.get(child_pid, {}).get('command') == str(executable):
                        os.kill(child_pid, signal.SIGTERM)
                        deadline = time.monotonic()+3
                        while child_pid in inventory() and time.monotonic()<deadline:
                            time.sleep(.05)
                        self.assertTrue(child_pid not in inventory(), 'Fixture reporter must exit cleanly')


if __name__ == '__main__':
    unittest.main()
