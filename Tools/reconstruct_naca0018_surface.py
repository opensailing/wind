#!/usr/bin/env python3
"""Build a labelled visualization reconstruction from the audited NACA points.

This creates supplemental triangle indices, not CFD output or original mesh
connectivity. Source fields, points, times and recording.json remain unchanged.
Requires NumPy, h5py and Matplotlib in the offline conversion environment.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile

import h5py
import matplotlib
import matplotlib.tri as mtri
import numpy as np

SOURCE_SHA256 = '513e30f370ecdd30c5a49dfcc7b9b0caf39204b6dec08ee513a53afeba657237'
DESCRIPTOR_SHA256 = '1ce4f9f4a7d71f060e60e62ecd0aa52de78e7f7d0328930ef0cdc952cd852a67'


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def cross(a, b):
    return a[..., 0] * b[..., 1] - a[..., 1] * b[..., 0]


def inside_polygon(points, polygon):
    """Even/odd test; callers separately handle polygon vertices and edges."""
    inside = np.zeros(len(points), dtype=bool)
    for a, b in zip(polygon, np.roll(polygon, -1, axis=0)):
        if a[1] == b[1]:
            continue
        crosses = (a[1] > points[:, 1]) != (b[1] > points[:, 1])
        x = a[0] + (points[:, 1] - a[1]) * (b[0] - a[0]) / (b[1] - a[1])
        inside ^= crosses & (points[:, 0] < x)
    return inside


def mesh_edges(triangles):
    return np.unique(np.sort(np.concatenate((triangles[:, [0, 1]], triangles[:, [1, 2]],
                                            triangles[:, [2, 0]])), axis=1), axis=0, return_counts=True)


def triangulate_fluid_surface(coordinates, boundary):
    """Validate a single supplied hole and return original-row triangle indices.

    Deliberately fails if unconstrained Delaunay does not retain every hole edge.
    It never bridges a solid, moves a source point or repairs a supplied boundary.
    """
    xy = np.asarray(coordinates, dtype=np.float64)
    boundary = np.asarray(boundary)
    if xy.ndim != 2 or xy.shape[1] != 2 or not np.isfinite(xy).all() or len(xy) < 6:
        raise ValueError('Expected finite two-dimensional source coordinates')
    if len(np.unique(xy, axis=0)) != len(xy):
        raise ValueError('Duplicate source coordinates need an explicit policy')
    if (boundary.ndim != 1 or boundary.dtype.kind not in 'iu' or len(boundary) < 3 or
            len(np.unique(boundary)) != len(boundary) or boundary.min() < 0 or boundary.max() >= len(xy)):
        raise ValueError('Expected distinct valid original-row boundary indices')
    polygon = xy[boundary]
    signed_area = cross(polygon, np.roll(polygon, -1, axis=0)).sum() / 2
    if signed_area <= 0:
        raise ValueError('Boundary must be a counterclockwise nondegenerate loop')
    full = mtri.Triangulation(xy[:, 0], xy[:, 1])
    triangles = full.triangles
    edges, _ = mesh_edges(triangles)
    edge_set = {tuple(e) for e in edges.tolist()}
    hole_edges = np.sort(np.column_stack((boundary, np.roll(boundary, -1))), axis=1)
    if any(tuple(e) not in edge_set for e in hole_edges.tolist()):
        raise ValueError('Delaunay does not preserve every boundary edge; constrained reconstruction required')
    # Existing Delaunay edges form a planar graph; a simple degree-two cycle is
    # non-self-intersecting. Reject non-boundary points enclosed by that cycle.
    interior = inside_polygon(xy, polygon)
    interior[boundary] = False
    if interior.any():
        raise ValueError('Candidate solid contains additional source samples')
    solid = inside_polygon(xy[triangles].mean(axis=1), polygon)
    fluid = triangles[~solid]
    if len(fluid) == 0 or solid.sum() != len(boundary) - 2:
        raise ValueError('Expected a fully triangulated simple solid hole')
    v = xy[fluid]
    determinants = cross(v[:, 1] - v[:, 0], v[:, 2] - v[:, 0])
    if (determinants <= 0).any():
        raise ValueError('Invalid or inverted fluid triangle')
    used = np.unique(fluid)
    if not np.array_equal(used, np.arange(len(xy))):
        raise ValueError('Reconstruction would discard original source points')
    fluid_edges, edge_counts = mesh_edges(fluid)
    counts = {tuple(e): int(n) for e, n in zip(fluid_edges.tolist(), edge_counts)}
    if any(counts.get(tuple(e)) != 1 for e in hole_edges.tolist()):
        raise ValueError('Solid boundary does not have exactly one fluid neighbour')
    if edge_counts.max() > 2 or len(xy) - len(fluid_edges) + len(fluid) != 0:
        raise ValueError('Expected a manifold surface with exactly one hole')
    removed = xy[triangles[solid]]
    removed_area = cross(removed[:, 1] - removed[:, 0], removed[:, 2] - removed[:, 0]).sum() / 2
    if not np.isclose(removed_area, signed_area, rtol=1e-11, atol=1e-15):
        raise ValueError('Removed triangles do not match the candidate solid area')
    stats = {'sourcePointCount': len(xy), 'triangleCount': len(fluid), 'boundaryPointCount': len(boundary),
             'removedSolidTriangles': int(solid.sum()), 'solidPolygonAreaM2': float(signed_area),
             'removedTriangleAreaM2': float(removed_area), 'minimumTriangleAreaM2': float(determinants.min() / 2),
             'eulerCharacteristic': 0, 'originalPointsRetained': True, 'boundaryFluidNeighbours': 1}
    return np.asarray(fluid, dtype='<u4'), stats


def write_json(path, obj):
    path.write_text(json.dumps(obj, indent=2, ensure_ascii=False) + '\n')


def write_indices(path, values):
    values = np.asarray(values, dtype='<u4')
    values.tofile(path)
    return {'path': path.name, 'dtype': 'uint32', 'byteOrder': 'little', 'shape': list(values.shape),
            'byteLength': path.stat().st_size, 'sha256': sha256(path)}


def reconstruct(original, descriptor_path, output):
    original, descriptor_path, output = Path(original), Path(descriptor_path), Path(output)
    if output.exists():
        raise ValueError('Choose a new output directory; existing results are never overwritten')
    if sha256(descriptor_path) != DESCRIPTOR_SHA256 or sha256(original) != SOURCE_SHA256:
        raise ValueError('Expected the exact audited original HDF5 and full 8000-frame v3 descriptor')
    descriptor = json.loads(descriptor_path.read_text())
    coordinates_path = descriptor_path.parent / descriptor['coordinates']['path']
    ids_path = descriptor_path.parent / descriptor['pointIds']['path']
    if sha256(coordinates_path) != descriptor['coordinates']['sha256'] or sha256(ids_path) != descriptor['pointIds']['sha256']:
        raise ValueError('Converted coordinates or point IDs differ from the pinned recording')
    with h5py.File(original, 'r') as h:
        xy = h['coordinates/block0_values'][:]
        converted = np.fromfile(coordinates_path, dtype='<f8').reshape(xy.shape)
        if not np.array_equal(xy, converted):
            raise ValueError('Coordinate conversion differs from original source')
        candidates = np.flatnonzero(h['velocity_magnitude/block0_values'][:, 0] == 0)
        if len(candidates) != 56:
            raise ValueError('Unexpected candidate wall set in audited source')
        checks = {}
        for field in ('velocity_u', 'velocity_v', 'velocity_magnitude'):
            values = h[field + '/block0_values'][candidates, :]
            if values.shape != (56, 8000) or np.count_nonzero(values):
                raise ValueError('Candidate wall points are not stationary zero-velocity samples throughout the source')
            checks[field] = {'checkedValues': int(values.size), 'nonzeroValues': 0}
    center = xy[candidates].mean(axis=0)
    angles = np.arctan2(xy[candidates, 1] - center[1], xy[candidates, 0] - center[0])
    boundary = candidates[np.argsort(angles)]
    triangles, stats = triangulate_fluid_surface(xy, boundary)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix='.surface-', dir=output.parent))
    try:
        manifest = {
            'version': 1, 'kind': 'spatial_reconstruction',
            'title': 'NACA 0018 · reconstructed 2D display surface',
            'source': {'recordingId': descriptor['id'], 'descriptorSHA256': DESCRIPTOR_SHA256,
                       'originalHDF5SHA256': SOURCE_SHA256, 'coordinatesSHA256': descriptor['coordinates']['sha256'],
                       'pointIdsSHA256': descriptor['pointIds']['sha256'], 'sourceURL': descriptor['sourceURL'],
                       'pointCount': descriptor['pointCount'], 'frameCount': descriptor['frameCount']},
            'spatialDimensions': 2, 'coordinateUnit': 'm', 'association': 'original_source_row',
            'origin': 'reconstructed', 'method': 'Delaunay with verified stationary-sample hole; piecewise-linear field interpolation',
            'boundaryOrigin': 'Inferred loop through 56 original samples with zero velocity at all 8000 times; no original patch identity supplied',
            'triangles': write_indices(temporary / 'triangles.u32', triangles),
            'solidBoundary': write_indices(temporary / 'solid-boundary.u32', boundary),
            'checks': {**stats, 'candidateAllFrameVelocity': checks},
            'tool': {'name': Path(__file__).name, 'sha256': sha256(__file__), 'numpy': np.__version__, 'matplotlib': matplotlib.__version__},
            'limitations': ['Visualization topology is reconstructed, not the original CFD mesh.',
                            'Solid association is inferred from source coordinates and persistent zero-velocity samples.',
                            'Interpolation between source points is approximate; no extrapolation outside the reconstruction.',
                            'No spanwise field, density, force history or additional physical-time frames are supplied.'],
            'acceptance': 'Converter output only; native reader, rendering and UI acceptance are separate.',
        }
        write_json(temporary / 'reconstruction.json', manifest)
        (temporary / 'ATTRIBUTION.txt').write_text(
            (descriptor_path.parent / 'ATTRIBUTION.txt').read_text() +
            '\nSupplemental display reconstruction: Delaunay triangles reference unchanged source rows. '
            'The inferred no-slip boundary and triangle connectivity are not author-supplied mesh data. '
            'No CFD values or time steps have been generated.\n')
        temporary.rename(output)
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original_hdf5', type=Path)
    parser.add_argument('recording_json', type=Path)
    parser.add_argument('output_directory', type=Path)
    args = parser.parse_args()
    result = reconstruct(args.original_hdf5, args.recording_json, args.output_directory)
    print(json.dumps(result['checks'], indent=2))
