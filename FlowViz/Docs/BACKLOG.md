# Backlog

Required by `plan.md` §3. Known gaps, deferred work, and things that are worse
than they look. Ordered by what blocks the most downstream work, not by effort.

This file is for things that are **true now**. A backlog that lists only future
features, and never the shortcuts already taken, is a marketing document. Items
that came out of a real investigation cite what was checked.

## Release-blocking

### 1. ~~No case data exists anywhere in the repository~~ — closed 2026-08-04

`python -m cfdviz generate-mock` is implemented, its 591-line suite passes
31/31, and `Samples/MockCylinderWake.cfdviz` is committed (`208b426`):
56×28×6 cells, 20 frames, 160 field files, 320 bricks, 3.4 MiB. `cfdviz
validate` reports OK and `known-values --check` reports all 81 samples
matching.

This was the item that blocked the most — every renderer milestone needs a
case to display, the packaged application must ship a low-resolution sample
per `plan.md` §3, and the blind visual review cannot produce a candidate frame
without one.

**What that does and does not establish.** The checks above are Python reading
what Python wrote, which cannot detect a shared misunderstanding of the spec.
The cross-language claim rests on `CFDVizKnownValuesTest.cpp` — Unreal decoding
the committed case and comparing against the values Python recorded. See item 3
below for its current state.

### 2. Milestone C is unstarted

Partially started as of 2026-08-05. The path is decided in
[ADR 002](ADR/002-runtime-volume-rendering.md).

**Done.** The GPU volume *data* path: `Private/Render/FlowVizVolumeTexture.cpp`
takes decoded CVF bytes to a sampleable 3D texture — format choice, strides,
brick placement, 3→4 widening with a quiet-NaN pad, the fail-closed status
volume, the cell/point half-voxel transform, and the `static_assert`-pinned
cbuffer block. Covered by `FlowVizVolumeTextureTest` (pure, `-nullrhi`) and
`FlowVizVolumeDeviceTest` (RHI, skips with a logged reason when there is no
device rather than passing). 17/17 mutants killed.

**Also done, as of 2026-08-05.** The scene layer (`d4cddbe`): volume
component, case actor, scene proxy, bounds, box hull, and the winding
correction for the negative-determinant solver transform. The transfer
function (`8dd1b29`): colour and opacity LUT, domain mapping, GPU resource.
The ray-march shader (`8a178c4`): the repo's first `.usf`, a compute shader
that writes a quantitative `OutValue` UAV — field value, alpha, invalid-reason
bits, step count — alongside the image, so the pass can be checked on numbers
rather than on whether a picture looks plausible. 47/47 tests pass, all three
GPU device tests execute for real against an Apple M4 Metal device under
`RHI=1` (verified independently: no skip block, zero shader compile errors).

**What the device test does and does not establish.** It proves the pass
dispatches, the shader compiled on Metal for both permutations, and the GPU
reads back the cbuffer bytes the CPU wrote. It does **not** prove the marching
loop produces correct imagery. Alpha/max/min/average compositing, iso-surface,
gradients, lighting, crop box, clip planes and jitter are all implemented and
compile, and none has been exercised against a populated volume. See item 2a.

**Not done, and this is still the gap that matters.** Nothing has been drawn.
Every piece above is verified against its own contract — cbuffer layout,
texture strides, index winding, LUT interpolation — and *none* of that is
evidence about pixels. The component's scene proxy currently draws only a
wireframe bounding box: with no dispatcher registered, the volume renders
nothing at all, and the proxy deliberately exposes `WasRayMarchDispatched()`
so "nothing was registered" stays distinguishable from "marched and drew
nothing". Those are the same black screen with unrelated causes.

So: **nothing has ever been rendered, and no visual review has ever run
against a real frame.** That is the release bar this project is measured
against, and every claim about how it looks remains unmade rather than merely
unproven. A green test suite is not a rendered image, and this file should
keep saying so until an image exists.

### 2a. The marching loop is exercised, including iso-surface, gradients and clipping

**Closed** by `3ad8e54` and its follow-up (`FlowViz.Render.VolumeMarch`).
Recorded here in full because the *shape* of the evidence is what matters, not
the fact that a test passes.

**What now runs over real data.** A 16×8×4 fixture whose field is its own
voxel index, `f(i,j,k) = i + 16j + 128k`, so a sampled value *decodes* to the
voxel it came from — which makes "sampled a voxel" and "sampled the RIGHT
voxel" separable assertions. Verified against closed forms, not pictures:

- the anisotropic step rule, as a number: 33 steps, where the max-spacing bug
  gives 9
