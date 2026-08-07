# FlowViz Rendering Overview: Why It Looks Wrong and What the Best-in-Class Do Instead

Synthesized from deep reads of FluidX3D (real-time LBM with a famously good built-in renderer) and OpenFOAM-13 (industry solver, viewing delegated to ParaView). Reference citations point into `/Users/bcardarella/projects/FluidX3D` and `/Users/bcardarella/projects/OpenFOAM-13`. FlowViz citations point into `/Users/bcardarella/projects/wind/FlowViz/Plugins/FlowVizRuntime/Source/FlowVizRuntime` (abbreviated below as `FVR/`). Claims about ParaView's own UI are general knowledge and marked as such — ParaView's source is not in the corpus; only OpenFOAM's reader plugin and tutorial dicts are.

One constraint shapes every recommendation below: **the current demo dataset is 56x28x6 — quasi-2D, ~9.4k cells.** Six z-cells means central-difference Q has at most 4 valid interior z-samples, so a 3D iso-surface on this data will be a few coarse stacked bands, not FluidX3D's signature tubes (those come from 256³–2000³ grids). The plan therefore distinguishes the honest hero shot for *this* dataset (the z-mid cut plane) from the 3D iso pipeline (built now, targeted at future full-resolution cases).

---

## 1. What a CFD wake animation is supposed to look like

Every "beautiful CFD" image you have ever seen — FluidX3D's YouTube demos, the propeller and motorbike shots from OpenFOAM tutorials, the DrivAer wake renders — is built from the same four ingredients:

1. **An opaque, lit, smooth solid obstacle.** The body is the anchor of the image: matte, neutral gray (FluidX3D uses flat 0xDFDFDF with headlight diffuse shading, kernel.cpp:2530-2572) or colored by surface pressure. Never translucent. Never a blob.
2. **A crisp iso-surface of a vortex criterion (Q, or Lambda2) colored by velocity magnitude.** This is *the* hero shot — at full 3D resolution. FluidX3D's signature image is a marching-cubes Q-criterion surface with per-vertex rainbow coloring from trilinearly interpolated velocity (kernel.cpp:2854-2919). OpenFOAM's propeller tutorial does exactly the same via `#includeFunc Q` + an isoSurface at Q=1000 with `interpolate true` (tutorials propeller/system/surfaces:33-39). Vortices appear as smooth, sharp-edged *tubes*, not fog. On a quasi-2D dataset, the equivalent hero shot is ingredient 3.
3. **A cutting plane through the wake with smoothly interpolated color.** The motorBike centerline y-normal plane sampling (p U) with `cellPoint` interpolation (motorBike/system/cutPlane:14-31) is the canonical 2D diagnostic. Gouraud-smooth, hard-edged, flat geometry. For our 56x28x6 demo case, a z-mid plane colored by z-vorticity or |U| *is* the picture — it degrades not at all with depth, unlike marching-cubes tubes.
4. **Streamlines (or sparse glyphs) with per-segment field coloring**, seeded upstream or in the wake (motorBike/system/streamlines:16-40; kernel.cpp:2772-2825). For *unsteady* wakes viewed with a time scrubber, the genre-standard motion cue is animated particles/pathlines rather than instantaneous streamlines — FluidX3D ships this as its own mode (VIS_PARTICLES in the same bitmask, defines.hpp:59-66); streamlines frozen per-frame are physically misleading in time-varying flow.

The unifying philosophy — explicit in both codebases — is **aggressive sparsity: draw only where the data is meaningful, and draw it as surfaces and lines**. FluidX3D early-outs every cell that is gas, solid, or below a velocity threshold (kernel.cpp:2470, 2593, 2842); its rainbow colormap deliberately ends at *black* so quiescent fluid fades into the black background instead of clamping to blue (kernel.cpp:106-129, defines.hpp:33). Even its one volumetric mode is importance-weighted so boring fluid is fully transparent (kernel.cpp:2631-2637). ParaView's default first render, as driven by OpenFOAM's reader plugin, is the mesh plus walls — nothing volumetric at all (vtkPVFoam.C:375-381).

**Why our current output reads as wrong to anyone who knows CFD:** we do the exact opposite of all four ingredients.

- Our primary (and effectively only) mode is a **full-volume translucent ray-march** (`FVR/Public/Render/FlowVizVolumeRayMarchShader.h`, `FVR/Private/Render/FlowVizVolumeRayMarchDispatcher.cpp`) — the thing neither reference uses as a default. A uniform opacity ramp means *every* voxel contributes, so quiescent air renders as haze and the whole domain becomes a glowing cloud. Real CFD viewers show mostly *empty space* with a few sharp structures in it.
- The **domain renders as a translucent white box** — the ray-march proxy's box hull (`FVR/Public/Scene/FlowVizVolumeComponent.h:65-121`) drawn as visible geometry. FluidX3D never draws the domain as a solid; what looks like a box in its demos is a 12-edge wireframe that emerges from silhouette-edge extraction of the boundary shell (kernel.cpp:2492-2517, lbm.cpp:596). A milky hull in front of everything is exactly the "washed out" look the user complained about, and no reference image contains one.
- The **obstacle is a flat green translucent blob** (NaN-masked voxels rendered through the volume path). Both references render the obstacle as the *most* opaque, most solid-looking thing in the scene — an exact boundary mesh in OpenFOAM/ParaView (sampledPatch.C:122-198), a marching-cubes surface in FluidX3D. We already have the exact-geometry path: `FlowVizBoundary::BuildPatches` (`FVR/Public/Scene/FlowVizBoundaryMesh.h:57-61`, `FVR/Private/Scene/FlowVizBoundaryMesh.cpp`) builds boundary patch triangles from the CVM file — it is simply never attached to the viewport as a lit opaque mesh. Translucent obstacles destroy the depth cue that makes wakes legible: vortices must *disappear behind* the body.
- Our **vortex cores are blurry purple columns** because they are being volume-integrated instead of extracted as a surface or shown on a plane. On this dataset, a z-mid cut plane of vorticity turns the same data into sharp, recognizable vortex-street lobes; on future 3D data, a Q iso-surface turns it into tubes.

