#!/usr/bin/env python3
"""Audit native image sequences against direct snapshots and original CFD arrays.

Requires Pillow, NumPy and VTK (the shared original-data reader imports VTK).
This is image/frame/probe/metadata acceptance, not a long-session or UI gate.
"""
import argparse
import csv
import io
import json
from pathlib import Path

import numpy as np
from PIL import Image
from verify_vtk_export import digest, source_rows


def read_png(path):
    with Image.open(path) as image:
        image.load()
        return np.array(image.convert('RGBA')), json.loads(image.info['LBMStudio'])


def same_view_value(key, actual, expected):
    if key == 'camera':
        a, b = dict(actual), dict(expected)
        qa, qb = np.array(a.pop('orientation_xyzw')), np.array(b.pop('orientation_xyzw'))
        # UE component restoration rounds orientation; preserve actual metadata.
        assert np.isfinite(qa).all() and np.isfinite(qb).all()
        assert min(np.max(np.abs(qa - qb)), np.max(np.abs(qa + qb))) <= 1e-9
        assert a == b
    else:
        assert actual == expected, key


def verify(root, exports, expect_movie=False):
    reports = []
    sources = [('su2', 'MeshGraphNets_Airfoil', None),
               ('wing', 'NACA0018_ReaderFixture', 'NACA0018_SurfaceFixture'),
               ('volume', 'Cylinder3D_ReaderFixture', 'Cylinder3D_VolumeFixture')]
    for name, source_name, reconstruction in sources:
        directory = exports / name
        output = directory / 'export'
        manifest = json.loads((output / 'sequence.json').read_text())
        entries = [json.loads(line) for line in (output / 'frames.jsonl').read_text().splitlines()]
        anchor = json.loads((directory / 'anchor.json').read_text())
        assert manifest['format'] == 'LBMStudio.ImageSequence'
        assert manifest['version'] == 1 and manifest['image_count'] == 3
        expected = [0, 300, 600] if name == 'su2' else [0, 1, 2]
        assert [entry['ordinal'] for entry in entries] == expected
        assert manifest['first_ordinal'] == 0 and manifest['last_ordinal_inclusive'] == expected[-1]
        assert manifest['stride'] == expected[1]
        files = {'sequence.json', 'frames.jsonl'}
        assert ('movie' in manifest) == expect_movie
        if expect_movie:
            assert manifest['movie']['file'] == 'flow.mp4'
            files.add('flow.mp4')
        for entry in entries:
            ordinal = entry['ordinal']
            original = source_rows(root / 'Content/Samples' / source_name, ordinal)
            filename = f'frame_{ordinal:06d}.png'
            assert entry['file'] == filename
            files.add(filename)
            png = output / filename
            assert entry['png_bytes'] == png.stat().st_size
            pixels, meta = read_png(png)
            reference_path = directory / f'reference_{ordinal:06d}.png'
            reference, reference_meta = read_png(reference_path)
            assert pixels.shape == reference.shape
            error = np.abs(pixels[..., :3].astype(np.int16) - reference[..., :3]).mean()
            assert error < 2, (name, ordinal, error)
            assert np.all(pixels[..., 3] == 255)
            # Exclude edge legends/labels so a mostly empty view cannot pass.
            height, width = pixels.shape[:2]
            center = pixels[height // 4:3 * height // 4, width // 4:3 * width // 4, :3].astype(np.int16)
            colored = int(((center.max(axis=2) > 55) & (center.max(axis=2) - center.min(axis=2) > 30)).sum())
            assert colored > width * height // 100, (name, ordinal, 'insufficient central flow coverage', colored)
            assert meta['ordinal'] == ordinal and meta['frame'] == original['step'] and meta['time_seconds'] == original['time']
            assert entry['source_step'] == original['step'] and entry['source_time_seconds'] == original['time']
            for key in ['metadata_sha256', 'payload_sha256']:
                assert meta[key] == original[key] == manifest[key]
            assert meta['dataset'] == original['descriptor']['id'] == manifest['dataset']
            reconstruction_hash = digest(root / 'Content/Samples' / reconstruction / 'reconstruction.json') if reconstruction else ''
            assert meta['reconstruction_sha256'] == reconstruction_hash == manifest['reconstruction_sha256']
            for key, value in manifest['view'].items():
                same_view_value(key, meta[key], value)
                assert value == anchor[key], (name, ordinal, key)
            for key in ['camera', 'projection_matrix_row_major', 'size', 'crop_minimum', 'crop_span', 'scalar', 'inspection_objects']:
                same_view_value(key, meta[key], reference_meta[key])
            samples = list(csv.DictReader(io.StringIO(meta['probe_samples_csv'])))
            if name != 'su2':
                assert len(samples) == 1
                sample = samples[0]
                assert sample['status'] == 'value' and sample['scalar_id'] == 'pressure'
                assert int(sample['frame_ordinal']) == ordinal and int(sample['frame_index']) == original['step']
                assert float(sample['time_s']) == original['time']
                assert int(sample['original_point_id']) == int(original['ids'][17])
                assert float(sample['value']) == original['values']['pressure'][17]
                assert sample['presentation_id'] == meta['capture']
                point = original['points'][17]
                assert [float(sample[f'source_{axis}_m']) for axis in 'xyz'] == point.tolist()
                marker = next(m for m in meta['resolved_overlay']['markers'] if m['object'] == meta['selected_object'])
                assert marker['position'] == point[[0, 2, 1]].tolist()
            else:
                assert not samples
            assert meta['vector_display']['glyph_count'] > 0
            if name == 'volume':
                assert meta['volume_renderer_active'] and meta['camera']['orthographic']
            reports.append({'case': name, 'ordinal': ordinal, 'source_step': original['step'], 'source_time_seconds': original['time'],
                            'png': str(png), 'sha256': digest(png), 'reference_sha256': digest(reference_path),
                            'mean_rgb_difference': float(error), 'central_colored_pixels': colored, 'probe_rows': len(samples), 'pixels': int(pixels.shape[0] * pixels.shape[1])})
        assert {path.name for path in output.iterdir()} == files
        assert not (directory / 'cancelled').exists() and not (directory / 'second').exists()
    return {'passed': True, 'images': reports, 'count': len(reports), 'pixels': sum(r['pixels'] for r in reports),
            'scope': 'Actual original CFD image identity, fixed view, direct-view pixel comparison and original probe values. No full-duration, physical UI or long-session claim.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exports', type=Path, required=True)
    parser.add_argument('--expect-movie', action='store_true')
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    report = verify(root, args.exports, args.expect_movie)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'count': report['count'], 'pixels': report['pixels']}))


if __name__ == '__main__':
    main()