- Maximum, Minimum and Average, each distinct, with Average proven to be the
  mean rather than the sum or an endpoint
- the crop box, in domain fractions: cropping to the far half of X moves the
  minimum by exactly 8 voxels
- the status gate, in both its roles — masked voxels excluded from the value,
  *and* reported in the reason bits so the cause is still nameable
- VISUAL_QA rule 1: `OutValue.x` bit-identical with lighting on and off, with
  a control asserting the *colour* changed, so the invariance cannot hold
  because lighting never ran

**Iso-surface, gradients and clipping now run too**, added in the follow-up:

- **Iso-surface extraction**, at a value deliberately ending in `.5` so no
  sample can take it. A renderer that snapped to the nearest lattice value
  would report an integer and is caught; the reported value can only have come
  from the linear-crossing branch. A second config sets the iso above the whole
  field and must find nothing, so "found a surface" is separable from "found
  the right surface".
- **Gradient estimation, and therefore lighting *direction*.** Because the
  field is linear, `∇f` is the constant `(8, 32, 512)` in local units
  *everywhere* — so the expected normal does not depend on where the ray hit,
  and the assertion does not first have to prove the hit position. Three
  renders differing ONLY in `LightDirection` recover the normal's components,
  and their ratios are the gradient's ratios: exactly `1 : 4 : 64`.
  Normalisation cancels, so this tests direction and is indifferent to
  magnitude. A swapped axis moves a measured ratio by at least 4×.
- **Clip planes**, as a cross-space equivalence. The plane `(1,0,0,-1)` and the
  crop box at `0.5` are the same geometric cut expressed in two different
  parameter spaces, so they must agree exactly — a stronger statement than
  either alone, because a plane evaluated in UVW or world units still produces
  a plausible clipped image.

Each of those was verified by differential (`Tools/tests/march_differential.sh`,
now **14/14**) rather than by having passed once. The identity-control arm — a
no-op edit inside the marching loop that must not change the image — is the one
that makes the rest mean anything. It killed on the first campaign, which
condemned every verdict in it: the harness was scoring "the filter matched no
tests" as a kill, so nothing had been tested at all.

**A campaign found a vacuous assertion, which is what campaigns are for.** The
arm replacing `TMin = max(TMin, THit)` with `TMin = THit` — a second clip plane
*overwriting* the first instead of intersecting with it — SURVIVED, against an
assertion whose text explicitly claimed to catch that bug. The cause was the
fixture, not the wording: every clip config wrote `TMin` exactly once, with a
value that already exceeded the box-entry `TMin`, so `max()` and plain
assignment agreed and the mutant was an identity map.

The fix is a fixture in which the accumulator is written TWICE with the LOOSER
value SECOND (keep `x >= 1.0`, then `x >= 0.5`). Intersecting keeps the tight
plane, +8 voxels; overwriting takes the looser one simply because it came last,
+4. The two orders differ by construction. The mirror case on the `min()`
accumulator got its own config and its own arm, since an assertion on one
accumulator cannot reach the other. Generally: **`max`/`min` narrowing is
indistinguishable from assignment unless something writes the accumulator a
second time, pointing the wrong way.**

Two of the author's own errors are worth recording, because both produced
confident wrong numbers rather than visible failures:

- A control asserted no recovered normal component was clamped by `saturate()`,
  written as `< 0.99`. It failed on a CORRECT render: the true Z component is
  `0.9979`, because the gradient is 64× steeper in Z. A `saturate()` clamp
  produces *exactly* `1.0`, so `< 1.0` is the test that names the property;
  `0.99` was an invented margin. A control is itself a check and can be wrong
  in the ordinary ways.
- The max-side clip was predicted to cap the maximum 7 above the row constant;
  it measured 8. The boundary voxel is *included*, symmetrically with the crop
  box at `0.5` raising the minimum TO voxel 8. The shader was right and the
  prediction assumed one boundary exclusive and the other inclusive.

A third was a near miss with no red test to catch it: a pre-flight probe run
with `sed` reported that both new clip mutations matched nothing, which would
have licensed dismissing the genuine SURVIVED verdict as a harness artifact.
The harness applies mutations with `perl -0pi`; re-probed with `perl`, both
changed exactly one line. **Probe with the tool that runs it**, and give the
probe a positive control.

Treat "the ray-marcher works" as made for compositing, stepping, cropping,
status, iso-surface, gradients and clipping — and still unmade for anything
about how it *looks*, which no assertion in this file addresses.

## Correctness gaps

### 2b. Nothing wires the ray-marcher to a scene — found 2026-08-05

**A volume placed in a real map renders nothing, and the suite is 49/49 green
while that is true.** The two halves of volume rendering both exist and are both
well covered; no production code joins them.