In short: the user's reaction is correct. The problem is not colormap taste — it is that we render a fog where the genre renders surfaces and planes.

---

## 2. How FluidX3D gets its look

Ranked by visual impact, most important first. A recurring theme: no OpenGL/Vulkan — everything is OpenCL compute kernels rasterizing/raytracing directly into a shared bitmap + atomic fixed-point z-buffer next to the simulation data (lbm.cpp:471-472, kernel.cpp:171-189). We do not need to copy the architecture, but the *techniques* transfer — with the caveat (see section 5, item on compositing) that its "everything z-buffers together" property does *not* transfer verbatim to UE's renderer.

1. **Velocity-colored Q-criterion marching-cubes iso-surface** (kernel.cpp:2854-2919). Per cell: compute Q = 0.5(||ω||² − ||S||²) from central differences at all 8 cube corners, run *interpolating* marching cubes at a small threshold (GRAPHICS_Q_CRITERION = 0.0001, defines.hpp:38 — note: LBM lattice units, not transferable as a number), color each vertex by rainbow(|u|) trilinearly sampled, apply headlight shading, interpolate barycentrically per pixel (kernel.cpp:282-316). Sub-cell vertex placement + per-vertex color is what makes tubes look smooth and continuous instead of voxelated. Highest-leverage technique for full-resolution cases; band-like on 6 z-cells.

2. **Opaque obstacle as a marching-cubes surface — "the fix for ugly blob obstacles"** (kernel.cpp:2530-2572). Binary solid flags → `marching_cubes_halfway` (vertices at edge midpoints, kernel.cpp:442-467) → flat opaque light gray 0xDFDFDF with headlight diffuse shading. Nearly every aerodynamics demo runs VIS_FLAG_SURFACE | VIS_Q_CRITERION (setup.cpp:395, 434, 462). Opaque gray keeps the obstacle visually neutral so colored flow pops against it, and occlusion carries the 3D read. (For us, the exact patch mesh beats voxel MC — see section 5.)

3. **Sparsity + a colormap that fades to the background.** Rainbow ends at black on a black background (kernel.cpp:106-129, defines.hpp:33); the diverging density map passes through the background color itself so neutral density is literally invisible (kernel.cpp:147-149). Velocity glyphs below threshold are culled (kernel.cpp:2593). Nothing "interesting-free" is ever drawn. Note this trick is *per-map*: it works because rainbow-to-black lands on the black background; viridis on black does not fade out (its low end is dark purple-blue, close but not equal), and no colormap fades out on a light background. The transferable principle is "boring values must approach the background color," not "use their rainbow" — which is also perceptually non-uniform; turbo exists precisely as the fix, and our own API already flags this (`IsPerceptuallyUniform`, `FVR/Public/CFDViz/CFDVizColorMaps.h:82`).

4. **Headlight diffuse shading with an ambient floor** (kernel.cpp:150-163). brightness = max(1.5·|cos(normal, view)|, 0.3). Camera-attached light means every view is well-lit with zero setup; 1.5× overdrive saturates facing surfaces, the 0.3 floor keeps silhouettes legible, |cos| makes it two-sided. Dead simple, and it is 100% of the surface shading in their screenshots.

5. **Importance-weighted volumetric rendering — the *right* way to do what we currently do** (kernel.cpp:2606-2707). Their single-GPU field mode DDA-marches each pixel ray, accumulating a *self-weighted* average where the weight is "interestingness" (|u| distance from mid-range; |ρ−1|; (T−T̄)²), and final opacity is derived from how much weighted material was crossed (kernel.cpp:2679-2680). Quiescent fluid contributes zero → transparent. Energetic regions glow. No uniform opacity ramp anywhere.

6. **Silhouette-edge wireframes for flags and the domain** (kernel.cpp:2462-2528). A cell edge is drawn only if adjacent neighbors differ in flag type, so a solid block collapses to its outline and the boundary shell collapses to a clean 12-edge domain box. This is what a domain boundary should look like: a hairline reference frame, not a hull.

7. **Bidirectional unit-arc-length streamlines** (kernel.cpp:2772-2825). Seeds on a sparse regular grid (GRAPHICS_STREAMLINE_SPARSE = 8, i.e. every 8 cells — which, note, yields 6/8 = **zero** seed layers on our 6-cell z-axis; the recipe needs dataset-appropriate seeding, see section 5), integrated forward *and* backward with unit-speed steps p += (dt/|u|)·u, terminated at solids/low velocity/domain exit, each segment colored by local |u|. Constant spatial segment density regardless of speed; each line doubles as a graph of the field. It also ships **VIS_PARTICLES**, a passive-tracer particle mode in the same bitmask — the unsteady-flow motion cue.

8. **Free-surface raytracing against a photographic skybox** (kernel.cpp:2980-3010, 639-655, 747-780; skybox image loaded at lbm.cpp:493). Fresnel-ish grazing reflection, Beer–Lambert absorption, refraction at n=1.333, gradient-interpolated C1 normals. Less relevant to a wind/wake tool, but the transferable lesson is that *environment lighting does most of the perceptual work* — one bounce off a good environment map beats fancy shading on flat color. In UE we get this nearly free from sky light / reflection captures.

9. **Critically damped camera** (graphics.hpp:137, 159-161). Log-space zoom and mouse rotation both eased with exponential decay (half-lives 0.083s / 0.031s). A large part of why their captured videos read as "professional."

10. **Self-consistent legend + HUD, baked into exported frames** (main.cpp:6-57). The colorbar samples the identical colormap function the kernels use, with SI-unit tick labels — the legend can never disagree with the render, and it ships inside every captured image, not just the interactive window.

