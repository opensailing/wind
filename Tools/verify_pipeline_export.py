#!/usr/bin/env python3
"""Read pipeline VTK/CSV in independent readers and compare retained output.

Checks every serialized coordinate/value/cell/ID against direct evaluation
buffers, and source rows/interpolated values against published CFD arrays.
Requires NumPy, VTK and Matplotlib; run Studio.PipelineExport first.
"""
import argparse
import csv
import json
from pathlib import Path

import numpy as np
import vtk
from vtk.util.numpy_support import vtk_to_numpy

from verify_pipeline_fields import digest
from verify_pipeline_geometry import Original
from verify_vtk_export import source_rows

CASES = ('surface', 'magnitude', 'lines', 'points', 'empty', 'translated',
         'probe', 'probe-off-plane', 'volume-slice', 'volume-contour')
STATUSES = ('value', 'outside_coverage', 'off_source_plane', 'point_unavailable',
            'field_unavailable', 'no_interpolation')


def exact(a, b):
    assert np.array_equal(a, b), (np.shape(a), np.shape(b))


def verify(samples, exported):
    originals = {False: Original(samples, False), True: Original(samples, True)}
    sources = {name: source_rows(samples / name, 1) for name in
               ('NACA0018_ReaderFixture', 'Cylinder3D_ReaderFixture', 'MeshGraphNets_Airfoil')}
    results, scientific = [], []
    for name in CASES:
        truth = json.loads((exported / f'{name}.json').read_text())
        vertices = np.fromfile(exported / f'{name}-vertices.f64', '<f8').reshape(-1, 4)
        identity = np.fromfile(exported / f'{name}-identity.i64', '<i8').reshape(-1, 2)
        triangles = np.fromfile(exported / f'{name}-triangles.i32', '<i4').reshape(-1, 3)
        lines = np.fromfile(exported / f'{name}-lines.i32', '<i4').reshape(-1, 2)
        recipe = truth['recipes'][0]
        source = recipe['source']
        field = truth['field']
        volume = name.startswith('volume')
        original = sources['MeshGraphNets_Airfoil' if name == 'translated' else
                           'Cylinder3D_ReaderFixture' if volume else 'NACA0018_ReaderFixture']
        assert source['metadataSHA256'] == original['metadata_sha256']
        assert source['payloadSHA256'] == original['payload_sha256']
        assert source['ordinal'] == 1 and source['timeSeconds'] == original['time']
        offset = np.array(source['sourceOffsetMeters'])
        valid_ids = identity[:, 0] != -1
        rows = identity[valid_ids, 0]
        exact(identity[valid_ids, 1], original['ids'][rows])
        exact(vertices[valid_ids, :3], original['points'][rows][:, [0, 2, 1]] + offset)
        if field == 'pressure':
            exact(vertices[valid_ids, 3], original['values'][field][rows])
        else:
            expected = np.hypot(original['values']['velocity_u'][rows], original['values']['velocity_v'][rows])
            np.testing.assert_allclose(vertices[valid_ids, 3], expected, rtol=2e-15, atol=1e-14)
        # Contour output is the requested level by definition. Other derived
        # vertices and available probe samples independently query source support.
        error = 0.
        if name not in ('lines', 'volume-contour', 'translated'):
            points, values = vertices[~valid_ids, :3], vertices[~valid_ids, 3]
            if truth['probe']:
                available = [r for r in truth['probe'] if 'value' in r]
                points = np.array([r['position'] for r in available]).reshape(-1, 3)
                values = np.array([r['value'] for r in available])
            for start in range(0, len(points), 2048):
                expected = originals[volume].sample(points[start:start+2048], field)
                error = max(error, float(np.max(np.abs(expected-values[start:start+2048]))))
                np.testing.assert_allclose(values[start:start+2048], expected, rtol=1e-8, atol=1e-10)
        if name in ('lines', 'volume-contour'):
            level = next(op['value'] for op in recipe['operations'] if op['enabled'] and op['kind'] == 4)
            assert np.all(vertices[:, 3] == level) and not valid_ids.any()
        if name == 'probe-off-plane':
            assert all(r['status'] == 2 and 'value' not in r for r in truth['probe'])
        if name == 'probe':
            assert any('value' in r for r in truth['probe']) and any('value' not in r for r in truth['probe'])
        scientific.append({'case': name, 'original_rows_exact': len(rows), 'independent_sample_max_error': error})
        for scene in (False, True):
            suffix = 'scene' if scene else 'source'
            expected_points = vertices[:, :3] if scene else (vertices[:, :3]-offset)[:, [0, 2, 1]]
            if not scene:
                expected_points[valid_ids] = original['points'][rows]
            for ext in (('csv',) if truth['probe'] else ('csv', 'vtp')):
                path = exported / f'{name}-{suffix}.{ext}'
                if ext == 'vtp':
                    events = []
                    reader = vtk.vtkXMLPolyDataReader()
                    reader.AddObserver('WarningEvent', lambda *_: events.append('warning'))
                    reader.AddObserver('ErrorEvent', lambda *_: events.append('error'))
                    assert reader.CanReadFile(str(path))
                    reader.SetFileName(str(path)); reader.Update()
                    assert not events and reader.GetErrorCode() == 0, (path, events)
                    data = reader.GetOutput()
                    meta = json.loads(vtk_to_numpy(data.GetFieldData().GetArray('LBMStudioMetadataUTF8')).tobytes())
                    exact(vtk_to_numpy(data.GetFieldData().GetArray('TimeValue')), [source['timeSeconds']])
                    assert data.GetNumberOfPoints() == len(vertices)
                    assert data.GetNumberOfPolys() == len(triangles) and data.GetNumberOfLines() == len(lines)
                    assert data.GetNumberOfVerts() == (len(vertices) if truth['kind'] == 0 else 0)
                    if len(vertices):
                        points = vtk_to_numpy(data.GetPoints().GetData())
                        assert points.dtype == np.dtype('float64'); exact(points, expected_points)
                        scalars = vtk_to_numpy(data.GetPointData().GetArray(field))
                        assert scalars.dtype == np.dtype('float64'); exact(scalars, vertices[:, 3])
                        assert data.GetPointData().GetScalars().GetName() == field
                        columns = meta['columns']
                        exact(vtk_to_numpy(data.GetPointData().GetArray(columns['original_row'])), identity[:, 0])
                        exact(vtk_to_numpy(data.GetPointData().GetArray(columns['point_id'])), np.where(valid_ids, identity[:, 1], 0))
                        exact(vtk_to_numpy(data.GetPointData().GetArray(columns['original_valid'])), valid_ids.astype('u1'))
                    for cells, expected, width in ((data.GetPolys(), triangles, 3), (data.GetLines(), lines, 2)):
                        exact(vtk_to_numpy(cells.GetConnectivityArray()), expected.ravel())
                        exact(vtk_to_numpy(cells.GetOffsetsArray()), np.arange(len(expected)+1)*width)
                    if truth['kind'] == 0:
                        exact(vtk_to_numpy(data.GetVerts().GetConnectivityArray()), np.arange(len(vertices)))
                        exact(vtk_to_numpy(data.GetVerts().GetOffsetsArray()), np.arange(len(vertices)+1))
                else:
                    with path.open(newline='') as stream:
                        line = stream.readline(); assert line.startswith('# LBMStudioMetadataUTF8 ')
                        meta = json.loads(line[len('# LBMStudioMetadataUTF8 '):])
                        records = list(csv.DictReader(stream))
                    columns = meta['columns']
                    if truth['probe']:
                        assert len(records) == len(truth['probe'])
                        for row, ref in zip(records, truth['probe']):
                            assert row[columns['status']] == STATUSES[ref['status']]
                            assert row[columns['point_id']] == ref.get('point_id', '')
                            if 'value' in ref: assert float(row[field]) == ref['value']
                            else: assert row[field] == ''
                            assert float(row[columns['distance']]) == ref['distance']
                            if 'position' in ref:
                                p = np.array(ref['position'])
                                if not scene: p = (p-offset)[[0, 2, 1]]
                                exact([float(row[columns[a]]) for a in 'xyz'], p)
                            else: assert all(row[columns[a]] == '' for a in 'xyz')
                    else:
                        assert len(records) == len(vertices)
                        for i, row in enumerate(records):
                            assert int(row[columns['row']]) == i
                            assert row[columns['original_row']] == (str(identity[i, 0]) if valid_ids[i] else '')
                            assert row[columns['point_id']] == (str(identity[i, 1]) if valid_ids[i] else '')
                            exact([float(row[columns[a]]) for a in 'xyz'], expected_points[i])
                            assert float(row[field]) == vertices[i, 3]
                assert meta['recipes'] == truth['recipes'] and meta['method'] == truth['method']
                assert meta['scalar_id'] == field and meta['scalar_unit'] == ('Pa' if field == 'pressure' else 'm/s')
                assert meta['metadata_sha256'] == source['metadataSHA256'] and meta['payload_sha256'] == source['payloadSHA256']
                assert meta['reconstruction_sha256'] == source['reconstructionSHA256']
                assert meta['frame_ordinal'] == source['ordinal'] and meta['source_step'] == source['step'] and meta['source_time_seconds'] == source['timeSeconds']
                assert meta['coordinate_system'] == ('scene_xzy_plus_offset' if scene else 'source_xyz')
                assert meta['topology'] == ('tabular_rows_no_connectivity' if ext == 'csv' else 'evaluated_polydata_connectivity')
                if field != 'pressure': assert meta['scalar_origin'] == 'pipeline-derived' and 'magnitude' in meta['scalar_expression']
                results.append({'file': path.name, 'sha256': digest(path), 'vertices': len(vertices), 'probe_rows': len(truth['probe']),
                                'triangles': len(triangles) if ext == 'vtp' else 0, 'segments': len(lines) if ext == 'vtp' else 0})
    return {'vtk_version': vtk.vtkVersion.GetVTKVersion(), 'files': results, 'scientific_checks': scientific,
            'verifier_sha256': digest(__file__), 'serialized_numbers_exact': True,
            'scope': 'Pipeline file encoding and independent scientific readback; no visible UI, native picker or long-session acceptance.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('samples', type=Path)
    parser.add_argument('exported', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = verify(args.samples, args.exported)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({'files': len(report['files']), 'vtk_version': report['vtk_version'], 'serialized_numbers_exact': True}, indent=2))