- `FFlowVizVolumeSceneProxy::GetDynamicMeshElements` guards the dispatch on
  `FlowVizVolumeRayMarch::GetDispatcher()` and silently draws nothing when it
  returns null (`Private/Scene/FlowVizVolumeComponent.cpp:332`).
- `SetDispatcher` had no caller outside `/Tests/` — grep across `Plugins` and
  `Source`, excluding `Intermediate`, returned only its declaration
  (`Public/Scene/FlowVizVolumeComponent.h:486`) and its definition
  (`Private/Scene/FlowVizVolumeComponent.cpp:166`).
- The only `IFlowVizVolumeRayMarchDispatcher` implementation lived in
  `Private/Tests/FlowVizVolumeComponentTest.cpp`.
- `FlowVizRayMarch::AddRayMarchPass`, the real RDG compute dispatch, was called
  only from `FlowVizVolumeRayMarchTest.cpp` and `FlowVizVolumeMarchTest.cpp`.

**Why no existing test could see it, which is the part worth keeping.** Every
test that exercises this path installs its own recording dispatcher first,
because it needs a double to assert against. Installing a dispatcher is
therefore a *precondition* of those tests, which makes every one of them
structurally incapable of noticing that production installs none. This is not an
oversight in any individual test; it is a property of testing a seam through a
mock. Catching it required a test that installs nothing and asks what startup
left behind — `Private/Tests/FlowVizRenderWiringTest.cpp`, `FlowViz.Render.Wiring`.

The hazard was anticipated. The seam comment on `IFlowVizVolumeRayMarchDispatcher`
already said "no marcher wired" and "marcher ran and produced nothing" look
identical on screen and have nothing in common as fixes. What was missing was an
assertion that could tell them apart.

Corroborating evidence: `Saved/Screenshots` holds 90,062 PNGs. 1,086 are the
stock template map from 2026-08-04 (sky, checkered floor, no volume); the
remaining ~89,000 are byte-identical pure-black 640x360 frames sharing one md5.
No frame has ever contained FlowViz geometry. `Tools/capture` itself is *not*
implicated — it is verified working on `L_CapTest` and its `verdict.py`
explicitly rejects a pure-black frame — which is what makes the blanks evidence
for this entry rather than against the harness.

### 2c. Three disclosure flags are produced and never consumed — found 2026-08-05

Same shape as 2b and found the same way: sweeping the source for comments that
name two states a user cannot tell apart, then looking for an assertion on
*each* branch. In all three the rendering half is implemented and tested and the
**telling** half stops at the comment. A test asserting a flag is set proves the
producer and never the pixel, so the coverage looks present exactly at the
defect.

**(i) `bClampToRange` never reaches the shader — CPU and GPU disagree.** The
highest-severity of the three, because a user-facing control silently does
nothing.

- Producer: `Private/Render/FlowVizTransferFunction.cpp:616`. Layout pinned by
  `static_assert` at `Public/Render/FlowVizTransferFunction.h:799`.
- Consumers: **none**. `grep -i ClampToRange Plugins/FlowVizRuntime/Shaders`
  returns 0 hits, and `FFlowVizVolumeRayMarchParameters`
  (`Public/Render/FlowVizVolumeRayMarchShader.h:165-209`) has no such
  `SHADER_PARAMETER`.
- The header states the contract at `:530` — "the shader must not clamp to it
  unless bClampToRange" — and the shader cannot honour it, returning the
  under/over colours unconditionally (`FlowVizVolumeRayMarch.usf:424-425`).
- The CPU path *does* honour it (`FlowVizTransferFunction.cpp:380,383`), so
  **enabling clamping changes the preview and not the render.** The file's own
  comment at `:378` names this as "a quantitative lie that looks like a correct
  render".
- Existing assertion is producer-only: `FlowVizTransferFunctionTest.cpp:1052`
  checks the struct field. Nothing asserts a clamped render differs from an
  unclamped one.
- Aggravating: the whole `FFlowVizTransferFunctionShaderParameters` cbuffer is
  built and offset-asserted but never bound by production.

**(ii) `DecodeReason` has zero call sites in the entire plugin, tests included**
(`Public/Render/FlowVizVolumeRayMarchShader.h:383`). Tests read `OutValue.z`
through a locally re-declared `ReasonBits()` helper with hand-written magic
numbers (`FlowVizVolumeMarchTest.cpp:186`), so **the C++ enum and the shader's
`FLOWVIZ_REASON_*` defines are never checked against each other** — renumbering
either side is silent. Bit coverage: `VALID` 7 assertions, `MASKED` 2, `NONE` 2;
`UNKNOWN`, `NaN`, `INFINITE`, `UNDER_RANGE`, `OVER_RANGE` have **zero**. The
header at `:379` promises "NaN from masked from absent" are separable, and
`NaN`/`Infinite` both map to `NaNColor` (`FlowVizVolumeRayMarch.usf:420-421`),
so `.z` is the only discriminator and it is untested.

