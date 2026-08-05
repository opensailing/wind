# FlowViz architecture

Required by `plan.md` §3. Describes the layer structure of `plan.md` §5, what
exists today, and — where the two differ — which is which.

**Status as of this revision: layer A is partly built, layer B is built for
the format types, and layers C through F do not exist.** Concretely: the
readers and the type vocabulary are real and tested; there is no subsystem, no
pipeline, no rendering component and no UI. Section headings below therefore
carry a status marker, and every claim of "exists" names a file you can open.

The status vocabulary is the one in
[`OPENFOAM_PARAVIEW_PARITY.md`](OPENFOAM_PARAVIEW_PARITY.md) — `Done`,
`Partial`, `Scaffolded`, `Not started` — and means the same thing here.
`Scaffolded` is the one that gets rounded up to "done"; it means types exist
and the feature does not work.

## Modules

Three build modules, two plugins.

| Module | Type | Purpose | Status |
| --- | --- | --- | --- |
| `FlowVizApp` | Primary game module | Application entry point | Scaffolded — entry point only |
| `FlowVizRuntime` | Runtime plugin, `PostConfigInit` | Format readers, data model, GPU path, rendering | Partial |
| `FlowVizEditor` | Editor-only plugin | Editor tooling | Scaffolded |

`FlowVizRuntime` loads at `PostConfigInit` specifically so its virtual shader
path mapping (`/Plugin/FlowViz` → the plugin's `Shaders/`) is registered before
any global shader compiles. Loading later makes shader compilation fail in ways
that are hard to attribute to the load order. See
[`BUILD.md`](BUILD.md).

The dependency direction is one-way and must stay that way:

```
    FlowVizApp  ─────►  FlowVizRuntime  ◄─────  FlowVizEditor
```

`FlowVizRuntime` depends on neither of the others. A packaged build ships
`FlowVizApp` + `FlowVizRuntime` with no editor module present, which is
`plan.md` goal 10 — a standalone application, not an Editor utility. Any
`#include` that reverses an arrow breaks the packaged build, and it breaks it
at package time rather than at desk.

## The layers

### A. Data source layer — `Not started` (readers exist beneath it)

The `ICFDVizDataSource` interface of §5A — `Open`/`Close`/`GetManifest`/
`RequestFrame`/`CancelRequest`/`GetAvailability`/`GetSourceStatistics` — **does
not exist yet**. Neither `FCFDVizFileDataSource` nor `FCFDVizMockDataSource`
has been written.

What exists is the layer below it: the format readers a file data source will
call.

| Component | File | Status |
| --- | --- | --- |
| Manifest parser | `Private/CFDViz/CFDVizManifest.cpp` | Done |
| CVM mesh reader | `Private/CFDViz/CFDVizMeshReader.cpp` | Done |
| CVA array reader | `Private/CFDViz/CFDVizArrayReader.cpp` | Done |
| CVF volume reader | `Private/CFDViz/CFDVizVolumeReader.cpp` | Partial — no committed test |
| Payload/codec | `Private/CFDViz/CFDVizPayload.cpp` | Done |
| CRC-32C | `Private/CFDViz/CFDVizCrc32C.cpp` | Done |
| Byte source/cursor | `Private/CFDViz/CFDVizByteSource.cpp`, `CFDVizByteCursor.h` | Partial — no direct test |
| Colormap tables | `Private/CFDViz/CFDVizColorMaps.cpp` | Done |
| Coordinate adapter | `Private/CFDViz/CFDVizTypes.cpp` | Done — belongs to layer E, listed here as it lives beside the readers |

`Done` here means: works, and has a committed automated test. Two rows are
`Partial` for the same reason — no test of their own:

- The **CVF reader** is the reader that decodes the volume every later
  milestone displays, so this is the most consequential gap in the table.
- The **byte source/cursor** is exercised indirectly by every reader test, but
  indirect coverage is not a test of its bounds checking. Its whole job is
  refusing to read past the end of a buffer (rule 12), and no committed test
  puts that behaviour under direct attack.

Adapters named in §5 as *reserved*, deliberately not built:
`FCFDVizLiveDataSource`, `FCFDVizOpenFOAMAdapter`, `FCFDVizVTKAdapter`. §5A is
explicit that the core visualizer must not depend on OpenFOAM libraries, so the
seam is the data-source interface and nothing upstream of it knows those
formats exist.

### B. Data model layer — `Partial`

The format-facing types exist in `Public/CFDViz/`:

| §5B name | Where it lives |
| --- | --- |
| `FCFDVizCaseManifest` | `FCFDVizCase` in `CFDVizManifest.h` |
| `FCFDVizCoordinateSystem` | `CFDVizTypes.h` |
| `FCFDVizTimeline` | `CFDVizManifest.h` |
| `FCFDVizGridDescriptor` | `FCFDVizGrid` in `CFDVizTypes.h` |
| `FCFDVizFieldDescriptor` | `FCFDVizField` in `CFDVizManifest.h` |
| `FCFDVizMeshDescriptor` | `FCFDVizMesh` in `CFDVizManifest.h` |
| `FCFDVizBoundaryPatch` | `CFDVizManifest.h` |
| `FCFDVizFieldStatistics` | `CFDVizManifest.h` |

Not yet written: `FCFDVizFrameRequest`, `FCFDVizFrameData`,
`FCFDVizTransferFunctionPreset`, and the UObject-facing `UCFDVizCase`. The
first two are the data-source layer's vocabulary and arrive with it.

Names differ from §5 wherever the plan says `...Descriptor` and the code says
the shorter noun (`FCFDVizCase`, `FCFDVizGrid`, `FCFDVizField`,
`FCFDVizMesh`). The short names are used at every call site and in every test;
renaming them to match the plan would be churn for its own sake. The mapping
above is the authoritative translation.

### C. Runtime subsystem — `Not started`

None of `UCFDVizSubsystem`, `FCFDVizFrameCache`, `FCFDVizRequestScheduler`,
`FCFDVizVolumeTextureUploader`, `FCFDVizCpuFieldSampler`,
`FCFDVizDerivedFieldService` or `FCFDVizSessionManager` exists.

This layer is where four of the non-negotiable rules are enforced, so its
design is constrained before it is written:

- **Rule 1** — no disk read, decompression, full-field computation or mesh
  generation on the game thread. The scheduler owns worker threads; the game
  thread only ever posts requests and consumes finished frames.
- **Rule 2/3** — no `UpdateResource` per display frame; double- or
  triple-buffered GPU resources. The uploader owns persistent RHI textures and
  cycles them. See [ADR 002](ADR/002-runtime-volume-rendering.md).
- **Rule 9** — `FCFDVizCpuFieldSampler` exists so probes read the *field*. A
  probe that samples the rendered texture would be easier and would report the
  colormap back to the user as data.
- **Rule 14** — expensive interactive updates debounced and cancellable, which
  is why `CancelRequest` is in the §5A interface rather than an afterthought.

### D. Pipeline layer — `Not started`

No `UCFDVizPipelineNode` and none of the twelve node types.

The load-bearing requirement is in §5D's tree example: one case feeds *many*
simultaneous visualizations — a volume, a slice with glyphs hanging off it,
streamlines, and a probe, all live at once. A design where the user picks one
active mode satisfies every individual node requirement and still fails this.

### E. Rendering layer — `Not started`

No components or actors. `Plugins/FlowVizRuntime/Shaders/` is empty.

The volume path is decided but unwritten — a custom `FGlobalShader`
ray-marcher over persistent RHI 3D textures, per
[ADR 002](ADR/002-runtime-volume-rendering.md), serving both the Scientific
and Presentation profiles from one sampler so the two cannot disagree about
what the data is.

**This is where the coordinate conversion happens, and nowhere else.** Per
[ADR 004](ADR/004-coordinate-systems.md), stored data stays right-handed in
solver units; the adapter in `CFDVizTypes.cpp` converts to Unreal's left-handed
centimetres at the render boundary. A second conversion site is how a value
gets mirrored twice, which is invisible because it is the identity.

### F. UI layer — `Not started`

No widgets and no view models. §5F requires core view state in C++ view models
rather than widget event graphs.

## Where the rules live

Each non-negotiable rule from `plan.md` §4 is enforced at a specific layer.
Recording it here so a rule does not become nobody's job:

| Rule | Enforced in | Today |
| --- | --- | --- |
| 1, 14 — nothing heavy on the game thread; cancellable | C (scheduler) | Not started |
| 2, 3 — no per-frame `UpdateResource`; multi-buffered | C (uploader) | Not started |
| 4 — physical data in solver units | A/B readers; conversion only in E | **Enforced** — readers store solver units; adapter is the only converter |
| 5, 6 — no silent normalize/quantize; declare quantization | A readers, E shader | Partial — readers do not alter values; no shader yet |
| 7 — interpolation visibly identified | D/F | Not started |
| 8 — stable global color ranges default | D/F | Not started |
| 9 — probes sample the field | C sampler | Not started |
| 10 — NaN/masked never silently zero | A readers, E shader | Partial — readers preserve them; no shader yet |
| 11 — neutral shading for pseudocolor | E | Not started |
| 12 — bounds-check before allocation | A readers | Partial — readers validate before allocating and are tested; the byte cursor doing the checking has no direct test |
| 13 — malformed case errors, never crashes | A readers | **Enforced** — `FCFDVizResult` everywhere; tested |
| 15 — no nonfunctional buttons | F | Not started |

Two rules are genuinely enforced and tested today (4, 13). Three are half
enforced (5, 10, 12) — 5 and 10 because only the reader half exists, 12 because
the primitive doing the checking is itself untested. The remaining ten have no
code at all. That distribution is the honest summary of this document: **the
layer that refuses bad input is built; the layer that draws anything is not.**

## Threading model

Stated ahead of the code because rule 1 is the easiest rule to violate by
accident.

```
    game thread          worker pool              render thread
    ───────────          ───────────              ─────────────
    post request  ─────► read, decompress,
                         derive fields
                              │
                              ▼
                         finished frame  ────────► upload into a
                                                   persistent RHI
    consume frame ◄──────────────────────────────  texture, cycle buffers
```

The game thread never touches disk, never decompresses, and never allocates a
field buffer. Anything that would block it belongs in the pool.

## Data flow, end to end

The path a value takes from disk to screen, and where it is allowed to change
form:

```
    manifest.json ──► FCFDVizCase                     (validated, rejected if malformed)
    .cvf / .cvm / .cva ──► readers ──► field values   (SOLVER UNITS, canonical frame)
                                    │
                                    ▼
                             frame cache / uploader   (no unit change)
                                    │
                                    ▼
                             RHI 3D texture           (no unit change)
                                    │
                                    ▼
                             ray-march shader ──► pixels
                                    ▲
                                    │
                       ONLY here: physical ──► Unreal cm, right ──► left handed
```

A value's units change exactly once, at the render boundary. A probe runs the
arrow backwards through `MakeUnrealToSolverTransform` and samples the stored
field — it never reads a pixel.

## Related documents

- [`BUILD.md`](BUILD.md) — how to build and test, and the unity-build trap
- [`CFDVIZ_FORMAT.md`](CFDVIZ_FORMAT.md) — the on-disk format
- [`OPENFOAM_PARAVIEW_PARITY.md`](OPENFOAM_PARAVIEW_PARITY.md) — feature status
- [`VISUAL_QA.md`](VISUAL_QA.md) — the blind visual review standard
- [ADR 002](ADR/002-runtime-volume-rendering.md) — volume rendering path
- [ADR 004](ADR/004-coordinate-systems.md) — coordinates and units
- [ADR 005](ADR/005-compression-codec.md) — compression codec

## Maintaining this document

A layer's status changes when its **tests** change, not when its code lands.
`Done` with no committed test is not a status, it is a claim — the same rule
the parity table uses, and the reason the CVF reader is listed `Partial` above
despite working.
