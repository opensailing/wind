#!/usr/bin/env python3
"""Audit original files produced by the packaged Studio.FieldExportUI workflow.

Uses Python CSV/XML parsers, VTK and NumPy against the published binary payloads.
Pass the runner's captures directory; this does not launch or modify the app.
"""
import argparse
import csv
import json
from pathlib import Path
import xml.etree.ElementTree as ET

import vtk

from verify_csv_field_export import verify_csv
from verify_vtk_export import digest, source_rows, verify


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exports', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    samples = Path(__file__).resolve().parents[1] / 'Content/Samples'
    airfoil = source_rows(samples / 'MeshGraphNets_Airfoil', 420)
    cylinder = source_rows(samples / 'Cylinder3D_ReaderFixture', 2)
    folder = args.exports
    reports = [
        verify(folder / 'selected-scene.vtp', airfoil, scene=True,
               selected=['pressure', 'density']),
        verify(folder / 'cylinder-points.vtp', cylinder),
        verify_csv(folder / 'cylinder-current.csv', cylinder),
    ]
    csv_folder = folder / 'csv-range'
    csv_index = csv_folder / 'frames.csv'
    with csv_index.open(newline='', encoding='utf-8') as stream:
        rows = list(csv.DictReader(stream))
    assert len(rows) == 2
    expected_files = {'frames.csv'}
    for row, ordinal in zip(rows, (1, 2)):
        original = source_rows(samples / 'Cylinder3D_ReaderFixture', ordinal)
        name = f'frame_{ordinal:06d}.csv'
        assert set(row) == {'frame_ordinal', 'source_step', 'source_time_s', 'file'}
        assert int(row['frame_ordinal']) == ordinal
        assert int(row['source_step']) == original['step']
        assert float(row['source_time_s']) == original['time']
        assert row['file'] == name
        reports.append(verify_csv(csv_folder / name, original, selected=['pressure']))
        expected_files.add(name)
    assert {p.name for p in csv_folder.iterdir()} == expected_files

    vtk_folder = folder / 'vtk-all'
    vtk_index = vtk_folder / 'flow.pvd'
    document = ET.parse(vtk_index).getroot()
    assert document.tag == 'VTKFile' and document.attrib['type'] == 'Collection'
    datasets = document.findall('./Collection/DataSet')
    assert len(datasets) == 3
    expected_files = {'flow.pvd'}
    for ordinal, dataset in enumerate(datasets):
        original = source_rows(samples / 'Cylinder3D_ReaderFixture', ordinal)
        name = f'frame_{ordinal:06d}.vtp'
        assert dataset.attrib['file'] == name
        assert float(dataset.attrib['timestep']) == original['time']
        assert dataset.attrib['part'] == '0' and dataset.attrib['group'] == ''
        reports.append(verify(vtk_folder / name, original, selected=['pressure']))
        expected_files.add(name)
    assert {p.name for p in vtk_folder.iterdir()} == expected_files
    report = {
        'vtk_version': vtk.vtkVersion.GetVTKVersion(),
        'method': 'Independent CSV/VTK readers and NumPy read every original binary coordinate, ID, selected scalar and supplied cell; XML/CSV index timestamps and relative filenames verified.',
        'boundary': 'PVD XML and its VTP files are verified separately; this does not exercise ParaView PVD reader playback.',
        'files': reports,
        'indices': [{'file': str(p), 'sha256': digest(p)} for p in (csv_index, vtk_index)],
        'original_point_rows': sum(r['points'] for r in reports),
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Verified {len(reports)} field files, {report["original_point_rows"]:,} original point rows and both sequence indices.')


if __name__ == '__main__':
    main()
