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

### 2a. Nothing has marched a populated volume

The gap immediately below the one above, and the one to close first.

Every ray-march feature is verified against its *interface* — the cbuffer
round-trips, the shader compiles, the pass dispatches, the flag colours are
distinct. Not one of them has been run over a volume with data in it. The
marching loop itself, which is where compositing, iso-surface extraction,
gradient estimation and clipping actually happen, is unexercised.

This is a correctness gap, not a polish one. A loop that steps by the largest
voxel spacing instead of the smallest, composites back-to-front, or samples at
cell corners instead of centres will still dispatch, still compile, still
round-trip its constants, and still produce a picture. Some of those pictures
look entirely reasonable.

The fix is a fixture volume holding an analytic field whose correct integral
is known in closed form — a linear ramp, a Gaussian blob, a plane at a known
iso-value — marched with assertions on the quantitative `OutValue` UAV rather
than on pixels. That makes a wrong loop fail on a number instead of on
somebody's judgement of an image, which is the only kind of visual claim this
project accepts.

Until then, treat "the ray-marcher works" as unmade. It compiles and it runs.

## Correctness gaps

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
  against banding — is a common cause of exactly that. Nothing renders yet, so
  this is unproven rather than untested.
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

## Maintaining this file

An item leaves this list when the gap closes, not when it is planned. Moving
something to "Deferred by decision" requires a reason and a reference — that
section is for choices, not for things that were merely never done.
