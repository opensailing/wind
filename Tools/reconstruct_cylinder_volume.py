#!/usr/bin/env python3
"""Explicit display reconstruction of audited 3D Fluent cylinder samples.

No flow values are generated. Delaunay tetrahedra select original rows and
barycentric weights for a uniform display grid. Tetrahedra intersecting the
author's solid cylinder or exceeding the chosen support length are excluded.
The output is an optional, source-hash-bound reconstruction, not a CFD mesh.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile

import numpy as np
from scipy.spatial import Delaunay
from vtkmodules.vtkCommonCore import vtkIdList,vtkPoints,vtkVersion
from vtkmodules.vtkCommonDataModel import vtkCellArray,vtkPolyData,vtkStaticCellLocator,vtkUnstructuredGrid
from vtkmodules.vtkFiltersCore import vtkProbeFilter
from vtkmodules.util.numpy_support import numpy_to_vtk,numpy_to_vtkIdTypeArray,vtk_to_numpy
from vtkmodules.util.vtkConstants import VTK_TETRA


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


class QualifiedLocator:
    """Find only supported original-coordinate tetrahedra, without snapping."""
    def __init__(self,triangulation,supported):
        ids=np.flatnonzero(supported)
        if not len(ids):raise ValueError('Insufficient supported fluid tetrahedra outside the solid')
        points=vtkPoints();points.SetData(numpy_to_vtk(np.ascontiguousarray(triangulation.points),deep=True))
        cells=vtkCellArray()
        cells.SetData(numpy_to_vtkIdTypeArray(np.arange(len(ids)+1,dtype=np.int64)*4,deep=True),
                      numpy_to_vtkIdTypeArray(np.ascontiguousarray(triangulation.simplices[ids],dtype=np.int64).ravel(),deep=True))
        self.source=vtkUnstructuredGrid();self.source.SetPoints(points);self.source.SetCells(VTK_TETRA,cells)
        identity=numpy_to_vtk(ids.astype(np.int64),deep=True);identity.SetName('tetrahedron_id');self.source.GetCellData().AddArray(identity)
        self.locator=vtkStaticCellLocator();self.locator.SetDataSet(self.source);self.locator.BuildLocator()
        self.supported=supported
        self.ids=ids
        self.transforms=triangulation.transform
        self.fallback_queries=0
        self.fallback_recovered=0

    def find(self,positions):
        points=vtkPoints();points.SetData(numpy_to_vtk(np.ascontiguousarray(positions),deep=True))
        queries=vtkPolyData();queries.SetPoints(points)
        probe=vtkProbeFilter();probe.SetInputData(queries);probe.SetSourceData(self.source);probe.SetCellLocator(self.locator)
        probe.ComputeToleranceOff();probe.SetTolerance(0.);probe.SnapToCellWithClosestPointOff();probe.Update()
        output=probe.GetOutput().GetPointData();valid=vtk_to_numpy(output.GetArray('vtkValidPointMask')).astype(bool)
        result=vtk_to_numpy(output.GetArray('tetrahedron_id')).astype(np.int64)
        if np.any(result[valid]<0) or np.any(result[valid]>=len(self.supported)) or not self.supported[result[valid]].all():
            raise ValueError('Spatial locator returned unsupported topology')
        result[~valid]=-1
        # vtkTetra::EvaluatePosition has a fixed 0.001 parametric tolerance,
        # even when vtkProbeFilter's spatial tolerance is zero. A first hit
        # can therefore be the adjacent tetrahedron, with negative weights.
        # Use VTK only as a spatial index; containment is checked here in
        # double precision. Search all bounding-box candidates on rejection.
        selected=np.flatnonzero(valid)
        transform=self.transforms[result[selected]]
        bary=np.einsum('nij,nj->ni',transform[:,:3],positions[selected]-transform[:,3])
        bary=np.column_stack((bary,1-bary.sum(axis=1)))
        exact=np.isfinite(bary).all(axis=1)&(bary>=0).all(axis=1)&(bary<=1).all(axis=1)
        result[selected[~exact]]=-1
        candidates=vtkIdList()
        for row in np.flatnonzero(result<0):
            self.fallback_queries+=1
            p=positions[row]
            self.locator.FindCellsWithinBounds((p[0],p[0],p[1],p[1],p[2],p[2]),candidates)
            if not candidates.GetNumberOfIds():continue
            local=np.fromiter((candidates.GetId(i) for i in range(candidates.GetNumberOfIds())),dtype=np.int64)
            ids=np.sort(self.ids[local])
            transform=self.transforms[ids]
            bary=np.einsum('nij,nj->ni',transform[:,:3],p-transform[:,3])
            bary=np.column_stack((bary,1-bary.sum(axis=1)))
            exact=np.isfinite(bary).all(axis=1)&(bary>=0).all(axis=1)&(bary<=1).all(axis=1)
            if exact.any():
                result[row]=ids[np.flatnonzero(exact)[0]]
                self.fallback_recovered+=1
        return result


def cylinder_clearance(vertices):
    """Exact distance from the origin to each tetrahedron's XY projection.

    A tetrahedron intersects an infinite axial cylinder iff its projected
    convex hull has distance less than the radius. Every hull edge is among
    these six segments; an origin inside the hull lies in at least one of the
    four projected triangles. Work in batches to bound temporary arrays.
    """
    xy = np.asarray(vertices)[..., :2]
    minimum_squared = np.full(len(xy), np.inf)
    for i in range(4):
        for j in range(i+1, 4):
            a, edge = xy[:, i], xy[:, j]-xy[:, i]
            length = np.sum(edge*edge, axis=1)
            t = np.divide(-np.sum(a*edge, axis=1), length,
                          out=np.zeros(len(xy)), where=length > 0)
            nearest = a + np.clip(t, 0, 1)[:, None]*edge
            minimum_squared = np.minimum(minimum_squared, np.sum(nearest*nearest, axis=1))
    def cross(a, b):
        return a[:, 0]*b[:, 1]-a[:, 1]*b[:, 0]
    for i, j, k in ((0,1,2), (0,1,3), (0,2,3), (1,2,3)):
        a,b,c=xy[:, i],xy[:, j],xy[:, k]
        area=cross(b-a,c-a)
        sides=np.stack((cross(b-a,-a),cross(c-b,-b),cross(a-c,-c)),axis=1)
        inside=(np.all(sides>=0,axis=1)|np.all(sides<=0,axis=1)) & (np.abs(area)>1e-24)
        minimum_squared[inside]=0
    return np.sqrt(minimum_squared)


def build_mapping(xyz, dimensions, radius, maximum_edge):
    xyz=np.asarray(xyz,dtype=np.float64)
    dimensions=np.asarray(dimensions,dtype=np.int64)
    if xyz.ndim!=2 or xyz.shape[1]!=3 or not np.isfinite(xyz).all():
        raise ValueError('Finite original 3D coordinates required')
    if dimensions.shape!=(3,) or np.any(dimensions<2) or np.any(dimensions>512) or np.prod(dimensions)>2*1024*1024:
        raise ValueError('Display grid exceeds native limits')
    if not np.isfinite(radius) or radius<=0 or not np.isfinite(maximum_edge) or maximum_edge<=0:
        raise ValueError('Explicit positive cylinder radius and support limit required')
    # Published wall-adjacent rows can lie just inside the analytic circle
    # (the original boundary connectivity is absent). Preserve those rows;
    # the conservative tetrahedron test excludes them from display stencils.
    inside_original=int(np.count_nonzero(np.linalg.norm(xyz[:,:2],axis=1)<radius))
    lower,upper=xyz.min(axis=0),xyz.max(axis=0)
    if np.any(upper<=lower):raise ValueError('Source does not span three dimensions')
    # Preserve original coordinates: no QJ coordinate jitter is permitted.
    triangulation=Delaunay(xyz,qhull_options='Qbb Qc Qz Q12')
    print(f'Derived {len(triangulation.simplices)} tetrahedra from {len(xyz)} original rows',flush=True)
    supported=np.zeros(len(triangulation.simplices),dtype=bool)
    cylinder_rejected=long_rejected=unstable_rejected=0
    for start in range(0,len(supported),32768):
        end=min(start+32768,len(supported))
        vertices=xyz[triangulation.simplices[start:end]]
        clear=cylinder_clearance(vertices)>=radius
        edge=np.zeros(end-start)
        for i in range(4):
            for j in range(i+1,4):edge=np.maximum(edge,np.linalg.norm(vertices[:,i]-vertices[:,j],axis=1))
        short=edge<=maximum_edge
        singular=np.linalg.svd(vertices[:,:3]-vertices[:,3:4],compute_uv=False)
        condition=np.divide(singular[:,0],singular[:,-1],out=np.full(end-start,np.inf),where=singular[:,-1]>0)
        stable=(condition<=1.e8)&np.isfinite(triangulation.transform[start:end]).all(axis=(1,2))
        supported[start:end]=clear & short & stable
        cylinder_rejected+=int(np.count_nonzero(~clear))
        long_rejected+=int(np.count_nonzero(~short))
        unstable_rejected+=int(np.count_nonzero(~stable))
    print(f'Qualified {int(supported.sum())} tetrahedra; building indexed lookup',flush=True)
    locator=QualifiedLocator(triangulation,supported)
    count=int(np.prod(dimensions))
    rows=np.full((count,4),np.iinfo(np.uint32).max,dtype='<u4')
    weights=np.zeros((count,4),dtype='<f8')
    classes=np.zeros(count,dtype=np.uint8)
    coordinate_error=0.
    for start in range(0,count,32768):
        end=min(start+32768,count)
        index=np.arange(start,end,dtype=np.int64)
        grid=np.stack((index%dimensions[0],index//dimensions[0]%dimensions[1],index//(dimensions[0]*dimensions[1])),axis=1)
        positions=lower+grid/(dimensions-1)*(upper-lower)
        solid=np.linalg.norm(positions[:,:2],axis=1)<radius
        simplex=locator.find(positions)
        valid=(simplex>=0)&~solid
        valid[valid] &= supported[simplex[valid]]
        candidates=np.flatnonzero(valid)
        transform=triangulation.transform[simplex[candidates]]
        bary=np.einsum('nij,nj->ni',transform[:,:3],positions[candidates]-transform[:,3])
        bary=np.column_stack((bary,1-bary.sum(axis=1)))
        # Negative barycentrics can occur at a floating-point hull boundary.
        # Reject those nodes; do not extrapolate or silently clamp weights.
        keep=np.isfinite(bary).all(axis=1)&(bary>=0).all(axis=1)&(bary<=1).all(axis=1)
        candidates=candidates[keep];bary=bary[keep]
        selected=triangulation.simplices[simplex[candidates]]
        rows[start+candidates]=selected
        weights[start+candidates]=bary
        classes[start+candidates]=1
        classes[start+np.flatnonzero(solid)]=2
        if len(candidates):
            actual=np.einsum('ni,nij->nj',bary,xyz[selected])
            coordinate_error=max(coordinate_error,float(np.max(np.abs(actual-positions[candidates]))))
        if start%262144==0:print(f'Mapped {end}/{count} display nodes',flush=True)
    report={'method':'3D Delaunay barycentric interpolation; tetrahedra crossing the analytic cylinder, exceeding maximum support length or with unstable transforms excluded.',
            'original_points':len(xyz),'tetrahedra':len(supported),'supported_tetrahedra':int(supported.sum()),
            'original_rows_inside_analytic_solid_excluded_from_interpolation':inside_original,
            'cylinder_rejected_tetrahedra':cylinder_rejected,'long_rejected_tetrahedra':long_rejected,
            'unstable_rejected_tetrahedra':unstable_rejected,'condition_limit':1.e8,
            'qhull_options':'Qbb Qc Qz Q12','coplanar_omitted_points':len(triangulation.coplanar),
            'locator':'vtkStaticCellLocator candidate lookup with strict double-precision barycentric containment and bounding-box candidate fallback; no extrapolation, snapping or clamping',
            'containment_fallback_queries':locator.fallback_queries,
            'containment_fallback_recovered':locator.fallback_recovered,
            'vtk_version':vtkVersion.GetVTKVersion(),
            'fluid_nodes':int(np.count_nonzero(classes==1)),'solid_nodes':int(np.count_nonzero(classes==2)),
            'unsupported_nodes':int(np.count_nonzero(classes==0)),
            'maximum_coordinate_reproduction_error_m':coordinate_error,
            'cylinder_radius_m':radius,'maximum_tetrahedron_edge_m':maximum_edge}
    if report['fluid_nodes']<8:raise ValueError('Insufficient supported fluid nodes outside the solid')
    return rows,weights,classes,lower,upper,report


def reconstruct(recording_path,output,dimensions,radius,maximum_edge):
    recording_path,output=Path(recording_path),Path(output)
    if output.exists():raise ValueError('Choose a new output folder; existing reconstructions are retained')
    source=json.loads(recording_path.read_text())
    if source['version']!=3 or source['spatialDimensions']!=3:raise ValueError('Original 3D point recording required')
    coordinates=source['coordinates'];member=recording_path.parent/coordinates['path']
    if member.parent!=recording_path.parent or digest(member)!=coordinates['sha256']:raise ValueError('Coordinate identity differs')
    xyz=np.fromfile(member,dtype='<f8').reshape((source['pointCount'],3))
    rows,weights,classes,lower,upper,report=build_mapping(xyz,dimensions,radius,maximum_edge)
    output.parent.mkdir(parents=True,exist_ok=True)
    stage=Path(tempfile.mkdtemp(prefix='.'+output.name+'-',dir=output.parent))
    def array(name,values,dtype):
        path=stage/name;values.tofile(path)
        return {'path':name,'dtype':dtype,'byteOrder':'little','byteLength':path.stat().st_size,'sha256':digest(path)}
    descriptor={'version':1,'kind':'volume_reconstruction','origin':'derived','layout':'x_fastest_node_grid',
                'title':'Cylinder wake · derived 3D display grid','sourceMetadataSHA256':digest(recording_path),
                'dimensions':list(map(int,dimensions)),'minimum':lower.tolist(),'maximum':upper.tolist(),
                'method':report['method'],'rows':array('rows.u32',rows,'uint32'),
                'weights':array('weights.f64',weights,'float64'),'classification':array('classification.u8',classes,'uint8'),
                'solid':{'kind':'cylinder_z','centerXY':[0,0],'radiusMeters':radius,
                         'origin':'Author setup geometry; grid classification is derived.'},
                'limitations':['Derived display topology; original Fluent connectivity was not supplied.',
                               'Author preprocessing already cropped and downsampled the original mesh.',
                               'Tetrahedra crossing the cylinder or exceeding support limits are excluded; unsupported regions remain empty.',
                               'Original rows inside the documented analytic cylinder are preserved for export but excluded from volume interpolation; original wall connectivity was not supplied.',
                               'Volume uses grid interpolation; original float64 rows remain available for quantitative export.',
                               'This converter alone does not accept native import, rendering or stability.']}
    report.update(source_metadata_sha256=digest(recording_path),converter_sha256=digest(__file__))
    (stage/'reconstruction-audit.json').write_text(json.dumps(report,indent=2)+'\n')
    (stage/'reconstruction.json').write_text(json.dumps(descriptor,indent=2)+'\n')
    os.rename(stage,output)
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('recording',type=Path)
    parser.add_argument('output',type=Path)
    parser.add_argument('--dimensions',nargs=3,type=int,default=[171,81,81])
    parser.add_argument('--radius',type=float,required=True)
    parser.add_argument('--maximum-edge',type=float,required=True)
    args=parser.parse_args()
    print(json.dumps(reconstruct(args.recording,args.output,args.dimensions,args.radius,args.maximum_edge),indent=2))
