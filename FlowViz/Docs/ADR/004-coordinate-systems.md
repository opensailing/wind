# ADR 004 — Coordinate systems and unit conversion

- **Status:** Accepted
- **Date:** 2026-08-04
- **Affects:** `Public/CFDViz/CFDVizTypes.h` §"Coordinate conversion",
  `Private/CFDViz/CFDVizTypes.cpp:405-470`, `FCFDVizUnits::TryGetLengthInMeters`,
  every renderer, probe and readout

## Context

Two coordinate systems meet in this application and they disagree on
handedness:

| | CFDViz canonical (`plan.md` §6.1) | Unreal |
| --- | --- | --- |
| Handedness | **Right**-handed | **Left**-handed |
| Up axis | Z | Z |
| Forward | X | X |
| Length unit | manifest-declared, SI metres preferred | centimetres |

Both are Z-up and X-forward, which is the dangerous part: the systems differ
only in the **sign of Y**, and a scale factor. A conversion bug is therefore not
a garbled render. It is a render that looks entirely plausible and is mirrored —
and a mirrored vortex street still looks like a vortex street. On the default
mock case (`plan.md` §785-788: 128 × 64 × 24 cells over ≈12 m × 4 m × 1 m,
alternating shed vortices) a Y-flip produces a *physically reasonable-looking*
wake shedding the wrong way.

This is the failure mode that motivates the whole ADR: **the wrong answer here
is not visibly wrong.**

`plan.md` §4 rule 4 requires physical data stay in solver units, with
conversion confined to the visualization adapter, and §6.1 requires that Unreal
conversion happen in *one* adapter.

## Decision

### 1. Stored data is never converted

Field values, grid origins, spacings and mesh vertex positions stay in
**canonical coordinates and manifest-declared units**. No centimetre value is
ever written back into a CFDViz structure, in memory or on disk. §6.1's "never
mix Unreal centimeters into stored scientific fields" is taken literally.

`FCFDVizGrid::Origin` and `Spacing` are `double`, not `float`: scientific
domains run from micrometre features to kilometre extents, and a float32 origin
loses metres of precision at kilometre magnitudes.

### 2. One adapter, at the render boundary

The conversion lives in exactly one place — the `CFDViz` coordinate-conversion
block in `CFDVizTypes.h`/`.cpp`. Readers never call it; renderers, glyphs and
probes never hand-roll it. A second conversion site is how a value gets
converted twice, and a double Y-flip is invisible because it is the identity.

### 3. The transform is a *reflection*, and every consequence is separate API

`MakeSolverToUnrealTransform(MetersToUnrealUnits)` negates Y and scales:

```
    U = (P.X * S,  -P.Y * S,  P.Z * S)
    S = metres_per_unit * 100        // CFDViz::MetersToUnrealCentimeters
```

Negating Y is the minimal correct way to change chirality: it leaves the
forward and up directions a user reasons about untouched. `det(M) < 0`, and the
three consequences each get their own named function so that picking the wrong
one is a visible choice rather than an omission:

| Quantity | Function | Result | Scaled? |
| --- | --- | --- | --- |
| Position | `SolverToUnrealPosition` | `(x, -y, z) * S` | **yes** |
| True vector (velocity, gradient) | `SolverToUnrealDirection` | `(x, -y, z)` | **no** |
| Pseudovector (vorticity, any cross product) | `SolverToUnrealPseudoVector` | `(-x, y, -z)` | **no** |
| Triangle winding | `TransformReversesWinding` | `det < 0` | — |

The pseudovector row is the one that gets missed. Vorticity picks up an extra
factor of `det(M) = -1`, so it is the true-vector result negated. Using
`SolverToUnrealDirection` for it reverses every vortex's sense of rotation —
and the result still looks like plausible flow.

`SolverToUnrealDirection` deliberately applies **no unit scale**. A velocity in
m/s stays in m/s (rule 4). Whether an arrow glyph is drawn 1 cm or 1 m long is
a display decision the adapter makes once and visibly, not something smuggled
in through a coordinate conversion.

Winding reversal is not optional: unreversed, every surface renders inside-out
with backwards normals.

### 4. Derived quantities are computed before conversion

