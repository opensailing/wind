# Third-party notices

Required by `plan.md` §3, and referenced by name from
`Tools/cfdviz/src/cfdviz/colormaps.py`, which promised this file before it
existed.

This file lists material in this repository that FlowViz did not originate, and
the terms under which it is here. It covers **what ships**: source in the
repository and dependencies a user must install to build or run FlowViz. It does
not list development-only conveniences that never reach a build artefact.

Two things this file deliberately does *not* do:

- It does not reproduce full license texts. Where a license requires its text to
  travel with a distribution, that obligation attaches to the packaged
  application, and §5 records which those are and where the text must go. A
  notices file that lists names without saying which ones carry that duty has
  answered the easy half of the question.
- It does not claim an audit. §6 states plainly what has *not* been checked, so
  this file cannot be read as a clearance it is not.

---

## 1. Unreal Engine 5

**Epic Games, Inc. — Unreal Engine End User License Agreement.**

Not vendored. FlowViz is an Unreal project and plugin; the engine is installed
separately by the user (see `Docs/BUILD.md`) and no engine source is copied into
this repository.

Relevant obligation: distributing a packaged FlowViz application distributes
engine binaries, which the Unreal EULA governs — including its attribution
requirement and, above a revenue threshold, royalty terms. That is a
distribution-time obligation on whoever ships the build, and it is not satisfied
by this file.

---

## 2. Colormap control points

The control points in `Tools/cfdviz/src/cfdviz/colormaps.py` are the single
source of truth for both the Python tools and the engine, so they are the one
place in this repository where externally-authored *data* is reproduced. Every
table is subsampled from a published source; none is original to FlowViz.

| Table | Origin | Terms |
|---|---|---|
| `viridis` | matplotlib | CC0 1.0 public domain dedication |
| `plasma` | matplotlib | CC0 1.0 public domain dedication |
| `inferno` | matplotlib | CC0 1.0 public domain dedication |
| `magma` | matplotlib | CC0 1.0 public domain dedication |
| `turbo` | Google | Apache License 2.0 |
| `coolwarm` | ParaView / Kitware (Moreland) | BSD 3-Clause |
| `blue-white-red` | Constructed from primaries | No third-party origin |
| `grayscale` | Constructed from endpoints | No third-party origin |

Notes that matter for compliance rather than for credit:

- The four perceptually uniform maps were dedicated to the public domain by
  their authors (Stéfan van der Walt and Nathaniel Smith) specifically so they
  could be reused without attribution. They are listed anyway, because a notices
  file that omits what it is not required to name is less useful than one that
  is complete.
- **Turbo is Apache 2.0, and that license has conditions** — it is the only
  colormap entry here that does. See §5.
- `coolwarm` derives from Kenneth Moreland's diverging colormap as shipped by
  ParaView. It is reproduced here as control points, not as code.
- The last two rows are in this table to make the table exhaustive. Neither
  carries a third-party claim: a black-to-white ramp and a blue/white/red ramp
  are not authored works.

The *interpolation* between these points, the pixel-centre sampling convention,
and both implementations of them are FlowViz's own — see the normative comment
at the top of `colormaps.py`.

---

## 3. Python dependencies

Declared in `Tools/cfdviz/pyproject.toml`. None is vendored; all are installed
from PyPI.

| Package | Required? | Terms |
|---|---|---|
| `numpy` (>=1.24,<3) | Required | BSD 3-Clause |
| `lz4` (>=4.3,<5) | Optional (`lz4` extra) | BSD 2-Clause |
| `jsonschema` (>=4.20,<5) | Optional (`schema` extra) | MIT |
| `pytest` (>=7.4,<9) | Development only | MIT |

`lz4` is optional in both directions and this is deliberate, per
[ADR 005](ADR/005-compression-codec.md): the Python writer emits `codec = 2`
only when the package is installed, and the engine decodes that codec through
Unreal's own `NAME_LZ4` rather than through this package. So a build without the
`lz4` extra can still *read* every case FlowViz can write.

`pytest` runs the test suite and is in no build artefact.

---

## 4. Code written for FlowViz that resembles third-party code

Two implementations in this repository follow published algorithms closely
enough that the resemblance is worth stating outright, so that no one later
mistakes either for copied code.

- **`CFDVizCrc32C.cpp`** implements CRC-32C (Castagnoli, polynomial `0x1EDC6F41`
  reflected). The table is *generated at initialisation from the polynomial*
  rather than pasted from a published table — see the comment at the top of the
  file, which says so and gives that as the reason. The algorithm is described
  in RFC 3720 §12.1 and is not subject to copyright; no third-party
  implementation was consulted or copied.
- **The compression codec IDs** in the CVF container follow
  [ADR 005](ADR/005-compression-codec.md). Decompression is delegated entirely to
  Unreal's `FCompression` (`NAME_Zlib`, `NAME_LZ4`, `NAME_Oodle`), which is
  engine code under the Unreal EULA (§1). FlowViz implements no decompressor.

---

## 5. Obligations that attach to a packaged build

The two entries above with conditions, and what discharging them requires:

1. **Apache License 2.0 (turbo colormap).** §4 of that license requires that a
   distribution include a copy of the license and retain attribution notices. A
   packaged FlowViz application that offers turbo therefore must carry the
   Apache 2.0 text and this attribution in its shipped notices, not merely in
   this repository file.
2. **BSD 2- and 3-Clause (`numpy`, `lz4`, `coolwarm`).** Each requires the
   copyright notice and disclaimer to accompany a binary distribution. This
   applies to `numpy` and `lz4` only if a build actually bundles the Python
   tools; the engine runtime does not.
3. **Unreal EULA (§1).** Attribution as the EULA specifies, discharged by
   whoever distributes the build.

**Status: not yet done.** There is no packaged build (see `Docs/BACKLOG.md`), so
none of these has been discharged. The packaging task must generate a shipped
notices file containing the full license texts named above; this repository file
is its input, not a substitute for it. Recorded here rather than in a commit
message so it cannot be shipped past unnoticed.

---

## 6. What has not been verified

Stated so this file is not read as a clearance:

- No automated license scan has been run over the dependency tree. The direct
  dependencies in §3 are read off `pyproject.toml`; **transitive dependencies
  have not been enumerated.** `numpy` and `pytest` both pull in further packages
  at install time, and none of those appears here.
- The engine's own third-party components (it has many, under various terms) are
  not inventoried. They are covered by the Unreal EULA from FlowViz's position
  as a licensee, but a distributor's obligations are not enumerated here.
- Nothing in this file is legal advice, and none of it has been reviewed by a
  lawyer. It is an engineering record of what was found by reading the sources
  named in each section.