**(iii) `WasRayMarchDispatched()` has no caller anywhere**
(`Private/Scene/FlowVizVolumeComponent.cpp:285`). It exists to resolve the exact
hazard quoted in 2b, and it is structurally unreachable — the proxy is a private
class inside a .cpp, so nothing outside that translation unit *can* call it.
**Distinct from 2b:** that gap is `SetDispatcher` having no production caller;
this is the reporting accessor being dead, which stays dead after the wiring
lands unless it is deliberately exposed.

Method note, and the reason this entry exists at all: the first sweep used a
6-term grep and returned 33 sites, which read as exhaustive. The full vocabulary
(adding `silently`, `plausible`, `reads as`, `plays as`, `passes as`,
`masquerad`, `wrong file`, `looks like`) returns **293** — `silently` alone
yields 126. Of those, 14 survived triage as genuine two-state hazards and 9 had
both branches asserted. **The count measured the auditor's vocabulary, not the
code.** Sweep `.usf` as well as C++: two of these three terminate in a shader.

**293 was also a floor.** A third tier was then predicted on the grounds that
both earlier vocabularies describe an *ambiguous* result and neither describes a
*wrong* one. Measured: `stale` 45, `quietly` 15, `no error` 9, `fall(s) back`
13, `previous frame` 6, `unreported` 2 — **~90 further hits, none in either
earlier sweep.** Two people independently widening a pattern still did not reach
the ceiling; each tier feels exhaustive from inside it. Treat any hazard-sweep
count as a lower bound and say so when reporting one.

**A second and worse shape to sweep for: prose claiming a hazard is already
HANDLED.** A comment naming an untested hazard leaves a debt an auditor can
find; a comment saying the hazard is handled *by a named mechanism* turns the
auditor away. 2c(iii) is exactly this — two comments said the black-screen
ambiguity was diagnosable via `WasRayMarchDispatched`, which has no caller and
is unreachable from outside its translation unit. The mechanism existed, which
is what let the claim survive review. **Grep such a mechanism for CALLERS, not
for its definition;** existence is neither reachability nor a consumer. Terms:
`so a test can`, `which makes that checkable`, `says so via`, `is what
guarantees`, `can be distinguished by`.

### 2d. Second-branch audit of the remaining hazards — 2026-08-05

Completes the sweep 2b and 2c came out of, over the sites those entries did not
claim. Format is: hazard, its two branches, status. **Three gaps were closed
with assertions; two new producer-only defects were found and recorded at the
field; the rest came back genuinely covered.** A clean result on a hazard is a
result — the covering test is named below so the next auditor need not
re-derive it.

**Closed by assertion in this pass** (commit `4742456`; each verified
falsifiable against a mutant of the specific guard named, and confirmed passing
on pristine code *first* — red-on-mutant alone is not a kill, it also appears
when the expectation is simply wrong):

| Hazard | Branch 1 | Branch 2 | Was |
|---|---|---|---|
| `CFDVizManifest.h:813` — five lookups return nullptr, "never a default-constructed object" | id matches → right object | id does not match → `nullptr`, not an empty stand-in | `FindGrid`/`FindMesh` had **neither** branch tested — untested public API; `FindDerivedField` had only branch 1 |
| `CFDVizMeshReader.h:413` — `BuildForPatch` fails rather than returning an empty mesh | absent patch fails | mesh carrying **no patchIds at all** fails, with a message naming that cause | two distinct guards, only the first tested |
| `FlowVizVolumeTexture.cpp:1142` — "an invalid layout with bytes attached must not pass as absent" | genuinely absent field is skipped | half-filled pair is **rejected** | only the absent half tested |

The third was traced end-to-end rather than assumed: `Validate` gates
`EnqueueUpload`, and `UploadOnRenderThread`'s per-target loop skips any target
whose `PayloadLayout` is invalid, leaving `bAllOk` true and the slot marked
`bHasContent`. A forgotten `VectorLayout` therefore uploads and displays
scalar-only, as complete, with nothing on that path logging. That guard is the
only thing in the way.

