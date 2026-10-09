#!/usr/bin/env python3
"""Retain original 3D snapshots for packaged tests; never an animation sample."""
import argparse
import copy
import json
import os
from pathlib import Path
import shutil
import tempfile
import numpy as np
from import_cylinder3d import digest,array_descriptor,write_json


def extract(converted,output,ordinals=(0,2950,5900)):
    converted,output=Path(converted),Path(output)
    if output.exists():raise ValueError('Choose a new fixture directory')
    original=json.loads((converted/'recording.json').read_text())
    if original['spatialDimensions']!=3 or original['frameCount']!=5901:raise ValueError('Full original 3D recording required')
    descriptor=copy.deepcopy(original);n=original['pointCount']
    output.parent.mkdir(parents=True,exist_ok=True)
    stage=Path(tempfile.mkdtemp(prefix='.'+output.name+'-',dir=output.parent))
    try:
        for name in ['coordinates.f64','point-ids.i64','author-setup.md','zenodo-record.json']:
            shutil.copyfile(converted/name,stage/name)
        for field in descriptor['fields']:
            path=converted/field['array']['path'];static=field['static']
            if digest(path)!=field['array']['sha256']:raise ValueError('Parent field identity differs')
            mapped=np.memmap(path,mode='r',dtype='<f8',shape=(n,) if static else (original['frameCount'],n))
            values=np.array(mapped if static else mapped[list(ordinals)],dtype='<f8');del mapped
            target=stage/path.name;values.tofile(target)
            field['array']=array_descriptor(target,values.shape,static)
        descriptor['id']='Cylinder3D_ReaderFixture_3OriginalFrames'
        descriptor['title']='Reader fixture · three original 3D cylinder snapshots'
        descriptor['frames']=[descriptor['frames'][i] for i in ordinals];descriptor['frameCount']=len(ordinals)
        descriptor['limitations'].append('Test-only sparse original snapshots. Use the full recording for animation.')
        parent_provenance=json.loads((converted/'provenance.json').read_text())
        provenance={'sourceURL':original['sourceURL'],'originalMemberSHA256':parent_provenance['originalMemberSHA256'],
                    'parentDescriptorSHA256':digest(converted/'recording.json'),'sourceOrdinals':list(ordinals),
                    'extractorSHA256':digest(__file__),'changes':'Original snapshots selected unchanged; all source point rows retained.'}
        write_json(stage/'provenance.json',provenance)
        (stage/'ATTRIBUTION.txt').write_text((converted/'ATTRIBUTION.txt').read_text()+'\nTest fixture only: three unchanged original snapshots; not the full animation.\n')
        descriptor['provenanceSHA256']=digest(stage/'provenance.json');descriptor['attributionSHA256']=digest(stage/'ATTRIBUTION.txt')
        write_json(stage/'recording.json',descriptor)
        os.rename(stage,output)
    except BaseException:
        shutil.rmtree(stage);raise
    return descriptor


def bind_mapping(original_mapping,original_recording,fixture,output):
    """Reuse the verified map only when both recordings retain identical geometry."""
    original_mapping,fixture,output=Path(original_mapping),Path(fixture),Path(output)
    if output.exists():raise ValueError('Choose a new mapping folder')
    volume=json.loads(original_mapping.read_text());source=json.loads(fixture.read_text())
    original=json.loads(Path(original_recording).read_text())
    if volume['sourceMetadataSHA256']!=digest(original_recording):raise ValueError('Original reconstruction binding differs')
    if source['spatialDimensions']!=3 or source['pointCount']!=original['pointCount']:raise ValueError('Identical 3D point rows required')
    for key in ('coordinates','pointIds'):
        if source[key]['sha256']!=original[key]['sha256']:raise ValueError('Original geometry differs')
    if source['sourceBounds']!=original['sourceBounds']:raise ValueError('Source bounds differ')
    output.mkdir(parents=True)
    for key in ('rows','weights','classification'):
        array=volume[key];member=original_mapping.parent/array['path']
        if digest(member)!=array['sha256']:raise ValueError('Mapping array changed')
        shutil.copyfile(member,output/array['path'])
    volume['sourceMetadataSHA256']=digest(fixture)
    volume['title']='Cylinder wake · derived 3D display grid' if source.get('frameCount')==5901 else 'Test fixture · derived cylinder volume'
    volume['geometryBinding']={'priorSourceMetadataSHA256':digest(original_recording),
                               'coordinatesSHA256':source['coordinates']['sha256'],
                               'pointIdsSHA256':source['pointIds']['sha256'],
                               'operation':'Identical original point geometry; display stencils unchanged.'}
    write_json(output/'reconstruction.json',volume)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('converted',type=Path);p.add_argument('output',type=Path)
    args=p.parse_args();extract(args.converted,args.output)
