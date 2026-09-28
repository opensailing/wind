"""Verify launch ownership with small Python processes; never launch Unreal."""
import contextlib
import io
import json
import os
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest
from unittest.mock import patch

import run_packaged_suite as runner


class PackagedProcessOwnership(unittest.TestCase):
    def setUp(self):
        debug = Path(__file__).resolve().parents[1]/'tmp/debug'
        debug.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix='owned-suite-', dir=debug)
        self.root = Path(self.temporary.name)
        self.lane = patch.dict(os.environ, {'STUDIO_RUNTIME_LANE_PATH': str(self.root/'fixture.lock')})
        self.lane.start()
        self.addCleanup(self.lane.stop)
        app = self.root/'Packaged/Mac/LBMStudio.app'
        (app/'Contents/MacOS').mkdir(parents=True)
        with (app/'Contents/Info.plist').open('wb') as stream:
            plistlib.dump({'CFBundleIdentifier': 'test.owned.process.fixture'}, stream)
        self.binary = app/'Contents/MacOS/LBMStudio'

    def tearDown(self):
        self.temporary.cleanup()

    def run_fixture(self, child=False, active=False, point_recording=False, application_default=False, startup_log=True,
                    surface_reconstruction=False, volume=False, missing_volume=False, performance=False):
        program = '''import json, pathlib, subprocess, sys, time
pathlib.Path(__file__).with_suffix('.args.json').write_text(json.dumps(sys.argv))
report = pathlib.Path(next(arg.split('=', 1)[1] for arg in sys.argv if arg.startswith('-ReportExportPath=')))
report.mkdir(parents=True)
CHILD
STARTUP
time.sleep(1.2)
(report/'index.json').write_text(json.dumps({'succeeded': 1, 'failed': 0, 'notRun': 0, 'succeededWithWarnings': 0}))
'''.replace('CHILD', "subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])" if child else '')
        program = program.replace('STARTUP', '''print('Studio startup: retaining memory tracking before engine initialization.')
print('LLM enabled CsvWriter: off TraceWriter: off')''' if application_default and startup_log else '')
        self.binary.write_text(f'#!{sys.executable}\n'+program)
        self.binary.chmod(0o755)
        arguments = ['run_packaged_suite.py', '--suite', 'Studio.Fixture.', '--count', '1', '--name', 'fixture']
        if performance:
            arguments[2] = 'ScientificAcceptance.PerformanceUI.MeasurePauseCameraAndRestore'
        if application_default:
            arguments += ['--startup-profile', 'application-default']
        if point_recording:
            recording = self.root/'field data $(literal)'/'recording.json'
            recording.parent.mkdir()
            recording.write_text('{}\n')
            arguments[2] = 'ScientificAcceptance.PointRecording.FullSequence'
            arguments += ['--point-recording', str(recording)]
        if surface_reconstruction:
            surface = self.root/'surface data $(literal)'/'reconstruction.json'
            surface.parent.mkdir()
            surface.write_text('{}\n')
            arguments[2] = 'ScientificAcceptance.Surface.FullSequence'
            arguments += ['--surface-reconstruction', str(surface)]
        if volume or missing_volume:
            original = self.root/'volume data $(literal)'/'recording.json'
            original.parent.mkdir()
            original.write_text('{}\n')
            reconstruction = self.root/'volume map $(literal)'/'reconstruction.json'
            reconstruction.parent.mkdir()
            reconstruction.write_text('{"fixture": true}\n')
            arguments[2] = 'Studio.VolumeStability.MixedUse'
            arguments += ['--volume-recording', str(original), '--volume-phase-seconds', '30']
            if not missing_volume:
                arguments += ['--volume-reconstruction', str(reconstruction)]
        # Root/home replacement confines all files to the temporary fixture.
        # Suppressing the real-app gate is limited to this known Python binary.
        gate = {123: {'command': '/test/LBMStudio'}} if active else {}
        with patch.object(runner, '__file__', str(self.root/'Tools/run_packaged_suite.py')), \
             patch.object(runner, 'relevant', return_value=gate), \
             patch.object(Path, 'home', return_value=self.root/'home'), \
             patch.object(sys, 'argv', arguments), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            result = runner.main()
        manifests = list((self.root/'tmp/debug').glob('fixture-*/manifest.json'))
        self.assertEqual(len(manifests), 1)
        return result, json.loads(manifests[0].read_text())

    def test_clean_process_and_report(self):
        code, report = self.run_fixture()
        self.assertEqual(code, 0)
        self.assertTrue(report['passed'])
        self.assertEqual(report['owned_processes_after'], {})
        self.assertEqual(len(report['binary_sha256']), 64)
        self.assertEqual(report['startup_arguments'], ['-LLM'])
        self.assertIn('-LLM', json.loads(self.binary.with_suffix('.args.json').read_text()))

    def test_performance_ui_needs_no_external_source_argument(self):
        code, report = self.run_fixture(performance=True)
        self.assertEqual(code, 0)
        self.assertTrue(report['passed'])
        self.assertEqual(report['owned_processes_after'], {})

    def test_reparented_child_is_cleaned_and_fails_acceptance(self):
        code, report = self.run_fixture(child=True)
        self.assertEqual(code, 1)
        self.assertFalse(report['passed'])
        self.assertIn('Owned processes required cleanup', report['errors'])
        self.assertEqual(report['owned_processes_after'], {})

    def test_application_default_requires_bootstrap_evidence_without_supplied_tracking(self):
        code, report = self.run_fixture(application_default=True)
        self.assertEqual(code, 0)
        self.assertEqual(report['startup_profile'], 'application-default')
        self.assertEqual(report['startup_arguments'], [])
        self.assertNotIn('-LLM', json.loads(self.binary.with_suffix('.args.json').read_text()))

    def test_application_default_rejects_missing_bootstrap_evidence(self):
        code, report = self.run_fixture(application_default=True, startup_log=False)
        self.assertEqual(code, 1)
        self.assertIn('Expected exactly one pre-main tracking bootstrap', report['errors'])
        self.assertIn('Engine did not confirm ordinary memory tracking', report['errors'])

    def test_recording_path_is_one_unquoted_process_argument(self):
        code, report = self.run_fixture(point_recording=True)
        self.assertEqual(code, 0)
        args = json.loads(self.binary.with_suffix('.args.json').read_text())
        expected = '-StudioPointRecording='+report['point_recording']['path']
        self.assertIn(expected, args)
        self.assertNotIn('"', expected)
        self.assertEqual(len(report['point_recording']['metadata_sha256']), 64)

    def test_full_surface_paths_are_inert_arguments_and_both_identities_are_retained(self):
        code, report = self.run_fixture(point_recording=True, surface_reconstruction=True)
        self.assertEqual(code, 0)
        args = json.loads(self.binary.with_suffix('.args.json').read_text())
        self.assertIn('-StudioPointRecording='+report['point_recording']['path'], args)
        self.assertIn('-StudioSurfaceReconstruction='+report['surface_reconstruction']['path'], args)
        self.assertEqual(len(report['surface_reconstruction']['metadata_sha256']), 64)

    def test_surface_gate_requires_the_original_recording(self):
        with self.assertRaises(SystemExit) as caught:
            self.run_fixture(surface_reconstruction=True)
        self.assertEqual(caught.exception.code, 2)
        self.assertFalse(self.binary.with_suffix('.args.json').exists())

    def test_volume_paths_are_inert_and_source_identities_are_retained(self):
        code, report = self.run_fixture(volume=True)
        self.assertEqual(code, 0)
        args = json.loads(self.binary.with_suffix('.args.json').read_text())
        for option, key in [('StudioVolumeRecording', 'volume_recording'),
                            ('StudioVolumeReconstruction', 'volume_reconstruction')]:
            expected = '-'+option+'='+report[key]['path']
            self.assertIn(expected, args)
            self.assertEqual(args.count(expected), 1)
            self.assertEqual(len(report[key]['sha256']), 64)
            self.assertIn('$(literal)', report[key]['path'])
        self.assertEqual(report['volume_phase_seconds'], 30)
        self.assertIn('-StudioVolumePhaseSeconds=30', args)
        self.assertIn('-StudioVolumeSoak', args)

    def test_volume_gate_refuses_missing_reconstruction_before_launch(self):
        with self.assertRaises(SystemExit) as caught:
            self.run_fixture(missing_volume=True)
        self.assertEqual(caught.exception.code, 2)
        self.assertFalse(self.binary.with_suffix('.args.json').exists())

    def test_existing_application_prevents_launch(self):
        with self.assertRaises(SystemExit) as caught:
            self.run_fixture(active=True)
        self.assertEqual(caught.exception.code, 2)
        self.assertFalse((self.root/'tmp/debug').exists())

    def test_reused_pid_is_not_owned(self):
        previous = {'started': 'original time', 'command': 'original executable'}
        self.assertFalse(runner.same_process(1, previous, {1: {'started': 'later time', 'command': 'original executable'}}))
        self.assertFalse(runner.same_process(1, previous, {1: {'started': 'original time', 'command': 'different executable'}}))
        self.assertFalse(runner.same_process(1, previous, {}))


if __name__ == '__main__':
    unittest.main()
