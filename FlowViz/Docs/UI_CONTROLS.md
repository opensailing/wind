# UI controls

Required by `plan.md` §3 (the `Docs/` manifest) and §17.

**Almost nothing in this document is operable yet.** It is written the way
[`PERFORMANCE.md`](PERFORMANCE.md) was — before the thing it describes exists —
because a controls reference written *alongside* the controls tends to describe
what was built, while one written before can still say what was promised and
mark honestly what is missing.

Every row below carries a state, and the states are measured rather than
recalled:

| State | Meaning |
|---|---|
| **operable** | A user can change it and the render changes |
| **wired** | Reaches the GPU, but nothing a user can press drives it |
| **frozen** | The shader implements it; no production code can set it |
| **absent** | Does not exist in any form |

The distinction between *wired* and *frozen* is the one this document exists to
keep visible. A parameter the shader handles correctly, that is covered by
passing tests, and that no caller can ever change, reads as finished from every
angle except the one that matters. See [`BACKLOG.md`](BACKLOG.md) item 2e.

---

## 1. Render controls

Measured 2026-08-05 by `Tools/check_frozen_params.sh`, which fails when a
declared shader parameter has no production writer outside
`FlowVizRayMarch::FillDefaults`. It reports **16 frozen of 74 declared**.

### 1.1 Wired — reach the GPU, no widget yet

These have a real production writer: the two view models'
`ApplyToRayMarchParameters` — nine from the transfer-function view model, three
from the clip view model. What they lack is a Slate control on the other end.

| Control | Parameter | Notes |
|---|---|---|
| Colour domain | `ValueRangeMin` / `ValueRangeMax` | Taken from the manifest's declared magnitude range. A zero-width domain is the invisible failure — every voxel reads LUT entry 0 and no reason bit fires — so the range is validated on the way in |
| Clamp to range | `bClampToRange` | Off: out-of-range samples take the under/over colours. On: they clamp into the ramp |
| Opacity | `OpacityMultiplier` | |
| Component | `ComponentMode` | X / Y / Z / W / Magnitude. **Not** the compositing mode — see 1.2 |
| Crop box | `CropBoxMin` / `CropBoxMax` | |
| Clip planes | `ClipPlanes` / `NumClipPlanes` | |
| Reason colours | `MaskedColor`, `NaNColor`, `UnderRangeColor`, `OverRangeColor` | The disclosure palette: each names *why* a voxel is not showing data |

### 1.2 Frozen — implemented in the shader, unreachable from production

`FillDefaults` is the only non-test writer of each, and it writes a literal
constant. The value in the table is the only value a shipped build can render
with.

| Control | Parameter | Welded to |
|---|---|---|
| Compositing mode | `CompositeMode` | `Alpha` (0) |
| Iso value | `IsoValue` | 0 |
| Lighting | `bEnableLighting` | 0 (unlit) |
| Ambient / diffuse | `AmbientStrength`, `DiffuseStrength` | 0.35 / 0.65 |
| Light direction | `LightDirection` | (-0.5, -0.6, -0.6) normalised |
| Jitter | `bEnableJitter`, `JitterAmount`, `JitterSeed` | off / 1.0 / 0 |
| Step size | `StepVoxels`, `ReferenceStepVoxels` | defaults |
| Step ceiling | `MaxSteps` | default, clamped |
| Early-out | `EarlyTerminationAlpha` | default |
| Field filtering | `bFilterField` | 1 |
| Strict status filter | `bStrictStatusFilter` | 0 |
| No-data colour | `NoDataColor` | default |

**The consequence worth stating plainly.** Five of the six compositing modes
cannot be selected: `Maximum`, `Minimum`, `Average`, `IsoSurface` and
`Diagnostic`. The volume row of
[`OPENFOAM_PARAVIEW_PARITY.md`](OPENFOAM_PARAVIEW_PARITY.md) §2 names four of
them — MIP, MinIP, average, iso-surface — as required by `plan.md` §9. The
gradient-lighting path is likewise dead behind a constant 0, and `Diagnostic` is
the cbuffer round-trip drift detector, so the one mode that could prove the
shader reads the bytes the CPU wrote is unreachable outside the tests.

The defaults themselves are deliberate and should survive the fix: jitter is off
because per-ray jitter causes temporal shimmer (ADR 002), and the render is unlit
because [`VISUAL_QA.md`](VISUAL_QA.md) rule 1 forbids lighting from modulating
apparent scalar value. The bug is that they are the *only* reachable values.

Five further parameters are frozen and legitimately so — `CropPad0`, `CropPad1`,
`LightPad0`, `FieldSampler`, `TransferFunctionSampler`. They are cbuffer padding
and static sampler states, exempted by name and reason in
`Plugins/FlowVizRuntime/Source/FlowVizRuntime/frozen_params_allow.txt`.

---

## 2. Console commands

`plan.md` §17 requires eight. **All eight are absent** — verified by searching
for each name and, as a control, for `FAutoConsoleCommand` and `IConsoleManager`
anywhere in `Plugins/` or `Source/`: zero files. No console-command
infrastructure exists yet, so this is "not started", not "started and broken".

| Command | State | Required behaviour |
|---|---|---|
| `FlowViz.LoadCase` | absent | Load a case directory |
| `FlowViz.ReloadCase` | absent | Re-read the current case from disk |
| `FlowViz.ClearCache` | absent | Drop CPU and GPU caches |
| `FlowViz.ShowDiagnostics` | absent | Toggle the diagnostics overlay |
| `FlowViz.SetCpuCacheMB` | absent | Resize the CPU cache budget |
| `FlowViz.SetGpuCacheMB` | absent | Resize the GPU cache budget |
| `FlowViz.DumpCase` | absent | Print the parsed manifest and field inventory |
| `FlowViz.Benchmark` | absent | Run the timing protocol in `PERFORMANCE.md` |

A note for whoever implements these: a command that silently does nothing when
the precondition fails is worse than no command. `FlowViz.LoadCase` with no
argument must refuse rather than pick the manifest's first field — that field is
typically a 3-component vector whose bytes never reach the scalar texture the
ray-marcher samples, so the volume draws its hull and nothing else, with no error
anywhere.

---

## 3. Keyboard and mouse

**Absent.** No input bindings exist. This section is a placeholder so that the
absence is recorded rather than merely unmentioned.

---

## 4. Transport

Playback controls (play/pause, scrub, frame step, loop) are in flight and not yet
committed. This section will be filled in from the shipped widget rather than
from its plan — the difference matters, and every row above that says *wired* or
*frozen* is there because someone wrote the plan version first somewhere else.

---

## 5. How to re-check this document

Do not trust the tables; re-derive them.

```sh
# Which render parameters no production caller can set.
# Exit 1 lists them by name; exit 2 means the scan could not
# establish an answer and must not be read as a pass.
./Tools/check_frozen_params.sh

# Its own known-answer tests, including the cases that would be
# green if the checker were broken in the obvious ways.
./Tools/tests/test_check_frozen_params.sh
```

For the console commands, search for the literal name **and** for
`FAutoConsoleCommand` as a control. A search that returns zero for both tells you
the search works and the commands are missing; a search that returns zero for the
first alone tells you nothing at all.