**New gap (i): `bStale` is dropped at the seam.** Recorded at the field in
`Public/Playback/FlowVizCasePlayer.h`. Same shape as `bInterpolationDegraded`
but a different field and struct, and it is the **normal** state during a scrub
rather than a rare one. The flag is correct and well tested *upstream* — both
branches, and a hardcoded-false mutant was killed in `5b90dc0` — but the only
consumer of `GetDisplay()` outside `/Tests/` is `ToVolumeFrameSelection`
(`Private/Playback/FlowVizCaseSeam.cpp`), whose destination
`FFlowVizVolumeFrameSelection` has **no field to receive it**. A held frame from
an earlier time renders under the current playhead's label, and the renderer
cannot tell that from a fresh frame. Every assertion on this flag sits upstream
of the seam that discards it. Do not read that coverage as evidence the
disclosure works.

**New gap (ii): two false all-clears in the transfer function** — the 2c(iii)
shape, found by the sweep 2c recommended. `Public/Render/FlowVizTransferFunction.h`
claimed `GetLut()` "is what the legend and the probe readout sample" and that
`EvaluateColor` gives one definition rather than one per consumer. Neither a
legend nor a probe readout exists anywhere in the plugin, and both functions
have **zero consumers outside `/Tests/`**. Both comments corrected in place
rather than deleted; the false version is the more dangerous artifact and the
correction is the finding.

*Aggravating, and the reason this outranks an ordinary stale comment:* the same
claim is load-bearing at the null-RHI early-out in
`Private/Render/FlowVizTransferFunction.cpp:875`, where "the CPU-side LUT is
still correct and is what the legend and the probe readout sample" is the stated
reason **not to log a missing LUT texture**. A false all-clear is buying silence
on a real GPU failure path. Fixing that call site belongs to the
transfer-function owner; `Private/Render` was not this auditor's file.

**Audited and genuinely covered** — named so nobody re-audits them:

- Dense-volume prefill, "a partially filled buffer would be indistinguishable
  from data" — `FlowViz.CFDViz.VolumeReader.Background` and `.Sparse`. Covered
  twice with **non-zero backgrounds** ((1,2,3) and NaN), which is what stops a
  zero-filled buffer from passing, plus a `BackgroundCount` anti-vacuity guard.
- Orphaned scene-capture component, "captures a black frame and reports no
  error" — `FlowViz.Capture.SpawnSceneCapture` asserts world membership,
  `IsRegistered()` and `bCaptureEveryFrame == false`, not merely non-null.
- Degenerate/non-finite extent vs failed load — `FlowViz.Scene.VolumeComponent`,
  both the zero-area and the non-finite axes.
- Unpinned frames tearing under scrub — `FlowViz.Scene.VolumeComponent` pins
  **both** A and B while blending.
- `bHasStatusTexture` fail-closed, "the branch most likely to be written
  backwards, because backwards renders a complete, plausible image" —
  `FlowViz.Render.RayMarchShader` asserts both directions, and the shader
  honours it at `FlowVizVolumeRayMarch.usf:249,292`.
- cbuffer drift, "a wrong volume that renders plausibly" — pinned three ways:
  `static_assert`, a CPU offset table, and a live GPU round-trip through
  `FLOWVIZ_MODE_DIAGNOSTIC` (`FlowViz.Render.RayMarchShaderDevice`).
- Shader-compiles vs shader-never-wired — `FlowViz.Render.RayMarchShaderDevice`
  asserts both permutations are found in the global shader map before it asserts
  any pixels.
- Double-vs-float alpha narrowing at the seam — `FlowViz.Playback.Seam`, with a
  fixture chosen so the narrowing is actually reachable. The model case for what
  a paid second branch looks like.
- Stalled loader vs stopped playhead — closed by a peer in `5b90dc0` while this
  audit was running.

**Scope, honestly.** 293 hits triaged under the wide vocabulary, 14 surviving as
genuine two-state hazards, 9 of those audited in depth; then a further ~90 under
the "confidently wrong result" vocabulary (`stale`, `quietly`, `falls back`,
`previous frame`, `unreported`), 7 survivors; and 57 under the
false-reassurance vocabulary, 8 of which named a concrete mechanism and so got a
consumer grep. The `.usf` files were swept directly — 5 hits, all rationale
prose. **The shader honours every flag it is handed; every gap found is upstream
of it, in flags the shader is never given.** Not verified: anything needing a
rendered-output comparison for the disclosure flags, since no consumer exists to
render one. Per 2c's own method note, treat these counts as a lower bound.

### 3. The CVF volume reader has no committed test

Every other reader has one. This is the reader that decodes the volume every
later milestone displays, so an undetected misparse here surfaces as a wrong
picture rather than an error. Tracked in
[`ARCHITECTURE.md`](ARCHITECTURE.md) and
[`OPENFOAM_PARAVIEW_PARITY.md`](OPENFOAM_PARAVIEW_PARITY.md) as the reason the
CVF row is `Partial` rather than `Done`.

