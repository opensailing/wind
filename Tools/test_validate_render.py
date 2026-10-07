"""Adversarial report fixtures, not scientific or rendered CFD data."""
import copy
import json
import unittest
import hashlib
from pathlib import Path
import struct
import tempfile
import zlib

from validate_render import CASE_NAMES, CHECK_NAMES, HOME4_CASE_NAMES, evaluate, embedded_archive, npy_member, expected_home4, evaluate_home4_pixels


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
            home4 = name in HOME4_CASE_NAMES
            if home4:
                masked = name.endswith('.masked');glyph = '.glyph.' in name
                identity.update(id='test-home4-slice', payload_sha256='', reconstruction_sha256='', dimensions=3,
                                ordinal=0, step=10, time=.2, interpolation='SourceSlice', original_nodes=81 if ".mask." in name else 30,
                                original_plane_only=True, volume_texture_bytes=0, surface_triangles=(20 if masked else 128) if ".mask." in name else 40,
                                glyphs=(28 if masked else 49) if glyph else 0,
                                scalar=('log10_tau_margin' if masked else 'mask_control') if '.mask.' in name else 'ux',
                                invalid_sample_available=not masked, valid_sample_available=True)
            self.expected[name] = identity
            row = {'name': name, 'passed': True, 'frame_matches_source': True, 'dataset': identity['id'],
                   'metadata_sha256': identity['metadata_sha256'], 'payload_sha256': identity['payload_sha256'],
                   'reconstruction_sha256': identity['reconstruction_sha256'], 'spatial_dimensions': identity['dimensions'],
                   'ordinal': identity['ordinal'], 'original_step': identity['step'], 'original_time': identity['time'],
                   'width': 640, 'height': 360, 'colored_pixels': 5000, 'mesh_bytes': 1024,
                   'scalar_texture_bytes': 1024, 'volume_required': not home4 and not wing and name != 'volume.isosurface',
                   'pixel_crc32': f'{i+1:08x}', 'orthographic': home4 or name == 'volume.inside_orthographic'}
            if home4:
                row.update({k: identity[k] for k in ('interpolation', 'original_nodes', 'original_plane_only', 'volume_texture_bytes', 'surface_triangles', 'glyphs', 'scalar', 'invalid_sample_available', 'valid_sample_available')})
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
            self.assertRejected(lambda r: r['cases'][CASE_NAMES.index('snapshot.original_frame')].update(snapshot_metadata=metadata))
        for difference in (None, 2., -1., float('nan'), True):
            self.assertRejected(lambda r: r['cases'][CASE_NAMES.index('snapshot.original_frame')].update(snapshot_mean_rgb_error=difference))

    def test_malformed_reports_fail_closed(self):
        for report in (None, {}, [], {'cases': None, 'checks': []}, {'cases': [None], 'checks': []},
                       {'cases': [{'name': []}], 'checks': []}):
            self.assertFalse(evaluate(report, self.expected)['passed'])

    def test_home4_masks_cannot_be_empty_extruded_or_use_volume(self):
        for name, key, value in [('home4.slice.xy','original_nodes',60), ('home4.slice.xz','interpolation','SourceGrid'),
                                 ('home4.slice.yz','volume_texture_bytes',1024), ('home4.slice.xy','original_plane_only',False),
                                 ('home4.mask.surface.masked','surface_triangles',0), ('home4.mask.surface.masked','surface_triangles',40),
                                 ('home4.mask.glyph.masked','glyphs',30), ('home4.mask.glyph.control','glyphs',0),
                                 ('home4.mask.surface.masked','invalid_sample_available',True)]:
            self.assertRejected(lambda r: r['cases'][CASE_NAMES.index(name)].update({key:value}))

    def test_incomplete_independent_expectations_fail(self):
        expected = dict(self.expected);expected.pop(HOME4_CASE_NAMES[0])
        self.assertFalse(evaluate(self.report, expected)['passed'])


