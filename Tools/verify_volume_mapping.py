#!/usr/bin/env python3
"""Independent checks of a derived volume against its original point recording.

This does not call the converter's interpolation or geometry helpers. It solves
selected tetrahedra directly and compares converted scalar values with optional
original HDF5 columns. The full display map is checked in bounded batches.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from import_naca0018 import digest


def verify(recording_path,reconstruction_path,original_hdf=None):
    recording_path,reconstruction_path=Path(recording_path),Path(reconstruction_path)
    source=json.loads(recording_path.read_text());volume=json.loads(reconstruction_path.read_text())
    if volume['sourceMetadataSHA256']!=digest(recording_path):raise ValueError('Source identity differs')
    def read(parent,descriptor,dtype,shape):
        path=parent/descriptor['path']
        if path.parent!=parent or digest(path)!=descriptor['sha256']:raise ValueError('Array identity differs')
        if path.stat().st_size!=int(np.prod(shape))*np.dtype(dtype).itemsize:raise ValueError('Array length differs')
        return np.memmap(path,mode='r',dtype=dtype,shape=shape)
    count=int(np.prod(volume['dimensions']));n=source['pointCount']
    xyz=read(recording_path.parent,source['coordinates'],'<f8',(n,3))
    rows=read(reconstruction_path.parent,volume['rows'],'<u4',(count,4))
    weights=read(reconstruction_path.parent,volume['weights'],'<f8',(count,4))
    classes=read(reconstruction_path.parent,volume['classification'],'u1',(count,))
    dimensions=np.array(volume['dimensions']);lower=np.array(volume['minimum']);upper=np.array(volume['maximum'])
    radius=volume['solid']['radiusMeters'];center=np.array(volume['solid']['centerXY'])
    coordinate_error=weight_error=0.
    fluid_count=0
    for start in range(0,count,16384):
        end=min(start+16384,count);ids=np.arange(start,end)
        lattice=np.column_stack((ids%dimensions[0],ids//dimensions[0]%dimensions[1],ids//(dimensions[0]*dimensions[1])))
        positions=lower+lattice*(upper-lower)/(dimensions-1)
        mask=classes[start:end];w=weights[start:end];r=rows[start:end]
        if np.any(mask>2):raise ValueError('Unknown classification')
        if not np.array_equal(mask==2,np.linalg.norm(positions[:,:2]-center,axis=1)<radius):raise ValueError('Solid mask differs')
        if np.any(w[mask!=1]!=0) or np.any(r[mask!=1]!=np.iinfo(np.uint32).max):raise ValueError('Masked source access')
        fluid=mask==1;fluid_count+=int(fluid.sum())
        if not fluid.any():continue
        if np.any(r[fluid]>=n) or np.any(w[fluid]<0) or not np.isfinite(w[fluid]).all():raise ValueError('Invalid stencil')
        weight_error=max(weight_error,float(np.max(np.abs(w[fluid].sum(axis=1)-1))))
        result=np.sum(xyz[r[fluid]]*w[fluid,:,None],axis=1)
        coordinate_error=max(coordinate_error,float(np.max(np.abs(result-positions[fluid]))))
    if coordinate_error>1e-9 or weight_error>1e-10:raise ValueError('Mapping does not reproduce coordinates')
    fluid=np.flatnonzero(classes==1)
    selected=fluid[np.linspace(0,len(fluid)-1,min(1024,len(fluid)),dtype=int)]
    original_rows=rows[selected]
    lattice=np.column_stack((selected%dimensions[0],selected//dimensions[0]%dimensions[1],selected//(dimensions[0]*dimensions[1])))
    positions=lower+lattice*(upper-lower)/(dimensions-1)
    vertices=xyz[original_rows]
    # Independent linear system, with no scipy triangulation transform reuse.
    matrices=np.concatenate((np.transpose(vertices,(0,2,1)),np.ones((len(selected),1,4))),axis=1)
    targets=np.column_stack((positions,np.ones(len(selected))))
    independent=np.linalg.solve(matrices,targets[...,None])[...,0]
    barycentric_error=float(np.max(np.abs(independent-weights[selected])))
    if barycentric_error>1e-8:raise ValueError('Independent barycentric solve differs')
    # Interior cell centers validate the native trilinear step independently of
    # the tetrahedral grid-node reconstruction. All eight nodes must be fluid.
    mask=(classes==1).reshape(tuple(dimensions[::-1]))
    valid=mask[:-1,:-1,:-1].copy()
    for dz in (0,1):
        for dy in (0,1):
            for dx in (0,1):valid &= mask[dz:dz+dimensions[2]-1,dy:dy+dimensions[1]-1,dx:dx+dimensions[0]-1]
    cz,cy,cx=np.nonzero(valid);cells=np.column_stack((cx,cy,cz))
    spacing=(upper-lower)/(dimensions-1);cell_min=lower+cells*spacing
    nearest=np.maximum(cell_min[:,:2],np.minimum(center,cell_min[:,:2]+spacing[:2]))-center
    cells=cells[np.sum(nearest*nearest,axis=1)>=radius*radius]
    if not len(cells):raise ValueError('No supported interpolation cells')
    cells=cells[np.linspace(0,len(cells)-1,min(16,len(cells)),dtype=int)]
    offsets=np.array([[dx,dy,dz] for dz in (0,1) for dy in (0,1) for dx in (0,1)])
    nodes=cells[:,None,:]+offsets
    indices=nodes[:,:,0]+dimensions[0]*(nodes[:,:,1]+dimensions[1]*nodes[:,:,2])
    node_positions=lower+nodes*spacing
    vertex_rows=rows[indices];vertex_xyz=xyz[vertex_rows]
    systems=np.concatenate((np.transpose(vertex_xyz,(0,1,3,2)),np.ones((len(cells),8,1,4))),axis=2)
    targets=np.concatenate((node_positions,np.ones((len(cells),8,1))),axis=2)
    independent_cell_weights=np.linalg.solve(systems,targets[...,None])[...,0]
    query_positions=lower+(cells+.5)*spacing
    queries=[];source_values_checked=0;sample_error=0.
    frame_ordinals=sorted(set([0,len(source['frames'])//2,len(source['frames'])-1]))
    store=None;original_sha=None
    if original_hdf:
        import h5py
        original_sha=digest(original_hdf)
        provenance=json.loads((recording_path.parent/'provenance.json').read_text())
        if original_sha!=provenance['originalMemberSHA256']:raise ValueError('Original published HDF5 identity differs')
        store=h5py.File(original_hdf,mode='r')
        if not np.array_equal(store['coordinates/block0_values'][:],xyz):raise ValueError('Original coordinates differ')
        labels=[x.decode('ascii') for x in store['velocity_u/axis0'][:]]
    try:
        for field in source['fields']:
            static=field['static'];shape=(n,) if static else (len(source['frames']),n)
            values=read(recording_path.parent,field['array'],'<f8',shape)
            originals={}
            if store is not None:
                checked_ordinals=[0] if static else frame_ordinals
                columns=[0 if static else labels.index(source['frames'][i]['label']) for i in checked_ordinals]
                # Original storage is point-major. Bounded sequential batches
                # avoid millions of tiny reads for its sparse time columns.
                actuals=np.empty((len(columns),n),dtype=np.float64)
                dataset=store[field['id']+'/block0_values']
                for begin in range(0,n,1024):
                    end=min(begin+1024,n);actuals[:,begin:end]=dataset[begin:end,:][:,columns].T
                originals=dict(zip(checked_ordinals,actuals))
            for ordinal in ([0] if static else frame_ordinals):
                row_values=values if static else values[ordinal]
                if store is not None:
                    actual=originals[ordinal]
                    if not np.array_equal(actual,row_values):raise ValueError('Converted source values differ from original HDF5')
                    source_values_checked+=n
                expected=np.sum(row_values[original_rows]*independent,axis=1)
                sampled=np.sum(row_values[original_rows]*weights[selected],axis=1)
                error=float(np.max(np.abs(sampled-expected)))
                sample_error=max(sample_error,error)
                tolerance=max(1.,float(np.max(np.abs(expected))))*1e-8
                if error>tolerance:raise ValueError('Independent scalar interpolation differs')
                cell_values=np.mean(np.sum(row_values[vertex_rows]*independent_cell_weights,axis=2),axis=1)
                for position,value in zip(query_positions,cell_values):
                    queries.append({'field':field['id'],'frame':ordinal,'position':position.tolist(),'value':float(value)})
    finally:
        if store is not None:store.close()
    report={'recording_sha256':digest(recording_path),'reconstruction_sha256':digest(reconstruction_path),
            'all_mapping_nodes_checked':count,'supported_fluid_nodes':fluid_count,
            'max_coordinate_error_m':coordinate_error,'max_weight_sum_error':weight_error,
            'independent_tetrahedral_queries':len(selected),'max_independent_weight_error':barycentric_error,
            'original_hdf5_values_compared':source_values_checked,'max_independent_scalar_error':sample_error,
            'original_hdf5_sha256':original_sha,'verifier_sha256':digest(__file__),
            'expected_queries':queries,'acceptance':'Numerical reconstruction checks only; renderer and full stability pending.'}
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('recording',type=Path);parser.add_argument('reconstruction',type=Path)
    parser.add_argument('--original-hdf',type=Path);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();report=verify(args.recording,args.reconstruction,args.original_hdf)
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='expected_queries'},indent=2))
