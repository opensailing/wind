"""Shade the original solid wing independently of quantitative field colors."""
import unreal

assets = unreal.AssetToolsHelpers.get_asset_tools()
edit = unreal.MaterialEditingLibrary
path = '/Game/Studio/M_FlowBody'
material = unreal.load_asset(path)
if material is None:
    material = assets.create_asset('M_FlowBody', '/Game/Studio', unreal.Material, unreal.MaterialFactoryNew())
edit.delete_all_material_expressions(material)
material.set_editor_property('two_sided', True)
material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_OPAQUE)
normal = edit.create_material_expression(material, unreal.MaterialExpressionPixelNormalWS)
view = edit.create_material_expression(material, unreal.MaterialExpressionCameraVectorWS)
vertex = edit.create_material_expression(material, unreal.MaterialExpressionVertexColor)
shade = edit.create_material_expression(material, unreal.MaterialExpressionCustom)
shade.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT3)
inputs = []
for name in ('Normal', 'View', 'Color'):
    item = unreal.CustomInput()
    item.set_editor_property('input_name', name)
    inputs.append(item)
shade.set_editor_property('inputs', inputs)
shade.set_editor_property('code', '''
float3 n=normalize(Normal), v=normalize(View);
if (dot(n,v)<0) n=-n;
float3 key=normalize(float3(-.35,-.55,.9));
float3 fill=normalize(float3(.6,.3,.25));
float diffuse=.4+.65*saturate(dot(n,key))+.2*saturate(dot(n,fill));
float specular=pow(saturate(dot(n,normalize(key+v))),48)*.16;
float rim=pow(1-saturate(dot(n,v)),4);
return Color*diffuse+specular*float3(.8,.9,1)+rim*float3(.035,.055,.075);
''')
assert edit.connect_material_expressions(normal, '', shade, 'Normal')
assert edit.connect_material_expressions(view, '', shade, 'View')
assert edit.connect_material_expressions(vertex, '', shade, 'Color')
assert edit.connect_material_property(shade, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
edit.recompile_material(material)
unreal.EditorAssetLibrary.save_asset(path)
unreal.log('STUDIO_BODY_MATERIAL_SAVED ' + path)
