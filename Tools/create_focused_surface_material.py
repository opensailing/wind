"""Translucent focused surface; retains the original R32 scalar interpolation."""
import unreal
assets = unreal.AssetToolsHelpers.get_asset_tools()
path = "/Game/Studio/M_FlowScalarFocus"
material = unreal.load_asset(path)
if material is None:
    material = assets.create_asset("M_FlowScalarFocus", "/Game/Studio", unreal.Material, unreal.MaterialFactoryNew())
unreal.MaterialEditingLibrary.delete_all_material_expressions(material)
material.set_editor_property("two_sided", True)
material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)

fallback = unreal.load_asset('/Game/Studio/T_ScalarFallback')
assert fallback is not None, 'Run create_materials.py first'

edit = unreal.MaterialEditingLibrary
sample = edit.create_material_expression(material, unreal.MaterialExpressionTextureSampleParameter2D, -600, 0)
sample.set_editor_property("parameter_name", "SourceScalars")
sample.set_editor_property("texture", fallback)
sample.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
sample.set_editor_property("mip_value_mode", unreal.TextureMipValueMode.TMVM_MIP_LEVEL)
sample.set_editor_property("const_mip_value", 0)
interpolator = edit.create_material_expression(material, unreal.MaterialExpressionVertexInterpolator, -350, 0)
assert edit.connect_material_expressions(sample, "R", interpolator, "VS")
palette = edit.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -350, 180)
palette.set_editor_property("parameter_name", "Palette")
palette.set_editor_property("default_value", 0.0)
color = edit.create_material_expression(material, unreal.MaterialExpressionCustom, -100, 0)
color.set_editor_property("description", "Interpolate original scalar, then map color")
color.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
inputs = []
for name in ("Scalar", "Palette", "LowColor", "MiddleColor", "HighColor"):
    item = unreal.CustomInput()
    item.set_editor_property("input_name", name)
    inputs.append(item)
color.set_editor_property("inputs", inputs)
color.set_editor_property("code", """
float t = saturate(Scalar);
if (Palette > 2.5) return t <= .5 ? lerp(LowColor,MiddleColor,t*2) : lerp(MiddleColor,HighColor,(t-.5)*2);
if (Palette > 1.5) return float3(t, t, t);
if (Palette > 0.5) {
    float3 lo = float3(.03, .15, .6);
    float3 mid = float3(.94, .94, .94);
    float3 hi = float3(.7, .035, .025);
    return t <= .5 ? lerp(lo, mid, t*2) : lerp(mid, hi, (t-.5)*2);
}
float3 colors[7] = {
    float3(.025,.02,.35), float3(.015,.14,.9), float3(0,.7,.95),
    float3(.02,.65,.25), float3(.95,.85,.015), float3(1,.25,.015), float3(.8,.015,.008)
};
float x = t*6;
int i = min(5, (int)floor(x));
return lerp(colors[i], colors[i+1], x-i);
""")
assert edit.connect_material_expressions(interpolator, "", color, "Scalar")
assert edit.connect_material_expressions(palette, "", color, "Palette")
for name, value in {'LowColor': (.03,.15,.6), 'MiddleColor': (.94,.94,.94), 'HighColor': (.7,.035,.025)}.items():
    node = edit.create_material_expression(material, unreal.MaterialExpressionVectorParameter)
    node.set_editor_property('parameter_name', name)
    node.set_editor_property('default_value', unreal.LinearColor(*value, 1))
    assert edit.connect_material_expressions(node, 'RGB', color, name)
assert edit.connect_material_property(color, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
opacity = edit.create_material_expression(material, unreal.MaterialExpressionScalarParameter)
opacity.set_editor_property('parameter_name', 'SurfaceOpacity')
opacity.set_editor_property('default_value', .16)
assert edit.connect_material_property(opacity, '', unreal.MaterialProperty.MP_OPACITY)
edit.recompile_material(material)
unreal.EditorAssetLibrary.save_asset(path)
unreal.log("STUDIO_MATERIAL_SAVED " + path)

