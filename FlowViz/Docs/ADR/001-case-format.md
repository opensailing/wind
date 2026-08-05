# ADR 001 — A purpose-built case format instead of an existing one

- **Status:** Accepted
- **Date:** 2026-08-04
- **Affects:** `Docs/CFDVIZ_FORMAT.md` (the whole document),
  `Tools/cfdviz/src/cfdviz/`, `Plugins/FlowVizRuntime/.../Private/CFDViz/`
- **Related:** [ADR 002](002-runtime-volume-rendering.md) (what consumes it),
  [ADR 004](004-coordinate-systems.md) (the coordinate convention it declares),
  [ADR 005](005-compression-codec.md) (the codec it carries)

## Context

`plan.md` §1 specifies a "documented, language-neutral CFDViz case format" and
then defines it in detail. It does not say *why* a new format, and that is the
decision with the widest blast radius in the project: it produced two
independent implementations (Python writer, C++ reader), a normative spec, a
cross-language verification bridge, and every reader test in the repository.
Left unrecorded, it reads like not-invented-here, and the first reviewer to
notice that VDB and VTK exist will reasonably ask.

The requirement it has to serve is narrower than "store CFD data":

- The renderer streams **one frame's worth of one field** per timeline step and
  must not read the rest (rule 1 — nothing expensive on the game thread).
- Every offset and size in the file arrives from an untrusted source and must be
  bounds-checked **before allocation** (rule 12).
- Values stay in solver units, `NaN` is legal data, and masked cells are
  explicit (rules 4, 5, 10) — so the container cannot be free to normalize,
  and its statistics must be defined to skip `NaN` rather than propagate it.
- Two languages must agree bit-for-bit, which requires a spec precise enough to
  implement twice without reading either implementation.

### Candidate A — OpenVDB / `.vdb`

Genuinely available on this platform (see ADR 002 for the specific engine
paths). It is the best sparse-volume container in existence and the right
choice for the offline export path, which is why ADR 002 keeps it as an
optional Presentation-only flag.

It fails as the *runtime* format for reasons of shape, not quality. VDB's tree
is optimized for sparse, narrow-band data; a CFD wake volume is dense in the
region of interest, so the tree buys little and costs a traversal. Reading one
frame means opening a grid whose layout is decided by the writer's tree
topology rather than by a directory the reader can bounds-check up front. And
`plan.md` §929 is explicit that VDB conversion must not be required for
ordinary runtime playback.

### Candidate B — VTK (`.vtu` / `.vti` / XML or HDF5)

The lingua franca of scientific visualization, and the format a user's data
most likely already exists in. Its data model is a superset of what we need.

Two disqualifiers. First, the core visualizer must not depend on VTK libraries
— `plan.md` §5A puts the seam at the data-source interface precisely so that a
VTK adapter is an *adapter*, and `FCFDVizVTKAdapter` is reserved for exactly
that. Second, VTK's legacy and XML readers are permissive by design: they
accept, warn, and continue where this project must reject. Rule 13 (malformed
input produces an error, not a crash) is not something one retrofits onto a
permissive parser.

### Candidate C — HDF5, or a raw dump plus JSON sidecar

HDF5 brings a large C dependency and a chunking model that duplicates what
bricking already does. A raw dump plus a sidecar is what we would converge on
anyway — and is, in substance, what CVF is, with the addition of a directory
that makes bounds-checking possible before the first allocation.

## Decision

**Define CFDViz Case Format 1.0** as a *directory* of small, single-purpose
binary files plus a JSON manifest, specified normatively in
`Docs/CFDVIZ_FORMAT.md`, and implement it twice — once in Python (writer and
reader) and once in C++ (reader only).

Four properties are load-bearing, and each maps to a rule:

| Property | Serves |
| --- | --- |
| A case is a directory; one file per field per frame | Rule 1 — a timeline step reads exactly what it displays |
| Fixed-size header, then an explicit brick directory | Rule 12 — every size is checkable against the real file size before allocating |
| Values in solver units; `NaN` legal; quantization declared | Rules 4, 5, 6, 10 |
| A normative spec, not a reference implementation | Two implementations can be shown to agree |

The reader is the strict half of Postel's law on purpose: unknown JSON
properties are ignored, but an unknown value of a *required* enum is rejected
(spec §1.4). Forward compatibility lives in the minor-version rule, not in
guessing what a writer meant.

### Language-neutral means the *spec* is normative

Neither implementation is the definition. The mechanism that keeps them honest
is `known_values.json` (spec §9): the Python writer records values it computed
at named locations, and the C++ reader is tested against those recorded values
rather than against Python. Two implementations that both read the same spec
and agree on the same bytes is evidence; one implementation ported to another
language is not.

## Consequences

**A user's existing data does not load.** Nothing in this repository reads
OpenFOAM, VTK, or CGNS. Every case must be written by the `cfdviz` tool. This
is a real cost, deferred deliberately — the adapter seam exists, and
`BACKLOG.md` records that only the interfaces are reserved.

**Two implementations means two chances to be wrong in the same way.** If both
were written by reading each other, the bridge proves nothing. This is why the
spec is normative and why `known_values.json` records *computed* values rather
than a checksum of the file.

**The strictness is not free.** A rejected case is a support burden that a
permissive reader would not have. The trade is deliberate: a wrong picture that
renders is worse than a case that refuses to open, because only one of those is
noticed.

**Statistics carry `NaNCount` and `ValidCount` as data, not diagnostics.**
`FCFDVizStatistics` reports `bValid = false` with a count rather than
substituting a range, and the manifest parser leaves both counts at zero
because the manifest does not record them — inventing one would let a caller
derive a NaN fraction that no writer ever measured.

**Bricking is in the format, not just in the renderer.** A future reader that
decides to load whole fields at once still has to honour the directory, which
keeps rule 12 checkable at the file layer rather than at the call site.

## Status of the implementation

Not a plan — what is true as of this ADR. Statuses here mirror
[`ARCHITECTURE.md`](../ARCHITECTURE.md), which is authoritative.

| Piece | State |
| --- | --- |
| `Docs/CFDVIZ_FORMAT.md` | Written, normative, §1–§9 |
| Python writer + reader | Implemented and tested |
| C++ manifest / mesh / payload readers | Implemented; tested |
| C++ volume (CVF) reader | Implemented; test in progress |
| `known_values.json` bridge | Specified; C++ side not yet written |
| Any actual case data | **None in the repository** — `BACKLOG.md` item 1 |

The last row is the one that matters: the format is specified and read, and
nothing has been written by the generator and read back by Unreal end to end.
Until that happens this ADR describes a decision that is implemented but not
yet demonstrated.