---

## 3. How the OpenFOAM/ParaView world does it

OpenFOAM renders **nothing** itself. `paraFoam` touches an empty sentinel file and execs ParaView (bin/paraFoam:379-386). The division of labor is strict and worth copying:

**The solver owns data and viz *geometry*:**
- **Derived fields** are one-line function objects: Q (Q.C:50-72), Lambda2, vorticity, enstrophy, yPlus, wallShearStress, fieldAverage (UMean/UPrime2Mean). Each registers its result field on the mesh database, so any derived field can immediately feed any sampler — "iso-surface of Q colored by |U|" is just two dict entries chained by field name (propeller/system/functions:22, surfaces:18-45). Note the propeller's Q=1000 threshold is in SI units (s⁻²) for that case's scale — like FluidX3D's lattice-unit 0.0001, it does not transfer as a number to any other dataset.
- **Extracted surfaces**: cutPlane (point+normal), isoSurface (isoField+isoValue), thresholdCellFaces, patch surfaces — all through one watertight polyhedral cutting kernel (cutPoly, cutPolyIsoSurface.C:59-159) with vertices placed by linear interpolation along real mesh edges.
- **Streamlines** via Lagrangian particle tracking: seed rake/sphere/patch, direction forward|backward|both, lifeTime cap, fields sampled along the track (streamlines.H:82-139, streamlines.C:107-174).
- **Field mapping choice**: `interpolate yes` → smooth per-point (Gouraud) color; `interpolate no` → honest flat per-cell values (sampledIsoSurfaceSurfaceTemplates.C:31-78). The shipped defaults use `cellPoint` smooth interpolation (etc/caseDicts surface.cfg).

**ParaView owns all presentation:** colormaps, ranges, legends, opacity, glyphs, camera, animation. The OpenFOAM repo contains *zero* colormap or styling configuration. The only defaults OpenFOAM asserts are *selection* defaults: first load enables internalMesh + wall/inlet/outlet patches, hides numerical constraint patches (vtkPVFoamUpdateInfo.C:491-521, etc/paraFoam:19), and pre-checks p/U/T/alpha fields (etc/paraFoam:17). (General knowledge, not from the corpus: ParaView does expose volume rendering with full opacity transfer functions and blend modes including MIP in its UI — but its default representation for a loaded dataset is Surface, and the tutorial recipes below never use volume rendering for the canonical pictures.)

**The canonical wake recipes are written down in tutorials**, and none of them is a volume cloud:
- motorBike: centerline cut plane (p U, cellPoint) + 20 upstream line-seeded streamlines + forceCoeffs (system/functions:17-19).
- drivaerFastback: sphere of 30 streamline seeds *in the wake*, x-normal wake plane, centerline plane, body patch surface colored by pressure (system/functions:20-45).
- propeller: Q field + isoSurface at Q=1000 + cut plane + propeller patch surface (system/surfaces:18-45).
- wallShearStress on the body patch is the standard "oil-flow" / surface-flow-pattern diagnostic; surface LIC of wall shear is its classic presentation (the function object exists in the corpus; the LIC presentation is general viz knowledge).

The lesson for FlowViz: our plugin is both solver-side sampler *and* viewer, so we get to define both halves — but the *vocabulary* users expect is exactly this: cut plane, iso-surface of a derived field colored by another field, patch surface, streamline rake, pathline particles.

---

## 4. Gap analysis for FlowViz

Ranked by distance from the expected look (worst first).

| # | Aspect | FlowViz today | What the references do | Distance |
|---|--------|---------------|------------------------|----------|
| 1 | Primary flow depiction | Full-volume alpha-composited ray-march with a linear opacity ramp (`FVR/Private/Render/FlowVizVolumeRayMarchDispatcher.cpp`, transfer function in `FVR/Public/Render/FlowVizTransferFunction.h`) | Sparse surfaces + lines: cut planes, Q iso-surfaces colored by \|U\| (kernel.cpp:2854-2919; propeller/system/surfaces:33-39), streamlines. Even FluidX3D's volume mode is importance-weighted so quiet fluid is invisible (kernel.cpp:2631-2637) | Severe — this is the core "doesn't look like CFD" problem |
| 2 | Obstacle | Flat green translucent NaN blob via the volume path | Opaque, lit, smooth solid: exact boundary patch mesh (sampledPatch.C:122-198, drivaerFastback functions:45) or gray MC surface (kernel.cpp:2530-2572). **Our exact-mesh builder exists**: `FlowVizBoundary::BuildPatches` (`FVR/Public/Scene/FlowVizBoundaryMesh.h:57`), tested in `FVR/Private/Tests/FlowVizBoundaryMeshTest.cpp` — it is not attached to the viewport | Severe |
| 3 | Domain hull | Ray-march proxy's box hull drawn as translucent geometry (`FVR/Public/Scene/FlowVizVolumeComponent.h:65-121`) | Hairline 12-edge wireframe (silhouette-edge extraction, kernel.cpp:2492-2517) or nothing; the box is a clipping/entry volume, not visible geometry | Severe — and the cheapest fix on this list |
| 4 | Vortex depiction | Blurry purple columns inside the volume cloud | On quasi-2D data: cut-plane vorticity (the honest hero for 56x28x6). On 3D data: crisp MC Q/Lambda2 tubes with sub-cell interpolation | High |
| 5 | Streamlines / glyphs | Built and tested — `BuildSliceGlyphs`/`BuildStreamlines` (`FVR/Public/Flow/FlowVizFlowInspection.h:110-137`) feed `UCFDVizFlowComponent` (instanced cone glyphs + `ULineBatchComponent` lines, `FVR/Public/Scene/FlowVizFlowComponent.h`) — but not wired into the default picture | On by default in every tutorial recipe; bidirectional, threshold-culled, field-colored (kernel.cpp:2772-2825; motorBike/system/streamlines). Plus a particle/pathline mode for unsteady flow (VIS_PARTICLES) | High — the code exists, it just isn't in the picture |
| 6 | Slices | Slice slab through the ray-march volume (`FVR/Public/UI/SFlowVizSlicePanel.h`, `FlowVizSliceViewModel.h`) | Hard 2D cut plane with smooth per-point interpolation (motorBike/system/cutPlane; kernel.cpp:2709-2769); optionally LIC on the plane | Medium |
| 7 | NaN / solid-mask handling | NaN voxels flow into sampling and shading with no stated policy | FluidX3D masks via flags and early-outs solids before any math touches them (kernel.cpp:2470) | Medium — silent, and it poisons every pipeline in section 5 unless fixed first |
| 8 | Background/opacity discipline | Uniform linear opacity ramp; everything contributes; default UE scene background | Colormaps approach the background at "boring" values; velocity threshold culls dead cells (kernel.cpp:106-129, 147-149, 2593) | Medium |
| 9 | Surface shading | Optional lighting toggle on a volume | Always-on headlight diffuse with ambient floor on every surface (kernel.cpp:150-163) | Medium (blocked on having surfaces to shade) |
| 10 | Legend | Colormap picker in Slate panel (`FVR/Public/UI/SFlowVizTransferFunctionPanel.h`); no in-viewport legend; captures un-annotated with color info despite an existing burn-in path (`FVR/Public/Capture/FlowVizAnnotate.h:60`) | Unit-aware colorbar sampling the identical colormap function, baked into exports (main.cpp:6-57) | Low-medium |
| 11 | Colormaps | 8 perceptual maps with uniformity/diverging metadata (`FVR/Public/CFDViz/CFDVizColorMaps.h:38-90`) | FluidX3D: 3 fixed maps (one non-uniform rainbow); ParaView: user choice | **Ahead on maps, behind on policy** — no field-to-map-class defaults, no NaN color (see section 5) |
| 12 | Camera | UE editor viewport + orbit camera (`FVR/Public/Scene/FlowVizOrbitCamera.h`); no ortho presets, no axes triad | Smoothed orbit (graphics.hpp:137, 159-161); ParaView-style axis-aligned views and orientation triad are the genre standard (general knowledge) | Low-medium |

