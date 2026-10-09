"""Exercise cross-process launch exclusion without starting or compiling Unreal."""
import contextlib
import io
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import runtime_lane as lane
from test_stability import relevant


class RuntimeLaneTests(unittest.TestCase):
    def setUp(self):
        self.tools = Path(__file__).resolve().parent
        debug = self.tools.parent/'tmp/debug'
        debug.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix='runtime-lane-', dir=debug)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.lock = self.root/'isolated.lock'
        environment = patch.dict(os.environ, {lane.PATH_ENV: str(self.lock),
                                             lane.FD_ENV: '-1', lane.TOKEN_ENV: '',
                                             'PYTHONPATH': str(self.tools)})
        environment.start()
        self.addCleanup(environment.stop)

    def python(self, program, *arguments, **kwargs):
        return subprocess.run([sys.executable, '-c', program, *arguments],
                              capture_output=True, text=True, timeout=10, **kwargs)

    def contender(self):
        return self.python('from pathlib import Path\nfrom runtime_lane import lease, LaneBusy\n'
                           'try:\n with lease(Path.cwd(), "contender"): print("entered")\n'
                           'except LaneBusy as error:\n print(error); raise SystemExit(2)\n')

    def stop(self, process):
        if process.poll() is None:
            process.kill()
        process.communicate(timeout=10)

    def owner(self, program, *arguments):
        process = subprocess.Popen([sys.executable, '-c', program, *arguments],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        self.addCleanup(self.stop, process)
        self.assertTrue(select.select([process.stdout], [], [], 10)[0], 'owner did not become ready')
        line = process.stdout.readline().strip()
        self.assertEqual(line, 'ready', line)
        return process

    def test_atomic_claim_precedes_any_native_process_and_releases_after_death(self):
        owner = self.owner('from pathlib import Path\nimport sys\nfrom runtime_lane import lease\n'
                           'with lease(Path.cwd(), "fixture before launch"):\n'
                           ' print("ready", flush=True)\n sys.stdin.read()\n')
        blocked = self.contender()
        self.assertEqual(blocked.returncode, 2, blocked.stderr)
        self.assertIn('fixture before launch', blocked.stdout)
        self.assertNotIn('entered', blocked.stdout)
        owner.kill(); owner.communicate(timeout=10)
        self.assertEqual(self.contender().returncode, 0)

    def test_stale_metadata_does_not_own_or_replace_the_lock_file(self):
        self.lock.write_text(json.dumps({'pid': os.getpid(), 'label': 'stale'}))
        inode = self.lock.stat().st_ino
        with lane.lease(self.root, 'current'):
            self.assertEqual(json.loads(self.lock.read_text())['label'], 'current')
        self.assertEqual(self.lock.stat().st_ino, inode)
        self.assertEqual(self.contender().returncode, 0)

    def test_nested_workspace_roots_share_the_outer_project_lock(self):
        outer = self.root/'outer'; inner = outer/'tmp/volume-workspace'
        inner.mkdir(parents=True)
        (outer/'LBMStudio.uproject').touch(); (inner/'LBMStudio.uproject').touch()
        with patch.dict(os.environ):
            os.environ.pop(lane.PATH_ENV)
            self.assertEqual(lane.lock_path(inner), lane.lock_path(outer))

    def test_unrelated_descriptor_or_token_cannot_borrow_a_live_lease(self):
        other = self.root/'other'; other.touch()
        with lane.lease(self.root, 'owner') as fd, other.open() as unrelated:
            token = json.loads(self.lock.read_text())['token']
            with patch.dict(os.environ, {lane.FD_ENV: str(unrelated.fileno()), lane.TOKEN_ENV: token}):
                self.assertIsNone(lane.inherited_descriptor(self.lock))
                with self.assertRaises(lane.LaneBusy):
                    with lane.lease(self.root, 'blocked'):
                        self.fail('unrelated descriptor borrowed a lease')
            with patch.dict(os.environ, {lane.FD_ENV: str(fd), lane.TOKEN_ENV: 'wrong'}):
                self.assertIsNone(lane.inherited_descriptor(self.lock))

    def test_valid_nested_borrow_does_not_close_the_owner_descriptor(self):
        with lane.lease(self.root, 'owner') as fd:
            token = json.loads(self.lock.read_text())['token']
            with patch.dict(os.environ, {lane.FD_ENV: str(fd), lane.TOKEN_ENV: token}):
                with lane.lease(self.root, 'nested') as borrowed:
                    self.assertEqual(fd, borrowed)
                os.fstat(fd)
                self.assertEqual(self.contender().returncode, 2)
        self.assertEqual(self.contender().returncode, 0)

    def test_exec_shell_and_python_entrypoint_preserve_lock_and_literal_arguments(self):
        # Stub only the legacy-process inventory; the real kernel lock, exec,
        # inherited descriptors and shell route are exercised end to end.
        entry = self.root/'entry.py'
        entry.write_text('import json, sys\nfrom pathlib import Path\nfrom runtime_lane import serialized\n'
                         '@serialized("nested Python entry")\ndef run():\n'
                         ' Path(__file__).with_suffix(".json").write_text(json.dumps(sys.argv[1:]))\n'
                         ' print("ready", flush=True)\n'
                         ' sys.stdin.read()\nrun()\n')
        script = self.root/'entry.sh'
        script.write_text('set -eu\n"$1" "$2/runtime_lane.py" --check-inherited\n'
                          'exec "$1" "$3" "$4"\n')
        literal = 'space $(literal) `inert` "quote"'
        owner = self.owner('import runtime_lane as lane\n'
                           'lane.existing_unreal_work=lambda: {}\nraise SystemExit(lane.main())\n',
                           '--label', 'exec owner', '--', '/bin/bash', str(script),
                           sys.executable, str(self.tools), str(entry), literal)
        self.assertEqual(self.contender().returncode, 2)
        _, errors = owner.communicate(input='', timeout=10)
        self.assertEqual(owner.returncode, 0, errors)
        self.assertEqual(json.loads(entry.with_suffix('.json').read_text()), [literal])
        self.assertEqual(self.contender().returncode, 0)

    def test_decorated_entrypoint_refuses_before_its_body(self):
        called = []
        @lane.serialized('blocked entry')
        def entry():
            called.append(True)
        with lane.lease(self.root, 'owner'), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(entry(), 2)
        self.assertEqual(called, [])

    def test_legacy_build_detection_includes_engine_dotnet_only(self):
        rows = {1: {'command': '/engine/dotnet'}, 2: {'command': '/engine/ShaderCompileWorker'},
                3: {'command': '/unrelated/dotnet'}, 4: {'command': '/engine/CrashReportClient'},
                5: {'command': '/usr/bin/python3'}}
        with patch.object(lane, 'process_arguments', side_effect=lambda pid:
                          '/engine/dotnet /engine/UnrealBuildTool.dll' if pid == 1 else 'dotnet unrelated.dll'):
            self.assertEqual(set(relevant(rows)), {1, 2, 4})

    def test_cli_refuses_existing_uncooperative_work_without_executing(self):
        with patch.object(sys, 'argv', ['runtime_lane.py', '--', 'unused']), \
             patch.object(lane, 'existing_unreal_work', return_value={42: '/test/LBMStudio'}), \
             patch.object(lane.os, 'execvpe') as execute, contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(lane.main(), 2)
            execute.assert_not_called()
        self.assertEqual(self.contender().returncode, 0)


if __name__ == '__main__':
    unittest.main()
