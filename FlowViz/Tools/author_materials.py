"""Author the FlowViz surface materials as plugin Content (renderer overhaul P3).

Run inside the editor:
  UnrealEditor-Cmd FlowViz.uproject -run=pythonscript -script=Tools/author_materials.py

Materials cannot be constructed in raw runtime C++, so this script IS the
source for the two .uasset files, committed beside them. Re-running it
recreates them identically; edit THIS, not the assets.

M_FlowVizColormapSurface: lit, opaque; BaseColor = ColorLUT sampled at UV0
  (the scalar the mesh payloads bake into UV0.x). One texture parameter,
  "ColorLUT", swapped by the runtime when the colormap changes -- geometry
  never rebuilt for a color change.
M_FlowVizSurface: the obstacle's matte neutral -- lit, opaque, constant
  base color, roughness 0.7 (FluidX3D's 0xDFDFDF look via UE's lit path).
"""

import unreal

CONTENT_ROOT = "/FlowVizRuntime"

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
mel = unreal.MaterialEditingLibrary
eal = unreal.EditorAssetLibrary


def make_material(name):
    path = f"{CONTENT_ROOT}/{name}"
    if eal.does_asset_exist(path):
        eal.delete_asset(path)
    material = asset_tools.create_asset(
        name, CONTENT_ROOT, unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property("two_sided", True)
    return material


# --- M_FlowVizColormapSurface -------------------------------------------------
colormap = make_material("M_FlowVizColormapSurface")

tex = mel.create_material_expression(
    colormap, unreal.MaterialExpressionTextureSampleParameter2D, -600, 0)
tex.set_editor_property("parameter_name", "ColorLUT")
# The LUT is linear color (sRGB off on the texture); LinearColor sampler type
# matches, and a Color sampler would double-decode.
tex.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
# A DEFAULT TEXTURE, or the sampler node has nothing to compile against and
# the whole material fails on Metal ("Failed to compile ... SF_METAL_SM6",
# default material used) -- observed as a BLACK cut plane in the first
# packaged capture. Any engine linear texture works; the runtime swaps in
# the real LUT per colormap.
tex.set_editor_property("texture", unreal.load_asset("/Engine/EngineResources/DefaultTexture"))

uv = mel.create_material_expression(
    colormap, unreal.MaterialExpressionTextureCoordinate, -820, 0)
uv.set_editor_property("coordinate_index", 0)
mel.connect_material_expressions(uv, "", tex, "UVs")

mel.connect_material_property(tex, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)

rough = mel.create_material_expression(
    colormap, unreal.MaterialExpressionConstant, -600, 220)
rough.set_editor_property("r", 0.65)
mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)

mel.recompile_material(colormap)
eal.save_asset(f"{CONTENT_ROOT}/M_FlowVizColormapSurface")

# --- M_FlowVizSurface ---------------------------------------------------------
surface = make_material("M_FlowVizSurface")

base = mel.create_material_expression(
    surface, unreal.MaterialExpressionConstant3Vector, -600, 0)
base.set_editor_property("constant", unreal.LinearColor(0.72, 0.72, 0.75, 1.0))
mel.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)

rough2 = mel.create_material_expression(
    surface, unreal.MaterialExpressionConstant, -600, 220)
rough2.set_editor_property("r", 0.7)
mel.connect_material_property(rough2, "", unreal.MaterialProperty.MP_ROUGHNESS)

mel.recompile_material(surface)
eal.save_asset(f"{CONTENT_ROOT}/M_FlowVizSurface")

unreal.log("FlowViz materials authored: M_FlowVizColormapSurface, M_FlowVizSurface")
