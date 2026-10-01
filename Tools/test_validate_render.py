"""Adversarial report fixtures, not scientific or rendered CFD data."""
import copy
import json
import unittest

from validate_render import CASE_NAMES, CHECK_NAMES, evaluate


class RendererReportTests(unittest.TestCase):
    def setUp(self):
        self.expected = {}
        self.report = {'version': 1, 'mode': 'windowless-gpu-commandlet', 'rhi': 'Metal',
                       'windowless': True, 'screenshots': 0, 'passed': True, 'errors': [],
                       'checks': [{'name': name, 'passed': True} for name in CHECK_NAMES+CASE_NAMES], 'cases': []}
        for i, name in enumerate(CASE_NAMES):
            wing = name.startswith('wing.')
            identity = {'id': 'test-report-wing' if wing else 'test-report-volume', 'metadata_sha256': 'a'*64,
                        'payload_sha256': 'b'*64 if wing else '', 'reconstruction_sha256': '' if wing else 'c'*64,
                        'dimensions': 2 if wing else 3, 'ordinal': 0 if name.endswith('.first') else 2,
                        'step': 0 if name.endswith('.first') else 10, 'time': 0. if name.endswith('.first') else 1.}
            self.expected[name] = identity
            row = {'name': name, 'passed': True, 'frame_matches_source': True, 'dataset': identity['id'],
                   'metadata_sha256': identity['metadata_sha256'], 'payload_sha256': identity['payload_sha256'],
                   'reconstruction_sha256': identity['reconstruction_sha256'], 'spatial_dimensions': identity['dimensions'],
                   'ordinal': identity['ordinal'], 'original_step': identity['step'], 'original_time': identity['time'],
                   'width': 640, 'height': 360, 'colored_pixels': 5000, 'mesh_bytes': 1024,
                   'scalar_texture_bytes': 1024, 'volume_required': not wing and name != 'volume.isosurface',
                   'pixel_crc32': f'{i+1:08x}', 'orthographic': name == 'volume.inside_orthographic'}
            if name == 'volume.restored':
                row['pixel_crc32'] = self.report['cases'][CASE_NAMES.index('volume.last')]['pixel_crc32']
            if name == 'snapshot.original_frame':
                row['snapshot_mean_rgb_error'] = 0.
                row['snapshot_metadata'] = json.dumps({'dataset': identity['id'], 'ordinal': identity['ordinal'],
                    'frame': identity['step'], 'time_seconds': identity['time'], 'metadata_sha256': identity['metadata_sha256'],
                    'reconstruction_sha256': identity['reconstruction_sha256'], 'size': [640, 360]})
            self.report['cases'].append(row)

    def assertRejected(self, mutation):
        report = copy.deepcopy(self.report)
        mutation(report)
        result = evaluate(report, self.expected)
        self.assertFalse(result['passed'], report)
        self.assertTrue(result['errors'])

    def test_complete_consistent_evidence(self):
        self.assertTrue(evaluate(self.report, self.expected)['passed'])

    def test_missing_duplicate_extra_or_unfinished_cases(self):
        for mutation in (lambda r: r['cases'].pop(), lambda r: r['cases'].append(copy.deepcopy(r['cases'][0])),
                         lambda r: r['cases'][0].update(name='unexpected'), lambda r: r['cases'][0].update(passed=False),
                         lambda r: r['checks'].pop(), lambda r: r['checks'][0].update(passed=False)):
            self.assertRejected(mutation)

    def test_nullrhi_window_or_screenshots_fail(self):
        for key, value in [('rhi', 'Null'), ('windowless', False), ('screenshots', 1), ('version', 2),
                           ('passed', False), ('errors', ['failure'])]:
            self.assertRejected(lambda r: r.update({key: value}))

    def test_wrong_source_or_original_frame_fails(self):
        for key, value in [('dataset', 'wrong'), ('metadata_sha256', 'd'*64), ('payload_sha256', ''),
                           ('original_step', 1), ('original_time', .1), ('ordinal', 1), ('spatial_dimensions', 3),
                           ('frame_matches_source', False)]:
            self.assertRejected(lambda r: r['cases'][0].update({key: value}))

    def test_blank_wrong_size_or_unbounded_render_fails(self):
        for key, value in [('colored_pixels', 0), ('colored_pixels', float('nan')), ('colored_pixels', True),
                           ('width', 1), ('mesh_bytes', 129*1024**2), ('scalar_texture_bytes', 'unavailable'),
                           ('pixel_crc32', None), ('orthographic', True)]:
            self.assertRejected(lambda r: r['cases'][0].update({key: value}))
        self.assertRejected(lambda r: r['cases'][3].update(scalar_texture_bytes=0))

    def test_pixel_checks_do_not_trust_success_flag(self):
        self.assertRejected(lambda r: r['cases'][1].update(pixel_crc32=r['cases'][0]['pixel_crc32']))
        self.assertRejected(lambda r: r['cases'][9].update(pixel_crc32='deadbeef'))
        self.assertRejected(lambda r: r['cases'][7].update(pixel_crc32=r['cases'][4]['pixel_crc32']))

    def test_snapshot_metadata_is_independently_checked(self):
        for metadata in ('invalid', '{}', '[]'):
            self.assertRejected(lambda r: r['cases'][-1].update(snapshot_metadata=metadata))
        for difference in (None, 2., -1., float('nan'), True):
            self.assertRejected(lambda r: r['cases'][-1].update(snapshot_mean_rgb_error=difference))

    def test_malformed_reports_fail_closed(self):
        for report in (None, {}, [], {'cases': None, 'checks': []}, {'cases': [None], 'checks': []},
                       {'cases': [{'name': []}], 'checks': []}):
            self.assertFalse(evaluate(report, self.expected)['passed'])


if __name__ == '__main__':
    unittest.main()
