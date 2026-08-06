# UI controls

Required by `plan.md` §3 (the `Docs/` manifest) and §17.

This document was written the way [`PERFORMANCE.md`](PERFORMANCE.md) was —
before the thing it describes existed — because a controls reference written
*alongside* the controls tends to describe what was built, while one written
before can still say what was promised and mark honestly what is missing.

**Sections 2 and 4 have since been filled in from shipped code.** The console
commands and the transport bar are operable; the render controls in §1 are
mostly not, and keyboard and mouse (§3) do not exist at all. The rows that still
read *wired* or *frozen* are the ones this document exists for.

§1 was rewritten 2026-08-06 when its guard started passing. Every shader
parameter now has a production writer, and thirteen render controls are still
unreachable from a shipped build — the freeze moved from the shader defaults up
into the view model's, where that guard does not look. A green check bounds the
defect it was written for, not the class.

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

Measured 2026-08-06 by `Tools/check_frozen_params.sh`, which fails when a
declared shader parameter has no production writer outside
`FlowVizRayMarch::FillDefaults`. It now reports **0 frozen of 74 declared** —
every parameter has a production writer, five allowlisted as deliberately
constant.

**That is a weaker statement than it sounds, and the difference is this
document's subject.** The 16 parameters §1.2 used to list as frozen are now
written by `FFlowVizRenderSettingsViewModel::ApplyToRayMarchParameters`, which
the dispatcher calls in production
(`FlowVizVolumeRayMarchDispatcher.cpp:186`). But that writer copies its own
member fields, and **14 of the view model's 17 setters have no production
caller** — no Slate panel, no console command, and session save/reload does not
carry them. So those parameters are no longer welded to `FillDefaults`'
constant; they are welded to the *view model's* default instead. The freeze
moved up one level, to where the checker cannot see it.

The checker is not wrong — "has a writer outside `FillDefaults`" is exactly what
it claims to measure. It is a reminder that a green guard bounds the defect it
was written for, not the class of defect. Re-deriving the historical 16 is in §5.

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

### 1.2 Reachable only from Blueprint — three of the sixteen

These have both a production writer *and* a production caller that can move
them. The caller is `UFlowVizCaptureLibrary`, whose entry points are
`UFUNCTION(BlueprintCallable)`, so they are reachable from Blueprint and from
the capture harness — but from no widget and no console command.

| Control | Parameter | Entry point |
|---|---|---|
| Compositing mode | `CompositeMode` | `SetVolumeCompositeMode` — validated, not cast; an unrecognised mode is refused and the previous one kept |
| Iso value | `IsoValue` | `SetVolumeCompositeMode`, applied whatever the mode, so selecting `IsoSurface` later needs no second call |
| Lighting | `bEnableLighting` | `SetVolumeLightingEnabled` |

All six compositing modes can now be selected, which retires this section's
former headline claim that five of six were unreachable. `FlowViz.Capture.RenderSettings`
asserts the read-modify-write property that makes them independent: changing the
mode must not silently reset lighting, and vice versa.

### 1.3 Frozen one level up — a default no shipped caller can move

The remaining thirteen reach the GPU through the same view model, so
`check_frozen_params.sh` counts them as written. Nothing in production calls
their setters. The value in the table is still the only value a shipped build
renders with; what changed is *which* default supplies it — the view model's
member initialiser rather than `FillDefaults`' literal.

| Control | Parameter | Welded to | Setter with no production caller |
|---|---|---|---|
| Ambient / diffuse | `AmbientStrength`, `DiffuseStrength` | 0.35 / 0.65 | `SetAmbientStrength`, `SetDiffuseStrength` |
| Light direction | `LightDirection` | (-0.5, -0.6, -0.6) normalised | `SetLightDirection` |
| Jitter | `bEnableJitter`, `JitterAmount`, `JitterSeed` | off / 1.0 / 0 | `SetJitterEnabled`, `SetJitterAmount`, `SetJitterSeed` |
| Step size | `StepVoxels`, `ReferenceStepVoxels` | `FlowVizRayMarch` defaults | `SetStepVoxels`, `SetReferenceStepVoxels` |
| Step ceiling | `MaxSteps` | default, clamped | `SetMaxSteps` |
| Early-out | `EarlyTerminationAlpha` | default | `SetEarlyTerminationAlpha` |
| Field filtering | `bFilterField` | 1 | `SetFieldFilteringEnabled` |
| Strict status filter | `bStrictStatusFilter` | 0 | `SetStrictStatusFilter` |
| No-data colour | `NoDataColor` | transparent black | `SetNoDataColor` |

