#!/usr/bin/env python3
"""Independently compare every converted 3-D value with the authors' pandas tables.

No converter helpers are imported. Bit comparisons use the exact float64
widening of each original value; no tolerance, resampling or generated field.
"""
import argparse
import hashlib
import json
from pathlib import Path
import time
import zlib

import numpy as np
import pandas as pd


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify(source, recording, report):
    require(not report.exists(), 'Choose a new verification report path')
    root = recording.resolve().parent

    def member(name):
        path = root / name
        require(Path(name).name == name and path.resolve().parent == root, 'Recording members must be local basenames')
        return path

    descriptor = json.loads(recording.read_text())
    require(descriptor['version'] == 3 and descriptor['spatialDimensions'] == 3, 'Expected a version 3 3-D recording')
    require(descriptor['pointCount'] == 296174 and descriptor['frameCount'] == 5901, 'Full source coverage required')
    require(descriptor['topology'] == {'kind': 'points', 'origin': 'source', 'connectivity': None}, 'Unexpected topology claim')
    require(descriptor['coordinateUnit'] == 'm' and descriptor['timeUnit'] == 's', 'Wrong physical units')
    for filename, key in (('provenance.json', 'provenanceSHA256'), ('ATTRIBUTION.txt', 'attributionSHA256')):
        require(sha(member(filename)) == descriptor[key], f'Integrity mismatch: {filename}')
    provenance = json.loads(member('provenance.json').read_text())
    require(sha(source) == provenance['originalMemberSHA256'], 'Original source identity differs')
    require(provenance['originalArchiveMD5'] == '95ae813b52fe6b188e930d9c699e6a3e' and
            provenance['originalMember'] == 'flow3d_dataset/cylinder_3d_re300_ds.h5', 'Wrong published source')
    require(descriptor['defaultScalar'] == 'velocity_magnitude', 'Default scalar differs from exported speed')
    expected_fields = ['velocity_u', 'velocity_v', 'velocity_w', 'pressure', 'velocity_magnitude', 'cell_volume']
    require([f['id'] for f in descriptor['fields']] == expected_fields, 'Missing or extra source fields')
    comparisons = checksum_count = 0
    progress = time.monotonic()
    with pd.HDFStore(source, mode='r') as original:
        coords = original['coordinates']
        require(list(coords.columns) == ['x', 'y', 'z'], 'Wrong source axes')
        xyz = coords.to_numpy(dtype='<f8')
        require(descriptor['sourceBounds'] == {'min': xyz.min(axis=0).tolist(), 'max': xyz.max(axis=0).tolist()},
                'Display bounds differ from original coordinates')
        require(descriptor['coordinates']['shape'] == [296174, 3] and
                descriptor['coordinates']['dtype'] == 'float64' and
                descriptor['coordinates']['byteOrder'] == 'little', 'Coordinate layout differs')
        require(descriptor['pointIds']['count'] == 296174 and descriptor['pointIds']['dtype'] == 'int64' and
                descriptor['pointIds']['byteOrder'] == 'little', 'Point identity layout differs')
        for field, expected in ((descriptor['coordinates'], coords.to_numpy(dtype='<f8')),
                                (descriptor['pointIds'], original['kept_node_ids'].to_numpy(dtype='<i8').ravel())):
            path = member(field['path'])
            require(sha(path) == field['sha256'], f'Integrity mismatch: {path.name}')
            dtype = '<i8' if field['dtype'] == 'int64' else '<f8'
            require(np.array_equal(np.fromfile(path, dtype=dtype).reshape(expected.shape).view('<u8'), expected.view('<u8')), f'Original geometry/identity mismatch: {path.name}')
        labels = list(original.select('velocity_u', start=0, stop=1).columns)
        timeline = [{'index': int(str(label)[2:]), 'label': str(label), 'time': int(str(label)[2:]) * .01} for label in labels]
        require([frame['index'] for frame in timeline] == list(range(2200, 8101)), 'Original step sequence differs')
        require(descriptor['frames'] == timeline, 'Original steps, labels or physical times changed')
        for field in descriptor['fields']:
            info = field['array']
            require(info['dtype'] == 'float64' and info['byteOrder'] == 'little' and
                    field['origin'] == 'source' and field['association'] == 'point', 'Unexpected conversion type/origin')
            unit = 'Pa' if field['id'] == 'pressure' else 'unspecified' if field['id'] == 'cell_volume' else 'm/s'
            require(field['unit'] == unit, f'Wrong original field units: {field["id"]}')
            if field['id'] in expected_fields[:3]:
                require(field['vector'] == 'velocity' and field['component'] == 'xyz'[expected_fields.index(field['id'])],
                        'Velocity component identity differs')
            else:
                require('vector' not in field and 'component' not in field, 'Scalar incorrectly presented as a vector component')
            require(isinstance(field['static'], bool) and (not field['static'] or field['id'] == 'cell_volume'),
                    'Changing scientific field cannot be represented as static')
            require(info['shape'] == ([296174] if field['static'] else [5901, 296174]), 'Field layout differs')
            path = member(info['path'])
            require(path.stat().st_size == info['byteLength'] == int(np.prod(info['shape'])) * 8, 'Converted byte count differs')
            require(sha(path) == info['sha256'], f'Integrity mismatch: {path.name}')
            values = np.memmap(path, dtype='<f8', mode='r', shape=tuple(info['shape']))
            minimum, maximum = float('inf'), float('-inf')
            try:
                for begin in range(0, len(coords), 128):
                    end = min(begin + 128, len(coords))
                    table = original.select(field['id'], start=begin, stop=end)
                    require(np.array_equal(table.index.to_numpy(), coords.index.to_numpy()[begin:end]), 'Source row identities differ')
                    expected = table.to_numpy(dtype='<f8')
                    require(list(table.columns) == labels or
                            (field['id'] == 'cell_volume' and expected.shape[1] == 1), 'Field time labels differ')
                    minimum, maximum = min(minimum, float(expected.min())), max(maximum, float(expected.max()))
                    actual = np.broadcast_to(values[begin:end, None], expected.shape) if field['static'] else values[:, begin:end].T
                    require(np.isfinite(expected).all() and np.array_equal(expected.view('<u8'), actual.view('<u8')), f'Original value bits differ: {field["id"]} rows {begin}:{end}')
                    comparisons += expected.size
                    if time.monotonic() - progress > 15:
                        print(f'Exact original comparison: {field["id"]}, {end}/{len(coords)} points', flush=True)
                        progress = time.monotonic()
                require(field['range'] == [minimum, maximum], f'Legend range differs from all original values: {field["id"]}')
                if not field['static']:
                    require(len(info['frameCRC32']) == len(timeline), 'Frame checksums incomplete')
                    for ordinal, checksum in enumerate(info['frameCRC32']):
                        require(zlib.crc32(values[ordinal].tobytes()) == checksum, f'Frame checksum mismatch: {field["id"]}/{ordinal}')
                        checksum_count += 1
            finally:
                del values
            print(f'All original values and checksums verified: {field["id"]}', flush=True)
    result = {'descriptor_sha256': sha(recording), 'original_member_sha256': provenance['originalMemberSHA256'],
              'exact_original_scalar_comparisons': int(comparisons), 'dynamic_frame_checksums_verified': checksum_count,
              'point_count': len(coords), 'frame_count': len(timeline),
              'source_elapsed_seconds': timeline[-1]['time'] - timeline[0]['time'],
              'metadata_verified': ['original identity', 'point IDs', 'coordinate layout and bounds',
                                    'field units and associations', 'vector component identities',
                                    'full original scalar ranges', 'original time labels and physical times'],
              'method': 'Independent pandas HDFStore rows vs converted float64 bits; source values widened exactly where needed.',
              'acceptance': 'Lossless offline conversion only. Native import, spatial reconstruction and volume rendering remain unaccepted.'}
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('recording', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    verify(args.source, args.recording, args.report)
