# FlowViz v0.1 — Final Report

Per `plan.md` §24. Everything below distinguishes **tested** functionality
(automated tests that ran on the reference machine) from **deferred** work.
Nothing untested is described as working.

## Environment

| | |
| --- | --- |
| Unreal Engine | 5.8.1 (`++UE5+Release-5.8`, CL 56057345) at `/Users/Shared/Epic Games/UE_5.8` |
| Host OS | macOS (Darwin 25.5.0), Apple M4, 32 GB, Metal |
| Build configuration | Development, Mac, arm64 |
| Python | 3.14 with numpy, pytest, jsonschema |

## Architecture summary

Three modules: `FlowVizApp` (game entry), `FlowVizRuntime` (runtime plugin —
readers, data model, rendering, UI), `FlowVizEditor` (editor tooling).
`FlowVizRuntime` loads `PostConfigInit` for its virtual shader path.

Layering inside the runtime plugin, dependencies pointing down only:

- **CFDViz readers** (`Private/CFDViz`) — manifest JSON, CVF bricked volumes,
  CVM surface meshes, CVA arrays. Solver units in, solver units out
  (engineering rule 4); the single solver→Unreal transform lives in
  `CFDVizTypes` and mirrors Y with the winding consequences handled at each
  consumer.
- **Playback** (`Private/Playback`) — the case player: worker-thread decode
  (rule 1: no disk I/O or zlib on the game thread), LRU frame cache under CPU
  and GPU byte budgets, display selection with interpolation disclosure.
- **Render** (`Private/Render`, `Shaders/`) — a hand-written global-shader ray
  marcher (ADR 002), six composite modes, clip planes/crop, transfer function
  LUT, status texture for invalid-voxel disclosure. All RHI resource calls
  confined to one file for engine-upgrade isolation.
- **Flow analysis** (`Private/Flow`) — CPU field sampler (trilinear,
  refusing), slice glyphs, RK4 streamlines, chart series. Pure builders,
  testable headless.
- **Scene** (`Private/Scene`) — the case actor and volume component (the
  render-thread seam), boundary patch geometry, the orbit camera model.
- **UI** (`Private/UI`) — view models (pure value state) and Slate panels.
  Panels edit view models and announce over delegates; `SFlowVizWorkspace`
  subscribes and pushes into the bound volume component. Session save/load
  round-trips every view model.
- **Capture** (`Private/Capture`, `FlowVizCaptureLibrary`) — headless PNG
  capture with the DoD 15 annotation footer (case, field, time, range,
  legend) burned into the pixels.

## Data format summary

`.cfdviz` = a directory: `manifest.json` (JSON Schema-validated), CVF bricked
volumes per field per frame (zlib, per-brick CRC-32C, per-brick statistics,
omitted-background-brick support), CVM triangle meshes with per-triangle patch
ids, CVA arrays with per-component statistics. Full spec:
`Docs/CFDVIZ_FORMAT.md`. Cross-language equivalence between the Python
reference reader and the C++ reader is pinned bit-exactly by
`known_values.json` (DoD 17, `CFDVizKnownValuesTest`).

## Commands

Generate/validate sample data, build, launch, test, package: see
[README.md](../README.md). All commands there are the ones run on this
machine.

## Test inventory (all green on the reference machine)

- **C++ automation**: 149 tests, all passing on the reference machine
  (`Tools/run_tests.sh FlowViz`), across `FlowViz.CFDViz.*` (readers),
  `FlowViz.Playback.*`, `FlowViz.Render.*`, `FlowViz.Scene.*`,
  `FlowViz.Flow.*`, `FlowViz.UI.*`, `FlowViz.Capture.*`. GPU-dependent arms
  run under `RHI=1`.
