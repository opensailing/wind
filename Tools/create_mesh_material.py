"""Triangle edges from exact connectivity; no scientific data generated here."""
import unreal

path = '/Game/Studio/M_MeshEdges'
material = unreal.load_asset(path)
if material is None:
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        'M_MeshEdges', '/Game/Studio', unreal.Material, unreal.MaterialFactoryNew())
edit = unreal.MaterialEditingLibrary
edit.delete_all_material_expressions(material)
material.set_editor_property('two_sided', True)
material.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_UNLIT)
material.set_editor_property('blend_mode', unreal.BlendMode.BLEND_TRANSLUCENT)
material.set_editor_property('disable_depth_test', False)
uv = edit.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate)
edge = edit.create_material_expression(material, unreal.MaterialExpressionCustom)
edge.set_editor_property('description', 'Pixel-width edges of the actual triangle')
edge.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT1)
item = unreal.CustomInput(); item.set_editor_property('input_name', 'Barycentric')
edge.set_editor_property('inputs', [item])
edge.set_editor_property('code', '''
float3 b = float3(Barycentric.x, Barycentric.y, 1-Barycentric.x-Barycentric.y);
float3 d = b / max(fwidth(b), 1.e-6);
return 1-smoothstep(.5, 1.1, min(d.x,min(d.y,d.z)));
''')
assert edit.connect_material_expressions(uv, '', edge, 'Barycentric')
color = edit.create_material_expression(material, unreal.MaterialExpressionConstant3Vector)
color.set_editor_property('constant', unreal.LinearColor(.2,.42,.55,1))
assert edit.connect_material_property(color, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
assert edit.connect_material_property(edge, '', unreal.MaterialProperty.MP_OPACITY)
edit.recompile_material(material)
unreal.EditorAssetLibrary.save_asset(path)
unreal.log('STUDIO_MESH_MATERIAL_SAVED ' + path)
