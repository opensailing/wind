#!/usr/bin/env python3
"""Independently audit prepared pipeline values against published CFD arrays.

Recompute triangle/tetrahedron barycentric coordinates with NumPy; never use
native sampled outputs or stored tetrahedron weights to derive expected values.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path

import numpy as np


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def array(parent, spec, dtype, shape):
    path = parent / spec['path']
    assert path.parent == parent and digest(path) == spec['sha256']
    assert path.stat().st_size == np.prod(shape) * np.dtype(dtype).itemsize
    return np.memmap(path, mode='r', dtype=dtype, shape=shape)


def source(parent):
    doc = json.loads((parent / 'recording.json').read_text())
    points = array(parent, doc['coordinates'], '<f8', (doc['pointCount'], doc['spatialDimensions']))
    ids = array(parent, doc['pointIds'], '<i8', (doc['pointCount'],))
    return doc, points, ids


def components(parent, doc, ordinal):
    result = []
    for component in 'uvw'[:doc['spatialDimensions']]:
        spec = next(s for s in doc['fields'] if s['id'] == 'velocity_' + component)
        assert spec['unit'] == 'm/s'
        shape = (doc['pointCount'],) if spec['static'] else (doc['frameCount'], doc['pointCount'])
        values = array(parent, spec['array'], '<f8', shape)
        result.append(values if spec['static'] else values[ordinal])
    return np.array(result).T


def case(exported, name, parent, doc, reconstruction):
    meta = json.loads((exported / (name + '.json')).read_text())
    assert meta['dataset'] == doc['id']
    assert meta['sourceSHA256'] == digest(parent / 'recording.json')
    assert meta['reconstructionSHA256'] == digest(reconstruction)
    assert meta['unit'] == 'm/s' and meta['field'] == 'derived.speed'
    assert 'interpolated before magnitude' in meta['expression']
    assert 0 <= meta['ordinal'] < doc['frameCount']
    frame = doc['frames'][meta['ordinal']]
    assert meta['step'] == frame['index'] and meta['timeSeconds'] == frame['time']
    with (exported / name).open() as stream:
        rows = list(csv.DictReader(stream))
    assert rows
    return meta, rows


def compare(rows, expected, valid):
    actual_valid = np.array([int(row['available']) for row in rows], bool)
    assert np.array_equal(actual_valid, valid), np.flatnonzero(actual_valid != valid).tolist()
    assert all(row['value'] == '' for row, has in zip(rows, valid) if not has)
    actual = np.array([float(row['value']) for row, has in zip(rows, valid) if has])
    truth = expected[valid]
    error = float(np.max(np.abs(actual-truth)))
    scale = max(float(np.max(np.abs(truth))), 1e-300)
    assert np.isfinite(actual).all() and error <= scale * 1e-9, (error, scale)
    return {'queries': len(rows), 'available': int(valid.sum()), 'missing': int((~valid).sum()),
            'max_absolute_error': error, 'scale_relative_error': error/scale}


def verify(samples, exported):
    naca = samples / 'NACA0018_ReaderFixture'
    surface = samples / 'NACA0018_SurfaceFixture' / 'reconstruction.json'
    doc, points, ids = source(naca)
    meta, rows = case(exported, 'naca-original.csv', naca, doc, surface)
    values = components(naca, doc, meta['ordinal'])
    assert len(rows) == len(points)
    assert np.array_equal([int(r['row']) for r in rows], np.arange(len(points)))
    assert np.array_equal([int(r['id']) for r in rows], ids)
    assert np.array_equal([[float(r[k]) for k in ('x', 'y')] for r in rows], points)
    assert all(float(r['z']) == 0 for r in rows)
    assert np.array_equal([[float(r[k]) for k in ('u', 'v')] for r in rows], values)
    magnitudes = np.hypot(values[:, 0], values[:, 1])
    actual = np.array([float(r['magnitude']) for r in rows])
    row_error = float(np.max(np.abs(actual-magnitudes)))
    assert row_error <= np.max(magnitudes) * 4 * np.finfo(float).eps

    meta, rows = case(exported, 'naca-probes.csv', naca, doc, surface)
    sd = json.loads(surface.read_text())
    triangles = array(surface.parent, sd['triangles'], '<u4', tuple(sd['triangles']['shape']))
    coords = points[triangles]
    matrices = np.stack((coords[:, 1]-coords[:, 0], coords[:, 2]-coords[:, 0]), axis=2)
    inverse = np.linalg.inv(matrices)
    samples2 = np.array([[float(r[k]) for k in ('x', 'z')] for r in rows])
    expected = np.full(len(rows), np.nan)
    valid = np.zeros(len(rows), bool)
    for i, p in enumerate(samples2):
        if not (.15 <= p[0] <= .4 and -.1 <= p[1] <= .1 and float(rows[i]['y']) == 0):
            continue
        ab = np.einsum('ijk,ik->ij', inverse, p-coords[:, 0])
        weights = np.column_stack((1-ab.sum(axis=1), ab))
        inside = np.flatnonzero(np.all(weights >= -1e-10, axis=1))
        if not len(inside):
            continue
        face = inside[0]
        interpolated = weights[face] @ values[triangles[face]]
        expected[i] = np.hypot(*interpolated)
        valid[i] = True
    naca_report = compare(rows, expected, valid)

    cylinder = samples / 'Cylinder3D_ReaderFixture'
    reconstruction = samples / 'Cylinder3D_VolumeFixture' / 'reconstruction.json'
    doc3, points3, _ = source(cylinder)
    meta3, rows3 = case(exported, 'cylinder-probes.csv', cylinder, doc3, reconstruction)
    data3 = components(cylinder, doc3, meta3['ordinal'])
    volume = json.loads(reconstruction.read_text())
    assert volume['sourceMetadataSHA256'] == digest(cylinder / 'recording.json')
    dimensions = np.array(volume['dimensions'])
    count = int(np.prod(dimensions))
    lower, upper = np.array(volume['minimum']), np.array(volume['maximum'])
    spacing = (upper-lower)/(dimensions-1)
    support = array(reconstruction.parent, volume['rows'], '<u4', (count, 4))
    classes = array(reconstruction.parent, volume['classification'], 'u1', (count,))
    center, radius = np.array(volume['solid']['centerXY']), volume['solid']['radiusMeters']
    positions = np.array([[float(r[k]) for k in ('x', 'z', 'y')] for r in rows3])
    offsets = np.array([[x, y, z] for z in (0, 1) for y in (0, 1) for x in (0, 1)])
    expected3 = np.full(len(rows3), np.nan)
    valid3 = np.zeros(len(rows3), bool)
    for i, p in enumerate(positions):
        if np.any(p < lower) or np.any(p > upper):
            continue
        q = (p-lower)/spacing
        cell = np.minimum(q.astype(int), dimensions-2)
        fraction = q-cell
        cell_min = lower+cell*spacing
        nearest = np.clip(center, cell_min[:2], cell_min[:2]+spacing[:2])-center
        if np.dot(nearest, nearest) < radius**2:
            continue
        nodes = cell+offsets
        indices = nodes[:, 0]+dimensions[0]*(nodes[:, 1]+dimensions[1]*nodes[:, 2])
        if np.any(classes[indices] != 1):
            continue
        source_rows = support[indices]
        assert np.all(source_rows < len(points3))
        tetrahedra = points3[source_rows]
        matrices = np.concatenate((np.transpose(tetrahedra, (0, 2, 1)), np.ones((8, 1, 4))), axis=1)
        targets = np.column_stack((lower+nodes*spacing, np.ones(8)))
        # Solve directly from original coordinates; saved weights are not read.
        weights = np.linalg.solve(matrices, targets[..., None])[..., 0]
        node_values = np.einsum('ij,ijk->ik', weights, data3[source_rows])
        trilinear = np.prod(np.where(offsets, fraction, 1-fraction), axis=1)
        uvw = trilinear @ node_values
        expected3[i] = np.hypot(np.hypot(uvw[0], uvw[1]), uvw[2])
        valid3[i] = True
    cylinder_report = compare(rows3, expected3, valid3)
    return {'original_rows': len(points), 'original_component_values': int(values.size),
            'original_row_max_magnitude_error': row_error,
            'naca_probe_queries': naca_report, 'cylinder_probe_queries': cylinder_report,
            'source_hashes': [digest(naca/'recording.json'), digest(cylinder/'recording.json')],
            'reconstruction_hashes': [digest(surface), digest(reconstruction)],
            'output_hashes': {p.name: digest(p) for p in sorted(exported.glob('*.csv*'))},
            'verifier_sha256': digest(__file__),
            'scope': 'Prepared scalar graph, original rows, clips and supported interpolation-before-magnitude queries; no contour geometry or native UI acceptance.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('samples', type=Path)
    parser.add_argument('exported', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = verify(args.samples, args.exported)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))
