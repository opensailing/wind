# OpenFOAM / ParaView feature parity

Required by `plan.md` §19. Maps each expected post-processing concept to the
FlowViz control, its implementation status, where the code lives, known
limitations, and the milestone that delivers it.

**Status as of this revision:** the reader layer is complete and tested, the
volume renderer draws real data through a real shader, and the Slate workspace
exists and is operable. What is largely still missing is the wiring *between*
them: several controls edit a correct, tested view model that no production
code reads. Those rows say `Scaffolded`, and the limitation column names the
missing caller, because that is the fact a reader needs.

## Status vocabulary

These words are load-bearing. They are not synonyms.

| Status | Means |
| --- | --- |
| `Done` | Works end to end, has automated tests, and has passed visual review where visual. |
| `Partial` | Works for a stated subset. The limitation column says which. |
| `Scaffolded` | Types, API, or UI exist; the feature does **not** produce a correct result. Never presented to a user as available. |
| `Not started` | No code. |

`Scaffolded` is the dangerous one — it is the status that gets rounded up to
"done" in a status meeting. Rule 15 of `plan.md` §4 applies to it directly:
**do not ship nonfunctional buttons.** A scaffolded feature is hidden or
visibly marked unavailable in the UI, never left clickable and inert.

### The failure mode this table exists to catch

Four times now, a unit has been built correctly, tested thoroughly, and joined
to nothing — `#26` (dispatcher), `#40`/`#41` (render settings), `#42` (clip
planes in the shader), `#50` (clip panel to volume). Every one was green in the
suite while producing no pixels, because each test installed the seam it was
testing.

So a row here is only `Done` when a **production** caller exists, and
`Scaffolded` explicitly means "the unit works; nothing calls it." The check is
`grep` for the symbol excluding `Private/Tests/`. Where a row says
"no production caller", that grep was run and returned nothing.

---

## 1. Pipeline and application shell