**The consequence worth stating plainly.** Lighting can now be switched on, but
the three parameters that decide what lit output *looks* like cannot be tuned
from a shipped build. Enabling lighting selects one fixed appearance. Jitter,
step size and the early-out are likewise fixed, so the quality/performance
trade-off in [`PERFORMANCE.md`](PERFORMANCE.md) cannot be exercised by a user.

The defaults themselves are deliberate and should survive the fix: jitter is off
because per-ray jitter causes temporal shimmer (ADR 002), and the render is unlit
by default because [`VISUAL_QA.md`](VISUAL_QA.md) rule 1 forbids lighting from
modulating apparent scalar value. The bug is that they are the *only* reachable
values.

Five further parameters are frozen and legitimately so — `CropPad0`, `CropPad1`,
`LightPad0`, `FieldSampler`, `TransferFunctionSampler`. They are cbuffer padding
and static sampler states, exempted by name and reason in
`Plugins/FlowVizRuntime/Source/FlowVizRuntime/frozen_params_allow.txt`.

---

## 2. Console commands

Measured 2026-08-06. `plan.md` §17 requires eight; **all eight are operable.**
Registered from `StartupModule` beside the tab spawner, so the wiring test is
asking about the production startup path rather than about static initialisation
that would have run in a build where `StartupModule` was never called.

| Command | State | Behaviour |
|---|---|---|
| `FlowViz.LoadCase` | operable | Load a case directory. Takes an optional field name |
| `FlowViz.ReloadCase` | operable | Re-read the current case from disk, keeping the bound field |
| `FlowViz.ClearCache` | operable | Drop every cached frame. Budgets and cumulative counters survive |
| `FlowViz.ShowDiagnostics` | operable | Toggle the on-screen overlay, and print its numbers to the log |
| `FlowViz.SetCpuCacheMB` | operable | Resize the CPU cache budget |
| `FlowViz.SetGpuCacheMB` | operable | Resize the GPU cache budget |
| `FlowViz.DumpCase` | operable | Print the parsed manifest and field inventory |
| `FlowViz.Benchmark` | operable | Run the timing protocol in `PERFORMANCE.md` |

Every one resolves its target through `FlowVizWorkspaceRegistry`. A command that
built its own workspace would report numbers about an object the user has never
seen — worse than reporting nothing, because it looks like an answer.

The note this section used to carry for whoever implemented these turned out to
matter, so it is kept as shipped behaviour rather than advice: a command that
silently does nothing when its precondition fails is worse than no command.
`FlowViz.LoadCase` with no field argument refuses rather than picking the
manifest's first field — that field is typically a 3-component vector whose
bytes never reach the scalar texture the ray-marcher samples, so the volume
would draw its hull and nothing else, with no error anywhere.

**Verified by mutation, not only by green tests.** 23 arms across
`FlowVizConsoleCommands.cpp`, its registration in `FlowVizRuntime.cpp`,
`SFlowVizDiagnosticsOverlay.cpp` and `SFlowVizWorkspace.cpp`: 22 killed, 1
survivor found and closed, 0 INVALID, 0 UNSCORED, with an identity control
surviving every run. Mutant lists are committed under `Tools/mutants/` —
`console-commands.txt`, `console-registration.txt`, `diagnostics-overlay*.txt`.

---

## 3. Keyboard and mouse

**Absent**, re-checked 2026-08-06: no `OnKeyDown`, `OnMouseButtonDown`,
`OnMouseMove` or `FUICommandList` in any UI source file. This section is a
placeholder so that the absence is recorded rather than merely unmentioned.

