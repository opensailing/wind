"""Normal-launch lifecycle checks with owned fixtures, never an Unreal app."""
import contextlib
import fcntl
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import run_studio as runner


class StudioLauncherTests(unittest.TestCase):
    def setUp(self):
        debug = Path(__file__).resolve().parents[1]/'tmp/debug'
        debug.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix='studio-launcher-', dir=debug)
        self.root = Path(self.temporary.name)
        self.lane = patch.dict(os.environ, {'STUDIO_RUNTIME_LANE_PATH': str(self.root/'fixture.lock')})
        self.lane.start()
        self.addCleanup(self.lane.stop)

    def tearDown(self):
        self.temporary.cleanup()

    def fixture(self, kind):
        reporter = self.root/'CrashReportClientEditor'
        if kind == 'detached':
            subprocess.run(['xcrun', 'clang', '-x', 'c', '-', '-o', str(reporter)],
                           input='#include <unistd.h>\nint main(void) { for (;;) pause(); }\n',
                           check=True, text=True, capture_output=True)
        script = self.root/'fixture.py'
        script.write_text('''import json, os, pathlib, signal, subprocess, sys, time
root = pathlib.Path(__file__).parent
(root/'args.json').write_text(json.dumps(sys.argv[1:]))
kind = sys.argv[1]
if kind == 'child':
    subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])
if kind == 'detached':
    report = root/('CrashReport-UE-LBMStudio-pid-%d-' % os.getpid() + '1234567890abcdef1234567890abcdef')
    subprocess.Popen([str(root/'CrashReportClientEditor'), str(report)], start_new_session=True)
if kind == 'interrupt':
    os.kill(os.getppid(), signal.SIGTERM)
    time.sleep(30)
if kind == 'failure':
    sys.exit(7)
''')
        # The real-app guard is suppressed only for this explicit Python fixture.
        with patch.object(runner, 'relevant', return_value={}), contextlib.redirect_stdout(io.StringIO()):
            code = runner.run_owned([sys.executable, str(script), kind, 'literal path $(not a command)'], self.root)
        path = next((self.root/'tmp/debug').glob('launch-*.json'))
        report = json.loads(path.read_text())
        self.assertEqual(report['owned_processes_after'], {})
        self.assertEqual(report['errors'], [])
        return code, report

    def test_clean_launch_preserves_arguments_and_exit(self):
        code, report = self.fixture('clean')
        self.assertEqual(code, 0)
        self.assertEqual(report['cleanup_required'], [])
        self.assertEqual(json.loads((self.root/'args.json').read_text()), ['clean', 'literal path $(not a command)'])

    def test_nonzero_application_exit_is_preserved(self):
        code, report = self.fixture('failure')
        self.assertEqual(code, 7)
        self.assertEqual(report['application_exit_code'], 7)

    def test_packaged_launch_keeps_memory_tracking_and_literal_arguments(self):
        binary = self.root/'Packaged/Mac/LBMStudio.app/Contents/MacOS/LBMStudio'
        binary.parent.mkdir(parents=True)
        binary.touch()
        with patch.object(runner, '__file__', str(self.root/'Tools/run_studio.py')), \
             patch.object(sys, 'argv', ['run_studio.py', '--packaged', '-Example=literal path']), \
             patch.object(runner, 'run_owned', return_value=0) as launch:
            self.assertEqual(runner.main(), 0)
        command, root = launch.call_args.args
        self.assertEqual(root, self.root)
        self.assertEqual(command[0], str(binary))
        self.assertEqual(command.count('-LLM'), 1)
        self.assertEqual(command[-1], '-Example=literal path')

    def test_reparented_child_is_cleaned(self):
        code, report = self.fixture('child')
        self.assertEqual(code, 0)
        self.assertTrue(report['cleanup_required'])

    @unittest.skipUnless(sys.platform == 'darwin', 'macOS crash-report launch semantics')
    def test_detached_reporter_is_cleaned_after_fast_exit(self):
        code, report = self.fixture('detached')
        self.assertEqual(code, 0)
        reporters = [r for r in report['owned_processes'].values() if 'crash_report_owner_pid' in r]
        self.assertEqual(len(reporters), 1)
        self.assertEqual(reporters[0]['crash_report_owner_pid'], report['pid'])
        self.assertTrue(report['cleanup_required'])

    def test_termination_cleans_owned_process_and_restores_handler(self):
        before = signal.getsignal(signal.SIGTERM)
        code, report = self.fixture('interrupt')
        self.assertEqual(code, 128+signal.SIGTERM)
        self.assertEqual(report['interrupted_signal'], signal.SIGTERM)
        self.assertEqual(signal.getsignal(signal.SIGTERM), before)

    def test_existing_app_refuses_launch_without_signalling(self):
        current = {123: {'command': '/test/LBMStudio'}}
        with patch.object(runner, 'inventory', return_value=current), \
             patch.object(runner, 'relevant', return_value=current), \
             patch.object(runner.subprocess, 'Popen') as launch, \
             patch.object(runner.os, 'kill') as kill, contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(runner.run_owned(['unused'], self.root), 1)
            launch.assert_not_called();kill.assert_not_called()

    def test_launcher_lock_prevents_concurrent_launch(self):
        (self.root/'Saved').mkdir()
        with (self.root/'Saved/.studio-launch.lock').open('a+') as held:
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with patch.object(runner, 'inventory') as snapshot, contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(runner.run_owned(['unused'], self.root), 1)
                snapshot.assert_not_called()

    def test_headless_deadline_cleans_owned_process_and_keeps_logs(self):
        log=self.root/'headless.log';report=self.root/'headless-process.json'
        with patch.object(runner,'relevant',return_value={}), contextlib.redirect_stdout(io.StringIO()):
            code=runner._run_owned([sys.executable,'-u','-c','import time; print("ready"); time.sleep(30)'],
                                   self.root,timeout=.4,log_path=log,report_path=report)
        result=json.loads(report.read_text())
        self.assertEqual(code,124)
        self.assertIn('ready',log.read_text())
        self.assertTrue(result['cleanup_required'])
        self.assertEqual(result['owned_processes_after'],{})
        self.assertIn('exceeded',result['errors'][0])

    def test_headless_clean_run_writes_requested_report(self):
        log=self.root/'headless.log';report=self.root/'headless-process.json'
        with patch.object(runner,'relevant',return_value={}), contextlib.redirect_stdout(io.StringIO()):
            code=runner._run_owned([sys.executable,'-c','print("machine result")'],self.root,
                                   timeout=10,log_path=log,report_path=report)
        result=json.loads(report.read_text())
        self.assertEqual(code,0)
        self.assertEqual(result['cleanup_required'],[])
        self.assertEqual(result['owned_processes_after'],{})
        self.assertEqual(log.read_text(),'machine result\n')
        self.assertGreater(result['elapsed_seconds'],0)


if __name__ == '__main__':
    unittest.main()
