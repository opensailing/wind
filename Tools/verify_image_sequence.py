#!/usr/bin/env python3
"""Independently audit writer-test PNGs and their original CFD metadata.

These images are encoder patterns, not rendered CFD. This audit cannot accept
GPU rendering, image-sequence controls, camera isolation or frame preparation.
Requires Pillow; reads original binary timestamps directly using struct.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from PIL import Image


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify(sequence, source):
    metadata = source / 'recording.json'
    payload = source / 'flow.bin'
    descriptor = json.loads(metadata.read_text())
    meta = json.loads((sequence / 'sequence.json').read_text())
    assert meta['format'] == 'LBMStudio.ImageSequence' and meta['version'] == 1
    assert meta['dataset'] == descriptor['id']
    assert meta['metadata_sha256'] == digest(metadata)
    assert meta['payload_sha256'] == digest(payload)
    assert meta['reconstruction_sha256'] == ''
    assert (meta['first_ordinal'], meta['last_ordinal_inclusive'], meta['stride'], meta['image_count']) == (3, 10, 3, 3)
    assert meta['index'] == 'frames.jsonl'
    assert meta['view']['camera']['position_meters'] == [.125, 2.5, 3.75]
    assert meta['view']['size'] == [64, 64]
    entries = [json.loads(row) for row in (sequence / meta['index']).read_text().splitlines()]
    assert [entry['ordinal'] for entry in entries] == [3, 6, 9]
    expected_names = {'sequence.json', 'frames.jsonl'}
    files = []
    with payload.open('rb') as stream:
        magic, version, points, triangles, boundary, frame_count = struct.unpack('<6i', stream.read(24))
        assert magic == 0x53553246 and version == 2
        start = 24 + 16 * points + 12 * triangles + 4 * boundary
        stride = 12 + 16 * points
        for entry in entries:
            ordinal = entry['ordinal']
            assert ordinal < frame_count
            name = f'frame_{ordinal:06d}.png'
            assert entry['file'] == name
            expected_names.add(name)
            stream.seek(start + ordinal * stride)
            step, seconds = struct.unpack('<id', stream.read(12))
            assert entry['source_step'] == step and entry['source_time_seconds'] == seconds
            path = sequence / name
            assert path.stat().st_size == entry['png_bytes']
            with Image.open(path) as image:
                image.load()
                assert image.size == (64, 64)
                assert image.convert('RGBA').tobytes() == bytes([17 + ordinal, 101, 213, 255]) * 4096
                embedded = json.loads(image.info['LBMStudio'])
            assert embedded['format'] == 'LBMStudio.Snapshot'
            assert embedded['ordinal'] == ordinal and embedded['frame'] == step and embedded['time_seconds'] == seconds
            for key in ['dataset', 'metadata_sha256', 'payload_sha256', 'reconstruction_sha256']:
                assert embedded[key] == meta[key], key
            for key, value in meta['view'].items():
                assert embedded[key] == value, key
            assert embedded['scalar']['id'] == 'pressure' and embedded['scalar']['unit'] == 'Pa'
            files.append({'path': str(path), 'sha256': digest(path), 'ordinal': ordinal, 'source_step': step, 'source_time_seconds': seconds, 'exact_pattern_pixels': 4096})
    assert {path.name for path in sequence.iterdir()} == expected_names
    return {'passed': True, 'scope': 'PNG encoder patterns and authentic source/view metadata only; no GPU-rendered CFD acceptance', 'images': files, 'total_exact_pattern_pixels': 12288,
            'manifest_sha256': digest(sequence / 'sequence.json'), 'index_sha256': digest(sequence / 'frames.jsonl')}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sequence', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    report = verify(args.sequence, args.source)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': True, 'images': len(report['images']), 'scope': report['scope']}))


if __name__ == '__main__':
    main()
