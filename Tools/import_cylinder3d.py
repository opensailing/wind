#!/usr/bin/env python3
"""Losslessly convert the audited published 3-D cylinder fields to recording v3.

Requires a completed original archive acquisition and independent array audit.
Retains every supplied point, component and snapshot. No mesh or volume is
invented: derived spatial reconstruction is a separate attachment/acceptance.
Requires NumPy/h5py in the offline data environment; never opens an application.
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

POINTS, FRAMES = 296174, 5901
PUBLISHED_MD5 = '95ae813b52fe6b188e930d9c699e6a3e'
MEMBER = 'flow3d_dataset/cylinder_3d_re300_ds.h5'
FIELDS = (
    ('velocity_u', 'Velocity X', 'm/s', 'x'),
    ('velocity_v', 'Velocity Y', 'm/s', 'y'),
    ('velocity_w', 'Velocity Z', 'm/s', 'z'),
    ('pressure', 'Pressure', 'Pa', None),
    ('velocity_magnitude', 'Exported speed', 'm/s', None),
    ('cell_volume', 'Exported cell_volume', 'unspecified', None),
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n')


def labels(dataset):
    return [x.decode('ascii') for x in dataset[:]]


def array_descriptor(path, shape, static):
    expected = int(np.prod(shape)) * 8
    require(path.stat().st_size == expected, f'Converted size mismatch: {path.name}')
    sha, checksums = hashlib.sha256(), []
    with path.open('rb') as stream:
        stride = 8 * 1024 * 1024 if static else shape[1] * 8
        while data := stream.read(stride):
            sha.update(data)
            if not static:
                require(len(data) == stride, 'Incomplete converted frame')
                checksums.append(zlib.crc32(data))
    result = {'path': path.name, 'dtype': 'float64', 'byteOrder': 'little',
              'shape': list(shape), 'byteLength': expected, 'sha256': sha.hexdigest()}
    if not static:
        require(len(checksums) == shape[0], 'Incomplete frame checksums')
        result['frameCRC32'] = checksums
    return result


def convert(source, output, acquisition_path, audit_path, record_path, author_readme):
    source, output = Path(source), Path(output)
    require(not output.exists(), 'Choose a new output directory; existing recordings are never overwritten')
    acquisition = json.loads(Path(acquisition_path).read_text())
    audit = json.loads(Path(audit_path).read_text())
    record = json.loads(Path(record_path).read_text())
    require(record['id'] == 20586598 and record['metadata']['license']['id'] == 'cc-by-4.0', 'Wrong publication or license')
    archive = next(x for x in record['files'] if x['key'] == 'flow3d_dataset.zip')
    require(archive['checksum'] == 'md5:' + PUBLISHED_MD5 and
            acquisition['published_archive_md5_verified'] == PUBLISHED_MD5, 'Unverified published archive')
    require(acquisition['member']['name'] == MEMBER and acquisition['member_crc32_verified'] is True, 'Unverified original member')
    require(source.stat().st_size == acquisition['member']['size'], 'Original member size differs')
    source_sha = digest(source)
    require(source_sha == acquisition['member_sha256'] == audit['member_sha256'], 'Original/audit member identities differ')
    require(audit['point_count'] == POINTS and audit['frame_count'] == FRAMES and
            audit['first_solver_step'] == 2200 and audit['last_solver_step'] == 8100 and
            audit['save_interval_seconds'] == .01, 'Audit does not cover the full original 3-D source')
    require(audit['point_id_source'].startswith('kept_node_ids/node_id'), 'Original node IDs must be independently audited')
    require(Path(author_readme).is_file(), 'Author setup documentation is required')
    output.parent.mkdir(parents=True, exist_ok=True)
    # Five changing fields expand to exact float64; one static field and geometry
    # are small. If cell_volume varies, retain its whole original sequence too.
    volume_static = audit['cell_volume_time_max_error'] == 0
    required_bytes = (5 + int(not volume_static)) * FRAMES * POINTS * 8 + 64 * 1024 * 1024
    require(shutil.disk_usage(output.parent).free >= required_bytes + 1024**3, 'Insufficient free space for lossless conversion')
    stage = Path(tempfile.mkdtemp(prefix='.' + output.name + '-', dir=output.parent))
    try:
        with h5py.File(source, 'r') as original:
            require(labels(original['coordinates/axis0']) == ['x', 'y', 'z'], 'Coordinate axes differ')
            xyz = original['coordinates/block0_values'][:]
            require(xyz.shape == (POINTS, 3) and xyz.dtype.kind == 'f' and np.isfinite(xyz).all(), 'Invalid source coordinates')
            rows = original['coordinates/axis1'][:]
            require(labels(original['kept_node_ids/axis0']) == ['node_id'], 'Original node ID column differs')
            require(np.array_equal(original['kept_node_ids/axis1'][:], rows), 'Original node IDs do not align with coordinates')
            ids = original['kept_node_ids/block0_values'][:].ravel()
            require(ids.shape == (POINTS,) and ids.dtype.kind in 'iu' and len(np.unique(ids)) == POINTS, 'Invalid original node identities')
            require(np.array_equal(ids, ids.astype('<i8')), 'Original IDs do not fit recording int64')
            ids.astype('<i8').tofile(stage / 'point-ids.i64')
            xyz.astype('<f8').tofile(stage / 'coordinates.f64')
            time_labels = labels(original['velocity_u/axis0'])
            require(all(re.fullmatch(r't_\d+', x) for x in time_labels), 'Unsupported time labels')
            steps = [int(x[2:]) for x in time_labels]
            require(steps == list(range(2200, 8101)), 'Original timeline differs')
            fields = []
            for key, title, unit, component in FIELDS:
                values = original[key + '/block0_values']
                require(values.dtype.kind == 'f' and values.dtype.itemsize in (4, 8), f'Unsupported source precision: {key}')
                require(list(values.shape) == audit['field_shapes'][key] and str(values.dtype) == audit['field_dtypes'][key], f'Audited storage differs: {key}')
                require(np.array_equal(original[key + '/axis1'][:], rows), f'Point order differs: {key}')
                static = key == 'cell_volume' and volume_static
                require(labels(original[key + '/axis0']) == time_labels or
                        (static and values.shape == (POINTS, 1)), f'Time labels differ: {key}')
                path = stage / (key + '.f64')
                mapped = None if static else np.memmap(path, dtype='<f8', mode='w+', shape=(FRAMES, POINTS))
                fixed = np.empty(POINTS, dtype='<f8') if static else None
                minimum, maximum = float('inf'), float('-inf')
                try:
                    for begin in range(0, POINTS, 128):
                        end = min(begin + 128, POINTS)
                        block = values[begin:end, :]
                        require(np.isfinite(block).all(), f'Nonfinite source values: {key}')
                        minimum = min(minimum, float(block.min()))
                        maximum = max(maximum, float(block.max()))
                        if static:
                            require(np.all(block == block[:, :1]), f'Field is not actually static: {key}')
                            fixed[begin:end] = block[:, 0]
                        else:
                            mapped[:, begin:end] = block.T
                    if static:
                        fixed.tofile(path)
                    else:
                        mapped.flush()
                finally:
                    if mapped is not None:
                        del mapped
                require([minimum, maximum] == audit['field_ranges'][key], f'Converted range differs from independent audit: {key}')
                field = {'id': key, 'label': title, 'unit': unit, 'association': 'point',
                         'origin': 'source', 'static': static, 'range': [minimum, maximum],
                         'array': array_descriptor(path, (POINTS,) if static else (FRAMES, POINTS), static)}
                if component:
                    field.update(vector='velocity', component=component)
                fields.append(field)
                print(f'Preserved all {FRAMES} original frames of {key}', flush=True)

        for source_path, name in ((record_path, 'zenodo-record.json'), (author_readme, 'author-setup.md'),
                                  (acquisition_path, 'original-acquisition.json'), (audit_path, 'original-array-audit.json')):
            shutil.copyfile(source_path, stage / name)
        authors = '; '.join(x['name'] for x in record['metadata']['creators'])
        (stage / 'ATTRIBUTION.txt').write_text(
            f"{record['metadata']['title']}\nCreators: {authors}\n"
            'Source: https://doi.org/10.5281/zenodo.20586598\n'
            'License: Creative Commons Attribution 4.0 International\nhttps://creativecommons.org/licenses/by/4.0/\n'
            'Changes: lossless layout conversion to frame-major float64; original float32 values, if supplied, '
            'are widened exactly; time-invariant cell_volume stored once; physical times calculated from '
            'original step labels and author save interval. No generated flow values, topology, resampling '
            'or temporal interpolation. Attribution does not imply endorsement.\n')
        provenance = {'sourceURL': 'https://zenodo.org/records/20586598', 'license': 'CC-BY-4.0',
                      'originalMember': MEMBER, 'originalMemberSHA256': source_sha,
                      'originalArchiveMD5': PUBLISHED_MD5, 'originalLocalPath': str(source.resolve()),
                      'originalFieldDtypes': audit['field_dtypes'], 'pointIdSource': audit['point_id_source'],
                      'authorSetupSHA256': digest(author_readme), 'arrayAuditSHA256': digest(audit_path),
                      'converter': 'Tools/import_cylinder3d.py', 'converterSHA256': digest(__file__),
                      'changes': 'Lossless storage layout/precision widening only; source geometry and fields unchanged.'}
        write_json(stage / 'provenance.json', provenance)
        descriptor = {'version': 3, 'kind': 'field_recording', 'id': 'Fluent_Cylinder3D_Zenodo20586598',
                      'title': '3D cylinder wake · Fluent · Re 300', 'sourceURL': provenance['sourceURL'],
                      'spatialDimensions': 3, 'coordinateUnit': 'm', 'timeUnit': 's', 'pointCount': POINTS,
                      'topology': {'kind': 'points', 'origin': 'source', 'connectivity': None},
                      'coordinates': array_descriptor(stage / 'coordinates.f64', (POINTS, 3), True),
                      'pointIds': {'path': 'point-ids.i64', 'dtype': 'int64', 'byteOrder': 'little',
                                   'count': POINTS, 'sha256': digest(stage / 'point-ids.i64')},
                      'sourceBounds': {'min': xyz.min(axis=0).tolist(), 'max': xyz.max(axis=0).tolist()},
                      'frameCount': FRAMES, 'frames': [{'index': step, 'label': label, 'time': step * .01}
                                                      for step, label in zip(steps, time_labels)],
                      'timeOrigin': 'Original solver step labels multiplied by author save interval 0.01 s.',
                      'fields': fields, 'defaultScalar': 'velocity_magnitude',
                      'provenanceSHA256': digest(stage / 'provenance.json'), 'attributionSHA256': digest(stage / 'ATTRIBUTION.txt'),
                      'limitations': ['Author-supplied cropped/downsampled 3D point rows, not the full original Fluent mesh.',
                                      'No original cell connectivity or boundary patch IDs.',
                                      'No density, residual or force history.',
                                      'Exported speed and vector-component norm remain distinct quantities.',
                                      'Exported cell_volume convention/units are unspecified.',
                                      'No volume reconstruction or native rendering acceptance is implied by conversion.']}
        write_json(stage / 'recording.json', descriptor)
        os.rename(stage, output)
        return descriptor
    except BaseException:
        shutil.rmtree(stage)
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    for option in ('acquisition', 'audit', 'record', 'author-readme'):
        parser.add_argument('--' + option, type=Path, required=True)
    args = parser.parse_args()
    result = convert(args.source, args.output, args.acquisition, args.audit, args.record, args.author_readme)
    print(f"Preserved {result['frameCount']} original 3-D snapshots in {args.output}")