- **Python**: `Tools/cfdviz` pytest suite (writers, readers, validators,
  the mock generator's analytic invariants).
- **Harness self-tests**: `Tools/run_harness_tests.sh` — 21 files / 355
  checks, including three tree-level checkers (shell portability, frozen
  shader parameters, uncalled view-model setters) with three-valued verdicts
  (pass / fail / UNSCORED-is-not-a-pass).
- **Mutation verification**: `Tools/mutate.sh` campaigns with worktree
  isolation, green-baseline enforcement, and per-arm logs; multiple survivors
  found and closed across the UI seams (see `Docs/BACKLOG.md` history).

## Implemented controls (tested)

Volume rendering with six composite modes; transfer-function editor
(colormaps, banding, reversal, manual/global/per-frame ranges, opacity curve,
clamp toggle, disclosure palette); clip planes with in-place editing, crop
box; slice rendered as a slab of the volume, coexisting with a second volume
(DoD 8); render settings panel (lighting, jitter, marching quality, no-data
colour); playback transport with loop/mode/speed/interpolation options;
per-frame range measurement and probe readback via the workspace sampling
service; boundary patch geometry with per-patch sections (DoD 13); velocity
glyphs on a slice (DoD 9); RK4 streamlines with disclosed end reasons
(DoD 10); chart series for line probes (distance/normalized axes) and point
probes over time, with first-class gaps (DoD 11, 12); pipeline panel with
field switching; Scientific/Presentation profile toggle; session save/load
including render settings and the profile flag (DoD 14); annotated screenshot
capture (DoD 15); diagnostics overlay and eight `FlowViz.*` console commands;
orbit camera model (frame/orbit/pan/zoom, session round-trip).

## Known limitations

- **The slice renders as a slab of the volume**, not a textured plane with
  its own sampler; slab opacity/trilinear affect the whole volume render.
- **Glyphs, streamlines and charts are computed and tested as geometry/series
  builders**; their in-viewport instanced-mesh / line-batch / chart-widget
  bindings are the thin consumers still to be attached to the workspace.
- **The orbit camera is a value model**; the input binding (pawn/viewport)
  is not yet attached in the packaged app.
- **Presentation profile is a preset bundle**, not VISUAL_QA §2's film-grade
  bar (multiple scattering, multi-scale density, tone mapping). That bar is
  explicitly open.
- **Windows is declared and untested.**
- **Playback texture streaming**: the player uploads via the workspace path;
  sustained-playback performance beyond the PERFORMANCE.md protocol is not
  characterised.

## Deferred (per plan.md §20's allowance)

FEA deformation, unstructured grids, AMR, GPU marching cubes, time-accurate
pathlines, OpenXR, Pixel Streaming, georeferencing, multi-case difference
fields, direct OpenFOAM reader, live solver networking.

## Engine compatibility notes

- UBT's `Build.sh` exits 0 on failure — every script greps for
  `Result: Succeeded` instead (documented in BUILD.md, enforced in tooling).
- `TCheckedFormatString` rejects mismatched `Printf` arity at compile time
  (caught one test bug).
- Slate attribute caches are not primed on bind
  (`UE_SLATE_WITH_ATTRIBUTE_INITIALIZATION_ON_BIND=0`); headless tests pump
  via `FlowVizSlateAttributePump`.
- `-nullrhi` and `RHI=1` runs are different suites; both are exercised.

## Third-party

Engine-bundled zlib and PNG (via `FImageUtils`); numpy/pytest/jsonschema on
the Python side. Notices: `Docs/THIRD_PARTY_NOTICES.md`.

## Packaged build

`Packaged/Mac/FlowViz.app` via `RunUAT.sh BuildCookRun` (commands in
README.md); the first cook's global-shader compile dominates the wall time.
**DoD 4 is verified on the packaged app**: with the sample staged beside the
packaged content, `FlowViz.LoadCase` from the packaged binary's console
opened it headless — `opened 'Mock Cylinder Wake (low resolution)' -- 20
frames, displaying 'U'` — with no manual asset importing.

Two defects the packaged smoke test caught that the editor never could:
GameDefaultMap named a level that did not exist (the cook WARNS and still
reports BUILD SUCCESSFUL; the app exited with MapNotFound before ExecCmds
ran), and the console commands' "open the FlowViz tab first" refusal pointed
at a menu only the editor has — they now invoke the tab themselves. Both are
the reason DoD 4 demands the PACKAGED check rather than the editor one.
