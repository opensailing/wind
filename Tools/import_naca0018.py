#!/usr/bin/env python3
"""Convert the original published NACA0018 HDF5 to optional-field recording v3.

Requires numpy and h5py. This preserves all source points, fields and snapshots;
it supplies no mesh connectivity, density, boundary patches or solver histories.
The native v3 reader is a separate implementation gate. Output is an external
recording directory, not an automatically installed or accepted sample.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
import zlib

import h5py
import numpy as np

SOURCE_SHA = '513e30f370ecdd30c5a49dfcc7b9b0caf39204b6dec08ee513a53afeba657237'
SOURCE_SIZE = 5988235784
POINTS, FRAMES = 18706, 8000
DT = .0025
FIELDS = (
    ('velocity_u', 'Velocity X', 'm/s', 'x'),
    ('velocity_v', 'Velocity Y', 'm/s', 'y'),
    ('pressure', 'Pressure', 'Pa', None),
    ('velocity_magnitude', 'Exported speed', 'm/s', None),
    # The file calls this cell_volume but supplies it on exported point rows.
    # Its physical convention is undocumented; do not assert m³ for 2D output.
    ('cell_volume', 'Exported cell_volume', 'unspecified', None),
)


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def labels(dataset):
    return [x.decode('ascii') for x in dataset[:]]


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False)+'\n')


def describe_array(path, shape, static=False):
    expected = int(np.prod(shape))*8
    if path.stat().st_size != expected:
        raise ValueError(f'Array size differs: {path.name}')
    sha = hashlib.sha256()
    checksums = []
    with path.open('rb') as stream:
        if static:
            while chunk := stream.read(65536):
                sha.update(chunk)
        else:
            frame_bytes = shape[1]*8
            for _ in range(shape[0]):
                chunk = stream.read(frame_bytes)
                if len(chunk) != frame_bytes:
                    raise ValueError('Incomplete converted frame')
                sha.update(chunk)
                checksums.append(zlib.crc32(chunk))
    result = {'path': path.name, 'dtype': 'float64', 'byteOrder': 'little',
              'shape': list(shape), 'byteLength': expected, 'sha256': sha.hexdigest()}
    if not static:
        result['frameCRC32'] = checksums
    return result


def convert(source, output, author_readme, record_path):
    source, output = Path(source), Path(output)
    if source.stat().st_size != SOURCE_SIZE or digest(source) != SOURCE_SHA:
        raise ValueError('Original NACA0018 member failed size/SHA-256 verification')
    if output.exists():
        raise ValueError('Choose a new output directory; existing recordings are never overwritten')
    record = json.loads(Path(record_path).read_text())
    if record['id'] != 20582405 or record['metadata']['license']['id'] != 'cc-by-4.0':
        raise ValueError('Expected original Zenodo record and CC-BY-4.0 license')
    readme_sha = digest(author_readme)
    if readme_sha != 'f7d3ef070c552e16fbf20a80fbbad3b97ea31baeeea89b03546fdb0393b7cc70':
        raise ValueError('Author setup documentation does not match the audited revision')
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.'+output.name+'-', dir=output.parent))
    try:
        with h5py.File(source, 'r') as original:
            xy = original['coordinates/block0_values'][:]
            if xy.shape != (POINTS, 2) or not np.isfinite(xy).all():
                raise ValueError('Invalid source coordinates')
            if labels(original['coordinates/axis0']) != ['x', 'y']:
                raise ValueError('Unexpected coordinate axes')
            # Preserve point order and labels, including non-contiguous IDs.
            ids = original['coordinates/axis1'][:]
            if ids.dtype.kind not in 'iu' or len(np.unique(ids)) != POINTS:
                raise ValueError('Invalid original point IDs')
            ids.astype('<i8').tofile(stage/'point-ids.i64')
            xy.astype('<f8').tofile(stage/'coordinates.f64')
            source_labels = labels(original['velocity_u/axis0'])
            if source_labels != [f't_{i}' for i in range(1001, 9001)]:
                raise ValueError('Source frame labels changed')
            fields = []
            for key, label, unit, component in FIELDS:
                data = original[key+'/block0_values']
                if data.shape != (POINTS, FRAMES) or labels(original[key+'/axis0']) != source_labels:
                    raise ValueError(f'Unexpected field shape/time labels: {key}')
                if not np.array_equal(original[key+'/axis1'][:], ids):
                    raise ValueError(f'Point identity mismatch: {key}')
                static = key == 'cell_volume'
                path = stage/(key+'.f64')
                mapped = None if static else np.memmap(path, mode='w+', dtype='<f8', shape=(FRAMES, POINTS))
                first_values = np.empty(POINTS, dtype='<f8') if static else None
                minimum, maximum = float('inf'), float('-inf')
                try:
                    for begin in range(0, POINTS, 256):
                        end = min(begin+256, POINTS)
                        chunk = data[begin:end, :]
                        if not np.isfinite(chunk).all():
                            raise ValueError(f'Nonfinite source field: {key}')
                        minimum, maximum = min(minimum, float(chunk.min())), max(maximum, float(chunk.max()))
                        if static:
                            if not np.all(chunk == chunk[:, :1]):
                                raise ValueError('cell_volume is no longer time invariant')
                            first_values[begin:end] = chunk[:, 0]
                        else:
                            mapped[:, begin:end] = chunk.T
                    if static:
                        first_values.tofile(path)
                    else:
                        mapped.flush()
                finally:
                    if mapped is not None:
                        del mapped
                field = {'id': key, 'label': label, 'unit': unit, 'association': 'point',
                         'origin': 'source', 'static': static, 'range': [minimum, maximum],
                         'array': describe_array(path, (POINTS,) if static else (FRAMES, POINTS), static)}
                if component:
                    field.update(vector='velocity', component=component)
                fields.append(field)
                print(f'Converted all {FRAMES} snapshots of {key}; original values retained', flush=True)
        shutil.copyfile(author_readme, stage/'author-setup.md')
        shutil.copyfile(record_path, stage/'zenodo-record.json')
        authors = '; '.join(item['name'] for item in record['metadata']['creators'])
        notice = (f"{record['metadata']['title']}\nCreators: {authors}\n"
                  'Source: https://doi.org/10.5281/zenodo.20582405\n'
                  'License: Creative Commons Attribution 4.0 International\n'
                  'https://creativecommons.org/licenses/by/4.0/\n'
                  'Changes: transposed original float64 arrays into frame-major binary files; '
                  'stored time-invariant cell_volume once; derived physical timestamps from '
                  'original step labels and the author time interval. No generated CFD values, '
                  'spatial reconstruction or temporal interpolation. Attribution does not imply endorsement.\n')
        (stage/'ATTRIBUTION.txt').write_text(notice)
        provenance = {'sourceURL': 'https://zenodo.org/records/20582405',
                      'originalMember': source.name, 'originalMemberBytes': SOURCE_SIZE,
                      'originalMemberSHA256': SOURCE_SHA, 'originalLocalPath': str(source.resolve()),
                      'authorRevision': '5137e7c5b8feebb838eb3783b52b02664fe79c57',
                      'authorSetupSHA256': readme_sha, 'license': 'CC-BY-4.0',
                      'converter': 'Tools/import_naca0018.py', 'converterSHA256': digest(__file__),
                      'changes': 'Lossless layout conversion and labelled step-to-time calculation only.'}
        write_json(stage/'provenance.json', provenance)
        descriptor = {'version': 3, 'kind': 'field_recording', 'id': 'Fluent_NACA0018_Zenodo20582405',
                      'title': 'NACA 0018 · Fluent · oscillating inflow',
                      'sourceURL': provenance['sourceURL'], 'spatialDimensions': 2,
                      'coordinateUnit': 'm', 'topology': {'kind': 'points', 'origin': 'source', 'connectivity': None},
                      'pointCount': POINTS, 'coordinates': describe_array(stage/'coordinates.f64', (POINTS, 2), True),
                      'pointIds': {'path': 'point-ids.i64', 'dtype': 'int64', 'byteOrder': 'little',
                                   'count': POINTS, 'sha256': digest(stage/'point-ids.i64')},
                      'sourceBounds': {'min': xy.min(axis=0).tolist(), 'max': xy.max(axis=0).tolist()},
                      'frameCount': FRAMES, 'timeUnit': 's',
                      'frames': [{'index': i, 'label': f't_{i}', 'time': i*DT} for i in range(1001,9001)],
                      'timeOrigin': 'Original solver step labels multiplied by author save interval 0.0025 s.',
                      'fields': fields, 'defaultScalar': 'velocity_magnitude',
                      'provenanceSHA256': digest(stage/'provenance.json'),
                      'attributionSHA256': digest(stage/'ATTRIBUTION.txt'),
                      'limitations': ['No original cell connectivity or boundary patch IDs.',
                                      'No density, force or residual history.',
                                      'Two-dimensional fields; no spanwise velocity.',
                                      'Exported speed differs from the norm of exported velocity components.',
                                      'cell_volume units are undocumented and are not inferred.',
                                      'Native v3 import/render acceptance remains pending.']}
        write_json(stage/'recording.json', descriptor)
        os.rename(stage, output)
        return descriptor
    except BaseException:
        shutil.rmtree(stage)
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--author-readme', type=Path, required=True)
    parser.add_argument('--record', type=Path, required=True)
    args = parser.parse_args()
    descriptor = convert(args.source, args.output, args.author_readme, args.record)
    print(f"Preserved {descriptor['frameCount']} original snapshots in {args.output}")