Cross products change sign under a reflection. Anything computing a normal, a
curl or a binormal *after* conversion gets the opposite answer from the same
quantity computed before it. Compute in physical space, then convert the
result; do not convert and then differentiate. `plan.md:808` already requires
finite differences be taken in physical coordinates.

### 5. An unknown length unit is an error, never a default

`FCFDVizUnits::TryGetLengthInMeters` is a **closed table** (m/cm/mm/km/in/ft
and their spellings) that returns `false` for anything else, leaving the output
untouched. Callers must ask or refuse. Defaulting an unrecognised unit to
metres would place a millimetre-scale case a thousand times too large with
nothing on screen to say so — a scale bug that presents as a camera framing
problem.

`MakeUnrealToSolverTransform` is written out longhand rather than obtained from
`FMatrix::Inverse`, which substitutes the identity for a near-singular matrix.
A coordinate conversion that silently returns the identity puts data in the
wrong units with no indication anything happened. A zero or non-finite scale is
logged as an error.

### 6. Three manifest fields are provenance, not geometry

`FCFDVizCoordinates` carries values that look like they belong in the
conversion and do not:

- **`coordinates.origin`** — records where the canonical frame sits relative to
  the source dataset. `grid.origin` alone positions cells; adding this to a
  grid position double-offsets the entire volume, which reads as a plausible
  translation and is wrong.
- **`coordinates.sourceToCanonical`** — describes how the source data was
  brought into canonical form. That already happened, upstream, before the case
  was written. Applying it at render time transforms canonical data a second
  time.
- **`coordinates.crs`** — reserved georeferencing metadata, no decoding meaning
  in 1.0.

None participates in the physical → Unreal conversion. Stated here because all
three are members an adapter author will see while writing exactly the code
that must ignore them.

`sourceToCanonical` is stored **transposed** by
`TryMakeMatrixFromRowMajorArray`: the manifest writes translation in the last
column, `FMatrix` keeps it in the last row. Anything that does begin consuming
it inherits that convention — a straight 16-element copy looks correct for pure
rotations and misplaces every translation.

### 7. Non-canonical manifests are rejected, not reinterpreted

`ParseCoordinates` (`CFDVizManifest.cpp:757`) treats
`handedness`/`upAxis`/`forwardAxis` as closed enums admitting only
`"right"`/`"Z"`/`"X"`. A manifest declaring anything else fails to load. The
data is stored in the canonical frame and cannot be re-derived from a
declaration, so accepting the declaration would mean rendering mislabelled
data. `FCFDVizCoordinateSystem` exists so a reader can *assert* canonicality
rather than assume it.

### 8. Probes read the field, never the render

`plan.md` §4 rule 9 already requires this; restated because it is a coordinate
concern. A probe converts the picked Unreal location *back* through
`MakeUnrealToSolverTransform` and samples the stored field. It never reads back
a pixel and never reports a centimetre value to the user.

## Consequences

- One adapter is one place to test and one place to get wrong. It is therefore
  held to a differential standard: **a round-trip test is not sufficient
  evidence.** `solver → Unreal → solver` returns the input whether the Y-flip
  is present in both directions or absent from both. Every conversion needs at
  least one absolute assertion against a hand-computed expected value.
- Fixtures must be **asymmetric in all three axes**. A test point like
  `(1, 1, 1)` passes under an axis transposition; a cube domain passes under a
  dimension swap. The mock domain is asymmetric in both extent and resolution,
  which is one reason to test against it.
- A Y-flip test must use a point with **non-zero Y**. This sounds too obvious
  to state; it is exactly the fixture written by accident when the interesting
  geometry is centred on the origin plane.
- Direction and pseudovector differ only in sign, so a test that asserts one
  must assert the other is *different*. A test that checks `|v|` is preserved
  passes under both, and under the identity.
- Stored CVM indices are canonical; winding reversal happens in the adapter, so
  rendered index buffers differ from stored ones.

## Open question

Nothing currently calls `TryGetLengthInMeters` to feed
`MakeSolverToUnrealTransform`. Both exist and are correct; the wiring between
them is the renderer's job and does not exist yet. Until it does, any caller
taking the `MetersToUnrealCentimeters` default is silently assuming the case is
in metres — correct for the mock case and wrong for the first millimetre case
we load. The default parameter is a convenience that will eventually need to be
removed.