### 4. ~~The byte cursor has no direct test~~ — closed 2026-08-04

`Private/Tests/CFDVizByteCursorTest.cpp` now covers it directly: `CanRead`
against `INT64_MAX`, `INT64_MIN`, `2^62` and a count chosen to wrap to exactly
zero (the only inputs on which the overflow-safe form and the naive
`Offset + Count <= Size` disagree); `Seek` rejecting negative, past-the-end,
`INT64_MIN` and `INT64_MAX` while leaving the cursor untouched; `ReadIsAllZero`
separating "the read happened" from "the bytes were zero", with the non-zero
byte placed both first and last; and NaN payloads compared as *bit patterns*,
since a quieted signalling NaN still satisfies `IsNaN` and `NaN != NaN` makes a
value comparison vacuous.

Kept here rather than deleted because the entry explains why rule 12 was
`Partial` in `ARCHITECTURE.md` for as long as it was.

Measured 2026-08-05 by a 28-mutant campaign over `CFDVizByteCursor.h`:
**killed 25, SURVIVED 2, INVALID 0, UNSCORED 1** as first run. After the two
follow-ups below — the sign-bit fixture fixed, the UNSCORED mutant re-scored
— the standing position is **killed 26, SURVIVED 1, UNSCORED 0** of 28.

That remaining survivor is the equivalent one, and it will stay a survivor
permanently: no input distinguishes it from the real code, so the honest
ceiling for this file is 26/28 and not 28/28. An entry claiming 100% here
would mean a test had been written asserting behaviour the code does not
have. Each non-kill is accounted for rather than tolerated:

- `ReadDouble: sign bit cleared` — a real gap, now closed. Every double
  fixture was `0x7FF0000123456789`, whose sign bit is already clear, so the
  mask was the identity map on the test data. Fixed by a negative-zero double
  case and confirmed by a two-arm differential (control SURVIVED, fixed
  killed).
- `CanRead: offset one past the end accepted` — **equivalent**, not uncovered.
  At `Offset == Size + 1` the relaxed clause is unreachable because
  `Count <= Size - Offset` becomes `Count <= -1` and `Count >= 0` already
  rejects. Verified exhaustively over every small case and the int64 extremes:
  zero disagreements, while the same search finds 45 for the neighbouring
  off-by-one mutant. No test can kill it and none should be written.
- `ReadIsAllZero: loop one short` — was **UNSCORED**; **re-run 2026-08-05 and
  killed.** UNSCORED was never a statement about coverage, only that the run
  produced no result, and re-running it is what turned "unknown" into a fact.
  The kill comes from the `LastCursor` case, whose comment already claimed
  that job: "the last byte is the one a loop with an off-by-one never
  inspects."

  Re-run with all four `ReadIsAllZero` mutants rather than the one, so the
  three already known killed acted as controls: **killed 4, SURVIVED 0,
  INVALID 0, UNSCORED 0.** Had the fourth come back UNSCORED again while the
  other three scored, the fault would have been in that mutant; had all four
  gone UNSCORED, in the harness. Re-running the single mutant alone could not
  have distinguished those. The original UNSCORED was therefore a
  scoring-run failure, not a gap in the suite — no test was needed and none
  was written.

### 4a. ~~The CVA element index was unobservable~~ — closed 2026-08-05

Two mutants of `FCFDVizArrayReader::GetElementIndex` — entity/component
transposed, and the entity upper bound dropped — survived the bridge campaign
at `e3da2bd`. Neither was a missing assertion. Every `.cva` the generator wrote
had `componentCount == 1`, and on one component

    Entity * ComponentCount + Component  ==  Component * ValueCount + Entity

so the transposition *is* the identity map. No assertion over that data could
have killed it. `5cab6f6` changed the data — a three-component wall-shear
array — and added no C++ at all.

Confirmed by differential, not by argument. Same mutant, same filter, same
assertions, same harness; only the sample data differs between the arms:

| | `e3da2bd` (1 component) | `5cab6f6` (3 components) |
| --- | --- | --- |
| transposition | SURVIVED | **killed** |
| dropped upper bound | SURVIVED | SURVIVED under `KnownValues` |

`git diff` between the two arms touches only `Samples/` and the Python
generator — `CFDVizArrayReader.h` and the tests are byte-identical — which is
what makes the flip attributable to the data.

