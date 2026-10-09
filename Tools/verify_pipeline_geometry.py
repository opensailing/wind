#!/usr/bin/env python3
"""Audit native pipeline geometry against source arrays and VTK contours.

Source samples, interpolation weights and VTK geometry are independent of the
native evaluator. Derived geometry is never treated as original CFD cells.
"""
import argparse
import itertools
import json
from pathlib import Path

import matplotlib.tri as tri
import numpy as np
import vtk
from vtk.util.numpy_support import numpy_to_vtk, numpy_to_vtkIdTypeArray, vtk_to_numpy

from verify_pipeline_fields import array, components, digest, source


def cells(indices):
    out = vtk.vtkCellArray()
    out.SetData(numpy_to_vtkIdTypeArray(np.arange(len(indices)+1, dtype=np.int64)*indices.shape[1], deep=True),
                numpy_to_vtkIdTypeArray(np.ascontiguousarray(indices, dtype=np.int64).ravel(), deep=True))
    return out


def dataset(positions, indices, values=None, tetra=False, lines=False):
    points = vtk.vtkPoints()
    points.SetData(numpy_to_vtk(np.ascontiguousarray(positions), deep=True))
    out = vtk.vtkUnstructuredGrid() if tetra else vtk.vtkPolyData()
    out.SetPoints(points)
    if tetra:
        out.SetCells(vtk.VTK_TETRA, cells(indices))
    elif lines:
        out.SetLines(cells(indices))
    else:
        out.SetPolys(cells(indices))
    if values is not None:
        scalars = numpy_to_vtk(np.ascontiguousarray(values), deep=True)
        scalars.SetName('scalar')
        out.GetPointData().SetScalars(scalars)
    return out


def contour(mesh, level):
    operation = vtk.vtkContourFilter()
    operation.SetInputData(mesh)
    operation.SetValue(0, level)
    operation.ComputeNormalsOff()
    operation.SetOutputPointsPrecision(vtk.vtkAlgorithm.DOUBLE_PRECISION)
    operation.Update()
    out = vtk.vtkPolyData()
    out.DeepCopy(operation.GetOutput())
    return out


def clip(mesh, operations):
    for op in operations:
        if not op['enabled'] or op['kind'] != 2:
            continue
        for axis, side in itertools.product(range(3), range(2)):
            plane = vtk.vtkPlane()
            origin, normal = np.zeros(3), np.zeros(3)
            origin[axis] = op['b' if side else 'a'][axis]
            normal[axis] = -1 if side else 1
            plane.SetOrigin(origin)
            plane.SetNormal(normal)
            operation = vtk.vtkClipPolyData()
            operation.SetInputData(mesh)
            operation.SetClipFunction(plane)
            operation.SetOutputPointsPrecision(vtk.vtkAlgorithm.DOUBLE_PRECISION)
            operation.Update()
            result = vtk.vtkPolyData()
            result.DeepCopy(operation.GetOutput())
            mesh = result
    return mesh


def triangles(mesh):
    f = vtk.vtkTriangleFilter()
    f.SetInputData(mesh)
    f.Update()
    d = f.GetOutput()
    vertices = vtk_to_numpy(d.GetPoints().GetData())
    faces = vtk_to_numpy(d.GetPolys().GetConnectivityArray()).reshape((-1, 3))
    return vertices, faces


def measure(mesh, lines):
    positions = vtk_to_numpy(mesh.GetPoints().GetData())
    if lines:
        indices = vtk_to_numpy(mesh.GetLines().GetConnectivityArray()).reshape((-1, 2))
        return np.linalg.norm(positions[indices[:, 1]]-positions[indices[:, 0]], axis=1).sum()
    positions, indices = triangles(mesh)
    p = positions[indices]
    return np.linalg.norm(np.cross(p[:, 1]-p[:, 0], p[:, 2]-p[:, 0]), axis=1).sum()/2


def surface_distance(points, target):
    locator = vtk.vtkStaticCellLocator()
    locator.SetDataSet(target)
    locator.BuildLocator()
    result = 0.
    closest = [0., 0., 0.]
    cell, sub, squared = vtk.reference(0), vtk.reference(0), vtk.reference(0.)
    for p in points:
        locator.FindClosestPoint(p, closest, cell, sub, squared)
        result = max(result, float(squared))
    return result**.5


