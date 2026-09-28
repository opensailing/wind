"""Exercise the actual pre-main Mac bootstrap without starting Unreal or a GPU."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(sys.platform == 'darwin', 'macOS executable startup')
class MacStartupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[1]
        debug = root/'tmp/debug'
        debug.mkdir(parents=True, exist_ok=True)
        cls.temporary = tempfile.TemporaryDirectory(prefix='mac-startup-', dir=debug)
        cls.root = Path(cls.temporary.name)
        fixture = cls.root/'fixture.cpp'
        fixture.write_text('''#include <cstdio>
#include <cstdlib>
#include <unistd.h>
int main(int argc, char** argv) {
    printf("%d\\n%s\\n", getpid(), getenv("STUDIO_STARTUP_FIXTURE"));
    for (int i=1; i<argc; ++i) printf("%s\\n", argv[i]);
    return 23;
}
''')
        cls.binaries = {}
        for profile, editor, development in [('game', 0, 1), ('editor', 1, 1), ('shipping', 0, 0)]:
            binary = cls.root/(profile+' with spaces')
            subprocess.run(['xcrun', 'clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            f'-DWITH_EDITOR={editor}', f'-DUE_BUILD_DEVELOPMENT={development}',
                            str(root/'Source/LBMStudio/Mac/StudioStartup.cpp'), str(fixture),
                            '-o', str(binary)], check=True, capture_output=True, text=True)
            cls.binaries[profile] = binary
        failed_exec = cls.root/'failed-exec.cpp'
        failed_exec.write_text('''#include <cerrno>
extern "C" int execv(const char*, char* const*) { errno=EACCES; return -1; }
''')
        cls.failure_binary = cls.root/'failed exec'
        subprocess.run(['xcrun', 'clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-DWITH_EDITOR=0', '-DUE_BUILD_DEVELOPMENT=1',
                        str(root/'Source/LBMStudio/Mac/StudioStartup.cpp'), str(fixture), str(failed_exec),
                        '-o', str(cls.failure_binary)], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def launch(self, arguments, profile='game', spoof_argv=False):
        binary = self.binaries[profile]
        command = ['not-the-loaded-image' if spoof_argv else str(binary), *arguments]
        proc = subprocess.Popen(command, executable=str(binary), cwd=self.root,
                                env={**os.environ, 'STUDIO_STARTUP_FIXTURE': 'retained'},
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        stdout, stderr = proc.communicate(timeout=10)
        self.assertEqual(proc.returncode, 23)
        lines = stdout.splitlines()
        self.assertEqual(int(lines[0]), proc.pid, 'exec must retain process ownership')
        self.assertEqual(lines[1], 'retained')
        return lines[2:], stderr

    def test_bare_launch_reexecutes_once_and_preserves_literal_arguments(self):
        values = ['-Example=literal $(text) with spaces', 'unicode-é', '-LLMCSV']
        arguments, stderr = self.launch(values, spoof_argv=True)
        self.assertEqual(arguments, [*values, '-LLM'])
        self.assertEqual(stderr.count('retaining memory tracking'), 1)

    def test_existing_tracker_argument_does_not_reexecute(self):
        for flag in ['-LLM', '-llm']:
            with self.subTest(flag=flag):
                arguments, stderr = self.launch(['first', flag, 'last'])
                self.assertEqual(arguments, ['first', flag, 'last'])
                self.assertEqual(stderr, '')

    def test_editor_and_shipping_profiles_do_not_reexecute(self):
        for profile in ['editor', 'shipping']:
            with self.subTest(profile=profile):
                arguments, stderr = self.launch(['unchanged'], profile)
                self.assertEqual(arguments, ['unchanged'])
                self.assertEqual(stderr, '')

    def test_failed_exec_does_not_continue_into_engine_startup(self):
        result = subprocess.run([str(self.failure_binary)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 78)
        self.assertEqual(result.stdout, '')
        self.assertIn('memory-tracker launch failed: Permission denied', result.stderr)


if __name__ == '__main__':
    unittest.main()