The diagnostics overlay is deliberately `HitTestInvisible` in anticipation of
this — it sits on top of the viewport region, and at Slate's default visibility
it would swallow every click and drag meant for the volume underneath. The
symptom would be "the viewport stopped responding to the mouse", which reads as
a broken viewport rather than as a text panel in front of it.

---

## 4. Transport

Measured 2026-08-06 from the shipped `SFlowVizTransportBar`, by listing every
`ViewModel->` call the widget makes — not from the plan. The difference matters,
and every row above that says *wired* or *frozen* is there because someone wrote
the plan version first somewhere else.

| Control | State | Notes |
|---|---|---|
| Play / pause | operable | One button, `TogglePlayPause`. Disabled when the case reports `CanPlay() == false` rather than failing silently |
| Scrub | operable | Normalised over **physical time**, not frame index — frames need not be evenly spaced |
| Step back / forward | operable | One stored frame, and pauses playback |
| Go to first / last | operable | |
| Frame badge | operable | Names *why* the displayed frame is not the playhead frame, rather than showing a bare number |
| Loop mode | **wired** | `FFlowVizTimelineViewModel::SetLoopMode` reaches `FFlowVizCasePlayer`, and no widget calls it. Same shape as §1.1: correct, tested, and unreachable from the UI |

The enable/disable attributes are capabilities of the *case*, not momentary
availabilities — see the header comment on `CanPause()` for why a per-frame
availability would carry no information beyond `IsPlaying() && CanPlay()`.

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

**Read that exit code directly.** As of 2026-08-06 the checker exits **0** and
reports 74 declared, none frozen. Do not pipe it into `tail` or `grep` and then
read `$?`: that is the pipeline's exit code, not the checker's, and it reports 0
while the scan is failing. Redirect to a file and check the code first.

**A 0 here is a narrow claim — confirm it can still fail.** The checker asks
only whether something outside `FillDefaults` writes each parameter. Delete the
one writer and it must find all sixteen again:

```sh
VM=Plugins/FlowVizRuntime/Source/FlowVizRuntime/Private/UI/FlowVizRenderSettingsViewModel.cpp
cp "$VM" /tmp/vm_keep.cpp
grep -vE '^\s+OutParameters\.[A-Za-z]' "$VM" > /tmp/vm_mut.cpp && cp /tmp/vm_mut.cpp "$VM"
./Tools/check_frozen_params.sh > /tmp/cfp.txt 2>&1; echo "EXIT: $?"   # 1
head -1 /tmp/cfp.txt                       # FROZEN PARAMETER(S) -- 16 of 74
cp /tmp/vm_keep.cpp "$VM"                  # restore, then re-run: EXIT 0
```

**Delete those lines, do not comment them out.** The checker matches source
text, and `// OutParameters.CompositeMode = ...` still contains
`.CompositeMode =`. Commenting the writer out leaves the verdict at 0, which
reads as "the mutation had no effect" when it means the mutation never landed.

That run is what re-derives §1.2/§1.3's sixteen. It does *not* re-derive the
count of setters with no production caller, which is the number §1 now turns on;
for that, see the loop over `Set*` declarations in
`FlowVizRenderSettingsViewModel.h` — 14 of 17 as of 2026-08-06.

For the console commands, search for the literal name **and** for
`FAutoConsoleCommand` as a control. A search that returns zero for both tells you
the search works and the commands are missing; a search that returns zero for the
first alone tells you nothing at all. Both now return hits, so the useful check
has moved on: the commands are *registered* — the question is whether each one
still does what its row claims. Run the suites, which assert on behaviour rather
than on presence:

```sh
Tools/build_lock.sh Tools/run_tests.sh FlowViz.UI.Console
Tools/build_lock.sh Tools/run_tests.sh FlowViz.UI.DiagnosticsOverlay
```

For the transport bar, re-derive the table rather than trusting it — list every
call the widget actually makes, and compare against the view model's API. A
control the view model offers and the widget never calls is a *wired* row:

```sh
grep -o 'ViewModel->[A-Za-z]*' \
  Plugins/FlowVizRuntime/Source/FlowVizRuntime/Private/UI/SFlowVizTransportBar.cpp \
  | sort -u
```