class Home4PixelTests(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[1]
        self.temporary = tempfile.TemporaryDirectory(dir=root/'tmp')
        self.addCleanup(self.temporary.cleanup);self.directory = Path(self.temporary.name)
        self.report = {'cases': []};self.data = {}
        for name in HOME4_CASE_NAMES:
            data = bytearray(bytes([0,0,0,255])*(640*360))
            # Independent fixed orthographic fixture camera: X points screen left,
            # source Y (scene Z) points screen up. Original center (.3,.4,.55).
            for y in range(100,260):
                for x in range(250,621):
                    source_x = .75-(x-320)/640
                    if not name.endswith('.masked') or source_x >= .75:
                        i=4*(y*640+x);data[i:i+4]=bytes([255,0,255,255])
            row={'name':name,'camera_focus':[.75,.5,.875],'camera_right':[-1.,0.,0.],
                 'camera_up':[0.,0.,1.], 'ortho_width':1.}
            self.report['cases'].append(row);self.write(name,data)

    def write(self,name,data):
        self.data[name]=data;(self.directory/(name+'.bgra')).write_bytes(data)
        row=next(r for r in self.report['cases'] if r['name']==name)
        row.update(pixel_crc32=f'{zlib.crc32(data):08x}',colored_pixels=sum(max(p[:3])>12 and max(p[:3])-min(p[:3])>4 for p in zip(*[iter(data)]*4)))

    def test_nonempty_masked_and_control_regions_pass(self):
        self.assertEqual(evaluate_home4_pixels(self.report,self.directory),[])

    def test_renderer_success_cannot_hide_missing_or_corrupt_readback(self):
        path=self.directory/(HOME4_CASE_NAMES[0]+'.bgra');path.write_bytes(b'bad')
        self.assertTrue(evaluate_home4_pixels(self.report,self.directory))

    def test_mask_that_draws_rejected_region_fails_even_with_correct_checksum(self):
        self.write('home4.mask.surface.masked',self.data['home4.mask.surface.control'])
        self.assertTrue(evaluate_home4_pixels(self.report,self.directory))

    def test_empty_valid_region_or_control_fails_even_with_correct_checksum(self):
        self.write('home4.mask.glyph.control',bytes([0,0,0,255])*(640*360))
        self.assertTrue(evaluate_home4_pixels(self.report,self.directory))

    def test_changed_pair_camera_fails(self):
        self.report['cases'][-1]['ortho_width']=1.1
        self.assertTrue(evaluate_home4_pixels(self.report,self.directory))

    def test_embedded_original_headers_define_distinct_source_planes(self):
        root=Path(__file__).resolve().parents[1]
        for key in ('xy','xz','yz','mask'):
            original=embedded_archive(root,key)
            shape,_=npy_member(original,'phi');_,spec=npy_member(original,'run_spec')
            self.assertEqual(shape,(9,9) if key=="mask" else (5,6));self.assertEqual(json.loads(spec)['archive']['axisOrder'],'xy' if key=='mask' else key)
        _,tau=npy_member(embedded_archive(root,'mask'),'tau_fld')
        self.assertEqual(sum(t>.5 for t in tau),45);self.assertEqual(sum(t<=.5 for t in tau),36)

class Home4ReceiptTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(__file__).resolve().parents[1]
        temporary = tempfile.TemporaryDirectory(dir=self.root/'tmp');self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        for key in ('xy','xz','yz','mask'):
            original=embedded_archive(self.root,key);folder=self.directory/'home4-fixtures'/key;folder.mkdir(parents=True)
            (folder.parent/(key+'.npz')).write_bytes(original)
            shape,_=npy_member(original,'phi');_,spec=npy_member(original,'run_spec');spec=json.loads(spec)
            axes=spec['archive']['axisOrder'];dims=[shape[axes.index(a)] if a in axes else 1 for a in 'xyz']
            _,origin=npy_member(original,'origin');_,spacing=npy_member(original,'spacing');_,step=npy_member(original,'iteration')
            def array(name,values,shape,kind='d'):
                raw=struct.pack('<'+str(len(values))+kind,*values);(folder/name).write_bytes(raw)
                return {'path':name,'sha256':hashlib.sha256(raw).hexdigest(),'byteLength':len(raw),'shape':shape,
                        'dtype':'float64' if kind=='d' else 'int64','byteOrder':'little','frameCRC32':[zlib.crc32(raw)]}
            coords=[(origin[a]+i*spacing[a])*.1 for z in range(dims[2]) for y in range(dims[1]) for x in range(dims[0]) for a,i in enumerate((x,y,z))]
            fields=[]
            def field(name,values,mask=None):
                row={'id':name,'array':array(name+'.f64',values,[1,len(values)])}
                if mask:row['validityMask']=mask
                fields.append(row)
            if key=='mask':
                _,tau=npy_member(original,'tau_fld');_,control=npy_member(original,'mask_control')
                tau=[tau[x*9+y] for y in range(9) for x in range(9)];control=[control[x*9+y] for y in range(9) for x in range(9)]
                field('tau_fld',tau);field('mask_control',control);field('tau_margin_valid',[float(t>.5) for t in tau])
                field('log10_tau_margin',[__import__('math').log10(t-.5) if t>.5 else 0. for t in tau],'tau_margin_valid')
            else:
                field('ux',[(origin[0]+x*spacing[0]+2*(origin[1]+y*spacing[1])+3*(origin[2]+z*spacing[2]))*.1/.02
                            for z in range(dims[2]) for y in range(dims[1]) for x in range(dims[0])])
            (folder/'provenance.json').write_text('{"fixture":"independent artificial receipt"}')
            sha=hashlib.sha256((folder/'provenance.json').read_bytes()).hexdigest()
            source={'version':3,'id':'HOME4_'+sha[:32],'provenanceSHA256':sha,'spatialDimensions':3,'pointCount':81 if key=='mask' else 30,
                    'frames':[{'index':step[0],'time':step[0]*.02,'label':f'step_{step[0]}'}],
                    'coordinates':array('coordinates.f64',coords,[len(coords)//3,3]),'pointIds':array('point-ids.i64',list(range(len(coords)//3)),[len(coords)//3],'q'),
                    'fields':fields,'structuredGrid':{'kind':'home4_structured_slice','dimensionsXYZ':dims,
                    'originalDimensionsXYZ':dims,'axisOrder':axes,'originalOriginXYZ':origin,'originalSpacingXYZ':spacing,
                    'cropMinimumXYZ':[0,0,0],'cropMaximumXYZ':dims,'previewStride':1,'sourceRunId':spec['runId'],
                    'units':{'dxMeters':.1,'dtSeconds':.02}}}
            (folder/'recording.json').write_text(json.dumps(source))

    def test_receipts_pin_all_seven_independent_expectations(self):
        expected=expected_home4(self.root,self.directory)
        self.assertEqual(set(expected),set(HOME4_CASE_NAMES))
        self.assertEqual(expected['home4.mask.glyph.masked']['glyphs'],45)

    def test_renderer_claimed_identity_cannot_replace_original_header(self):
        path=self.directory/'home4-fixtures/xy/recording.json';source=json.loads(path.read_text())
        source['frames'][0]['time']=1.;path.write_text(json.dumps(source))
        with self.assertRaises(ValueError):expected_home4(self.root,self.directory)

    def test_original_file_swap_is_rejected(self):
        path=self.directory/'home4-fixtures/xy.npz';path.write_bytes(embedded_archive(self.root,'xz'))
        with self.assertRaises(ValueError):expected_home4(self.root,self.directory)

    def test_extruded_coordinates_are_rejected_even_with_updated_array_hash(self):
        folder=self.directory/'home4-fixtures/xy';path=folder/'recording.json';source=json.loads(path.read_text())
        data=bytearray((folder/'coordinates.f64').read_bytes());data[16:24]=struct.pack('<d',.41)
        (folder/'coordinates.f64').write_bytes(data);source['coordinates']['sha256']=hashlib.sha256(data).hexdigest()
        path.write_text(json.dumps(source))
        with self.assertRaises(ValueError):expected_home4(self.root,self.directory)

    def test_removed_validity_dependency_is_rejected(self):
        path=self.directory/'home4-fixtures/mask/recording.json';source=json.loads(path.read_text())
        next(f for f in source['fields'] if f['id']=='log10_tau_margin')['validityMask']=''
        path.write_text(json.dumps(source))
        with self.assertRaises(ValueError):expected_home4(self.root,self.directory)


if __name__ == '__main__':
    unittest.main()