---

## 5. Recommended rendering direction

Prioritized. Items 0–3 are the "stop looking horrible" tier. Each item names the class being changed.

**0. Establish the NaN policy first — it underpins everything below.**
NaN = solid mask, formalized in one place (extend `FFlowVizFieldSampler`, `FVR/Public/Flow/FlowVizFieldSampler.h:33`, or a sibling mask type):
- Derivative stencils (Q, vorticity): one-sided differences at mask boundaries; never central-difference across a NaN neighbor.
- Trilinear sampling (`FFlowVizFieldSampler::Sample`/`SampleVector`): a sample whose support touches NaN either fails (current `bool` return — keep it) or clamps to the valid sub-cell; pick one and test it.
- Marching cubes: skip any cube with a NaN corner (the obstacle surface comes from the patch mesh, not from field MC, so no visual hole results).
- Slices: mask-out cells render as background/obstacle color, hard-edged — not interpolated across.
- Streamline/particle integration: terminate on entering the mask (this is FluidX3D's solid-termination rule translated to our representation).
This is FluidX3D's flag-masking discipline (kernel.cpp:2470) translated to a NaN representation, and it is pure-function testable per the TDD rule.
*Difficulty:* low-medium; mostly specification plus tests.

**1. Kill the white hull; replace with a hairline wireframe box.**
In `UFlowVizVolumeComponent` (`FVR/Public/Scene/FlowVizVolumeComponent.h:65-121`): keep the box hull as ray-march *entry geometry* when volume mode is on, but stop drawing it as a visible translucent surface. Draw the domain as 12 thin lines instead — a `ULineBatchComponent` on `AFlowVizCaseActor` (`FVR/Private/Scene/FlowVizCaseActor.cpp`), following FluidX3D's silhouette-outline convention (kernel.cpp:2492-2517).
*Payoff:* immediate, dramatic — the wash-out disappears and contrast of everything else doubles. *Difficulty:* trivial (an afternoon; mostly deletion).

**2. Draw the obstacle as an opaque lit mesh — attach `FlowVizBoundary::BuildPatches` output to the viewport.**
The exact patch geometry builder exists and is tested (`FVR/Private/Scene/FlowVizBoundaryMesh.cpp`, `FVR/Private/Tests/FlowVizBoundaryMeshTest.cpp`). Wire its `FFlowVizBoundaryPatchGeometry` triangles into `AFlowVizCaseActor` as a `UProceduralMeshComponent`/dynamic mesh with an **opaque** matte material (neutral gray, roughness ~0.7), lit normally — FluidX3D's 0xDFDFDF headlight look (kernel.cpp:2530-2572) for free from the engine. **The exact patch mesh is the primary path, full stop.** Halfway marching cubes on the NaN mask (kernel.cpp:442-467 is the reference algorithm) is a fallback for datasets without patch geometry only — and at 6 z-cells it will look visibly blocky, so it must never be presented as a peer option. Remove the green blob entirely.
*Payoff:* the scene gains its anchor; occlusion starts doing depth work. *Difficulty:* low — attachment + material.

**3. Default view for the demo dataset = z-mid cut plane, opaque and smooth. Iso-surface is built now but is the hero only at full resolution.**
- **Cut plane first.** Convert the slice slab (`FlowVizSliceViewModel`, `SFlowVizSlicePanel`) into a true 2D quad sampled bilinearly via `FFlowVizFieldSampler`, Gouraud-shaded, rendered **opaque** (kernel.cpp:2709-2769; ParaView-equivalent `cellPoint` interpolation, motorBike/system/cutPlane:14-31). Default field on the demo case: z-vorticity (computed per the NaN policy) or |U|. This is the honest hero image for 56x28x6 and is instantly recognizable as CFD.
- **Iso-surface pipeline, targeted at future full-res cases.** Compute Q from the velocity field via one-sided-aware central differences (formula: Q.C:61; stencil approach: kernel.cpp:2854-2919). Extract an *interpolating* marching-cubes surface (true edge interpolation, kernel.cpp:416-441 — not blocky midpoint) into a procedural mesh with per-vertex color = colormap(trilinear |U|) and normals from field gradients (kernel.cpp:722-731), rebuilt per frame — microseconds at demo size. If 3D iso is shown on the demo data at all, trilinearly upsample the field 2–4x in z first and label it as smoothed; do not promise tubes from 4 interior z-samples.
- **Iso-value default that actually defaults:** neither FluidX3D's 0.0001 (lattice units, defines.hpp:38) nor propeller's 1000 (SI s⁻²) transfers. Default to a fixed percentile of positive Q — P90, computed per case (or per frame when auto-range is on) — with the slider in physical units, a live field min/max readout (the volFieldValue min/max pattern, volFieldValue.H:84-96), and the chosen value persisted per field.
*Payoff:* blurry purple columns become a sharp, correctly-scaled wake picture today, and the tube shot is ready when the data is. *Difficulty:* medium.

**4. Attach flow primitives; fix the seeding for this grid; add particles for unsteady playback.**
- Wire the existing `UCFDVizFlowComponent` (`FVR/Private/Scene/FlowVizFlowComponent.cpp`) into the default picture via `SetFlowData`, fed by `BuildStreamlines`/`BuildSliceGlyphs` (`FVR/Private/Flow/FlowVizFlowInspection.cpp`).
- **Do not copy FluidX3D's every-8-cells seed grid verbatim** — 6/8 = 0 z-layers = zero streamlines on the demo data. Seed dataset-appropriately: an upstream rake of ~20 points in the z-mid plane (motorBike/system/streamlines:16-40 style), or a sparse grid with per-axis spacing (z-spacing 2). Keep FluidX3D's good parts: bidirectional integration, unit-arc-length steps, termination at mask/low-|u|/domain-exit, per-segment |U| color, seed culling below a velocity threshold (kernel.cpp:2772-2825).
- Glyphs: sparse, color encodes speed; audit `MakeGlyphTransforms`' current batch-max length normalization (`FVR/Public/Scene/FlowVizFlowComponent.h:67`) against the uniform-length convention and add a uniform-length mode if needed. Keep instanced static meshes for the demo-scale glyph counts; move to Niagara only if counts grow to where ISM update cost shows.
- **Add a particle/pathline mode to the roadmap now** (advect passive tracers through the time-varying field during playback, fading trails). For a time-scrubbing viewer of an unsteady wake this is the genre-standard motion cue (FluidX3D's VIS_PARTICLES); per-frame streamlines alone are physically misleading in unsteady flow. Acceptable to ship after streamlines, not acceptable to omit from the plan.
*Difficulty:* low-medium — renderers exist; integrator, seeding UI, and the particle advector are the new work.

**5. Face the UE compositing rules explicitly — the reference's z-buffer model does not transfer.**
FluidX3D composites all modes through its own software z-buffer (kernel.cpp:171-189). In UE, translucent draws do not z-write and sort per-object, so stacking translucent volume + translucent slice + translucent lines produces exactly the artifacts we are trying to escape. The rules for FlowViz:
- **Opaque, z-writing:** obstacle mesh, iso-surface, cut plane, domain wireframe. All of them. This is what makes occlusion legible and is why mode toggles can compose freely.
- **Translucent:** the volume mode only, and it must depth-test against the opaque set (scene-depth-aware ray termination in the ray-marcher).
- The per-vertex-colored MC and cut-plane meshes need a vertex-color-driven opaque material (colormap LUT applied on CPU into vertex colors, or a LUT texture sampled by vertex UV).
- Per-frame geometry updates go through the ProceduralMesh/dynamic-mesh update path (positions + colors), not full component recreation — same discipline `UFlowVizVolumeComponent` already documents for its own proxy updates (`FVR/Public/Scene/FlowVizVolumeComponent.h:1123-1170`).
*Difficulty:* this is a design constraint on items 2–4, not a separate task — but it must be written down before the materials are authored.

**6. Demote and fix volume rendering.**
Keep it as an optional mode, off by default. When on: importance-weight the accumulation in the ray-march shader (`FVR/Private/Render/FlowVizVolumeRayMarchDispatcher.cpp`) — weight ∝ distance from "boring" (kernel.cpp:2631-2637, 2679-2680) — and anchor the transfer function (`FVR/Private/Render/FlowVizTransferFunction.cpp`) so the bottom ~20–30% of the value range is fully transparent. Viewport background: a dark **unlit** backdrop or flat sky with **fixed manual exposure** — auto-exposure will fight a mostly-black scene and pump the image, so it must be disabled in this view mode. Ray-march jitter: **on by default**, with TAA/TSR-friendly per-frame noise (blue noise or interleaved gradient), trading banding for grain the temporal AA resolves; step count fixed at a tested default, not a user toggle.
*Payoff:* when someone does want the fog, it reads as "energetic regions glow" instead of "milk." *Difficulty:* low — shader and material changes to the existing path.

**7. Roadmap: LIC.** 2D LIC on the cut plane is the canonical dense wake diagnostic and is *ideally* suited to a quasi-2D 56x28 grid — it degrades gracefully at low resolution where MC tubes cannot. Rank it directly behind the basic cut plane. Surface LIC of wall shear stress on the obstacle patches ("oil-flow" visualization) is the standard use of the wallShearStress function object and slots in once item 2's mesh exists. Both are deferred, neither is optional for a credible CFD viewer.

**8. Camera and polish tier.**
- Axis-aligned orthographic view presets (+X/−X/+Y/−Y/top) — the standard way cut planes are viewed — plus an orientation triad/axes widget in the viewport corner. Cheap, and half of why reference screenshots look "standard." Extend `UFlowVizOrbitCamera` (`FVR/Public/Scene/FlowVizOrbitCamera.h`).
- View bookmarks + camera-pose copy-to-clipboard (the G-key idea, info.cpp:116-123) for reproducible shots; exponential-decay smoothing for captured video (graphics.hpp:137).
- **Legend burned into captures, not just Slate.** The burn-in path already exists: `FlowVizAnnotate::BurnFooter`/`DrawText` (`FVR/Public/Capture/FlowVizAnnotate.h:60-75`, tested in `FVR/Private/Tests/FlowVizAnnotateTest.cpp`) behind `FVR/Public/FlowVizCaptureLibrary.h`. Extend it to bake a colorbar sampling the identical `CFDViz::ColorMaps` LUT the material uses, with units, value range, time/frame stamp, and case name — FluidX3D's bake-the-HUD-into-exports discipline (main.cpp:6-57). The in-viewport Slate legend is the interactive twin of the same drawing code, not a separate implementation.

Per the global TDD instruction: items 0, 3, and 4 have pure-function cores (NaN-aware stencils and sampling, Q computation, MC topology/vertex placement, streamline/particle integration and termination, percentile iso-default) that should get failing tests first — they are functions over arrays, ideal for it, and the existing `FVR/Private/Tests/` suite (e.g. `FlowVizFlowInspectionTest.cpp`, `FlowVizBoundaryMeshTest.cpp`) is the pattern to follow.

---

## 6. UI controls: keep / add / drop

**Keep (validated by the references):**
- **Colormap picker + value range (global/per-frame/manual)** (`FVR/Public/UI/SFlowVizTransferFunctionPanel.h`). ParaView owns exactly this presentation layer; FluidX3D users suffer from *not* having it at runtime (its ranges are compile-time #defines, defines.hpp:34-42). Our per-frame auto-range is something FluidX3D lacks — keep it. **But add colormap policy**, which the maps alone don't provide: default map *class* per field — sequential (viridis/turbo) for |U|, Q, vorticity magnitude; diverging (CoolWarm) with zero-anchored midpoint and symmetric range for pressure/Cp and signed vorticity; log scale option for k/epsilon. The metadata hooks already exist (`IsDiverging`, `IsPerceptuallyUniform`, `FVR/Public/CFDViz/CFDVizColorMaps.h:82-90`) — wire them to defaults. Add an explicit NaN/out-of-range color (ParaView has one — general knowledge — and our NaN-as-solid representation makes it mandatory).
- **Slice panel and clip planes** (`FVR/Public/UI/SFlowVizSlicePanel.h`, `SFlowVizClipPanel.h`). Direct analogs of cutPlane and clipping. Keep; repoint the slice at the new opaque cut plane, with a z-mid preset button for the demo case.
- **Probes + line chart** (`FVR/Public/UI/SFlowVizProbePanel.h`, `FVR/Public/Flow/FlowVizChartSeries.h`). OpenFOAM's `probes` function object writes this as time-series files (probes.H:98-107); we do it live. In our read of FluidX3D's input handling we found no interactive point-probing in the render window. A genuine differentiator — keep.
- **Transport bar with speeds** (`FVR/Public/UI/SFlowVizTransportBar.h`). Absent from FluidX3D (no scrubbing/rewind) and solver-side in OpenFOAM. Interactive time scrubbing is a legitimate differentiator — and it is exactly what makes the particle/pathline mode (section 5, item 4) matter. Keep.
- **Field list.** Matches the reader-plugin pattern; pre-check the common fields the way OpenFOAM's plugin does (etc/paraFoam:17).

**Add:**
- **Visualization-mode toggles as the top-level UI** — a row of independent on/off switches: Obstacle surface / Cut plane / Iso-surface / Streamlines / Particles / Glyphs / Volume. This is FluidX3D's interaction model (a bitmask of independently toggleable VIS_* modes, defines.hpp:59-66, bound to number keys in its input handling). It works there because everything shares one z-buffer; it works for us **only** under section 5's compositing rules (all modes opaque except volume) — which is a second reason those rules are load-bearing. This replaces the current single-mode framing.
- **Iso-surface controls:** derived-field choice (Q / vorticity / |U|), iso-value slider in physical units with live min/max and the P90-of-positive-Q default, and a separate "color by" field selector. "Iso-surface of X colored by Y" is the composable primitive both references are built on (propeller/system/surfaces:33-39).
- **Streamline/particle seed controls:** rake line or sphere placement, seed count, direction (forward/backward/both), max length (streamlines.H:134-139, drivaerFastback functions:20-45); particle count and trail length for the pathline mode. Default seeding must be dataset-aware (z-mid rake on the demo case — see section 5, item 4).
- **In-viewport colorbar legend + capture burn-in** tied to the actual `CFDViz::ColorMaps` LUT and range, sharing drawing code with `FlowVizAnnotate` so viewport, legend, and exported frames can never disagree.
- **Camera preset row:** +X/−Y/top ortho buttons, axes triad toggle, view bookmarks.

**Drop or demote:**
- **Global opacity slider as a primary control — this is the smell.** It exists only because our primary mode is a translucent cloud. FluidX3D's transparency is a compile-time expert option that *disables shading* (defines.hpp:44, kernel.cpp:160-162); OpenFOAM's shipped recipes never rely on scene-wide translucency. (ParaView the application does expose per-representation opacity and full volume transfer-function editing — general knowledge — but as per-object properties inside an expert panel, not a scene-wide slider, and not in the default picture.) Once surfaces are the default, demote opacity to the volume mode's advanced settings (`FVR/Public/UI/SFlowVizRenderSettingsPanel.h`).
- **Composite mode (MIP/iso/alpha) at top level.** Volume-rendering jargon; fold into the volume mode's advanced panel (ParaView keeps its equivalent blend-mode control inside the volume representation's properties — general knowledge). The "iso" composite mode is superseded by real marching-cubes iso-surfaces.
- **Lighting/jitter toggles.** Commit to defaults instead of exposing switches: surface shading always on (headlight-style, ambient floor, kernel.cpp:150-163 — or simply UE's lit path with a fixed key light); ray-march jitter always on with TAA-friendly noise and a fixed step count; manual exposure in dark-background mode. FluidX3D ships zero lighting toggles and looks better for it.
- **The hull.** Delete any UI implying the domain box is a renderable object; it becomes a wireframe reference frame (and, invisibly, the volume mode's ray entry geometry).

**Bottom line:** our foundations — runtime ranges, scrubbing, live probes, a colormap library with uniformity metadata — are genuinely ahead of FluidX3D. The gap is in what gets drawn and in unstated policy (NaN handling, per-field map defaults, compositing rules, dataset-aware seeding). Ship items 0–3 of section 5 and the same demo data will produce the picture people recognize as CFD: an opaque gray obstacle in a hairline wireframe box, sitting on a sharp z-mid vorticity plane — with the iso-surface pipeline ready for the day the data is deep enough to grow tubes.
---

## 7. Beyond correct: the film / AAA tier

Sections 1–6 get FlowViz to *credible* — the picture a CFD engineer recognizes. This section is the explicit goal past that: frames you could cut into a nature documentary or a AAA game's wind-tunnel scene. The references top out here — FluidX3D's renderer is a software rasterizer with a headlight; ParaView is an analysis tool. UE5 is the one place in this comparison built for film-quality real-time imagery, and we are already inside it. The gap is ours to take.

What separates "correct CFD viz" from "film CFD viz" is not more data — it is light, material, motion, and shot language:

**7.1 Light and shadow do the physics reading.**
- The iso-surface and cut plane become **Lumen-lit dynamic meshes**: the Q-tubes should receive soft GI and sky occlusion, and — the big one — **cast shadows onto the obstacle and the floor**. A vortex tube whose shadow slides along the cylinder reads as a physical object; an unlit colored surface reads as a plot. This is free once the geometry is real meshes with `bCastDynamicShadow` (procedural mesh sections already support it) — no custom renderer work.
- A deliberate **three-point studio rig** as the Presentation profile's world: key light angled across the flow axis (grazes the tubes, brings out surface curvature), cool fill, rim light from behind the wake so tube silhouettes glow at their edges. Scientific profile keeps the flat headlight. The profile system (`FFlowVizWorkspaceModel::SetPresentationMode`) already exists as the switch.
- **Volumetric shadowing inside the fog mode**: when the volume is on, self-shadowed single scattering (a shadow ray toward the key light per march step, coarse-stepped) turns "glowing milk" into "smoke caught in light" — the single highest-impact upgrade the volume path can receive. Our ray-march shader owns its loop, so this is additive.

**7.2 Materials that read as matter, not as color.**
- Obstacle: a true PBR material — brushed metal or matte lab-model gray with a clearcoat, normal-mapped micro-detail, so the studio rig has something to catch. Colored-by-pressure remains a mode: pressure drives a colormap into Base Color while roughness/spec stay physical.
- Iso-surfaces: colormap in Base Color but with **low roughness + slight subsurface/opacity falloff at grazing angles** (Fresnel-driven), which is what makes film smoke/water surfaces feel wet and alive rather than plastic.
- Streamlines upgrade from `ULineBatchComponent` debug lines (fixed-width, unlit, aliased — fine for scaffolding, never film) to **ribbon/tube geometry**: camera-facing ribbons or swept tubes with radius ∝ local |U| or seeded-constant, lit, shadowed, colormap along length. This is precisely what Niagara Ribbons exist for.
- Particles/pathlines: **Niagara GPU simulation sampling our velocity field**. We already upload the field as a 3D texture for the ray-marcher; a Niagara data-interface (or a simple custom module sampling the same texture) advects hundreds of thousands of sprites/ribbons in real time — the "million dust motes tracing the wake" shot that no CPU advector reaches. Motion-blurred sprites with depth-of-field are the signature film-CFD look.

**7.3 Temporal and cinematic polish.**
- **TSR + motion vectors everywhere**: procedural meshes rebuilt per frame must write velocities (previous-frame positions on the mesh sections) so temporal AA and motion blur treat the evolving iso-surface as a moving object, not per-frame noise. This is the difference between "flickering plot" and "flowing surface" in motion.
- **Frame interpolation as shipped behavior**: our player already blends between stored frames; the film tier makes sub-frame interpolation drive *geometry* too (morph MC vertices between adjacent frames where topology allows, cross-fade where it does not).
- **Cinematic camera moves**: the orbit camera gains a dolly/flythrough track mode — a spline the user records (or presets: slow orbit, wake fly-through, obstacle close-up push-in) with exponential smoothing, plus cine-camera DoF focused on the obstacle. Pairs with the capture library for one-click "render this orbit as a 4K sequence."
- **Movie Render Queue integration** for offline-quality export: MRQ gives us temporal supersampling, high-res tiling, and — where the licensee build allows — the **path tracer**, which turns the Presentation still into an actual film frame (real GI on the tubes, soft area shadows, physically correct volume scattering). The annotate/burn-in pipeline runs as an MRQ post pass so exports stay self-describing.
- **Tone mapping discipline**: manual exposure locked per profile (auto-exposure pumps on dark scientific backgrounds), filmic tonemapper defaults, optional very subtle bloom on the brightest colormap end — enough to make peak vorticity *glow*, never enough to halo the legend.

**7.4 The bar, made testable.** "Looks AAA" is subjective; these are not:
- Every visible primitive is lit and shadow-casting (no unlit debug geometry in Presentation mode) — assert by material/component audit.
- Zero temporal shimmer on a static shot at 4K/TSR (screenshot-diff two consecutive frames of a paused sim).
- The blind side-by-side: capture our cylinder wake next to a FluidX3D wake render and an OpenFOAM/ParaView propeller shot; a harsh reviewer (human or model) judging "which is the film frame" must pick ours at better than chance. This is the acceptance test the project's original brief asked for, now concrete.

---

## 8. UE5 architecture brief

How the above maps onto engine systems — what we build custom, what we lean on the engine for, and where the seams go. The organizing rule: **simulation data is engine-agnostic; everything the viewer draws is a thin, swappable consumer of tested pure builders.** That rule already holds (`BuildPatches`, `BuildSliceGlyphs`, `BuildStreamlines`, the samplers — all pure, all tested); the architecture below extends it rather than replacing it.

```
                        ┌─────────────────────────────────────────────┐
                        │            FlowVizRuntime (plugin)          │
  case on disk ───────► │  CFDViz IO (pure C++, no engine types)      │
  (.cfdviz)             │  FFlowVizCasePlayer  (decode, cache, clock) │
                        │  Pure builders: MC/Q, cut-plane, stream-    │
                        │  lines, glyph xforms, boundary patches      │  ◄── all TDD'd,
                        └───────────────┬─────────────────────────────┘      headless
                                        │ typed geometry/field payloads
            ┌───────────────────────────┼──────────────────────────────────┐
            ▼                           ▼                                  ▼
  ┌──────────────────┐      ┌────────────────────────┐        ┌─────────────────────┐
  │ Volume path       │      │ Surface path           │        │ Flow path            │
  │ SceneProxy + RDG  │      │ UDynamicMesh/PMC       │        │ Niagara systems      │
  │ compute ray-march │      │ sections: obstacle,    │        │ GPU particles, ribbon│
  │ (exists today)    │      │ iso, cut plane, wire   │        │ streamlines, glyphs  │
  │ depth-aware, self-│      │ box; Lumen-lit, shadow,│        │ sampling the SAME 3D │
  │ shadowed scatter  │      │ motion vectors         │        │ field textures       │
  └──────────────────┘      └────────────────────────┘        └─────────────────────┘
            │                           │                                  │
            └───────────── one world: ACFDVizCaseActor components ─────────┘
                                        │
                     Slate workspace (view models → push, unchanged)
                     Profiles: Scientific (flat) / Presentation (studio rig)
                     Capture: FlowVizCaptureLibrary → MRQ for film export
```

**The three render paths and their engine substrates:**

1. **Volume path (custom, exists).** Stays a scene-proxy compute dispatch through RDG. Gains: scene-depth-aware termination (composite correctly behind opaque surfaces), single-scatter shadow ray, blue-noise jitter that TSR resolves. This is the one path where we own every pixel — right choice for DVR, wrong default for everything else.

2. **Surface path (engine-native, new).** Obstacle patches, marching-cubes iso-surfaces, cut planes, and the wireframe domain box are **mesh components** (`UProceduralMeshComponent` now; `UDynamicMeshComponent` if per-frame rebuild cost demands its faster update path). They opt into everything the engine does well — Lumen GI, virtual shadow maps, TSR motion vectors, path-traced export — for free. Per-frame updates go through section-update calls (positions/normals/colors), never component recreation; vertex color carries the colormap via one shared LUT-sampling material function so viewport, legend, and burned-in captures can never disagree. The pure builders emit `FFlowVizMeshPayload` (positions/indices/normals/colors + previous-frame positions for velocity) on worker threads; the component applies on game thread — the exact worker/apply split the sampling service already uses.

3. **Flow path (Niagara, new).** Streamline ribbons, glyph fields, and the film-tier particle advection become Niagara systems fed by a **field data-interface**: the same 3D textures the volume path uploads, exposed read-only to Niagara GPU sims. CPU-side `BuildStreamlines` remains the tested reference implementation and the headless-test seam; Niagara is the *presentation* of the same math at film scale. Determinism note: captures that must be reproducible pin the Niagara seed and tick it from the player's clock, not wall time.

**Cross-cutting rules (the load-bearing ones):**
- **Compositing:** opaque + z-write for every surface; the volume alone is translucent and must read scene depth. Mode toggles compose because of this rule and only because of it (section 5, item 5).
- **One clock:** `FFlowVizCasePlayer` remains the single time authority. Every path — texture uploads, mesh rebuilds, Niagara ticks — consumes its frame selection; nothing keeps a second clock. Blending weight for sub-frame interpolation flows to all three paths from the same selection struct.
- **One color authority:** `CFDViz::ColorMaps` LUTs feed the material function, the Slate legend, and the capture burn-in. A colormap added in one place appears in all three or the build fails a parity test.
- **Threading:** decode and geometry building on `UE::Tasks` workers (already the pattern); render-thread work stays in proxies/RDG; game thread only applies payloads and pushes view-model state. No path blocks on the GPU.
- **Profiles are world-state, not shader flags:** Presentation swaps in the studio light rig, PBR obstacle material, particle density, and exposure lock as an actor/component configuration; Scientific swaps back. The renderer code paths do not branch on profile — the *scene* does.
- **Testing stays honest:** every new pure builder (MC topology, Q stencils, ribbon geometry, payload velocity generation) lands test-first like its predecessors; engine-side consumers stay thin enough that the existing "thin consumer" doctrine (documented in FINAL_REPORT) keeps applying. The film-tier acceptance gate is the blind side-by-side in 7.4.

**Sequencing note.** The architecture is deliberately incremental from today's code: item 1 of section 5 deletes hull geometry, items 2–3 stand up the Surface path with builders that exist or are specified, section 7's lighting/material work then lands *on top of the same components* without rework, and Niagara replaces debug lines only after the CPU reference implementations are attached and verified. No step strands a previous one.
