# OpenFOAM / ParaView feature parity

Required by `plan.md` §19. Maps each expected post-processing concept to the
FlowViz control, its implementation status, where the code lives, known
limitations, and the milestone that delivers it.

**Status as of this revision: nothing in the visualization pipeline is
implemented.** The reader layer (manifest, CVF, CVM, CVA, CRC-32C, colormap
tables) exists and is tested; every row below that describes a *view* is
`Not started`. This document exists now, ahead of the features, because §19
says *"do not claim full parity where a feature is only scaffolded"* — and the
cheapest moment to write an honest status table is before there is anything to
oversell.

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

---

## 1. Pipeline and application shell

| ParaView concept | FlowViz control | Status | Source | Known limitation | Milestone |
| --- | --- | --- | --- | --- | --- |
| Pipeline browser | Pipeline tree panel | Not started | — | — | F |
| Properties / display settings | Properties panel | Not started | — | — | F |
| Multiple views | Split view layout | Not started | — | Plan §17 defers synchronized side-by-side views past MVP | Deferred |
| Save / load state | Session save/load | Not started | — | — | F |
| Screenshot / movie export | Export panel | Not started | — | Headless still capture exists for QA (`Tools/capture/`), which is not the same thing as a user-facing export | F |
| Animation controls | Timeline transport | Not started | — | — | C |
| Camera presets | Camera toolbar | Not started | — | — | D |

## 2. Representations

| ParaView concept | FlowViz control | Status | Source | Known limitation | Milestone |
| --- | --- | --- | --- | --- | --- |
| Surface | Boundary patch surface | Not started | — | CVM/CVA mesh reading is `Done`; nothing renders it yet | C |
| Surface with edges | Surface + wireframe overlay | Not started | — | — | D |
| Wireframe | Wireframe mode | Not started | — | — | D |
| Points | Point cloud mode | Not started | — | — | D |
| Volume | Ray-marched volume renderer | Not started | — | The AAA deliverable. Modes required by §9: front-to-back alpha, MIP, MinIP, average, iso-surface | C |

## 3. Filters — geometry

| ParaView concept | FlowViz control | Status | Source | Known limitation | Milestone |
| --- | --- | --- | --- | --- | --- |
| Slice | Arbitrary slice plane | Not started | — | — | D |
| Clip | Clip planes | Not started | — | §9 requires *multiple* clipping planes, not one | D |
| Threshold | Threshold filter | Not started | — | — | E |
| Contour / iso-surface | Ray-marched iso-surface | Not started | — | GPU marching cubes is explicitly deferred (§20); the 0.1 iso-surface is ray-marched | D |
| Transform | Transform gizmo | Not started | — | — | E |
| Warp by vector | Warp filter | Not started | — | Full FEA deformation rendering is deferred (§20) | Deferred |
| Crop box | Crop box | Not started | — | — | D |

## 4. Filters — flow inspection

| ParaView concept | FlowViz control | Status | Source | Known limitation | Milestone |
| --- | --- | --- | --- | --- | --- |
| Glyph | Vector glyphs | Not started | — | — | E |
| Stream tracer | Streamlines | Not started | — | — | E |
| Particle tracer / pathlines | Particles | Not started | — | Fully accurate time-dependent pathlines are deferred (§20). 0.1 particles are advected through the stored frame sequence and must be labelled as such | E / Deferred |
| Calculator | Derived-field calculator | Not started | — | — | E |

## 5. Quantitative readout

Everything in this table is subject to rule 9 of `plan.md` §4: **numeric probes
must sample the underlying field, not the rendered color texture.** A probe
that reads back pixels is a wrong answer that looks precise, which is worse
than no probe.

| ParaView concept | FlowViz control | Status | Source | Known limitation | Milestone |
| --- | --- | --- | --- | --- | --- |
| Probe location | Point probe | Not started | — | — | E |
| Plot over line | Line probe | Not started | — | — | E |
| Plot over time | Time-series chart | Not started | — | — | E |
| Histogram | Histogram panel | Not started | — | — | E |
| Field minimum / maximum | Statistics panel | Not started | — | Manifest-declared statistics are already parsed and carry a `bValid` flag; they are *declared* values, not recomputed from the decoded field | E |
| Integrate variables | Integration panel | Not started | — | — | E |
| Surface sampling | Surface probe | Not started | — | — | E |

