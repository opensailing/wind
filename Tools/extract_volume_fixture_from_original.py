#!/usr/bin/env python3
"""Extract three unchanged original snapshots for tests while full conversion runs.

This sparse test fixture is never the animation recording. All original point
rows, float32 values (exactly widened to float64) and source identities remain.
"""
import argparse,json,os,shutil,tempfile
from pathlib import Path
import h5py
import numpy as np
from import_cylinder3d import FIELDS,POINTS,FRAMES,digest,array_descriptor,write_json


def extract(source_folder,author_readme,output):
    source_folder,author_readme,output=map(Path,(source_folder,author_readme,output))
    if output.exists():raise ValueError('Existing fixture is retained; choose a new output folder')
    audit=json.loads((source_folder/'array-audit.json').read_text())
    acquisition=json.loads((source_folder/'acquisition.json').read_text())
    record=json.loads((source_folder/'record.json').read_text())
    source=source_folder/'cylinder_3d_re300_ds.h5'
    if record['id']!=20586598 or record['metadata']['license']['id']!='cc-by-4.0':raise ValueError('Publication or license differs')
    if not acquisition['member_crc32_verified'] or acquisition['member_sha256']!=audit['member_sha256'] or digest(source)!=audit['member_sha256']:
        raise ValueError('Original acquisition and audit identities differ')
    if audit['point_count']!=POINTS or audit['frame_count']!=FRAMES:raise ValueError('Full original source was not audited')
    ordinals=[0,2950,5900]
    output.parent.mkdir(parents=True,exist_ok=True);stage=Path(tempfile.mkdtemp(prefix='.'+output.name+'-',dir=output.parent))
    try:
        with h5py.File(source,'r') as f:
            xyz=f['coordinates/block0_values'][:].astype('<f8')
            ids=f['kept_node_ids/block0_values'][:].ravel().astype('<i8')
            if xyz.shape!=(POINTS,3) or ids.shape!=(POINTS,) or not np.isfinite(xyz).all():raise ValueError('Original geometry differs')
            xyz.tofile(stage/'coordinates.f64');ids.tofile(stage/'point-ids.i64')
            labels=[x.decode('ascii') for x in f['velocity_u/axis0'][:]]
            if labels!=[f't_{i}' for i in range(2200,8101)]:raise ValueError('Audited timeline differs')
            fields=[]
            for key,title,unit,component in FIELDS:
                data=f[key+'/block0_values'];static=key=='cell_volume' and audit['cell_volume_time_max_error']==0
                values=np.empty((POINTS,) if static else (len(ordinals),POINTS),dtype='<f8')
                for begin in range(0,POINTS,1024):
                    end=min(begin+1024,POINTS);block=data[begin:end,:]
                    if static:values[begin:end]=block[:,0]
                    else:values[:,begin:end]=block[:,ordinals].T
                if not np.isfinite(values).all():raise ValueError('Nonfinite original snapshot')
                path=stage/(key+'.f64');values.tofile(path)
                field={'id':key,'label':title,'unit':unit,'association':'point','origin':'source','static':static,
                       'range':audit['field_ranges'][key],'array':array_descriptor(path,values.shape,static)}
                if component:field.update(vector='velocity',component=component)
                fields.append(field);print('Retained original test snapshots:',key,flush=True)
        for src,name in [(source_folder/'record.json','zenodo-record.json'),(author_readme,'author-setup.md'),
                         (source_folder/'array-audit.json','original-array-audit.json'),(source_folder/'acquisition.json','original-acquisition.json')]:
            shutil.copyfile(src,stage/name)
        (stage/'ATTRIBUTION.txt').write_text(record['metadata']['title']+'\nCreators: '+'; '.join(x['name'] for x in record['metadata']['creators'])+
            '\nSource: https://doi.org/10.5281/zenodo.20586598\nLicense: CC BY 4.0 — https://creativecommons.org/licenses/by/4.0/\n'
            'Changes: three unchanged original snapshots selected for automated tests, exact widening to float64 and frame-major layout. '
            'All original points retained. Not the full animation recording; no generated flow or original mesh claim.\n')
        provenance={'sourceURL':'https://zenodo.org/records/20586598','license':'CC-BY-4.0','originalMemberSHA256':audit['member_sha256'],
                    'sourceOrdinals':ordinals,'arrayAuditSHA256':digest(source_folder/'array-audit.json'),'extractorSHA256':digest(__file__),
                    'changes':'Three unchanged original snapshots, all supplied rows; no temporal interpolation. Test fixture only.'}
        write_json(stage/'provenance.json',provenance)
        descriptor={'version':3,'kind':'field_recording','id':'Cylinder3D_ReaderFixture_3OriginalFrames',
                    'title':'Reader fixture · three original 3D cylinder snapshots','sourceURL':provenance['sourceURL'],
                    'spatialDimensions':3,'coordinateUnit':'m','timeUnit':'s','pointCount':POINTS,
                    'topology':{'kind':'points','origin':'source','connectivity':None},
                    'coordinates':array_descriptor(stage/'coordinates.f64',xyz.shape,True),
                    'pointIds':{'path':'point-ids.i64','dtype':'int64','byteOrder':'little','count':POINTS,'sha256':digest(stage/'point-ids.i64')},
                    'sourceBounds':{'min':xyz.min(axis=0).tolist(),'max':xyz.max(axis=0).tolist()},
                    'frameCount':len(ordinals),'frames':[{'index':2200+i,'label':labels[i],'time':(2200+i)*.01} for i in ordinals],
                    'timeOrigin':'Original solver step labels times author save interval 0.01 seconds.',
                    'fields':fields,'defaultScalar':'velocity_magnitude','provenanceSHA256':digest(stage/'provenance.json'),
                    'attributionSHA256':digest(stage/'ATTRIBUTION.txt'),
                    'limitations':['Sparse original test snapshots. Use all 5901 original frames for animation.',
                                   'Author-cropped/downsampled point rows; original cell connectivity and wall faces are unavailable.',
                                   'No residual, density or force histories. Exported speed is retained separately from the vector norm.',
                                   'Exported cell_volume convention/units are unspecified.']}
        write_json(stage/'recording.json',descriptor);os.rename(stage,output)
    except BaseException:
        shutil.rmtree(stage);raise


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('source_folder','author_readme','output'):p.add_argument(name,type=Path)
    a=p.parse_args();extract(a.source_folder,a.author_readme,a.output)
