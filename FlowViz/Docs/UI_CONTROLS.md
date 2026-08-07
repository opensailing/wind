# UI controls

Required by `plan.md` §3 (the `Docs/` manifest) and §17.

This document was written the way [`PERFORMANCE.md`](PERFORMANCE.md) was —
before the thing it describes existed — because a controls reference written
*alongside* the controls tends to describe what was built, while one written
before can still say what was promised and mark honestly what is missing.

**Sections 2 and 4 have since been filled in from shipped code.** The console
commands, the transport bar and (as of #74) the render controls are operable;
keyboard and mouse (§3) do not exist at all. The rows that still read *wired*
or *frozen* are the ones this document exists for.

§1 was rewritten twice on 2026-08-06. First when its guard started passing —
every shader parameter had gained a production writer while thirteen render
controls stayed unreachable, because the freeze had moved from the shader
defaults up into the view model's, where that guard does not look. Then again
when #74 closed that gap: `SFlowVizRenderSettingsPanel` now drives all
seventeen setters, and a second checker (`check_uncalled_setters.sh`) watches
the level the first one cannot. A green check bounds the defect it was written
for, not the class — which is why there are now two of them.

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

Measured 2026-08-06, twice, by two checkers that ask successive questions:

- `Tools/check_frozen_params.sh` — does anything outside
  `FlowVizRayMarch::FillDefaults` write each declared shader parameter?
  **0 frozen of 74 declared** (5 allowlisted as deliberately constant).
- `Tools/check_uncalled_setters.sh` — does each view model setter have a
  production caller? **0 unallowlisted of 58 across 6 view models** (14
  allowlisted as open gaps, tracked as #75 — none of them render settings).

The second checker exists because the first went green while 13 render controls
were still welded: the parameters had gained a writer
(`FFlowVizRenderSettingsViewModel::ApplyToRayMarchParameters`, called by the
dispatcher at `FlowVizVolumeRayMarchDispatcher.cpp:186`), but 14 of that view
model's 17 setters had no production caller, so the freeze had moved one level
up — from `FillDefaults`' literals to the view model's member initialisers,
past the edge of what the first checker measures. A green guard bounds the
defect it encodes, not the class. Re-deriving both numbers is in §5.

### 1.1 Operable — the "Render" section of the workspace side panel

`SFlowVizRenderSettingsPanel`, added with #74, drives every previously-welded
render setting. The workspace subscribes to its `OnRenderSettingsChanged` and
pushes over `FFlowVizWorkspaceModel::PushRenderSettingsToVolume` — the same
event-not-call channel as the clip and colour panels, so the panel itself never
mentions a volume. `FlowViz.UI.RenderSettingsPanel.*` (3 tests) asserts the
controls drive the view model, refused and no-op edits do not announce, and an
unbound panel is disabled and null-safe; `FlowViz.UI.Workspace.RenderSettings*`
(2 tests) asserts an edit in the panel reaches the bound volume's render-thread
payload and that binding a volume pushes settings chosen before it existed.

| Control | Parameter | Notes |
|---|---|---|
| Compositing mode | `CompositeMode` | All six modes, one button each. Also `BlueprintCallable` via `UFlowVizCaptureLibrary::SetVolumeCompositeMode` |
| Iso value | `IsoValue` | Editable in Iso-surface mode only — no other mode reads it |
| Lighting | `bEnableLighting` | Toggle; also `SetVolumeLightingEnabled` from Blueprint. Off by default per `VISUAL_QA.md` rule 1 |
| Ambient / diffuse | `AmbientStrength`, `DiffuseStrength` | Live only while lit; clamped to [0, 1] |
| Light direction | `LightDirection` | Per-axis entry; normalised on the way in, an all-zero direction refused |
| Jitter | `bEnableJitter`, `JitterAmount`, `JitterSeed` | Toggle plus terms; off by default per ADR 002 |
| Step size | `StepVoxels`, `ReferenceStepVoxels` | Zero refused — it is a stall and a divisor respectively |
| Step ceiling | `MaxSteps` | Clamped to the shader's limit |
| Early-out | `EarlyTerminationAlpha` | 1.0 never triggers, which is reference quality |
| Field filtering | `bFilterField` | Interpolated / nearest toggle |
| Strict status filter | `bStrictStatusFilter` | Reject filtered samples touching invalid voxels |
| No-data colour | `NoDataColor` | Fixed swatch palette, not a picker — rule 4 requires the disclosure colours stay distinguishable |

### 1.2 Wired — reach the GPU, no widget yet

These have a production writer and, mostly, a widget — the exceptions are what
keeps this section alive. Nine parameters flow from the transfer-function view
model and three from the clip view model.

| Control | Parameter | Notes |
|---|---|---|
| Colour domain | `ValueRangeMin` / `ValueRangeMax` | Operable — the range boxes in "Color & Opacity" (Manual mode) |
| Clamp to range | `bClampToRange` | **No widget toggles the model** (`SetClampToRange` has no production caller — #75) |
| Opacity | `OpacityMultiplier` | Operable — the opacity slider |
| Component | `ComponentMode` | X / Y / Z / W / Magnitude. **Not** the compositing mode. Derived from the view model's component choice; no widget selects it (#75) |
| Crop box | `CropBoxMin` / `CropBoxMax` | Operable — the crop controls in "Clipping" |
| Clip planes | `ClipPlanes` / `NumClipPlanes` | Operable — preset planes can be added, removed, inverted; in-place editing has no control (`SetPlane`, #75) |
| Reason colours | `MaskedColor`, `NaNColor`, `UnderRangeColor`, `OverRangeColor` | The disclosure palette: each names *why* a voxel is not showing data. Shipped defaults only — not editable anywhere (#75) |

The render defaults survived the fix on purpose: jitter is off because per-ray
jitter causes temporal shimmer (ADR 002), and the render is unlit by default
because [`VISUAL_QA.md`](VISUAL_QA.md) rule 1 forbids lighting from modulating
apparent scalar value. What changed is that they are now defaults rather than
the only reachable values.

Five parameters are frozen and legitimately so — `CropPad0`, `CropPad1`,
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

That run is what re-derives the historical sixteen. The setter-level question —
the one §1 now turns on — has its own checker, wired into the harness sweep
beside this one:

```sh
# Which view model setters no production code calls. Exit 1 names
# them; exit 2 is UNSCORED, same convention as above. The allowlist
# (uncalled_setters_allow.txt) holds the open gaps tracked as #75,
# and the checker refuses a stale entry, so a gap that closes cannot
# leave its exemption behind.
./Tools/check_uncalled_setters.sh

./Tools/tests/test_check_uncalled_setters.sh
```

Its differential is the same shape: delete `SFlowVizRenderSettingsPanel`'s
handler bodies (or its `.OnRenderSettingsChanged` subscription in
`SFlowVizWorkspace.cpp` — that arm is what `FlowViz.UI.Workspace.
RenderSettingsBinding` kills, verified 2026-08-06) and the workspace edits a
model the renderer never sees again.

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
