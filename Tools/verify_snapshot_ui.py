#!/usr/bin/env python3
"""Audit actual Snapshot-menu PNG/sequence exports against original CFD metadata.

Pillow decodes every pixel; NumPy/VTK support the shared independent source reader.
Pixel fidelity against direct views is a separate renderer acceptance suite.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from verify_rendered_image_sequence import read_png, same_view_value
from verify_vtk_export import digest, source_rows


def verify(root, captures):
    works = list(captures.glob('*/range/sequence.json'))
    assert len(works) == 1, 'Expected exactly one completed native UI run'
    work = works[0].parent.parent
    rows = []

    def png(path, source_name, ordinal, anchor, manifest=None):
        pixels, meta = read_png(path)
        assert pixels.shape == (360, 640, 4) and np.all(pixels[..., 3] == 255)
        original = source_rows(root / 'Content/Samples' / source_name, ordinal)
        for key in ('dataset', 'metadata_sha256', 'payload_sha256'):
            expected = original['descriptor']['id'] if key == 'dataset' else original[key]
            assert meta[key] == expected, key
        assert meta['ordinal'] == ordinal and meta['frame'] == original['step'] and meta['time_seconds'] == original['time']
        if manifest:
            for key, value in manifest['view'].items():
                same_view_value(key, meta[key], value)
                same_view_value(key, value, anchor[key])
        else:
            for key in ('project', 'camera', 'projection_matrix_row_major', 'scalar', 'inspection_objects', 'display_settings', 'size', 'crop_minimum', 'crop_span'):
                same_view_value(key, meta[key], anchor[key])
        assert meta['scalar']['id'] in original['values']
        assert meta['annotations'] and meta['legend'] and meta['frame_label']
        center = pixels[90:270, 160:480, :3].astype(np.int16)
        colored = int(((center.max(axis=2) > 55) & (center.max(axis=2) - center.min(axis=2) > 30)).sum())
        assert colored > 2304, (path, colored)
        rows.append({'path': str(path), 'sha256': digest(path), 'dataset': meta['dataset'], 'ordinal': ordinal,
                     'step': original['step'], 'time': original['time'], 'central_colored_pixels': colored})

    png(work / 'single.png', 'MeshGraphNets_Airfoil', 420, json.loads((work / 'single-anchor.json').read_text()))
    for name, source, ordinals, reconstruction in [
        ('range', 'MeshGraphNets_Airfoil', [0, 300, 600], None),
        ('all-volume', 'Cylinder3D_ReaderFixture', [0, 1, 2], 'Cylinder3D_VolumeFixture')]:
        output = work / name
        manifest = json.loads((output / 'sequence.json').read_text())
        entries = [json.loads(line) for line in (output / 'frames.jsonl').read_text().splitlines()]
        anchor = json.loads((work / f'{name}-anchor.json').read_text())
        assert manifest['image_count'] == 3 and [e['ordinal'] for e in entries] == ordinals
        assert manifest['stride'] == ordinals[1] and manifest['last_ordinal_inclusive'] == ordinals[-1]
        assert manifest['reconstruction_sha256'] == (digest(root / 'Content/Samples' / reconstruction / 'reconstruction.json') if reconstruction else '')
        expected = {'sequence.json', 'frames.jsonl'}
        for entry in entries:
            path = output / entry['file'];expected.add(path.name)
            assert path.name == f"frame_{entry['ordinal']:06d}.png"
            assert entry['png_bytes'] == path.stat().st_size
            png(path, source, entry['ordinal'], anchor, manifest)
            assert entry['source_time_seconds'] == rows[-1]['time'] and entry['source_step'] == rows[-1]['step']
        assert {p.name for p in output.iterdir()} == expected
    assert not (work / 'cancelled').exists()
    return {'passed': True, 'images': rows, 'count': len(rows),
            'scope': 'Native UI output, source identity, exact original selection, frozen view and PNG decoding. GPU fidelity is verified separately.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--captures', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    report = verify(Path(__file__).resolve().parents[1], args.captures)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': True, 'images': report['count']}))


if __name__ == '__main__':
    main()
