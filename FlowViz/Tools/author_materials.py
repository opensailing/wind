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

# VERTEX COLOR -> EMISSIVE, UNLIT. Two rules meet here:
# 1. The payloads carry authority-sampled colors per vertex (the GPU LUT hop
#    died of Metal ambiguities, each invisible until a capture).
# 2. VISUAL_QA rule 1: in the Scientific profile, LIGHTING MUST NOT MODULATE
#    APPARENT SCALAR VALUE. A lit data plane under the scene's key light and
#    auto-exposure rendered viridis dark purple as bright magenta -- the
#    number's color changed with the lighting, the exact lie the rule names.
#    Unlit emissive is the ParaView convention: the color IS the datum.
colormap.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
vc = mel.create_material_expression(
    colormap, unreal.MaterialExpressionVertexColor, -600, 0)
mel.connect_material_property(vc, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

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

# --- M_FlowVizObstaclePBR (P7): the film-tier obstacle -----------------------
# Matte lab-model gray with a clearcoat: enough spec for the studio rig to
# catch, low enough roughness variation to stay visually neutral so colored
# flow pops against it (the FluidX3D 0xDFDFDF philosophy through UE's lit path).
pbr = make_material("M_FlowVizObstaclePBR")

pbr_base = mel.create_material_expression(
    pbr, unreal.MaterialExpressionConstant3Vector, -600, 0)
pbr_base.set_editor_property("constant", unreal.LinearColor(0.62, 0.63, 0.66, 1.0))
mel.connect_material_property(pbr_base, "", unreal.MaterialProperty.MP_BASE_COLOR)

pbr_rough = mel.create_material_expression(
    pbr, unreal.MaterialExpressionConstant, -600, 200)
pbr_rough.set_editor_property("r", 0.45)
mel.connect_material_property(pbr_rough, "", unreal.MaterialProperty.MP_ROUGHNESS)

pbr_spec = mel.create_material_expression(
    pbr, unreal.MaterialExpressionConstant, -600, 300)
pbr_spec.set_editor_property("r", 0.6)
mel.connect_material_property(pbr_spec, "", unreal.MaterialProperty.MP_SPECULAR)

mel.recompile_material(pbr)
eal.save_asset(f"{CONTENT_ROOT}/M_FlowVizObstaclePBR")

# --- M_FlowVizColormapPresentation (P7): the film-tier iso/plane -------------
# Same LUT-at-UV0 contract as the Scientific colormap surface, plus what makes
# film surfaces feel wet and alive: low roughness and a Fresnel-driven
# emissive lift at grazing angles (research doc 7.2).
pres = make_material("M_FlowVizColormapPresentation")

pres_tex = mel.create_material_expression(
    pres, unreal.MaterialExpressionVertexColor, -800, 0)
mel.connect_material_property(pres_tex, "", unreal.MaterialProperty.MP_BASE_COLOR)

pres_rough = mel.create_material_expression(
    pres, unreal.MaterialExpressionConstant, -800, 240)
pres_rough.set_editor_property("r", 0.25)
mel.connect_material_property(pres_rough, "", unreal.MaterialProperty.MP_ROUGHNESS)

# Fresnel * LUT color * small constant -> emissive: the grazing-angle rim lift.
pres_fresnel = mel.create_material_expression(
    pres, unreal.MaterialExpressionFresnel, -800, 340)
pres_fresnel.set_editor_property("exponent", 3.0)
pres_scale = mel.create_material_expression(
    pres, unreal.MaterialExpressionMultiply, -560, 340)
mel.connect_material_expressions(pres_tex, "RGB", pres_scale, "A")
mel.connect_material_expressions(pres_fresnel, "", pres_scale, "B")
pres_dim = mel.create_material_expression(
    pres, unreal.MaterialExpressionMultiply, -380, 340)
pres_half = mel.create_material_expression(
    pres, unreal.MaterialExpressionConstant, -560, 460)
pres_half.set_editor_property("r", 0.35)
mel.connect_material_expressions(pres_scale, "", pres_dim, "A")
mel.connect_material_expressions(pres_half, "", pres_dim, "B")
mel.connect_material_property(pres_dim, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

mel.recompile_material(pres)
eal.save_asset(f"{CONTENT_ROOT}/M_FlowVizColormapPresentation")

unreal.log("FlowViz materials authored: M_FlowVizColormapSurface, M_FlowVizSurface, "
           "M_FlowVizObstaclePBR, M_FlowVizColormapPresentation")