The dropped upper bound is **killed** by `FlowViz.CFDViz.ArrayReader`
(`killed 2  SURVIVED 0`). It survives the bridge filter because the bridge
samples only in-range entities and so cannot observe a past-the-end read. That
is a correct division of labour between the two suites, not a gap. The
question `5cab6f6` left open is now answered.

### 5. Nothing wires the length unit into the coordinate adapter

`FCFDVizUnits::TryGetLengthInMeters` and `MakeSolverToUnrealTransform` both
exist and are correct; nothing calls the first to parameterise the second. Every
current caller takes the `MetersToUnrealCentimeters` default and so silently
assumes the case is in metres — right for the mock case, wrong by 1000× for the
first millimetre case loaded. Recorded as the open question in
[ADR 004](ADR/004-coordinate-systems.md); the default parameter should
eventually be removed so the assumption cannot be made by omission.

### 6. `coordinates.sourceToCanonical` is parsed and never applied

The manifest field is read, transposed into Unreal's convention and stored. No
code consumes it. That is *correct* under
[ADR 004](ADR/004-coordinate-systems.md) §6 — the transform describes work done
upstream before the case was written, and applying it again would transform
canonical data twice — but the field's presence invites exactly that mistake.
Listed here so the next person to find it reads the ADR before "fixing" it.

## Process and tooling

### 7. Only the Presentation half of the blind review is still blocked

Narrowed 2026-08-05. This entry used to say a clean checkout could not run a
blind review at all. That is now true of only one of the two profiles.

**Scientific: unblocked.** `Tools/capture/make_reference_figures.py` generates
the corpus on demand from the committed `MockCylinderWake.cfdviz` sample —
three 1920×1080 figures (velocity magnitude, signed vorticity on a
zero-centred diverging map, vorticity-magnitude MIP). The PNGs are *not*
committed; `.gitignore:31` catches them and the generator is what ships, so a
clean checkout runs one command instead of needing files it cannot obtain.
Colormaps come from `cfdviz.colormaps.build_lut` rather than matplotlib, and
a test asserts matplotlib's own viridis would *fail* — the two differ by up to
103/255 in a channel, which is exactly the cross-implementation drift the
two-language design exists to catch.

Verified independently rather than taken on report: 88/88 capture tests pass,
the figures regenerate with the stated ranges, and a metadata scan written
without using the project's own `blind.audit()` confirms no `tEXt`/`iTXt`/
`tIME` chunk survives to leak which image is which. The vorticity figure was
looked at: six alternating cores with five sign changes, blue above the
centreline and red below — a Kármán street, not noise.

The checks were mutation-tested and three initially SURVIVED, including a NaN
painted as the colormap minimum: `exact_color_count` had been scanning the
whole PNG, and the legend's own invalid swatch cleared the threshold by
itself. Now counted only inside the data axes, with a second assertion that
the swatch is still present, so the test distinguishes the two regions. Final
18/18.

**Presentation: still blocked, and correctly so.** Those references are
third-party film and AAA frames. They are not committed, cannot be, and no
generator can substitute for them. `review.iterate()` still refuses to grade
against an empty corpus rather than reporting an unopposed pass. See
`Tools/capture/references/README.md`.

**What this does not establish.** No FlowViz render has been compared against
any of it. The figures are verified as good *references*; the first actual
blind run has not happened, because nothing has been rendered yet (item 2).

### 8. `Build.sh` exits 0 when the build fails

Verified 2026-08-04: a run printing `Result: Failed (ConflictingInstance)`
still returned exit code 0. Any gate keyed on `$?` — a CI step, an `&&` chain,
a mutation harness deciding whether a mutant was killed — passes
unconditionally. Everything in this repo must grep the printed
`Result: Succeeded` text instead. Documented in [`BUILD.md`](BUILD.md).

A mutation run scored on the exit code reports every mutant killed while
proving nothing, which is the failure this project treats as worse than having
no test.

**`Result: Succeeded` is not sufficient either.** Found 2026-08-05: a
full-module build returned exit 0, printed `Result: Succeeded`, and had zero
`error:` lines — while the log read `Target is up to date` and
`Using Unreal Build Accelerator local executor to run 0 action(s)`. It
compiled nothing, because a concurrent agent's build had finished five seconds
earlier. If the point of a build was to check that new sources compile, also
confirm the action count is non-zero. A no-op build answers a question you did
not ask.

### 8a. The GPU test arm was not part of routine verification

Found 2026-08-05, and it had been hiding a real bug.

`run_tests.sh` defaults to `-nullrhi`. The three `*Device` tests skip
themselves when there is no GPU — honestly, each logging what went unverified
and how to re-run it — but the engine records a self-skipped test as
`Result={Success}`, so they landed in the pass count. The suite reported
`47/47 passed` while three of those 47 had checked nothing about the GPU, and
the reasons sat 2000 lines deep in a log nobody reads when the last line is
green.

