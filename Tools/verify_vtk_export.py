#!/usr/bin/env python3
"""Read Studio.VTKExport fixtures with VTK and compare every original source row.

Requires NumPy and VTK. Run Tools/test.sh first, then pass --report with an
ignored evidence path. No application reader or interpolation code is reused.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

import numpy as np
import vtk
from vtk.util.numpy_support import vtk_to_numpy


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def source_rows(folder, ordinal):
    descriptor_path = folder / 'recording.json'
    descriptor = json.loads(descriptor_path.read_text())
    dimensions = descriptor['spatialDimensions']
    result = {
        'descriptor': descriptor, 'ordinal': ordinal,
        'metadata_sha256': digest(descriptor_path), 'payload_sha256': '',
        'triangles': None, 'expressions': {},
    }
    if descriptor['version'] == 1:
        payload = folder / 'flow.bin'
        result['payload_sha256'] = digest(payload)
        assert result['payload_sha256'] == descriptor['payloadSHA256']
        with payload.open('rb') as stream:
            magic, version, points, triangles, boundary, frames = struct.unpack('<6i', stream.read(24))
            assert magic == 0x53553246 and version == 2 and 0 <= ordinal < frames
            xy = np.fromfile(stream, dtype='<f8', count=points * 2).reshape(points, 2)
            result['triangles'] = np.fromfile(stream, dtype='<i4', count=triangles * 3).reshape(triangles, 3)
            stream.seek(24 + 16 * points + 12 * triangles + 4 * boundary + ordinal * (12 + 16 * points))
            result['step'], result['time'] = struct.unpack('<id', stream.read(12))
            values = np.fromfile(stream, dtype='<f4', count=points * 4).reshape(points, 4).astype(np.float64)
        result['points'] = np.column_stack((xy, np.zeros(points)))
        result['ids'] = np.arange(points, dtype=np.int64)
        result['values'] = dict(zip(('velocity_x', 'velocity_y', 'pressure', 'density'), values.T))
        result['values']['velocity_magnitude'] = np.sqrt(values[:, 0] ** 2 + values[:, 1] ** 2)
        result['expressions']['velocity_magnitude'] = 'sqrt(velocity_x^2 + velocity_y^2)'
        result['scalars'] = descriptor['scalars']
    else:
        assert descriptor['version'] == 3
        coordinates = descriptor['coordinates']
        assert digest(folder / coordinates['path']) == coordinates['sha256']
        xyz = np.fromfile(folder / coordinates['path'], dtype='<f8').reshape(coordinates['shape'])
        result['points'] = xyz if dimensions == 3 else np.column_stack((xyz, np.zeros(len(xyz))))
        ids = descriptor['pointIds']
        assert digest(folder / ids['path']) == ids['sha256']
        result['ids'] = np.fromfile(folder / ids['path'], dtype='<i8')
        result['step'] = descriptor['frames'][ordinal]['index']
        result['time'] = descriptor['frames'][ordinal]['time']
        result['scalars'] = descriptor['fields']
        result['values'] = {}
        for scalar in result['scalars']:
            array = scalar['array']
            assert array['dtype'] == 'float64' and array['byteOrder'] == 'little'
            assert digest(folder / array['path']) == array['sha256']
            values = np.fromfile(folder / array['path'], dtype='<f8').reshape(array['shape'])
            result['values'][scalar['id']] = values if scalar['static'] else values[ordinal]
            result['expressions'][scalar['id']] = scalar.get('expression', '')
    return result


def exact(actual, expected, label):
    assert actual.shape == expected.shape, f'{label}: shape differs'
    assert np.array_equal(actual, expected), f'{label}: original values differ'


def verify(path, original, scene=False, reconstruction_hash='', selected=None):
    messages = []
    reader = vtk.vtkXMLPolyDataReader()
    reader.AddObserver('ErrorEvent', lambda *_: messages.append('ErrorEvent'))
    reader.AddObserver('WarningEvent', lambda *_: messages.append('WarningEvent'))
    assert reader.CanReadFile(str(path)), f'VTK cannot read {path}'
    reader.SetFileName(str(path))
    reader.Update()
    assert not messages and reader.GetErrorCode() == 0, f'{path}: {messages}'
    data = reader.GetOutput()
    field_data = data.GetFieldData()
    metadata_array = field_data.GetArray('LBMStudioMetadataUTF8')
    assert metadata_array is not None and metadata_array.GetDataType() == vtk.VTK_UNSIGNED_CHAR
    metadata = json.loads(vtk_to_numpy(metadata_array).tobytes().decode('utf-8'))
    descriptor = original['descriptor']
    expected_metadata = {
        'format': 'LBMStudio.OriginalField', 'version': 1,
        'dataset': descriptor['id'], 'metadata_sha256': original['metadata_sha256'],
        'payload_sha256': original['payload_sha256'],
        'view_reconstruction_sha256': reconstruction_hash,
        'frame_ordinal': original['ordinal'], 'source_step': original['step'],
        'source_time_seconds': original['time'],
        'spatial_dimensions': descriptor['spatialDimensions'], 'coordinate_unit': 'm',
        'coordinate_system': 'scene_xzy_plus_offset' if scene else 'source_xyz',
        'source_to_scene_offset_meters': descriptor.get('sourceOffset', [0, 0, 0]),
        'display_reconstruction_applied': False,
        'topology': 'original_source_triangles' if original['triangles'] is not None else 'original_points_as_vertex_cells',
    }
    for key, expected in expected_metadata.items():
        assert metadata[key] == expected, f'{path.name}: {key} differs'
    assert len(metadata['operations']) == (2 if scene else 1)
    assert 'Original source components' in metadata['scalar_basis']
    exact(vtk_to_numpy(field_data.GetArray('TimeValue')), np.array([original['time']]), 'source time')
    expected_points = original['points']
    if scene:
        expected_points = expected_points[:, [0, 2, 1]] + np.array(expected_metadata['source_to_scene_offset_meters'])
    points = vtk_to_numpy(data.GetPoints().GetData())
    assert points.dtype == np.dtype('float64')
    exact(points, expected_points, 'coordinates')
    ids = vtk_to_numpy(data.GetPointData().GetArray(metadata['point_id_array']))
    assert ids.dtype == np.dtype('int64')
    exact(ids, original['ids'], 'original point IDs')
    fields = selected if selected is not None else list(original['values'])
    scalars = {s['id']: s for s in original['scalars']}
    exported = {s['id']: s for s in metadata['scalars']}
    assert set(exported) == set(fields)
    assert data.GetPointData().GetNumberOfArrays() == len(fields) + 1
    assert data.GetPointData().GetScalars().GetName() == metadata['scalars'][0]['id']
    errors = {}
    for name in fields:
        actual = vtk_to_numpy(data.GetPointData().GetArray(name))
        expected = original['values'][name]
        assert actual.dtype == np.dtype('float64') and np.isfinite(actual).all()
        if descriptor['version'] == 1 and name == 'velocity_magnitude':
            # The declared derived norm can differ by ARM fused arithmetic.
            # Supplied source scalars below always require exact equality.
            np.testing.assert_array_max_ulp(actual, expected, maxulp=2)
        else:
            exact(actual, expected, name)
        errors[name] = float(np.max(np.abs(actual - expected)))
        for key in ('id', 'label', 'unit', 'origin'):
            assert exported[name][key] == scalars[name][key], f'{name}: {key} differs'
        assert exported[name]['vtk_array'] == name
        assert exported[name]['expression'] == original['expressions'].get(name, '')
    triangles = original['triangles']
    assert data.GetNumberOfLines() == data.GetNumberOfStrips() == 0
    if triangles is not None:
        assert data.GetNumberOfVerts() == 0 and data.GetNumberOfPolys() == len(triangles)
        exact(vtk_to_numpy(data.GetPolys().GetConnectivityArray()).reshape(-1, 3), triangles, 'source triangles')
        exact(vtk_to_numpy(data.GetPolys().GetOffsetsArray()), np.arange(len(triangles) + 1) * 3, 'polygon offsets')
    else:
        assert data.GetNumberOfPolys() == 0 and data.GetNumberOfVerts() == len(points)
        exact(vtk_to_numpy(data.GetVerts().GetConnectivityArray()), np.arange(len(points)), 'vertex cells')
        exact(vtk_to_numpy(data.GetVerts().GetOffsetsArray()), np.arange(len(points) + 1), 'vertex offsets')
    return {
        'file': str(path), 'sha256': digest(path), 'bytes': path.stat().st_size,
        'points': len(points), 'triangles': data.GetNumberOfPolys(), 'vertices': data.GetNumberOfVerts(),
        'scalar_max_absolute_error': errors, 'metadata': metadata, 'vtk_reader_messages': messages,
    }


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exports', type=Path, default=root / 'Saved/Automation/VTKExport')
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    samples = root / 'Content/Samples'
    airfoil = source_rows(samples / 'MeshGraphNets_Airfoil', 420)
    naca = source_rows(samples / 'NACA0018_ReaderFixture', 2)
    cylinder = source_rows(samples / 'Cylinder3D_ReaderFixture', 2)
    reports = [
        verify(args.exports / 'airfoil-source.vtp', airfoil),
        verify(args.exports / 'airfoil-scene.vtp', airfoil, scene=True),
        verify(args.exports / 'naca-points.vtp', naca),
        verify(args.exports / 'cylinder-points.vtp', cylinder),
        verify(args.exports / 'cylinder-from-view.vtp', cylinder, selected=['pressure'],
               reconstruction_hash=digest(samples / 'Cylinder3D_VolumeFixture/reconstruction.json')),
    ]
    report = {
        'vtk_version': vtk.vtkVersion.GetVTKVersion(),
        'method': 'Independent VTK reader; NumPy reads source payloads directly. Every coordinate, ID, source scalar and cell matches exactly. Only explicitly derived SU2 speed allows 2 ULP.',
        'files': reports,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f'VTK {report["vtk_version"]}: verified {len(reports)} files, {sum(r["points"] for r in reports):,} original point rows; no reader errors or warnings.')


if __name__ == '__main__':
    main()
