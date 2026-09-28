"""Author the scientific volume ray marcher; no CFD data is generated here."""
from pathlib import Path
import unreal

edit = unreal.MaterialEditingLibrary
assets = unreal.AssetToolsHelpers.get_asset_tools()
path = '/Game/Studio/M_FlowVolume'
material = unreal.load_asset(path)
if material is None:
    material = assets.create_asset('M_FlowVolume', '/Game/Studio', unreal.Material, unreal.MaterialFactoryNew())
edit.delete_all_material_expressions(material)
material.set_editor_property('two_sided', True)
material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_TRANSLUCENT)
material.set_editor_property('disable_depth_test', True)

# Compile-time texture type only. The volume component stays hidden until a
# verified scientific frame replaces this resource; noise is never shown as CFD.
fallback_path = '/Game/Studio/T_VolumeTypeFallback'
fallback = unreal.load_asset(fallback_path)
if fallback is None:
    source = unreal.load_asset('/Engine/EngineResources/DefaultVolumeTexture')
    assert isinstance(source, unreal.VolumeTexture)
    fallback = assets.duplicate_asset('T_VolumeTypeFallback', '/Game/Studio', source)
assert isinstance(fallback, unreal.VolumeTexture)
fallback.set_editor_property('srgb', False)
unreal.EditorAssetLibrary.save_asset(fallback_path)

inputs = {}
texture = edit.create_material_expression(material, unreal.MaterialExpressionTextureObjectParameter)
texture.set_editor_property('parameter_name', 'VolumeScalars')
texture.set_editor_property('texture', fallback)
texture.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
inputs['VolumeScalars'] = (texture, '')
for name, value in {
    'VolumeMinimum': (0,0,0), 'VolumeSize': (100,100,100), 'GridDimensions': (2,2,2),
    'SourceMinimum': (0,0,0), 'SourceSize': (1,1,1), 'Cylinder': (0,0,0),
    'ClipMinimum': (0,0,0), 'ClipMaximum': (1,1,1), 'OpacityCurve': (0,.4,1),
    'RenderCameraPosition': (0,0,0), 'RenderCameraForward': (1,0,0),
    'LowColor': (.03,.15,.6), 'MiddleColor': (.94,.94,.94), 'HighColor': (.7,.035,.025),
}.items():
    node = edit.create_material_expression(material, unreal.MaterialExpressionVectorParameter)
    node.set_editor_property('parameter_name', name)
    node.set_editor_property('default_value', unreal.LinearColor(*value, 0))
    inputs[name] = (node, 'RGB')
for name, value in {'Opacity': .28, 'Palette': 0, 'StepVoxels': 1,
                    'ThresholdMinimum': 0, 'ThresholdMaximum': 1,
                    'ThresholdEnabled': 0, 'Orthographic': 0, 'CameraNearDepth': 0, 'CameraFarDepth': 1e20}.items():
    node = edit.create_material_expression(material, unreal.MaterialExpressionScalarParameter)
    node.set_editor_property('parameter_name', name)
    node.set_editor_property('default_value', value)
    inputs[name] = (node, '')
for name, cls in [('WorldPosition', unreal.MaterialExpressionWorldPosition),
                  ('OpaqueDepth', unreal.MaterialExpressionSceneDepth)]:
    inputs[name] = (edit.create_material_expression(material, cls), '')
custom = edit.create_material_expression(material, unreal.MaterialExpressionCustom)
custom.set_editor_property('code', (Path(__file__).parent/'volume_raymarch.hlsl').read_text())
custom.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT4)
custom_inputs = []
for name in inputs:
    item = unreal.CustomInput()
    item.set_editor_property('input_name', name)
    custom_inputs.append(item)
custom.set_editor_property('inputs', custom_inputs)
for name, (node, output) in inputs.items():
    assert edit.connect_material_expressions(node, output, custom, name)
for prop, channels in [(unreal.MaterialProperty.MP_EMISSIVE_COLOR, (True,True,True,False)),
                       (unreal.MaterialProperty.MP_OPACITY, (False,False,False,True))]:
    mask = edit.create_material_expression(material, unreal.MaterialExpressionComponentMask)
    for name, value in zip(('r','g','b','a'), channels): mask.set_editor_property(name, value)
    assert edit.connect_material_expressions(custom, '', mask, '')
    assert edit.connect_material_property(mask, '', prop)
edit.recompile_material(material)
unreal.EditorAssetLibrary.save_asset(path)
unreal.log('STUDIO_VOLUME_MATERIAL_SAVED ' + path)

# Isosurfaces carry one physical scalar value. Normal-based shading supplies
# a depth cue for their geometry without shading quantitative slice/volume maps.
iso_path = '/Game/Studio/M_FlowIsosurface'
iso = unreal.load_asset(iso_path)
if iso is None:
    iso = assets.create_asset('M_FlowIsosurface', '/Game/Studio', unreal.Material, unreal.MaterialFactoryNew())
edit.delete_all_material_expressions(iso)
iso.set_editor_property('two_sided', True)
iso.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
normal = edit.create_material_expression(iso, unreal.MaterialExpressionPixelNormalWS)
vertex = edit.create_material_expression(iso, unreal.MaterialExpressionVertexColor)
shade = edit.create_material_expression(iso, unreal.MaterialExpressionCustom)
shade.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT3)
shade_inputs = []
for name in ('Normal', 'Color'):
    item = unreal.CustomInput(); item.set_editor_property('input_name', name); shade_inputs.append(item)
shade.set_editor_property('inputs', shade_inputs)
shade.set_editor_property('code', 'float3 n=normalize(Normal); float light=.28+.58*abs(dot(n,normalize(float3(.3,.5,.8))))+.14*abs(dot(n,normalize(float3(-.7,.4,.2)))); return Color*light;')
assert edit.connect_material_expressions(normal, '', shade, 'Normal')
assert edit.connect_material_expressions(vertex, '', shade, 'Color')
assert edit.connect_material_property(shade, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
edit.recompile_material(iso); unreal.EditorAssetLibrary.save_asset(iso_path)
unreal.log('STUDIO_ISOSURFACE_MATERIAL_SAVED ' + iso_path)