Closed by `60b446d`: the summary now names any test that passed without
verifying anything, so a skipped device test can no longer hide inside a green
total.

One caution for whoever reads a device-test failure in a log. Running the
`RHI=1` arm produced this:

```
FlowViz.Render.RayMarchShaderDevice  FAILED
Expected 'cbuffer row 2 (VoxelSpacing/MinVoxelSpacing): .w' to be 0.041667,
but it was 0.093750
```

That looks exactly like a constant-buffer member drifted one row — 0.09375 is
`MaxVoxelSpacing`, 0.0416667 is `MinVoxelSpacing`. **It was not a defect.** An
agent was deliberately breaking the shader to prove the assertion could fail,
and the build observed the broken intermediate state. The cbuffer is correct:
the C++ struct, the `static_assert`s and the HLSL agree, and the same test
passes against the unmodified shader.

Recorded because the confusion is structural, not a one-off. This project
verifies its assertions by breaking the code under them, and while a
break-verify-restore cycle is in flight the tree is genuinely red for reasons
that are indistinguishable, from the outside, from a real bug. Before filing a
failure seen during a shared-tree run, check whether the source is currently
mutated — `git diff` and a grep for `BROKEN` markers answer it in seconds.

The general lesson is the one this file keeps relearning: **a green total that
includes tests which verified nothing is a vacuous pass.** Counting is not the
same as checking.

### 9. Windows is a declared target and has never been built

`FlowViz.uproject` lists it. Nothing has been compiled or run there. Do not
assume it works.

## Deferred by decision

These are not oversights. Each was considered and deliberately not done.

| Item | Why deferred | Reference |
| --- | --- | --- |
| Sparse Volume Texture path | Imported-asset pipeline, engine-owned lossy sampling; kept as an optional Presentation-only flag, never the runtime path | [ADR 002](ADR/002-runtime-volume-rendering.md) |
| Direct OpenFOAM parser | Core visualizer must not depend on OpenFOAM libraries; the seam is the data-source interface | `plan.md` §5A |
| `FCFDVizLiveDataSource`, VTK adapter | Interfaces reserved, not implemented in the initial release | `plan.md` §5A |
| Multiple scattering in the Presentation profile | Undesigned. Single-scattering smoke reads flat, which is a hard failure against the stated visual bar — so this is deferred *and* known to be necessary | [ADR 002](ADR/002-runtime-volume-rendering.md) |
| Second-order tensor transforms | No FEA support yet; the transform rule is written down so it is not re-derived wrongly later | [ADR 004](ADR/004-coordinate-systems.md) |

## Unproven claims

Things believed true that no test establishes. Distinguished from the gaps
above because these are *risks*, not known defects.

- **Temporal stability under camera motion.** A still frame that looks perfect
  and shimmers in motion has failed the visual bar. Per-ray jitter — needed
  against banding — is a common cause of exactly that. No volume has ever
  reached a screen (gap 2b: the marcher is built and tested but not wired to a
  scene), so this is unproven rather than untested.
- **Uploaded texels are what a shader would sample.** `FlowVizVolumeDeviceTest`
  proves the Metal driver *accepts* the create and the upload and *reports*
  the format we chose off the created resource. It does not prove the bytes
  landed where the strides say. There is no 3D texel readback on this path, so
  verifying uploaded values needs a staging-copy the layer does not have. A
  stride error that the driver tolerates would pass every test and render a
  sheared volume. The first version of that test claimed a readback it never
  performed — the `ReadBack` array was declared and never filled — and the
  UNORM mutant SURVIVED it; that is how the false claim was found, and it is
  why this entry is stated narrowly.
- **Packaged-build parity.** The packaged target ships without
  `FlowVizEditor`. A reversed `#include` would break it at package time rather
  than at desk, and no packaged build has been produced.
- **Any control the user can set actually reaches the renderer.** `bClampToRange`
  is confirmed not to (gap 2c(i)): it is produced, offset-asserted, unit-tested,
  and no shader parameter exists to carry it. That was found by sweeping for
  producer/consumer gaps, not by a test — nothing in the suite would have failed.
  The transfer function's other fields travel the same untested route, and the
  same question is open for every parameter added from here on. **The general
  form: a struct field with a `static_assert` on its offset and a test on its
  value has two proofs that it exists and none that anything reads it.**

## Maintaining this file

An item leaves this list when the gap closes, not when it is planned. Moving
something to "Deferred by decision" requires a reason and a reference — that
section is for choices, not for things that were merely never done.