| ParaView concept | FlowViz control | Status | Source | Known limitation | Milestone |
| --- | --- | --- | --- | --- | --- |
| Pipeline browser | Pipeline tree panel | Partial | `Private/UI/SFlowVizPipelinePanel.cpp` | The open case's volume fields as clickable rows; clicking switches the displayed field through `SetField` (its first widget caller) and re-pushes. Deliberately a flat list, not a tree: the runtime holds one case, and a tree implying pipeline editing would be rule 15 at the metaphor level. `FlowViz.UI.PipelinePanel.FieldsDriveTheModel` | F |
| Properties / display settings | Properties panel | Partial | `Private/UI/SFlowVizWorkspace.cpp` | The workspace hosts four panels — transfer function, clip, slice, probe — plus a transport bar, in an `SSplitter` layout, registered as a nomad tab (`FlowVizWorkspaceTab.cpp`). It is not a general property browser: each panel is hand-built for its view model, and three of the four drive nothing (see their rows) | F |
| Multiple views | Split view layout | Not started | — | `plan.md` §16.6 asks for the comparison data-model interfaces and a backlog item *now*, implementing synchronized side-by-side only if the milestone allows. The interfaces do not exist either, so this row owes §16.6 more than it owes a view | F |
| Save / load state | Session save/load | Partial | `Private/UI/FlowVizSession.cpp`, `Private/UI/FlowVizWorkspaceModel.cpp`, `Private/UI/SFlowVizWorkspace.cpp` | Serialisation, round-trip, relinking, capture/apply and the workspace entry points (`SFlowVizWorkspace::{SaveSession,LoadSession,LoadState}`) are implemented and tested, and a load pushes the restored state into the bound volume so the render follows. **No UI affordance and no file picker**: the entry points take a path, and nothing in the tab offers a menu item or a dialog, so a user still cannot reach them without a console command (task #53). The picker is deliberately not here — this module has no `DesktopPlatform` dependency and adding one would break packaged builds | F |
| Screenshot / movie export | Export panel | Partial | `Private/Capture/FlowVizAnnotate.cpp`, `FlowVizCaptureLibrary.cpp` | `CaptureAnnotatedPNG` (BlueprintCallable) burns the DoD 15 footer — case, field, displayed time, range, colour-bar legend sampled from the real colormap table — into the pixels; a footer that does not fit fails the capture rather than writing an unannotated file. No movie export, no widget invoking it | F |
| Animation controls | Timeline transport | Partial | `Private/UI/SFlowVizTransportBar.cpp`, `Private/Playback/FlowVizCasePlayer.cpp`, `Private/UI/SFlowVizWorkspace.cpp` | Transport bar, timeline view model and case player all work and are tested, and `SFlowVizWorkspace::SetVolume` installs `MakeFrameSource(Model->Player)` into the bound component — a live read, so a scrub long after the binding still moves the image (task #57, `FlowViz.UI.Workspace.FrameSeam`). A clock now drives it: `SFlowVizWorkspace` registers an `FTSTicker` handler in `Construct` and removes it in the destructor, so Play advances the playhead and the display pair on its own and the proxy is re-marked when the pair changes (task #65, `FlowViz.UI.Workspace.ClockSeam`, four mutants killed). **Still missing: the picture does not follow.** Nothing uploads voxels during playback — `FFlowVizCasePlayer::SetTextureSet` has no caller at all, so the player's texture set is null and `DrainCompletedLoads` skips its `EnqueueUpload`; the component's own `UploadFrame` works but its only caller is the headless capture library. The playhead, the display selection and the dispatch are all correct and all point at texture slots nothing wrote (task #66) | C |
| Camera presets | Camera toolbar | Partial | `Private/Scene/FlowVizOrbitCamera.cpp` | The orbit camera model: frame-box, orbit (distance-preserving by construction), pan (view-plane, never dollies), zoom (multiplicative, clamped), pole clamp, and a session location/focus round trip — `FlowViz.Scene.OrbitCamera`. No input binding or toolbar yet; the model is the testable half | D |

## 2. Representations

| ParaView concept | FlowViz control | Status | Source | Known limitation | Milestone |
| --- | --- | --- | --- | --- | --- |
| Surface | Boundary patch surface | Partial | `Private/Scene/FlowVizBoundaryMesh.cpp` | `BuildPatches` produces one section per patch in Unreal space (winding swapped for the Y mirror), per-patch welding so visibility is a section toggle (DoD 13), undeclared patch ids collected loudly, empty patches removed. Verified against the sample's real geometry (the 45 cm cylinder). The scene-component binding that instantiates the sections is the remaining thin consumer | C |
| Surface with edges | Surface + wireframe overlay | Not started | — | — | D |
| Wireframe | Wireframe mode | Not started | — | — | D |
| Points | Point cloud mode | Not started | — | — | D |
| Volume | Ray-marched volume renderer | Partial | `Shaders/FlowVizVolumeRayMarch.usf`, `Private/Render/FlowVizVolumeRayMarchDispatcher.cpp`, `Private/Scene/FlowVizVolumeComponent.cpp` | All five composite modes required by `plan.md` §9 are implemented in the shader — `FLOWVIZ_MODE_ALPHA`, `MAXIMUM`, `MINIMUM`, `AVERAGE`, `ISOSURFACE` — plus a `DIAGNOSTIC` echo mode used to verify parameter transport. Dispatch runs from the scene proxy and is verified end to end under a real RHI. As of 2026-08-06 all six are selectable from the workspace's Render panel (`SFlowVizRenderSettingsPanel`, #74) as well as from Blueprint. Still not `Done`: visual review has not passed | C |

## 3. Filters — geometry

| ParaView concept | FlowViz control | Status | Source | Known limitation | Milestone |
| --- | --- | --- | --- | --- | --- |
| Slice | Arbitrary slice plane | Partial | `Private/UI/FlowVizSliceViewModel.cpp` (`MakeSlabPlanes`), `Private/UI/FlowVizWorkspaceModel.cpp` (`ComposeClipWithSlice`) | The slice reaches the renderer as a SLAB of the volume: two opposed clip planes composed into the pushed clip at push time (#77), through the existing clip shader path. Hiding retracts exactly its own planes; the user's planes win the six-plane budget; zero thickness renders as a 2% minimum slab. `FlowViz.UI.Workspace.SliceSeam` asserts by which points survive; `SliceCoexistence` is DoD 8. Not a textured plane with its own sampler — that remains open | D |
| Clip | Clip planes | Done | `Private/UI/FlowVizClipViewModel.cpp`, `Private/UI/SFlowVizClipPanel.cpp`, `Private/UI/FlowVizWorkspaceModel.cpp` (`PushClipToVolume`) | Up to `MaxClipPlanes` planes, per-plane enable and invert, presets, and a crop box, all normalised against the volume's own domain. Panel edits reach the renderer through `SLATE_EVENT(OnClipChanged)` → `SFlowVizWorkspace::HandleClipChanged` → `PushClipToVolume` → `UCFDVizVolumeComponent::SetClip`. Verified by `FlowViz.UI.Workspace.VolumeBinding`, which clicks a real preset button and asserts the plane arrives at the volume | D |
| Threshold | Threshold filter | Not started | — | The shader has a value-range reject path (`bRejectNonFinite`, `ValueRangeMin/Max`), which is data validity, not a user threshold filter | E |
| Contour / iso-surface | Ray-marched iso-surface | Partial | `Shaders/FlowVizVolumeRayMarch.usf` (`FLOWVIZ_MODE_ISOSURFACE`) | Implemented in the shader and exercised over a fixture volume. GPU marching cubes is explicitly deferrable per `plan.md` §20, which states the release must not block on it "if the ray-marched iso-surface works correctly". No user control selects the mode | D |
| Transform | Transform gizmo | Not started | — | — | E |
| Warp by vector | Warp filter | Not started | — | Full FEA deformation rendering is deferred (§20) | Deferred |
| Crop box | Crop box | Done | `Private/UI/FlowVizClipViewModel.cpp` | Normalised against the volume's physical size at push time, not the UI's. The crop is captured and restored around `SetDomainSize` (which resets it deliberately), so a crop survives every subsequent push — asserted directly, because getting it wrong discards the user's drag silently rather than failing | D |

## 4. Filters — flow inspection

| ParaView concept | FlowViz control | Status | Source | Known limitation | Milestone |
| --- | --- | --- | --- | --- | --- |
| Glyph | Vector glyphs | Partial | `Private/Flow/FlowVizFlowInspection.cpp` | `BuildSliceGlyphs`: a grid over the slice plane, unit directions from the trilinear sampler, magnitude carried for length/colour, outside samples refused (no glyph, never a zero arrow). Asserted against the mock's strictly-downstream flow. The instanced-mesh viewport binding is the remaining consumer | E |
| Stream tracer | Streamlines | Partial | `Private/Flow/FlowVizFlowInspection.cpp` | `BuildStreamlines`: RK4 over the normalised direction field (constant arc length per step), seed rake, both directions, end reasons disclosed (LeftDomain/Stagnant/MaxSteps -- a line that stops mid-domain says why). Steady-state per plan.md section 20; asserted against the mock wake's two regimes (free-stream exits downstream, wake seeds may orbit vortex cores). The ribbon/line-batch viewport binding is the remaining consumer | E |
| Particle tracer / pathlines | Particles | Not started | — | `plan.md` §20 defers *fully accurate* time-dependent pathlines; §10.9 still expects 0.1 to advect particles through the stored frame sequence, labelled as such. Neither exists | E / Deferred |
| Calculator | Derived-field calculator | Not started | — | — | E |

## 5. Quantitative readout

Everything in this table is subject to rule 9 of `plan.md` §4: **numeric probes
must sample the underlying field, not the rendered color texture.** A probe
that reads back pixels is a wrong answer that looks precise, which is worse
than no probe.

| ParaView concept | FlowViz control | Status | Source | Known limitation | Milestone |
| --- | --- | --- | --- | --- | --- |
| Probe location | Point probe | Partial | `Private/UI/FlowVizProbeViewModel.cpp`, `Private/UI/FlowVizWorkspaceModel.cpp` (sampling service) | The workspace sampling service (#75) samples every visible probe at the DISPLAYED frame on a worker and delivers through `SetProbeReading` -- the "nothing samples" era is over, and `FlowViz.UI.WorkspaceModel.Sampling`'s differential (no reading before the service runs, a finite nonzero one after, at an independently pinned cell) is what says so. The rule 10 guard (`UnsampledProbeDoesNotReadAsZero`) still stands for the pre-sample window. No viewport pick, no marker | E |
| Plot over line | Line probe | Partial | `Private/Flow/FlowVizChartSeries.cpp`, `Private/UI/FlowVizProbeViewModel.cpp` | `BuildLineSeries` samples the field's magnitude along the line at the displayed frame, distance or 0..1 axis (axis relabels, never resamples -- asserted), gaps as first-class points (rule 10: a line through the masked cylinder shows the solid body as gaps, never zeros). `BuildTimeSeries` is the point probe's DoD 11 counterpart, keeping the full time axis across missing frames. The chart widget that draws a series is the remaining consumer | E |
| Plot over time | Time-series chart | Partial | `Private/Flow/FlowVizChartSeries.cpp` | `BuildTimeSeries` (DoD 11): one point per frame at the probe's position, physical-time axis, missing frames as gaps at their own time rather than skipped. The chart widget is the remaining consumer | E |
| Histogram | Histogram panel | Not started | — | — | E |
| Field minimum / maximum | Statistics panel | Not started | — | Manifest-declared statistics are already parsed and carry a `bValid` flag; they are *declared* values, not recomputed from the decoded field | E |
| Integrate variables | Integration panel | Not started | — | — | E |
| Surface sampling | Surface probe | Not started | — | — | E |

## 6. Time

| ParaView concept | FlowViz control | Status | Source | Known limitation | Milestone |
| --- | --- | --- | --- | --- | --- |
| Temporal interpolation | GPU frame interpolation | Not started | — | Rule 7 (§4): any visual temporal interpolation **must be visibly identified as interpolation** on screen. The timeline view model already carries an interpolation badge for this; nothing renders it | C |
| Temporal statistics | Temporal statistics filter | Not started | — | — | E |
| Animation / playback | Timeline scrub + transport | Scaffolded | `Private/Playback/FlowVizCasePlayer.cpp`, `Private/UI/FlowVizTimelineViewModel.cpp` | Same gap as "Animation controls" above: the player decodes frames, the transport bar drives the view model, and `SetFrameSource` is never called outside tests (task #57) | C |

## 7. Color and legends

| ParaView concept | FlowViz control | Status | Source | Known limitation | Milestone |
| --- | --- | --- | --- | --- | --- |
| Color map editor | Colormap picker | Scaffolded | `Private/CFDViz/CFDVizColorMaps.cpp`, `Private/UI/SFlowVizTransferFunctionPanel.cpp` | Colormap tables, LUT construction, perceptual-uniformity and diverging queries are implemented and tested, and the picker UI exists. The dispatcher hard-codes `MakeDefault(ColorMaps::Default, 0, 1)` — its own comment says "no component owns a transfer function yet" — so **choosing a colormap changes nothing on screen** (task #48) | C / D |
| Opacity transfer function | Transfer-function editor | Scaffolded | `Private/UI/FlowVizTransferFunctionViewModel.cpp` | `FFlowVizTransferFunctionViewModel::ApplyToRayMarchParameters` is implemented and tested and has **no production caller** — note that the identically-named clip and render-settings methods *are* called, from `FlowVizVolumeRayMarchDispatcher.cpp:202` and `:181`, so grepping the bare method name is misleading here. Also note the degenerate-data trap in task #48: the view model's default and the dispatcher's hard-coded default are the same values, so a test comparing them proves nothing | D |
| Scalar legend | Legend overlay | Not started | — | `VISUAL_QA.md` §1 makes this a hard requirement of the Scientific profile: every pseudocolored view carries field name, units, and numeric range. Every occurrence of "legend"/"scalar bar"/"color bar" in the source is inside a comment — several of them comments describing what the legend *would* consume. No widget, no type, no draw call | C |
| Stable global color range | Range mode control | Partial | `Private/UI/FlowVizTransferFunctionViewModel.cpp` | Range binding, clamping and per-field rebinding are implemented and tested, and `bClampToRange` reaches the shader (#29). The *labelling* required by rule 8 — per-frame auto range must be visibly marked, because a color that changes meaning between frames fabricates apparent physics — has no UI | C |

## 8. Data access

This is the part that is real.

| ParaView concept | FlowViz control | Status | Source | Known limitation | Milestone |
| --- | --- | --- | --- | --- | --- |
| Reader (case open) | CFDViz case reader | Done | `Private/CFDViz/CFDVizManifest.cpp` | Manifest 1.0. Rejects `mesh-vertex`/`mesh-element` grids per format §3.2; `structures[]` is reserved and parsed-when-present. Path containment is defence-in-depth: `IsSafeRelativePath` rejects `..` segments lexically first, so the `IsUnderDirectory` check is unreachable through `ResolveRelativePath` and is tested as a direct assertion rather than a differential | B |
| Volume data | CVF bricked-volume reader | Done | `Private/CFDViz/CFDVizVolumeReader.cpp` | `CFDVizVolumeReaderTest.cpp` and `CFDVizVolumeIntegrityTest.cpp` have landed, including the sparse-brick case that was failing when this row last read `Partial` | B |
| Surface mesh | CVM reader | Done | `Private/CFDViz/CFDVizMeshReader.cpp` | — | B |
| Mesh arrays | CVA reader | Done | `Private/CFDViz/CFDVizArrayReader.cpp` | Cross-language bridge against the Python reference is verified (`CFDVizKnownValuesTest.cpp`) | B |
| Boundary / block visibility | Patch visibility list | Not started | — | Patch IDs are read from CVM; nothing exposes them | D |
| Asynchronous load | Background case loader | Partial | `Private/Playback/FlowVizCasePlayer.cpp`, `Private/Playback/FlowVizFrameCache.cpp` | The player decodes off the game thread with a bounded frame cache, satisfying rule 1 for playback. Not `Done` while the player reaches no renderer (task #57), because the property that matters — no hitch on the game thread during scrub — cannot be observed until it does | B/C |
| Direct OpenFOAM reader | — | Not started | — | Explicitly deferred (§20). FlowViz reads the CFDViz format; conversion is upstream | Deferred |

## 9. Explicitly out of parity scope

Listed so the absences read as decisions rather than gaps. Both lists are
`plan.md`'s own, quoted rather than paraphrased — a parity document that
restates a scope boundary in its own words is a second source of truth for it.

Out of scope entirely (§21): a production LBM solver, a production FEA solver,
meshing, boundary-condition setup, solver convergence controls, direct editing
of OpenFOAM dictionaries, full CAD repair, general-purpose material authoring,
a cloud backend, authentication, database storage, multi-user synchronization,
mobile builds.

Version 0.1 may defer (§20): full FEA deformation rendering, unstructured CFD
grids, AMR, GPU marching cubes, fully accurate time-dependent pathlines,
OpenXR, Pixel Streaming deployment, georeferencing, multi-case difference
fields, a direct OpenFOAM reader, live solver networking.

§20 closes with a constraint that governs every `Scaffolded` row above: "Do not
allow deferred work to leave broken visible controls in the version 0.1 UI."

---

## Maintaining this document

Update the row in the same commit that changes the status. A parity table
edited later is a parity table that is wrong in between, and this one is
specifically the document a reader will consult to find out whether a feature
is real.

When a row moves to `Done`, it must be able to point at a test. `Done` with an
empty Source column is not a status, it is a claim.

And a row does not move to `Done` on the strength of a green test alone. Ask
first whether the test installs the seam it is testing — if it does, it proves
the unit works and says nothing about whether production ever builds it. That
is how four separate features were green and invisible. The clip row above cites
`FlowViz.UI.Workspace.VolumeBinding` rather than the clip view model's own tests
for exactly this reason: it clicks a button and looks at the volume.
