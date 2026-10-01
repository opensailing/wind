#!/usr/bin/env python3
"""Decode native H.264 output with FFmpeg and compare every frame to its PNG.

Requires ffprobe/ffmpeg, Pillow and NumPy. Original CFD provenance is checked
by verify_rendered_image_sequence.py; encoder patterns have no CFD claim.
"""
import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import subprocess

import numpy as np
from PIL import Image


def audit(directory, source_sample=None):
    manifest = json.loads((directory / 'sequence.json').read_text())
    entries = [json.loads(line) for line in (directory / 'frames.jsonl').read_text().splitlines()]
    movie = manifest['movie']
    assert movie['file'] == 'flow.mp4' and movie['codec'] == 'H.264' and movie['container'] == 'MPEG-4'
    assert movie['frame_count'] == manifest['image_count'] == len(entries)
    assert [e['ordinal'] for e in entries] == list(range(manifest['first_ordinal'], manifest['last_ordinal_inclusive'] + 1, manifest['stride']))
    rate = movie['frame_rate']
    assert 1 <= rate <= 60 and movie['duration_seconds'] == len(entries) / rate
    width, height = manifest['view']['size']
    path = directory / movie['file']
    probe = json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-count_frames',
        '-show_streams', '-show_frames', '-show_format', '-of', 'json', str(path)], text=True))
    stream, = probe['streams']
    assert stream['codec_name'] == 'h264' and stream['width'] == width and stream['height'] == height
    assert stream['pix_fmt'] == 'yuv420p'
    assert int(stream['nb_read_frames']) == len(entries)
    assert Fraction(stream['avg_frame_rate']) == rate
    assert abs(float(stream['duration']) - len(entries) / rate) < 1e-5
    assert abs(float(probe['format']['duration']) - len(entries) / rate) < 1e-5
    assert 'Playback time is not simulation time' in probe['format']['tags'].get('comment', '')
    frames = probe['frames']
    assert len(frames) == len(entries)
    rows = []
    if source_sample:
        from verify_vtk_export import source_rows
        source = Path(__file__).resolve().parents[1] / 'Content/Samples' / source_sample
    command = ['ffmpeg', '-v', 'error', '-i', str(path), '-map', '0:v:0', '-fps_mode', 'passthrough',
               '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1']
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as decoder:
        try:
            for i, (entry, timing) in enumerate(zip(entries, frames)):
                assert entry['movie_frame'] == i and entry['movie_time_seconds'] == i / rate
                assert abs(float(timing['best_effort_timestamp_time']) - i / rate) < 1e-5
                data = decoder.stdout.read(width * height * 3)
                assert len(data) == width * height * 3, 'Missing decoded original frame'
                actual = np.frombuffer(data, np.uint8).reshape(height, width, 3)
                with Image.open(directory / entry['file']) as original:
                    expected = np.array(original.convert('RGB'))
                    meta = json.loads(original.info['LBMStudio'])
                assert entry['png_bytes'] == (directory / entry['file']).stat().st_size
                assert expected.shape == actual.shape
                assert meta['ordinal'] == entry['ordinal']
                assert meta['frame'] == entry['source_step'] and meta['time_seconds'] == entry['source_time_seconds']
                assert meta['dataset'] == manifest['dataset']
                for key in ('metadata_sha256', 'payload_sha256', 'reconstruction_sha256'):
                    assert meta[key] == manifest[key]
                if source_sample:
                    original = source_rows(source, entry['ordinal'])
                    assert meta['dataset'] == original['descriptor']['id']
                    assert meta['frame'] == original['step'] and meta['time_seconds'] == original['time']
                    assert meta['scalar']['id'] in original['values']
                    for key in ('metadata_sha256', 'payload_sha256'):
                        assert meta[key] == original[key]
                difference = actual.astype(np.float64) - expected.astype(np.float64)
                mean = float(np.abs(difference).mean())
                mse = float(np.square(difference).mean())
                psnr = float(10 * np.log10(255**2 / mse)) if mse else 100.
                # H.264 is deliberately lossy. These limits catch blank, flipped,
                # channel-swapped or wrong frames, not scientific equivalence.
                assert mean < 8 and psnr > 25, (path, i, mean, psnr)
                quadrants = []
                for y, x in ((0, 0), (0, 1), (1, 0), (1, 1)):
                    region = difference[y*height//2:(y+1)*height//2, x*width//2:(x+1)*width//2]
                    error = float(np.abs(region.mean(axis=(0, 1))).max())
                    assert error < 8, (path, i, 'spatial/channel error', error)
                    quadrants.append(error)
                rows.append({'movie_frame': i, 'original_ordinal': entry['ordinal'],
                             'source_time': entry['source_time_seconds'], 'presentation_time': i / rate,
                             'mean_rgb_error': mean, 'psnr_db': psnr, 'quadrant_channel_mean_errors': quadrants})
            assert not decoder.stdout.read(1), 'Unexpected extra frame'
            errors = decoder.stderr.read().decode()
            assert decoder.wait(timeout=30) == 0 and not errors, errors
        finally:
            if decoder.poll() is None:
                decoder.kill()
                decoder.wait()
    return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'frames': rows, 'count': len(rows), 'rate': rate, 'duration': len(entries) / rate,
            'size': [width, height], 'worst_mean_rgb_error': max(r['mean_rgb_error'] for r in rows),
            'minimum_psnr_db': min(r['psnr_db'] for r in rows)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exports', type=Path, required=True, help='Bundle or parent containing movie sequences')
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--source-sample', help='Also audit every original identity/time against this bundled sample (requires VTK)')
    args = parser.parse_args()
    paths = sorted(args.exports.rglob('sequence.json'))
    bundles = [p.parent for p in paths if 'movie' in json.loads(p.read_text())]
    assert bundles, 'No movie bundles found'
    reports = [audit(p, args.source_sample) for p in bundles]
    result = {'passed': True, 'movies': reports, 'count': len(reports), 'frames': sum(r['count'] for r in reports),
              'scope': 'Native movie container, every decoded frame, spatial/channel fidelity, fixed presentation timeline and lossless-image identity; not lossless video or long-session acceptance.'}
    args.report.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'passed': True, 'movies': result['count'], 'frames': result['frames']}))


if __name__ == '__main__':
    main()