## 6. Time

| ParaView concept | FlowViz control | Status | Source | Known limitation | Milestone |
| --- | --- | --- | --- | --- | --- |
| Temporal interpolation | GPU frame interpolation | Not started | — | Rule 7 (§4): any visual temporal interpolation **must be visibly identified as interpolation** on screen | C |
| Temporal statistics | Temporal statistics filter | Not started | — | — | E |
| Animation / playback | Timeline scrub + transport | Not started | — | — | C |

## 7. Color and legends

| ParaView concept | FlowViz control | Status | Source | Known limitation | Milestone |
| --- | --- | --- | --- | --- | --- |
| Color map editor | Colormap picker | Partial | `Private/CFDViz/CFDVizColorMaps.cpp` | Colormap tables, LUT construction, perceptual-uniformity and diverging queries are implemented and tested. There is no editor UI and nothing consumes the LUT yet | C / D |
| Opacity transfer function | Transfer-function editor | Not started | — | — | D |
| Scalar legend | Legend overlay | Not started | — | `VISUAL_QA.md` §1 makes this a hard requirement of the Scientific profile: every pseudocolored view carries field name, units, and numeric range | C |
| Stable global color range | Range mode control | Not started | — | Rule 8 (§4): global range is the default; per-frame auto range must be visibly labelled, because a color that changes meaning between frames fabricates apparent physics | C |

## 8. Data access

This is the part that is real.

| ParaView concept | FlowViz control | Status | Source | Known limitation | Milestone |
| --- | --- | --- | --- | --- | --- |
| Reader (case open) | CFDViz case reader | Done | `Private/CFDViz/CFDVizManifest.cpp` | Manifest 1.0. Rejects `mesh-vertex`/`mesh-element` grids per format §3.2; `structures[]` is reserved and parsed-when-present | B |
| Volume data | CVF bricked-volume reader | Partial | `Private/CFDViz/CFDVizVolumeReader.cpp` | Implemented and it compiles, but it is the one reader with **no committed test** — the largest file in the layer, unverified. Not `Done` until `CFDVizVolumeReaderTest.cpp` lands | B |
| Surface mesh | CVM reader | Done | `Private/CFDViz/CFDVizMeshReader.cpp` | — | B |
| Mesh arrays | CVA reader | Done | `Private/CFDViz/CFDVizArrayReader.cpp` | — | B |
| Boundary / block visibility | Patch visibility list | Not started | — | Patch IDs are read from CVM; nothing exposes them | D |
| Asynchronous load | Background case loader | Not started | — | Rule 1 (§4) forbids disk reads, decompression and full-field computation on the game thread. Not yet enforced by anything, because there is no loader | B/C |
| Direct OpenFOAM reader | — | Not started | — | Explicitly deferred (§20). FlowViz reads the CFDViz format; conversion is upstream | Deferred |

## 9. Explicitly out of parity scope

`plan.md` §21. Listed so the absences read as decisions rather than gaps:
production LBM/FEA solvers, meshing, boundary-condition setup, solver
convergence controls, OpenFOAM dictionary editing, CAD repair,
general-purpose material authoring, cloud backend, authentication, database
storage, multi-user sync, mobile builds.

Also deferred past 0.1 (§20): unstructured grids, AMR, GPU marching cubes,
OpenXR, Pixel Streaming, georeferencing, multi-case difference fields, live
solver networking.

---

## Maintaining this document

Update the row in the same commit that changes the status. A parity table
edited later is a parity table that is wrong in between, and this one is
specifically the document a reader will consult to find out whether a feature
is real.

When a row moves to `Done`, it must be able to point at a test. `Done` with an
empty Source column is not a status, it is a claim.
