#!/usr/bin/env python3
"""Audit exported native slice meshes against verified original point arrays.

Independently solve every needed tetrahedron from original coordinates, then
interpolate in the declared grid. A summed-volume support mask checks entire
triangle bounds, including interiors that sparse point checks can miss.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify(recording, reconstruction, exported):
    source = json.loads(recording.read_text())
    volume = json.loads(reconstruction.read_text())
    manifest = json.loads((exported / 'manifest.json').read_text())
    prior = reconstruction.parent / 'independent-validation.json'
    original = json.loads(prior.read_text())
    assert manifest['source'] == volume['sourceMetadataSHA256'] == digest(recording)
    assert manifest['reconstruction'] == original['reconstruction_sha256'] == digest(reconstruction)
    assert original['recording_sha256'] == digest(recording)
    assert original['original_hdf5_values_compared'] > 0 and original['original_hdf5_sha256']
    assert manifest['byteOrder'] == 'little'

    def read(parent, spec, dtype, shape):
        path = parent / spec['path']
        assert path.parent == parent and digest(path) == spec['sha256']
        assert path.stat().st_size == int(np.prod(shape)) * np.dtype(dtype).itemsize
        return np.memmap(path, mode='r', dtype=dtype, shape=shape)

    dimensions = np.array(volume['dimensions'])
    count = int(np.prod(dimensions))
    lower, upper = np.array(volume['minimum']), np.array(volume['maximum'])
    spacing = (upper-lower)/(dimensions-1)
    points = read(recording.parent, source['coordinates'], '<f8', (source['pointCount'], 3))
    rows = read(reconstruction.parent, volume['rows'], '<u4', (count, 4))
    classes = read(reconstruction.parent, volume['classification'], 'u1', tuple(dimensions[::-1]))
    shape = dimensions-1
    bad = np.zeros(tuple(shape[::-1]), bool)
    for z in (0, 1):
        for y in (0, 1):
            for x in (0, 1):
                bad |= classes[z:z+shape[2], y:y+shape[1], x:x+shape[0]] != 1
    z, y, x = np.indices(bad.shape)
    cell_min = lower + np.stack((x,y,z), axis=-1)*spacing
    center = np.array(volume['solid']['centerXY'])
    nearest = np.clip(center, cell_min[..., :2], cell_min[..., :2]+spacing[:2])-center
    bad |= np.sum(nearest**2, axis=-1) < volume['solid']['radiusMeters']**2
    prefix = np.pad(bad.astype(np.int64), ((1,0),(1,0),(1,0))).cumsum(0).cumsum(1).cumsum(2)
    offsets = np.array([[dx,dy,dz] for dz in (0,1) for dy in (0,1) for dx in (0,1)])
    reports = []
    for case in manifest['cases']:
        name = case['prefix']
        assert Path(name).name == name
        vertex_path, index_path = exported/(name+'-vertices.f64'), exported/(name+'-indices.i32')
        vertices = np.fromfile(vertex_path, dtype='<f8').reshape((-1, 4))
        indices = np.fromfile(index_path, dtype='<i4').reshape((-1, 3))
        assert len(vertices) == case['vertices'] and indices.size == case['indices'] and len(indices) > 0
        assert indices.min() >= 0 and indices.max() < len(vertices)
        # Source XYZ is scene XZY. Unindexed placeholder vertices are not CFD.
        positions = vertices[:, [0,2,1]]
        selected = np.unique(indices)
        assert np.isfinite(vertices[selected]).all()
        triangles = positions[indices]
        bounds_lo, bounds_hi = triangles.min(axis=1), triangles.max(axis=1)
        assert np.all(bounds_lo >= lower) and np.all(bounds_hi <= upper)
        low = np.floor((bounds_lo-lower)/spacing).astype(int)
        high = np.minimum(np.floor((bounds_hi-lower)/spacing).astype(int), dimensions-2)+1
        low = np.minimum(low, dimensions-2)
        missing = np.zeros(len(indices), np.int64)
        for corner in range(8):
            q = np.where(np.array([(corner >> a)&1 for a in range(3)]), high, low)
            missing += (-1 if (3-corner.bit_count()) % 2 else 1)*prefix[q[:,2],q[:,1],q[:,0]]
        assert np.all(missing == 0), f'{name}: {np.count_nonzero(missing)} triangles bridge missing support'
        field = next(f for f in source['fields'] if f['id'] == case['field'])
        array_shape = (source['pointCount'],) if field['static'] else (source['frameCount'], source['pointCount'])
        all_values = read(recording.parent, field['array'], '<f8', array_shape)
        values = all_values if field['static'] else all_values[case['frame']]
        max_error = 0.
        for start in range(0, len(selected), 2048):
            ids = selected[start:start+2048]
            grid = (positions[ids]-lower)/spacing
            cells = np.clip(np.floor(grid).astype(int), 0, dimensions-2)
            fraction = grid-cells
            nodes = cells[:,None,:]+offsets
            node_ids = nodes[:,:,0]+dimensions[0]*(nodes[:,:,1]+dimensions[1]*nodes[:,:,2])
            support = rows[node_ids]
            assert np.all(support < source['pointCount'])
            tetrahedra = points[support]
            matrices = np.concatenate((np.transpose(tetrahedra,(0,1,3,2)), np.ones((len(ids),8,1,4))), axis=2)
            grid_positions = lower+nodes*spacing
            target = np.concatenate((grid_positions,np.ones((len(ids),8,1))), axis=2)
            independent = np.linalg.solve(matrices,target[...,None])[...,0]
            node_values = np.sum(values[support]*independent,axis=2)
            trilinear = np.prod(np.where(offsets[None,:,:], fraction[:,None,:], 1-fraction[:,None,:]),axis=2)
            expected = np.sum(node_values*trilinear,axis=1)
            max_error = max(max_error,float(np.max(np.abs(expected-vertices[ids,3]))))
        scale = max(abs(field['range'][0]), abs(field['range'][1]), 1e-300)
        assert max_error <= scale*1e-8, (name,max_error,scale)
        reports.append({'case':name,'field':case['field'],'source_frame':source['frames'][case['frame']],
                        'indexed_vertices_compared':len(selected),'triangle_bounds_checked':len(indices),
                        'unsupported_triangle_bounds':int(np.count_nonzero(missing)),
                        'maximum_scalar_error':max_error,'field_scale_relative_error':max_error/scale,
                        'vertices_sha256':digest(vertex_path),'indices_sha256':digest(index_path)})
    return {'source_sha256':digest(recording),'reconstruction_sha256':digest(reconstruction),
            'prior_original_hdf5_verification_sha256':digest(prior),'original_hdf5_sha256':original['original_hdf5_sha256'],
            'original_values_verified_previously':original['original_hdf5_values_compared'],
            'verifier_sha256':digest(__file__),'cases':reports,
            'scope':'Native 3D named-slice geometry and scalar samples; no GPU pixel or full-source playback acceptance.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('recording',type=Path)
    parser.add_argument('reconstruction',type=Path)
    parser.add_argument('exported',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    report = verify(args.recording,args.reconstruction,args.exported)
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
