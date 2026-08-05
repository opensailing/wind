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

**Not done, and this is the gap that matters.** Nothing *consumes* that
contract. `Shaders/` holds only `FlowVizCommon.ush`; there is no ray-march
`.usf`, no scene proxy, no component, no actor, no transfer function, no
playback. **Nothing has ever been rendered, so no visual review has ever run
against a real frame** — which is the release bar this project is measured
against, and every claim about how it looks remains unmade rather than
unproven.

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

Measured 2026-08-05 by a 27-mutant campaign over `CFDVizByteCursor.h`:
**killed 25, SURVIVED 2, INVALID 0, UNSCORED 1.** Each non-kill is accounted
for rather than tolerated:

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
- `ReadIsAllZero: loop one short` — **UNSCORED**, which is not a pass. The
  test run produced no results, so nothing is known about this mutant. Tracked
  as its own item; `mutate.sh` correctly exits non-zero on any UNSCORED, so
  the campaign as a whole did not report success.

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

### 7. A clean checkout cannot run a real blind visual review

`Tools/capture/references/` is empty by design: the reference stills are
third-party film and AAA frames that are not committed. The harness
(`blind.py`, `review.py`) is implemented and tested, and `review.iterate()`
refuses to grade against an empty corpus rather than reporting an unopposed
pass. But until someone drops images in, the review cannot run at all. See
`Tools/capture/references/README.md`.

### 8. `Build.sh` exits 0 when the build fails

Verified 2026-08-04: a run printing `Result: Failed (ConflictingInstance)`
still returned exit code 0. Any gate keyed on `$?` — a CI step, an `&&` chain,
a mutation harness deciding whether a mutant was killed — passes
unconditionally. Everything in this repo must grep the printed
`Result: Succeeded` text instead. Documented in [`BUILD.md`](BUILD.md).

A mutation run scored on the exit code reports every mutant killed while
proving nothing, which is the failure this project treats as worse than having
no test.

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
