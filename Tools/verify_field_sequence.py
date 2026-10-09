#!/usr/bin/env python3
"""Audit exported original-frame collections using VTK and original binary data.

Requires NumPy and VTK. Run Studio.FieldSequence model tests first.
The PVD container is parsed with ElementTree; every referenced VTP is read by
VTK. This does not claim a ParaView GUI/PVD-reader acceptance run.
"""
import argparse
import json
from pathlib import Path
import xml.etree.ElementTree as ET

import vtk

from verify_vtk_export import digest, source_rows, verify


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exports', type=Path, default=root / 'Saved/Automation/FieldSequence')
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    reports = []
    for source, ordinals, selected, scene in [
        ('MeshGraphNets_Airfoil', range(419, 422), ['pressure', 'density'], True),
        ('NACA0018_ReaderFixture', range(3), ['pressure'], False),
        ('Cylinder3D_ReaderFixture', range(3), ['pressure', 'velocity_w'], False),
    ]:
        folder = args.exports / source
        collection = folder / 'flow.pvd'
        xml = ET.parse(collection).getroot()
        assert xml.tag == 'VTKFile' and xml.attrib['type'] == 'Collection'
        children = xml.findall('Collection/DataSet')
        assert len(children) == len(ordinals)
        expected_names = {'flow.pvd'}
        files = []
        for node, ordinal in zip(children, ordinals):
            name = f'frame_{ordinal:06d}.vtp'
            original = source_rows(root / 'Content/Samples' / source, ordinal)
            assert node.attrib == {'timestep': node.attrib['timestep'], 'group': '', 'part': '0', 'file': name}
            assert float(node.attrib['timestep']) == original['time']
            expected_names.add(name)
            files.append(verify(folder / name, original, scene=scene, selected=selected))
        assert {p.name for p in folder.iterdir()} == expected_names
        reports.append({'source': source, 'collection': str(collection),
                        'collection_sha256': digest(collection), 'files': files})
    report = {'vtk_version': vtk.vtkVersion.GetVTKVersion(),
              'method': 'Independent ElementTree PVD and VTK VTP readers; every original point, scalar, ID, cell and timestamp checked against original binary arrays.',
              'paraview_pvd_reader_tested': False, 'sequences': reports}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    files = [f for s in reports for f in s['files']]
    print(f'Verified {len(reports)} collections, {len(files)} original frames, '
          f'{sum(f["points"] for f in files):,} point rows; all values exact.')


if __name__ == '__main__':
    main()
