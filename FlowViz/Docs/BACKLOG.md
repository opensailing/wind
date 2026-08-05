# Backlog

Required by `plan.md` §3. Known gaps, deferred work, and things that are worse
than they look. Ordered by what blocks the most downstream work, not by effort.

This file is for things that are **true now**. A backlog that lists only future
features, and never the shortcuts already taken, is a marketing document. Items
that came out of a real investigation cite what was checked.

## Release-blocking

### 1. No case data exists anywhere in the repository

`python -m cfdviz generate-mock` is specified in `plan.md` §537 and §769 and is
**not implemented**. There is no `.cvf`, `.cvm` or `.cva` file in the tree and
no manifest for a real case.

This is the item that blocks the most: every renderer milestone (C, D, E) needs
a case to display, the packaged application must ship a low-resolution sample
per §3, and the blind visual review cannot produce a candidate frame without
one. It is also what makes several tests below untestable rather than merely
untested.

*In progress.* The test suite (`Tools/cfdviz/tests/test_mock.py`, 591 lines)
is written and currently fails at import with `No module named 'cfdviz.mock'` —
which is the correct state for test-first work, not a defect.

### 2. Milestone C is unstarted

`Plugins/FlowVizRuntime/Shaders/` is empty. No scene proxy, no component, no
uploader. The path is decided in
[ADR 002](ADR/002-runtime-volume-rendering.md) and none of it is written, so
nothing has ever been rendered and no visual review has ever run against a real
frame.

## Correctness gaps

### 3. The CVF volume reader has no committed test

Every other reader has one. This is the reader that decodes the volume every
later milestone displays, so an undetected misparse here surfaces as a wrong
picture rather than an error. Tracked in
[`ARCHITECTURE.md`](ARCHITECTURE.md) and
[`OPENFOAM_PARAVIEW_PARITY.md`](OPENFOAM_PARAVIEW_PARITY.md) as the reason the
CVF row is `Partial` rather than `Done`.

### 4. The byte cursor has no direct test

`Private/CFDViz/CFDVizByteCursor.h` is exercised indirectly by every reader
test, and indirect coverage is not a test of its bounds checking — which is its
entire job, and the mechanism behind rule 12.

Specifically untested: `CanRead`'s overflow-safe formulation
(`Count <= Size - Offset` rather than `Offset + Count <= Size`, which is the
addition that overflows on a hostile 2^63 length); `Seek` rejecting a negative
or past-the-end offset while leaving the cursor untouched; `ReadIsAllZero` on
a reserved field; and NaN payload bits surviving `ReadFloat`/`ReadDouble`
bit-exactly, which format rule 1.7 requires.

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
- **Packaged-build parity.** The packaged target ships without
  `FlowVizEditor`. A reversed `#include` would break it at package time rather
  than at desk, and no packaged build has been produced.

## Maintaining this file

An item leaves this list when the gap closes, not when it is planned. Moving
something to "Deferred by decision" requires a reason and a reference — that
section is for choices, not for things that were merely never done.
