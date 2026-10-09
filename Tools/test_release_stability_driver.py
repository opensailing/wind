"""Adversarial resource evidence fixtures, not generated CFD output."""
import csv
from pathlib import Path
import tempfile
import unittest

from test_volume_stability import analyze


class ReleaseEvidenceTests(unittest.TestCase):
    def setUp(self):
        root=Path(__file__).resolve().parents[1]/'tmp/debug'
        root.mkdir(parents=True, exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(prefix='release-evidence-',dir=root)
        self.addCleanup(self.temp.cleanup)

    def fixture(self, mutate=lambda rows: None, seconds=1200):
        rows=[];elapsed=0
        for phase in (1,10,2,4):
            duration=seconds*(2 if phase in (1,4) else 1)
            for t in range(0,duration+1,10):
                slot=(t//10)%3 if phase==2 else 2
                rows.append(dict(elapsed_s=elapsed+t,phase=phase,phase_s=t,footprint_bytes=1024**3,
                    mesh_bytes=40*1024**2,workers=0,sections=4,scalar_texture_bytes=8*1024**2,
                    point_value_bytes=8*1024**2,point_readers=1,rhi_bytes=0,rhi_count=0,
                    device_allocated_bytes=1024**3,source_frames=5901,volume_attached=1,volume_enabled=1,
                    captures=500 if phase==4 else t,minimized=int(phase==4 and t>=duration/2),source_slot=slot,
                    playback_loops=1,export_active=0,export_scenes=0,total_scene_workers=0,
                    export_completed=3 if phase==4 else 0,export_cancelled=1 if phase==4 else 0,
                    camera_edits=1000 if phase in (10,2,4) else 0,slice_edits=100 if phase in (10,2,4) else 0))
            elapsed+=duration+10
        mutate(rows)
        path=Path(self.temp.name)/'resources.csv'
        with path.open('w',newline='') as output:
            writer=csv.DictWriter(output,fieldnames=rows[0]);writer.writeheader();writer.writerows(rows)
        return path

    def test_two_hour_schedule_and_short_rehearsal_are_distinct(self):
        result=analyze(self.fixture(),1200,release=True)
        self.assertTrue(result['passed'],result)
        self.assertEqual(result['acceptance'],'two-hour integrated stability')
        short=analyze(self.fixture(seconds=60),60,release=True)
        self.assertTrue(short['passed'],short)
        self.assertEqual(short['acceptance'],'driver rehearsal only')

    def test_old_one_hour_schedule_cannot_pass_release_gate(self):
        def shorten(rows):rows[:]=[r for r in rows if r['phase']!=10 and r['phase_s']<=1200]
        result=analyze(self.fixture(shorten),1200,release=True)
        self.assertFalse(result['passed'])
        self.assertEqual(sum('duration/samples' in e for e in result['errors']),3)

    def test_hidden_capture_export_actor_and_worker_fail(self):
        for key,value,expected in [('captures',501,'captures continued'),('export_scenes',1,'Idle retains'),('total_scene_workers',1,'Idle retains')]:
            with self.subTest(key=key):
                def alter(rows):next(r for r in rows if r['phase']==4 and r['phase_s']>1200)[key]=value
                result=analyze(self.fixture(alter),1200,release=True)
                self.assertTrue(any(expected in e for e in result['errors']),result)

    def test_retained_memory_growth_and_extra_export_ownership_fail(self):
        def grow(rows):
            for r in rows:
                if r['phase']==2 and r['phase_s']>900:r['footprint_bytes']+=400*1024**2
            rows[0]['export_scenes']=2
        result=analyze(self.fixture(grow),1200,release=True)
        self.assertTrue(any('retained process growth' in e for e in result['errors']),result)
        self.assertTrue(any('ownership' in e for e in result['errors']),result)

    def test_nonfinite_missing_telemetry_and_missing_work_fail(self):
        for mutation,expected in [(lambda rows:rows[0].update(footprint_bytes=float('nan')),'Nonfinite'),
            (lambda rows:[r.pop('export_active') for r in rows],'Missing telemetry'),
            (lambda rows:[r.update(export_cancelled=0) for r in rows],'cancellation'),
            (lambda rows:[r.update(slice_edits=0) for r in rows],'Camera phase')]:
            with self.subTest(expected=expected):
                result=analyze(self.fixture(mutation),1200,release=True)
                self.assertTrue(any(expected in e for e in result['errors']),result)
                self.assertFalse(result['passed'])
                self.assertEqual(result['acceptance'],'two-hour integrated stability')

    def test_empty_telemetry_reports_failure_without_losing_scope(self):
        path=Path(self.temp.name)/'resources.csv'
        path.write_text('phase,phase_s\n')
        result=analyze(path,30,release=True)
        self.assertFalse(result['passed'])
        self.assertEqual(result['acceptance'],'driver rehearsal only')
        self.assertIn('No volume resource telemetry',result['errors'])


if __name__=='__main__':unittest.main()
