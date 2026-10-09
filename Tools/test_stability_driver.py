"""Stability evidence/ownership regressions using synthetic telemetry and Python processes.

No synthetic rows are CFD data and no Unreal application is launched here.
"""
import contextlib
import csv
import io
import json
import os
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest
from unittest.mock import patch

import test_stability as runner


class StabilityEvidenceTests(unittest.TestCase):
    def setUp(self):
        debug = Path(__file__).resolve().parents[1]/'tmp/debug'
        debug.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix='stability-driver-', dir=debug)
        self.root = Path(self.temporary.name)
        self.lane = patch.dict(os.environ, {'STUDIO_RUNTIME_LANE_PATH': str(self.root/'fixture.lock')})
        self.lane.start()
        self.addCleanup(self.lane.stop)

    def tearDown(self):
        self.temporary.cleanup()

    def telemetry(self, duration=30, include_point=True, loops=1, idle_captures=False, include_surface=False):
        rows = []
        for phase in (1, 2, 4):
            for i in range(7):
                slot = (i % 3 if phase == 2 else 2) if include_point else i % 2
                rows.append(dict(phase=phase, phase_s=i*duration/6, footprint_bytes=1024**3,
                                 mesh_bytes=50*1024**2, live_frame_bytes=9*1024**2, live_readers=1,
                                 workers=0, sections=4, rhi_bytes=0, device_allocated_bytes=1024**3,
                                 rhi_count=0, captures=100+i if phase != 4 or idle_captures else 100,
                                 minimized=int(phase == 4 and i >= 4), source_slot=slot,
                                 source_frames=8000 if slot == 2 else 601, point_readers=int(slot == 2),
                                 point_value_bytes=9*1024**2 if slot == 2 else 0, playback_loops=loops))
                if include_surface:
                    enabled = phase != 2 or i % 2 == 1
                    rows[-1].update(surface_attached=int(slot == 2), surface_enabled=int(enabled),
                                    scalar_texture_bytes=131072 if slot == 2 and enabled else 0,
                                    points_enabled=1, frame_current=1,
                                    vertices=18706 if enabled else 18706*18)
        path = self.root/'resources.csv'
        with path.open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=rows[0]);writer.writeheader();writer.writerows(rows)
        return path

    def test_complete_source_mix_is_required(self):
        good = runner.analyze(self.telemetry(), 30, True)
        self.assertEqual(good['errors'], [])
        self.assertEqual(good['source_coverage']['2'], [0, 1, 2])
        bad = runner.analyze(self.telemetry(include_point=False), 30, True)
        self.assertTrue(any('full point recording' in e for e in bad['errors']))
        self.assertTrue(any('No live point reader' in e for e in bad['errors']))

    def test_long_duration_requires_natural_sequence_completion(self):
        bad = runner.analyze(self.telemetry(duration=1200, loops=0), 1200, True)
        self.assertIn('Sustained playback never completed a full point-source loop', bad['errors'])
        self.assertEqual(runner.analyze(self.telemetry(duration=1200), 1200, True)['errors'], [])

    def test_hidden_view_captures_fail_acceptance(self):
        result = runner.analyze(self.telemetry(idle_captures=True), 30, True)
        self.assertIn('Idle/minimized phase submitted additional captures', result['errors'])

    def test_overlapping_geometry_workers_fail_acceptance(self):
        path = self.telemetry()
        with path.open(newline='') as stream:
            rows = list(csv.DictReader(stream))
        rows[3]['workers'] = '2'
        with path.open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=rows[0]);writer.writeheader();writer.writerows(rows)
        result = runner.analyze(path, 30, True)
        self.assertTrue(any('workers exceeded budget' in error for error in result['errors']))

    def test_surface_gate_requires_actual_texture_and_representation_coverage(self):
        self.assertEqual(runner.analyze(self.telemetry(include_surface=True), 30, True, True)['errors'], [])
        missing = runner.analyze(self.telemetry(), 30, True, True)
        self.assertTrue(any('Surface telemetry missing' in e for e in missing['errors']))
        path = self.telemetry(include_surface=True)
        with path.open(newline='') as stream:
            rows = list(csv.DictReader(stream))
        for row in rows:
            row['scalar_texture_bytes'] = '0'
        with path.open('w', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=rows[0]);writer.writeheader();writer.writerows(rows)
        bad = runner.analyze(path, 30, True, True)
        self.assertIn('Phase 1 did not retain a rendered reconstructed surface', bad['errors'])
        self.assertIn('Phase 4 did not retain a rendered reconstructed surface', bad['errors'])
        self.assertIn('Mixed phase did not measure both surface and original-point representations', bad['errors'])

    def test_hidden_or_stale_points_cannot_satisfy_surface_gate(self):
        for invalid in ({'points_enabled': '0'}, {'frame_current': '0'},
                        {'vertices': '1000'}, {'scalar_texture_bytes': '131072'}):
            with self.subTest(invalid=invalid):
                path = self.telemetry(include_surface=True)
                with path.open(newline='') as stream:
                    rows = list(csv.DictReader(stream))
                for row in rows:
                    if row['phase'] == '2' and row['source_slot'] == '2' and row['surface_enabled'] == '0':
                        row.update(invalid)
                with path.open('w', newline='') as stream:
                    writer = csv.DictWriter(stream, fieldnames=rows[0]);writer.writeheader();writer.writerows(rows)
                bad = runner.analyze(path, 30, True, True)
                self.assertIn('Mixed phase did not measure both surface and original-point representations', bad['errors'])

    def test_surface_gate_requires_original_recording_before_launch(self):
        source = self.root/'reconstruction.json';source.write_text('{}')
        with patch.object(sys, 'argv', ['test_stability.py', '30', '--surface-reconstruction', str(source)]), \
             contextlib.redirect_stderr(io.StringIO()), patch.object(runner.subprocess, 'Popen') as launch:
            with self.assertRaises(SystemExit) as caught:
                runner.main()
            self.assertEqual(caught.exception.code, 2)
            launch.assert_not_called()

    def fixture(self, child=False, active=False):
        app = self.root/'Packaged/Mac/LBMStudio.app'
        binary = app/'Contents/MacOS/LBMStudio'
        binary.parent.mkdir(parents=True)
        with (app/'Contents/Info.plist').open('wb') as stream:
            plistlib.dump({'CFBundleIdentifier': 'test.stability.fixture'}, stream)
        program = '''import json, pathlib, subprocess, sys, time
report = pathlib.Path(next(arg.split('=', 1)[1] for arg in sys.argv if arg.startswith('-ReportExportPath=')))
report.mkdir(parents=True)
(report.parent/'MixedUse').mkdir(exist_ok=True)
(report.parent/'MixedUse/resources.csv').write_text('synthetic ownership fixture')
CHILD
(report/'index.json').write_text(json.dumps({'succeeded': 1, 'failed': 0, 'notRun': 0, 'succeededWithWarnings': 0}))
'''.replace('CHILD', "subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])" if child else '')
        binary.write_text(f'#!{sys.executable}\n'+program);binary.chmod(0o755)
        gate = {123: {'command': '/test/LBMStudio'}} if active else {}
        with patch.object(runner, '__file__', str(self.root/'Tools/test_stability.py')), \
             patch.object(runner, 'relevant', return_value=gate), \
             patch.object(runner, 'analyze', return_value={'errors': []}), \
             patch.object(Path, 'home', return_value=self.root/'home'), \
             patch.object(sys, 'argv', ['test_stability.py', '30']), \
             contextlib.redirect_stdout(io.StringIO()):
            code = runner.main()
        path = next((self.root/'tmp/debug').glob('stability-*/manifest.json'))
        return code, json.loads(path.read_text())

    def test_clean_process_retains_scoped_manifest(self):
        code, report = self.fixture()
        self.assertEqual(code, 0)
        self.assertEqual(report['acceptance'], 'driver_rehearsal')
        self.assertEqual(report['source_mix'], 'both_legacy')
        self.assertEqual(report['owned_processes_after'], {})

    def test_reparented_child_is_reaped_and_fails_gate(self):
        code, report = self.fixture(child=True)
        self.assertEqual(code, 1)
        self.assertIn('Owned processes required cleanup', report['errors'])
        self.assertEqual(report['owned_processes_after'], {})

    def test_existing_application_is_left_untouched(self):
        with self.assertRaises(SystemExit):
            self.fixture(active=True)
        self.assertFalse((self.root/'tmp/debug').exists())

    def test_unverified_point_source_is_rejected_before_launch(self):
        source = self.root/'recording.json';source.write_text('{}')
        with patch.object(sys, 'argv', ['test_stability.py', '30', '--point-recording', str(source)]), \
             contextlib.redirect_stderr(io.StringIO()), patch.object(runner.subprocess, 'Popen') as launch:
            with self.assertRaises(SystemExit) as caught:
                runner.main()
            self.assertEqual(caught.exception.code, 2)
            launch.assert_not_called()


if __name__ == '__main__':
    unittest.main()
