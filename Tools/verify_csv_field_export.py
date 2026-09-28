#!/usr/bin/env python3
"""Independently compare CSV field exports with published original binary rows.

Requires NumPy and VTK (for the shared original-binary reader module). Uses
Python's csv reader, exact integer parsing and float64 round trips. Run the
Studio.CSVExport and Studio.FieldSequence model cases first.
"""
import argparse
import csv
import json
from pathlib import Path

import numpy as np

from verify_vtk_export import digest, exact, source_rows


def verify_csv(path, original, scene=False, selected=None):
    with path.open(newline='', encoding='utf-8') as stream:
        first = stream.readline()
        assert first.startswith('# LBMStudioMetadataUTF8 ')
        metadata = json.loads(first.removeprefix('# LBMStudioMetadataUTF8 '))
        reader = csv.reader(stream)
        header = next(reader)
        rows = list(reader)
    descriptor = original['descriptor']
    expected = {
        'format': 'LBMStudio.OriginalFieldCSV', 'version': 1,
        'dataset': descriptor['id'], 'metadata_sha256': original['metadata_sha256'],
        'payload_sha256': original['payload_sha256'], 'view_reconstruction_sha256': '',
        'frame_ordinal': original['ordinal'], 'source_step': original['step'],
        'source_time_seconds': original['time'], 'spatial_dimensions': descriptor['spatialDimensions'],
        'coordinate_unit': 'm', 'coordinate_system': 'scene_xzy_plus_offset' if scene else 'source_xyz',
        'source_to_scene_offset_meters': descriptor.get('sourceOffset', [0, 0, 0]),
        'display_reconstruction_applied': False, 'topology': 'original_points_no_connectivity',
    }
    for key, value in expected.items():
        assert metadata[key] == value, f'{path}: {key} differs'
    assert len(metadata['operations']) == (2 if scene else 1)
    assert 'Original source components' in metadata['scalar_basis']
    selected = selected if selected is not None else list(original['values'])
    assert header == [metadata['point_id_array'], *metadata['coordinate_columns'],
                      *(s['csv_column'] for s in metadata['scalars'])]
    assert len(header) == len(set(header)) == len(selected) + 4
    assert all(len(row) == len(header) for row in rows)
    # Parsing IDs as float would hide lost Int64 precision, so use int directly.
    ids = np.array([int(row[0]) for row in rows], dtype=np.int64)
    exact(ids, original['ids'], 'point IDs')
    data = np.array([row[1:] for row in rows], dtype=np.float64)
    assert np.isfinite(data).all()
    points = original['points']
    if scene:
        points = points[:, [0, 2, 1]] + np.array(expected['source_to_scene_offset_meters'])
    exact(data[:, :3], points, 'point coordinates')
    expected_scalars = {s['id']: s for s in original['scalars']}
    assert {s['id'] for s in metadata['scalars']} == set(selected)
    errors = {}
    for k, scalar in enumerate(metadata['scalars']):
        name = scalar['id']
        assert scalar['csv_column'] == name
        for key in ('id', 'label', 'unit', 'origin'):
            assert scalar[key] == expected_scalars[name][key]
        assert scalar['expression'] == original['expressions'].get(name, '')
        values = original['values'][name]
        if descriptor['version'] == 1 and name == 'velocity_magnitude':
            np.testing.assert_array_max_ulp(data[:, k + 3], values, maxulp=2)
        else:
            exact(data[:, k + 3], values, name)
        errors[name] = float(np.max(np.abs(data[:, k + 3] - values)))
    return {'file': str(path), 'sha256': digest(path), 'bytes': path.stat().st_size,
            'points': len(rows), 'scalar_max_absolute_error': errors, 'metadata': metadata}


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exports', type=Path, default=root / 'Saved/Automation/CSVExport')
    parser.add_argument('--sequences', type=Path, default=root / 'Saved/Automation/FieldSequence')
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    samples = root / 'Content/Samples'
    airfoil = source_rows(samples / 'MeshGraphNets_Airfoil', 420)
    reports = [
        verify_csv(args.exports / 'airfoil-source.csv', airfoil),
        verify_csv(args.exports / 'airfoil-scene.csv', airfoil, scene=True),
        verify_csv(args.exports / 'naca.csv', source_rows(samples / 'NACA0018_ReaderFixture', 2)),
        verify_csv(args.exports / 'cylinder.csv', source_rows(samples / 'Cylinder3D_ReaderFixture', 2)),
    ]
    indices = []
    for source, ordinals, selected, scene in [
        ('MeshGraphNets_Airfoil', range(419, 422), ['pressure', 'density'], True),
        ('NACA0018_ReaderFixture', range(3), ['pressure'], False),
        ('Cylinder3D_ReaderFixture', range(3), ['pressure', 'velocity_w'], False),
    ]:
        folder = args.sequences / (source + '_csv')
        index = folder / 'frames.csv'
        with index.open(newline='', encoding='utf-8') as stream:
            rows = list(csv.DictReader(stream))
        assert len(rows) == len(ordinals)
        files = {'frames.csv'}
        for row, ordinal in zip(rows, ordinals):
            original = source_rows(samples / source, ordinal)
            name = f'frame_{ordinal:06d}.csv'
            assert set(row) == {'frame_ordinal', 'source_step', 'source_time_s', 'file'}
            assert int(row['frame_ordinal']) == ordinal and int(row['source_step']) == original['step']
            assert float(row['source_time_s']) == original['time'] and row['file'] == name
            reports.append(verify_csv(folder / name, original, scene, selected))
            files.add(name)
        assert {p.name for p in folder.iterdir()} == files
        indices.append({'file': str(index), 'sha256': digest(index), 'frames': len(rows)})
    report = {'method': 'Python csv parser and NumPy compare every original binary value and ID; metadata, units, array order, coordinates and frame index/time verified. Only declared SU2 speed allows 2 ULP.',
              'files': reports, 'sequence_indices': indices}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Verified {len(reports)} CSV frames, {sum(r["points"] for r in reports):,} original point rows and {len(indices)} sequence indices.')


if __name__ == '__main__':
    main()