def compare_geometry(native, reference, lines):
    assert native.GetNumberOfCells() and reference.GetNumberOfCells()
    p = np.unique(vtk_to_numpy(native.GetPoints().GetData()), axis=0)
    q = np.unique(vtk_to_numpy(reference.GetPoints().GetData()), axis=0)
    forward, backward = surface_distance(p, reference), surface_distance(q, native)
    a, b = float(measure(native, lines)), float(measure(reference, lines))
    assert max(forward, backward) < 2e-10, (forward, backward)
    assert abs(a-b) <= max(a, b)*1e-8, (a, b)
    return {'native_cells': native.GetNumberOfCells(), 'vtk_cells': reference.GetNumberOfCells(),
            'native_to_vtk_max_distance_m': forward, 'vtk_to_native_max_distance_m': backward,
            'native_measure': a, 'vtk_measure': b, 'measure_unit': 'm' if lines else 'm2'}


class Original:
    def __init__(self, samples, volume):
        self.parent = samples / ('Cylinder3D_ReaderFixture' if volume else 'NACA0018_ReaderFixture')
        self.path = samples / ('Cylinder3D_VolumeFixture' if volume else 'NACA0018_SurfaceFixture') / 'reconstruction.json'
        self.doc, self.points, self.ids = source(self.parent)
        self.reconstruction = json.loads(self.path.read_text())
        self.ordinal = 1
        self.vectors = components(self.parent, self.doc, self.ordinal)
        spec = next(v for v in self.doc['fields'] if v['id'] == 'pressure')
        self.pressure = array(self.parent, spec['array'], '<f8', (self.doc['frameCount'], self.doc['pointCount']))[self.ordinal]
        if volume:
            v = self.reconstruction
            assert v['sourceMetadataSHA256'] == digest(self.parent/'recording.json')
            self.dimensions = np.array(v['dimensions'])
            self.low, self.high = np.array(v['minimum']), np.array(v['maximum'])
            self.spacing = (self.high-self.low)/(self.dimensions-1)
            self.rows = array(self.path.parent, v['rows'], '<u4', (int(np.prod(self.dimensions)), 4))
            self.classes = array(self.path.parent, v['classification'], 'u1', (int(np.prod(self.dimensions)),))
            self.center, self.radius = np.array(v['solid']['centerXY']), v['solid']['radiusMeters']
        else:
            spec = self.reconstruction['triangles']
            self.faces = array(self.path.parent, spec, '<u4', tuple(spec['shape']))
            self.finder = tri.Triangulation(self.points[:, 0], self.points[:, 1], self.faces).get_trifinder()

    def values(self, field):
        return self.pressure if field == 'pressure' else self.vectors

    @staticmethod
    def scalar(values):
        return values if values.ndim == 1 else np.linalg.norm(values, axis=-1)

    def grid_values(self, node_ids, field):
        d = self.dimensions
        nodes = np.column_stack((node_ids % d[0], node_ids//d[0] % d[1], node_ids//(d[0]*d[1])))
        original_rows = self.rows[node_ids]
        assert np.all(original_rows < len(self.points))
        tetrahedra = self.points[original_rows]
        matrix = np.concatenate((np.transpose(tetrahedra, (0, 2, 1)), np.ones((len(nodes), 1, 4))), axis=1)
        target = np.column_stack((self.low+nodes*self.spacing, np.ones(len(nodes))))
        weights = np.linalg.solve(matrix, target[..., None])[..., 0]
        values = self.values(field)[original_rows]
        return np.einsum('ij,ij->i', weights, values) if field == 'pressure' else np.einsum('ij,ijk->ik', weights, values)

    def sample(self, positions, field):
        p = positions[:, [0, 2, 1]]
        if self.doc['spatialDimensions'] == 2:
            assert np.all(p[:, 2] == 0)
            faces = self.finder(p[:, 0], p[:, 1])
            assert np.all(faces >= 0)
            rows = self.faces[faces]
            xyz = self.points[rows]
            matrix = np.stack((xyz[:, 1]-xyz[:, 0], xyz[:, 2]-xyz[:, 0]), axis=2)
            ab = np.linalg.solve(matrix, (p[:, :2]-xyz[:, 0])[..., None])[..., 0]
            weights = np.column_stack((1-ab.sum(axis=1), ab))
            values = self.values(field)[rows]
            sampled = np.einsum('ij,ij->i', weights, values) if field == 'pressure' else np.einsum('ij,ijk->ik', weights, values)
        else:
            assert np.all(p >= self.low) and np.all(p <= self.high)
            grid = (p-self.low)/self.spacing
            cell = np.minimum(np.floor(grid).astype(int), self.dimensions-2)
            fraction = grid-cell
            offsets = np.array([[x,y,z] for z in (0,1) for y in (0,1) for x in (0,1)])
            nodes = cell[:, None]+offsets
            ids = nodes[:, :, 0]+self.dimensions[0]*(nodes[:, :, 1]+self.dimensions[1]*nodes[:, :, 2])
            assert np.all(self.classes[ids] == 1)
            minimum = self.low+cell*self.spacing
            near = np.clip(self.center, minimum[:, :2], minimum[:, :2]+self.spacing[:2])-self.center
            assert np.all(np.sum(near**2, axis=1) >= self.radius**2)
            flat = self.grid_values(ids.ravel(), field)
            values = flat.reshape((len(p), 8) if field == 'pressure' else (len(p), 8, 3))
            weights = np.prod(np.where(offsets, fraction[:, None], 1-fraction[:, None]), axis=2)
            sampled = np.einsum('ij,ij->i', weights, values) if field == 'pressure' else np.einsum('ij,ijk->ik', weights, values)
        return self.scalar(sampled)

    def volume_contour(self, field, level, ops):
        d = self.dimensions
        nodes = np.flatnonzero(self.classes == 1)
        values = np.full(len(self.classes), np.nan)
        for start in range(0, len(nodes), 8192):
            ids = nodes[start:start+8192]
            values[ids] = self.scalar(self.grid_values(ids, field))
        z,y,x = np.indices(tuple((d-1)[::-1]))
        corners = np.array([dx+d[0]*(dy+d[1]*dz) for dz in (0,1) for dy in (0,1) for dx in (0,1)])
        bases = (x+d[0]*(y+d[1]*z)).ravel()
        xyz = np.column_stack((x.ravel(),y.ravel(),z.ravel()))
        low = self.low+xyz*self.spacing
        near = np.clip(self.center, low[:, :2], low[:, :2]+self.spacing[:2])-self.center
        candidates = np.sum(near**2, axis=1) >= self.radius**2
        for corner in corners:
            candidates &= self.classes[bases+corner] == 1
        ids = bases[candidates, None]+corners
        samples = values[ids]
        ids = ids[(samples.min(axis=1) < level) & (samples.max(axis=1) >= level)]
        tetrahedra = np.array([[0,1,3,7],[0,3,2,7],[0,2,6,7],[0,6,4,7],[0,4,5,7],[0,5,1,7]])
        tets = ids[:, tetrahedra].reshape((-1,4))
        unique, inverse = np.unique(tets, return_inverse=True)
        q = np.column_stack((unique % d[0], unique//d[0] % d[1], unique//(d[0]*d[1])))
        points = (self.low+q*self.spacing)[:, [0,2,1]]
        return clip(contour(dataset(points, inverse.reshape((-1,4)), values[unique], tetra=True), level), ops)


def load(exported, name, original):
    stem = exported/name
    doc = json.loads(stem.with_suffix('.json').read_text(encoding='utf-8-sig'))
    assert doc['source'] == digest(original.parent/'recording.json')
    recipe = doc['recipes'][0]
    # Point-only sources explicitly have no reconstruction.
    if recipe['source']['interpolation']:
        assert doc['reconstruction'] == digest(original.path)
    else:
        assert not doc['reconstruction']
    frame = original.doc['frames'][doc['ordinal']]
    assert doc['ordinal'] == original.ordinal and doc['step'] == frame['index'] and doc['timeSeconds'] == frame['time']
    assert doc['byteOrder'] == 'little'
    vertices = np.fromfile(str(stem)+'-vertices.f64', dtype='<f8').reshape((-1,4))
    ids = np.fromfile(str(stem)+'-identity.i64', dtype='<i8').reshape((-1,2))
    faces = np.fromfile(str(stem)+'-triangles.i32', dtype='<i4').reshape((-1,3))
    lines = np.fromfile(str(stem)+'-lines.i32', dtype='<i4').reshape((-1,2))
    assert len(vertices) == len(ids) == doc['vertices'] and len(faces) == doc['triangles'] and len(lines) == doc['lines']
    assert np.isfinite(vertices).all()
    for indices in (faces, lines):
        if len(indices):assert indices.min() >= 0 and indices.max() < len(vertices)
    return doc, vertices, ids, faces, lines


def verify(samples, exported):
    naca, cylinder = Original(samples, False), Original(samples, True)
    reports = []
    for name in ('naca-surface','naca-points','naca-contour','naca-magnitude-contour','naca-slice-input','naca-slice-contour','cylinder-contour','cylinder-magnitude-contour','cylinder-slice'):
        original = cylinder if name.startswith('cylinder') else naca
        doc, vertices, ids, faces, lines = load(exported, name, original)
        ops = doc['recipes'][0]['operations']
        field = doc['field']
        report = {'case':name, 'vertices':len(vertices), 'triangles':len(faces), 'lines':len(lines)}
        contour_op = next((o for o in ops if o['enabled'] and o['kind'] == 4), None)
        for o in ops:
            if o['enabled'] and o['kind'] == 2:
                assert np.all(vertices[:, :3] >= np.array(o['a'])-1e-12) and np.all(vertices[:, :3] <= np.array(o['b'])+1e-12)
            if o['enabled'] and o['kind'] == 3:
                assert np.max(np.abs((vertices[:, :3]-o['a'])@np.array(o['b']))) < 1e-10
        if contour_op:
            assert np.all(vertices[:, 3] == contour_op['value']) and np.all(ids[:, 0] == -1)
            if original is cylinder:
                reference = original.volume_contour(field, contour_op['value'], ops)
            else:
                if any(o['enabled'] and o['kind'] == 3 for o in ops):
                    _, input_vertices, _, input_faces, _ = load(exported, 'naca-slice-input', naca)
                    upstream = dataset(input_vertices[:, :3], input_faces, input_vertices[:, 3])
                else:
                    positions = np.column_stack((naca.points[:, 0], np.zeros(len(naca.points)), naca.points[:, 1]))
                    upstream = dataset(positions, naca.faces, naca.scalar(naca.values(field)))
                reference = clip(contour(upstream, contour_op['value']), ops)
            native = dataset(vertices[:, :3], lines if len(lines) else faces, lines=bool(len(lines)))
            report.update(compare_geometry(native, reference, bool(len(lines))))
        else:
            rows = ids[:, 0]
            keep = rows >= 0
            assert np.array_equal(ids[keep, 1], original.ids[rows[keep]])
            p = original.points[rows[keep]]
            position = np.column_stack((p[:,0],np.zeros(len(p)),p[:,1])) if p.shape[1] == 2 else p[:,[0,2,1]]
            assert np.array_equal(vertices[keep,:3], position)
            assert np.array_equal(vertices[keep,3], original.scalar(original.values(field))[rows[keep]])
            if name == 'naca-points':
                inside = np.ones(len(naca.points),bool)
                positions = np.column_stack((naca.points[:,0],np.zeros(len(naca.points)),naca.points[:,1]))
                for o in ops:
                    if o['enabled'] and o['kind']==2:inside &= np.all((positions >= o['a']) & (positions <= o['b']),axis=1)
                assert np.array_equal(rows, np.flatnonzero(inside))
                report['original_rows_exact']=int(keep.sum())
            error=0.
            selected=np.flatnonzero(~keep)
            for start in range(0,len(selected),2048):
                indices=selected[start:start+2048]
                expected=original.sample(vertices[indices,:3],field)
                error=max(error,float(np.max(np.abs(expected-vertices[indices,3]))))
            scale=max(float(np.max(np.abs(vertices[:,3]))),1e-300)
            assert error <= scale*1e-8, (name,error,scale)
            report['independent_sample_max_error']=error
        reports.append(report)
    return {'cases':reports,'vtk_version':vtk.vtkVersion.GetVTKVersion(),'verifier_sha256':digest(__file__),
            'source_hashes':[digest(naca.parent/'recording.json'),digest(cylinder.parent/'recording.json')],
            'reconstruction_hashes':[digest(naca.path),digest(cylinder.path)],
            'scope':'Native pipeline output and source-array numerical readback with independent VTK contour geometry; no native UI or long-session acceptance.'}


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('samples',type=Path)
    parser.add_argument('exported',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    result=verify(args.samples,args.exported)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
