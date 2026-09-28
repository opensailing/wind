#!/usr/bin/env python3
"""Measure display-grid error against unchanged audited original CFD samples.

The result describes visualization resampling, not original Fluent connectivity
or a conservative integration mesh. Masked regions are reported separately.
"""
import argparse,json
from pathlib import Path
import numpy as np
from reconstruct_cylinder_volume import digest


def metrics(actual,reference):
    error=actual-reference
    span=float(reference.max()-reference.min())
    rms=float(np.sqrt(np.mean(error**2)))
    return {'count':int(error.size),'rms':rms,'maximum_absolute':float(np.max(np.abs(error))),
            'rms_fraction_of_observed_range':rms/span if span else None,
            'observed_range':[float(reference.min()),float(reference.max())]}


def measure(recording,mapping,audit,sample_archive):
    recording,mapping,audit,sample_archive=map(Path,(recording,mapping,audit,sample_archive))
    source=json.loads(recording.read_text());grid=json.loads(mapping.read_text());report=json.loads(audit.read_text())
    if digest(recording)!=grid['sourceMetadataSHA256']:raise ValueError('Mapping source differs')
    if digest(sample_archive)!=report['audited_samples_sha256']:raise ValueError('Independent samples differ')
    provenance=json.loads((recording.parent/'provenance.json').read_text())
    if provenance['originalMemberSHA256']!=report['member_sha256']:raise ValueError('Original CFD member differs')
    samples=np.load(sample_archive,allow_pickle=False);xyz=samples['coordinates']
    dims=np.array(grid['dimensions']);n=int(np.prod(dims));lo=np.array(grid['minimum']);hi=np.array(grid['maximum'])
    def load(key,dtype,shape):
        p=mapping.parent/grid[key]['path']
        if p.parent!=mapping.parent or digest(p)!=grid[key]['sha256']:raise ValueError('Mapping arrays differ')
        return np.memmap(p,dtype=dtype,mode='r',shape=shape)
    rows=load('rows','<u4',(n,4));weights=load('weights','<f8',(n,4));classes=load('classification','u1',(n,))
    selected=np.flatnonzero(classes==1)
    coordinate_path=recording.parent/source['coordinates']['path']
    if digest(coordinate_path)!=source['coordinates']['sha256']:raise ValueError('Original coordinates changed')
    if not np.array_equal(np.fromfile(coordinate_path,dtype='<f8').reshape(xyz.shape),xyz):raise ValueError('Audit and display source geometry differ')
    spacing=(hi-lo)/(dims-1);node=(xyz-lo)/spacing;cell=np.clip(np.floor(node).astype(int),0,dims-2);fraction=node-cell
    offsets=np.array([[x,y,z] for z in (0,1) for y in (0,1) for x in (0,1)])
    nodes=cell[:,None,:]+offsets
    index=nodes[:,:,0]+dims[0]*(nodes[:,:,1]+dims[1]*nodes[:,:,2])
    supported=np.all(classes[index]==1,axis=1)&np.all((xyz>=lo)&(xyz<=hi),axis=1)
    minimum=lo+cell*spacing;center=np.array(grid['solid']['centerXY']);radius=grid['solid']['radiusMeters']
    closest=np.clip(center,minimum[:,:2],minimum[:,:2]+spacing[:2])-center
    supported &= np.sum(closest*closest,axis=1)>=radius*radius
    if not supported.any():raise ValueError('No original locations have supported grid queries')
    fractions=fraction[supported];indices=index[supported]
    trilinear=np.prod(np.where(offsets[None,:,:]>0,fractions[:,None,:],1-fractions[:,None,:]),axis=2)
    measurements={}
    for field in source['fields']:
        values=samples[field['id']]
        if values.ndim==1:values=values[:,None]
        nodes=np.zeros((n,values.shape[1]),dtype=np.float64)
        for start in range(0,len(selected),16384):
            ids=selected[start:start+16384]
            nodes[ids]=np.einsum('nk,nkf->nf',weights[ids],values[rows[ids]])
        interpolated=np.einsum('nk,nkf->nf',trilinear,nodes[indices])
        measurements[field['id']]={'unit':field['unit'],**metrics(interpolated,values[supported])}
    return {'recording_sha256':digest(recording),'reconstruction_sha256':digest(mapping),
            'audit_sha256':digest(audit),'samples_sha256':digest(sample_archive),
            'all_original_points':len(xyz),'supported_original_queries':int(supported.sum()),
            'supported_fraction':float(np.mean(supported)),'masked_original_queries':int((~supported).sum()),
            'source_frame_ordinals':report['selected_frame_ordinals'],'measurements':measurements,
            'interpretation':'Errors describe display resampling at supported original positions. Unsupported cells and cells crossing the solid are excluded and counted. Original rows remain unchanged.'}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('recording','mapping','audit','samples','output'):p.add_argument(name,type=Path)
    a=p.parse_args();r=measure(a.recording,a.mapping,a.audit,a.samples);a.output.write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2))
