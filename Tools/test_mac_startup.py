"""Exercise the real Foundation argument adapter; any self-exec fails the test."""
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
        fixture = cls.root/'fixture.mm'
        fixture.write_text('''#import <Foundation/Foundation.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
extern "C" int execv(const char*, char* const*) {
    fputs("forbidden fixture self-exec\\n", stderr);
    _exit(96);
}
int main() { @autoreleasepool {
    printf("%d\\n%s\\n", getpid(), getenv("STUDIO_STARTUP_FIXTURE"));
    NSArray<NSString*>* arguments=NSProcessInfo.processInfo.arguments;
    for (NSUInteger i=1; i<arguments.count; ++i) printf("%s\\n", arguments[i].UTF8String);
    if (![arguments isEqualToArray:NSProcessInfo.processInfo.arguments]) return 95;
    return 23;
} }
''')
        cls.binaries = {}
        for profile, editor, development in [('game', 0, 1), ('editor', 1, 1), ('shipping', 0, 0)]:
            binary = cls.root/(profile+' with spaces')
            subprocess.run(['xcrun', 'clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            f'-DWITH_EDITOR={editor}', f'-DUE_BUILD_DEVELOPMENT={development}',
                            '-framework', 'Foundation',
                            str(root/'Source/LBMStudio/Mac/StudioStartup.mm'), str(fixture),
                            '-o', str(binary)], check=True, capture_output=True, text=True)
            cls.binaries[profile] = binary

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
        self.assertEqual(int(lines[0]), proc.pid, 'startup must retain process ownership')
        self.assertEqual(lines[1], 'retained')
        return lines[2:], stderr

    def test_bare_launch_never_executes_and_preserves_literal_arguments(self):
        values = ['-Example=literal $(text) with spaces', 'unicode-é', '-LLMCSV']
        arguments, stderr = self.launch(values, spoof_argv=True)
        self.assertEqual(arguments, [*values, '-LLM'])
        self.assertEqual(stderr, '')

    def test_existing_tracker_argument_is_not_duplicated(self):
        for flag in ['-LLM', '-llm']:
            with self.subTest(flag=flag):
                arguments, stderr = self.launch(['first', flag, 'last'])
                self.assertEqual(arguments, ['first', flag, 'last'])
                self.assertEqual(stderr, '')

    def test_editor_and_shipping_profiles_keep_original_arguments(self):
        for profile in ['editor', 'shipping']:
            with self.subTest(profile=profile):
                arguments, stderr = self.launch(['unchanged'], profile)
                self.assertEqual(arguments, ['unchanged'])
                self.assertEqual(stderr, '')

    def test_empty_command_line_and_repeated_foundation_reads_are_stable(self):
        arguments, stderr = self.launch([])
        self.assertEqual(arguments, ['-LLM'])
        self.assertEqual(stderr, '')


if __name__ == '__main__':
    unittest.main()
