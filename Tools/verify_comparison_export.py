#!/usr/bin/env python3
"""Read comparison exports with VTK/CSV and check all published original rows.

Requires NumPy and VTK. Run Studio.ComparisonExport model cases first.
Checks VTM through VTK's multiblock reader, independently recomputes alignment,
and verifies both field files against the authors' retained binary arrays.
"""
import argparse
import json
from pathlib import Path

import numpy as np
import vtk
from vtk.util.numpy_support import vtk_to_numpy

from verify_csv_field_export import verify_csv
from verify_vtk_export import digest, exact, source_rows, verify


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exports', type=Path, default=root / 'Saved/Automation/ComparisonExport')
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    reports = []
    cases = [
        ('MeshGraphNets_Airfoil', 420, 'MeshGraphNets_Airfoil_test010', 420),
        ('MeshGraphNets_Airfoil', 420, 'NACA0018_ReaderFixture', 0),
        ('Cylinder3D_ReaderFixture', 1, 'Cylinder3D_ReaderFixture', 1),
        ('NACA0018_ReaderFixture', 0, 'Cylinder3D_ReaderFixture', 0),
    ]
    for kind, (a, ai, b, bi) in enumerate(cases):
        originals = [source_rows(root / 'Content/Samples' / name, ordinal) for name, ordinal in [(a, ai), (b, bi)]]
        for csv in (False, True):
            for scene in (False, True):
                folder = args.exports / f'pair{kind}-{"csv" if csv else "vtk"}-{"scene" if scene else "source"}'
                meta_path = folder / 'comparison.json'
                meta = json.loads(meta_path.read_text())
                assert meta['format'] == 'LBMStudio.OriginalComparison' and meta['version'] == 1
                assert meta['name'] == 'Wing "A/B" · α'
                assert meta['scalar_id'] == 'pressure' and meta['scalar_unit'] == 'Pa'
                assert meta['coordinate_system'] == ('scene_xzy_plus_offset' if scene else 'source_xyz')
                assert meta['coordinate_unit'] == 'm' and meta['shared_display_range'] is True
                assert meta['display_reconstruction_applied'] is False
                assert meta['topology'] == ('original_points_no_connectivity' if csv else 'original_source_triangles_or_vertex_cells')
                assert 'no temporal interpolation' in meta['method'] and 'subtraction' in meta['method']
                offset = -2.5025 if kind == 1 else 0
                assert meta['secondary_offset_seconds'] == offset
                assert meta['maximum_mismatch_seconds'] == (.1 if kind == 1 else 0)
                assert meta['alignment'] == ('elapsed_from_each_start' if kind == 3 else 'secondary_manual_offset' if kind == 1 else 'recorded_time')
                assert meta['matching'] == ('nearest_earlier_on_tie' if kind == 1 else 'exact')
                times = [originals[0]['time'], originals[1]['time'] + offset]
                if kind == 3:
                    times = [original['time'] - original['descriptor']['frames'][0]['time'] for original in originals]
                assert meta['secondary_minus_primary_aligned_seconds'] == times[1] - times[0]
                if kind == 1:
                    timeline = [f['time'] + offset for f in originals[1]['descriptor']['frames']]
                    assert timeline[0] <= times[0] <= timeline[-1]
                    matched = min(range(len(timeline)), key=lambda i: (abs(timeline[i] - times[0]), i))
                    assert matched == bi and abs(times[1] - times[0]) <= .1
                files = []
                names = {'comparison.json'}
                for index, side in enumerate(('primary', 'secondary')):
                    original = originals[index]
                    side_meta = meta[side]
                    name = f'{side}.{"csv" if csv else "vtp"}'
                    assert side_meta['file'] == name
                    assert side_meta['ordinal'] == original['ordinal'] and side_meta['step'] == original['step']
                    assert side_meta['timeSeconds'] == original['time'] and side_meta['aligned_time_seconds'] == times[index]
                    assert side_meta['dataset'] == original['descriptor']['id']
                    assert side_meta['metadataSHA256'] == original['metadata_sha256']
                    assert side_meta['payloadSHA256'] == original['payload_sha256']
                    assert side_meta['reconstructionSHA256'] == ''
                    assert side_meta['sourceOffsetMeters'] == original['descriptor'].get('sourceOffset', [0, 0, 0])
                    assert side_meta['original_points'] == len(original['points'])
                    assert side_meta['original_triangles'] == (len(original['triangles']) if original['triangles'] is not None else 0)
                    scalar = next(s for s in original['scalars'] if s['id'] == 'pressure')
                    assert [side_meta['source_scalar_minimum'], side_meta['source_scalar_maximum']] == scalar['range']
                    names.add(name)
                    files.append((verify_csv if csv else verify)(folder / name, original, scene=scene, selected=['pressure']))
                reader_messages = []
                if not csv:
                    names.add('comparison.vtm')
                    reader = vtk.vtkXMLMultiBlockDataReader()
                    reader.AddObserver('ErrorEvent', lambda *_: reader_messages.append('error'))
                    reader.AddObserver('WarningEvent', lambda *_: reader_messages.append('warning'))
                    reader.SetFileName(str(folder / 'comparison.vtm'))
                    reader.Update()
                    pair = reader.GetOutput()
                    assert pair.GetNumberOfBlocks() == 2 and not reader_messages
                    for index, original in enumerate(originals):
                        block = pair.GetBlock(index)
                        assert pair.GetMetaData(index).Get(vtk.vtkCompositeDataSet.NAME()) == ('A' if index == 0 else 'B')
                        points = original['points']
                        if scene:
                            points = points[:, [0, 2, 1]] + np.array(original['descriptor'].get('sourceOffset', [0, 0, 0]))
                        exact(vtk_to_numpy(block.GetPoints().GetData()), points, 'multiblock points')
                        exact(vtk_to_numpy(block.GetPointData().GetArray('pressure')), original['values']['pressure'], 'multiblock pressure')
                assert {p.name for p in folder.iterdir()} == names
                reports.append({'folder': str(folder), 'metadata_sha256': digest(meta_path), 'files': files,
                                'vtm_reader_tested': not csv, 'vtm_reader_messages': reader_messages})
    result = {'vtk_version': vtk.vtkVersion.GetVTKVersion(), 'comparisons': reports,
              'method': 'Independent VTK multiblock/PolyData and Python CSV readers; exact original points, IDs, scalars, cells and signed time alignment.'}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2) + '\n')
    files = [f for pair in reports for f in pair['files']]
    print(f'Verified {len(reports)} comparisons, {len(files)} fields, {sum(f["points"] for f in files):,} original point rows; all values exact.')


if __name__ == '__main__':
    main()
