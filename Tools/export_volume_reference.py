#!/usr/bin/env python3
"""Export a derived display grid for independent scientific-viewer inspection.

The VTI retains double scalar values, source coordinates in meters, original
time/identity and explicit hidden cells. It is not the original Fluent mesh.
An optional VTK cross-section render is a separate GPU operation.
"""
import argparse,json
from pathlib import Path
import numpy as np
from reconstruct_cylinder_volume import digest


def export(recording,mapping,output,ordinal,field_id,render=False):
    import vtk
    from vtk.util.numpy_support import numpy_to_vtk
    recording,mapping,output=map(Path,(recording,mapping,output))
    if output.exists():raise ValueError('Choose a new reference output directory')
    source=json.loads(recording.read_text());grid=json.loads(mapping.read_text())
    if source['spatialDimensions']!=3 or digest(recording)!=grid['sourceMetadataSHA256']:
        raise ValueError('Bound original 3D recording required')
    if not 0<=ordinal<source['frameCount']:raise ValueError('Original frame ordinal out of range')
    field=next(f for f in source['fields'] if f['id']==field_id)
    dims=np.array(grid['dimensions']);n=int(np.prod(dims));shape=tuple(dims[::-1])
    lo=np.array(grid['minimum']);hi=np.array(grid['maximum']);spacing=(hi-lo)/(dims-1)
    def read(folder,array,dtype,shape):
        path=folder/array['path']
        if path.parent!=folder or digest(path)!=array['sha256']:raise ValueError('Array identity differs')
        return np.memmap(path,mode='r',dtype=dtype,shape=shape)
    rows=read(mapping.parent,grid['rows'],'<u4',(n,4));weights=read(mapping.parent,grid['weights'],'<f8',(n,4))
    classes=read(mapping.parent,grid['classification'],'u1',(n,))
    source_shape=(source['pointCount'],) if field['static'] else (source['frameCount'],source['pointCount'])
    original=read(recording.parent,field['array'],'<f8',source_shape)
    values=original if field['static'] else original[ordinal]
    scalars=np.full(n,np.nan);valid=np.flatnonzero(classes==1)
    for begin in range(0,len(valid),16384):
        ids=valid[begin:begin+16384];scalars[ids]=np.sum(weights[ids]*values[rows[ids]],axis=1)
    nodes=(classes==1).reshape(shape)
    cells=np.ones(tuple(dims[::-1]-1),dtype=bool)
    for z in (0,1):
        for y in (0,1):
            for x in (0,1):cells &= nodes[z:z+dims[2]-1,y:y+dims[1]-1,x:x+dims[0]-1]
    # Conservative native cell rule, including an analytic boundary crossing
    # even when all eight corners happen to be outside the cylinder.
    center=np.array(grid['solid']['centerXY']);radius=grid['solid']['radiusMeters']
    x=lo[0]+np.arange(dims[0]-1)*spacing[0];y=lo[1]+np.arange(dims[1]-1)*spacing[1]
    dx=np.clip(center[0],x,x+spacing[0])-center[0]
    dy=np.clip(center[1],y,y+spacing[1])-center[1]
    cells &= (dy[:,None]**2+dx[None,:]**2>=radius**2)[None,:,:]
    image=vtk.vtkImageData();image.SetDimensions(*map(int,dims));image.SetOrigin(*lo);image.SetSpacing(*spacing)
    array=numpy_to_vtk(scalars,deep=True);array.SetName(field_id);image.GetPointData().SetScalars(array)
    valid_array=numpy_to_vtk(cells.ravel().astype(np.uint8),deep=True);valid_array.SetName('supported_cell')
    image.GetCellData().AddArray(valid_array)
    ghosts=numpy_to_vtk(np.where(cells.ravel(),0,vtk.vtkDataSetAttributes.HIDDENCELL).astype(np.uint8),deep=True)
    ghosts.SetName(vtk.vtkDataSetAttributes.GhostArrayName());image.GetCellData().AddArray(ghosts)
    time=vtk.vtkDoubleArray();time.SetName('original_time_seconds');time.InsertNextValue(source['frames'][ordinal]['time']);image.GetFieldData().AddArray(time)
    for key,value in {'recording_sha256':digest(recording),'reconstruction_sha256':digest(mapping),
                      'source_url':source['sourceURL'],'field_units':field['unit'],
                      'geometry':'Derived uniform display grid; original Fluent connectivity unavailable'}.items():
        item=vtk.vtkStringArray();item.SetName(key);item.InsertNextValue(value);image.GetFieldData().AddArray(item)
    output.mkdir(parents=True)
    writer=vtk.vtkXMLImageDataWriter();writer.SetFileName(str(output/'display-grid.vti'));writer.SetInputData(image)
    if writer.Write()!=1:raise RuntimeError('VTK reference export failed')
    # Read the serialized artifact back through VTK before using it as evidence.
    reader=vtk.vtkXMLImageDataReader();reader.SetFileName(str(output/'display-grid.vti'));reader.Update()
    if reader.GetOutput().GetNumberOfPoints()!=n:raise RuntimeError('VTK grid readback differs')
    if render:
        supported=vtk.vtkThreshold();supported.SetInputConnection(reader.GetOutputPort())
        supported.SetInputArrayToProcess(0,0,0,vtk.vtkDataObject.FIELD_ASSOCIATION_CELLS,'supported_cell')
        supported.SetLowerThreshold(1);supported.SetUpperThreshold(1);supported.SetThresholdFunction(vtk.vtkThreshold.THRESHOLD_BETWEEN)
        plane=vtk.vtkPlane();plane.SetOrigin(*((lo+hi)*.5));plane.SetNormal(0,0,1)
        cut=vtk.vtkCutter();cut.SetInputConnection(supported.GetOutputPort());cut.SetCutFunction(plane)
        colors=vtk.vtkColorTransferFunction()
        for t,color in enumerate([( .025,.02,.35),(.015,.14,.9),(0,.7,.95),(.02,.65,.25),(.95,.85,.015),(1,.25,.015),(.8,.015,.008)]):
            colors.AddRGBPoint(field['range'][0]+t/6*(field['range'][1]-field['range'][0]),*color)
        mapper=vtk.vtkPolyDataMapper();mapper.SetInputConnection(cut.GetOutputPort());mapper.SetLookupTable(colors)
        mapper.SetScalarModeToUsePointFieldData();mapper.SelectColorArray(field_id);mapper.SetScalarRange(*field['range']);mapper.InterpolateScalarsBeforeMappingOn()
        actor=vtk.vtkActor();actor.SetMapper(mapper);actor.GetProperty().LightingOff()
        renderer=vtk.vtkRenderer();renderer.AddActor(actor);renderer.SetBackground(.003,.009,.016)
        camera=renderer.GetActiveCamera();center=(lo+hi)*.5
        camera.SetPosition(center[0],center[1],hi[2]+1);camera.SetFocalPoint(*center);camera.SetViewUp(0,1,0)
        camera.ParallelProjectionOn();camera.SetParallelScale(max((hi[1]-lo[1])*.55,(hi[0]-lo[0])*.55/2))
        window=vtk.vtkRenderWindow();window.SetOffScreenRendering(1);window.SetSize(1200,600);window.AddRenderer(renderer);window.Render()
        pixels=vtk.vtkWindowToImageFilter();pixels.SetInput(window);pixels.ReadFrontBufferOff();pixels.Update()
        png=vtk.vtkPNGWriter();png.SetFileName(str(output/'vtk-midspan.png'));png.SetInputConnection(pixels.GetOutputPort());png.Write();window.Finalize()
    report={'recording_sha256':digest(recording),'reconstruction_sha256':digest(mapping),'original_frame':source['frames'][ordinal],
            'field':field_id,'unit':field['unit'],'source_range':field['range'],'dimensions_xyz':dims.tolist(),
            'source_slice_z_m':float((lo[2]+hi[2])*.5),'supported_cells':int(cells.sum()),'vti_sha256':digest(output/'display-grid.vti'),
            'vtk_version':vtk.vtkVersion.GetVTKVersion(),'reference_image_rendered':render,
            'interpretation':'Independent VTK serialization, readback and optional slice rendering of the bound display reconstruction; original CFD precision is retained at grid nodes.'}
    (output/'reference.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('recording','mapping','output'):p.add_argument(name,type=Path)
    p.add_argument('--frame',type=int,default=0);p.add_argument('--field',default='velocity_magnitude');p.add_argument('--render',action='store_true')
    a=p.parse_args();export(a.recording,a.mapping,a.output,a.frame,a.field,a.render)
