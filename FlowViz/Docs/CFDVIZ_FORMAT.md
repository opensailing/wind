# CFDViz Case Format 1.0 — Normative Specification

Status: **normative**. This document is the single source of truth for every CFDViz
reader and writer, in every language. Where this document and any implementation
disagree, this document wins and the implementation is a bug.

Two independent implementations must agree byte-for-byte:

| Implementation | Location |
| --- | --- |
| Python writer + reader (reference) | `Tools/cfdviz/src/cfdviz/` |
| Unreal C++ reader | `Plugins/FlowVizRuntime/Source/FlowVizRuntime/` |

Agreement is proven mechanically by the cross-language bridge in §9, not by inspection.

---

## 1. Scope and general rules

A CFDViz *case* is a **directory**, not a single opaque file:

```
CaseName.cfdviz/
  manifest.json          UTF-8 JSON, no BOM
  known_values.json      cross-language verification bridge (§9)
  meshes/
    obstacle.cvm
    boundaries.cvm
  frames/
    000000/
      U.cvf  pressure.cvf  speed.cvf  vorticity.cvf  qCriterion.cvf  validMask.cvf
    000001/
      ...
  optional/
    thumbnails/  provenance/  attachments/
```

General rules, all normative:

1. All binary data is **little-endian**. A reader MUST reject big-endian data
   rather than byte-swap it (detected via the endian marker, §4.1).
2. All JSON is UTF-8 without a byte-order mark.
3. All paths in `manifest.json` are **relative to the case root**, use `/` as the
   separator, and MUST NOT escape the case root. A reader MUST reject any path
   containing a `..` segment, an absolute path, a drive letter, or a leading `/`.
   This check happens on the *lexical* path, before any filesystem call.
4. Version handling: the format version is `MAJOR.MINOR.PATCH`.
   - Unsupported **major** version → reject with a clear error.
   - Newer **minor** version → accept, provided every construct marked *required*
     in this document is present and understood.
   - Unknown JSON object properties → **ignore silently**. Unknown values of a
     *required enum* → reject.
5. Every offset, length, count, and uncompressed size read from a file MUST be
   bounds-checked against the actual file size **before** any allocation is made.
   A reader MUST NOT allocate a buffer whose size came from untrusted input
   without first proving the input is consistent with the file on disk.
6. Field values are stored in **solver units**, declared in the manifest. No
   reader or writer may normalize, quantize, clamp, smooth, or rescale values.
   Any quantization is a property of the *format* (e.g. `uint8` storage) and MUST
   be declared in the manifest.
7. `NaN` is a legal value in floating-point payloads and MUST be preserved
   exactly. A reader MUST NOT convert `NaN` to zero. Statistics ignore `NaN` and
   ignore cells rejected by the mask field.

---

## 2. Canonical physical coordinates

The canonical CFDViz coordinate system is:

- Right-handed
- **Z-up**, **X-forward**
- Length unit declared in `units.length`; SI metres preferred

A case MAY declare `coordinates.sourceToCanonical`, a 4×4 **row-major** matrix
transforming source coordinates into canonical coordinates.

Conversion into Unreal's coordinate system (left-handed, Z-up, centimetres)
happens in exactly **one** adapter and nowhere else. Unreal centimetres MUST
NEVER be written back into stored scientific fields. See `Docs/ADR/004-coordinate-systems.md`.

---

## 3. manifest.json

The JSON Schema is `Tools/cfdviz/src/cfdviz/schema/cfdviz-1.0.schema.json`, which
is itself normative and MUST validate every case this project produces.

Required top-level properties: `format`, `version`, `case`, `units`,
`coordinates`, `timeline`, `grids`, `fields`.
Optional: `meshes`, `derivedFields`, `structures`, `provenance`.

`format` MUST be the exact string `"CFDViz"`.

### 3.1 Required invariants

A manifest is invalid, and MUST be rejected with a specific message, if any hold:

- `timeline.times` is not strictly monotonically increasing.
- `len(timeline.times) != timeline.frameCount`, or likewise for `timeline.steps`.
- Any `field.grid` does not name a declared grid `id`.
- Any `grid.maskField` does not name a declared field `id`.
- Any two fields share a `numericId`, or share an `id`.
- `field.componentCount` disagrees with `len(field.components)`.
- `field.dataType` is not one of `float16`, `float32`, `uint8`.
- `field.association` is not one of `cell`, `point`.
- Any grid `dimensions` component is `< 1`, or any `spacing` component is `<= 0`.
- `field.storage.pathPattern` fails the path-traversal check of §1.3.

### 3.2 Field associations

Version 1.0 supports `cell` and `point` only. Reserved for future major/minor
versions, and MUST be rejected in 1.0: `mesh-vertex`, `mesh-element`,
`integration-point`, `face`, `particle`.

For a grid of `dimensions = [nx, ny, nz]`:

- `cell` association → the field has exactly `nx * ny * nz` values. Value
  `(i,j,k)` is located at cell **centre**
  `origin + spacing * (vec(i,j,k) + 0.5)`.
- `point` association → the field has exactly `(nx+1)*(ny+1)*(nz+1)` values.
  Value `(i,j,k)` is located at `origin + spacing * vec(i,j,k)`.

Getting this half-cell offset wrong is the single most common source of
visualisation error; it is covered by a dedicated test on both sides.

### 3.3 Data types

`float64` field storage is **not** supported for GPU rendering in 1.0. The
manifest MAY record original solver precision in `field.solverPrecision` for
provenance; it does not affect storage.

---

## 4. CVF — bricked volume field

One `.cvf` file holds **one field at one frame**. It is independently readable:
the brick directory permits random brick access, region-of-interest loading, and
empty-space skipping without decoding the whole volume.

Serialization is **manual, field by field**. An implementation MUST NOT `memcpy`
a native struct, because C++ padding is compiler-dependent.

### 4.1 Header — fixed 128 bytes

| Offset | Size | Type | Name |
| --- | --- | --- | --- |
| 0 | 8 | char[8] | `magic` = `CFDVOL1\0` |
| 8 | 4 | uint32 | `headerBytes` = 128 |
| 12 | 2 | uint16 | `majorVersion` = 1 |
| 14 | 2 | uint16 | `minorVersion` = 0 |
| 16 | 4 | uint32 | `endianMarker` = `0x01020304` |
| 20 | 4 | uint32 | `flags` |
| 24 | 4 | uint32 | `frameIndex` |
| 28 | 4 | uint32 | `fieldNumericId` |
| 32 | 8 | float64 | `simulationTime` |
| 40 | 4 | uint32 | `dimensionX` |
| 44 | 4 | uint32 | `dimensionY` |
| 48 | 4 | uint32 | `dimensionZ` |
| 52 | 2 | uint16 | `brickSizeX` |
| 54 | 2 | uint16 | `brickSizeY` |
| 56 | 2 | uint16 | `brickSizeZ` |
| 58 | 1 | uint8 | `componentCount` |
| 59 | 1 | uint8 | `dataType` |
| 60 | 1 | uint8 | `association` |
| 61 | 1 | uint8 | `codec` |
| 62 | 2 | uint16 | `reserved` = 0 |
| 64 | 8 | uint64 | `brickCount` |
| 72 | 8 | uint64 | `directoryOffset` |
| 80 | 8 | uint64 | `payloadOffset` |
| 88 | 16 | float32[4] | `backgroundValue` |
| 104 | 4 | uint32 | `headerCrc32c` |
| 108 | 20 | byte[20] | `reserved` = 0 |

`endianMarker` read as anything other than `0x01020304` → reject as
"byte order not supported"; do not attempt to swap.

`headerCrc32c` is CRC-32C (§8) over bytes `[0, 128)` **with bytes `[104, 108)`
set to zero** during computation.

### 4.2 Enumerations

```
dataType:      1 = float16 (IEEE 754 binary16)
               2 = float32
               3 = uint8

association:   0 = cell
               1 = point

codec:         0 = none
               1 = zstd     RESERVED — see §7. A 1.0 reader MUST reject this
                            value with the message documented in §7.
               2 = lz4
               3 = zlib     DEFAULT
```

### 4.3 Brick directory — 80 bytes per entry

Located at `directoryOffset`, `brickCount` entries, contiguous.

| Offset | Size | Type | Name |
| --- | --- | --- | --- |
| 0 | 4 | uint32 | `brickIndexX` |
| 4 | 4 | uint32 | `brickIndexY` |
| 8 | 4 | uint32 | `brickIndexZ` |
| 12 | 2 | uint16 | `validSizeX` |
| 14 | 2 | uint16 | `validSizeY` |
| 16 | 2 | uint16 | `validSizeZ` |
| 18 | 2 | uint16 | `flags` |
| 20 | 8 | uint64 | `absolutePayloadOffset` |
| 28 | 4 | uint32 | `compressedBytes` |
| 32 | 4 | uint32 | `uncompressedBytes` |
| 36 | 16 | float32[4] | `componentMin` |
| 52 | 16 | float32[4] | `componentMax` |
| 68 | 4 | uint32 | `payloadCrc32c` |
| 72 | 8 | byte[8] | `reserved` = 0 |

`payloadCrc32c` is CRC-32C over the **compressed** payload bytes as stored, i.e.
over exactly `compressedBytes` bytes at `absolutePayloadOffset`. Verifying the
CRC therefore requires no decompression.

### 4.4 Payload rules

1. Within a brick, **X varies fastest**, then Y, then Z.
2. Components are **interleaved per voxel**: `v0.x, v0.y, v0.z, v1.x, ...`.
3. Edge bricks are **not padded**. A brick covering a partial region stores
   exactly `validSizeX * validSizeY * validSizeZ` voxels.
4. Therefore, normatively:
   `uncompressedBytes == validSizeX * validSizeY * validSizeZ * componentCount * sizeof(dataType)`
   A reader MUST verify this equality **before allocating** and reject on
   mismatch. This is the primary defence against a malicious size field.
5. A brick whose every voxel equals `backgroundValue` MAY be omitted from the
   directory entirely. Any brick absent from the directory evaluates to
   `backgroundValue` for all its voxels.
6. `NaN` is legal in float payloads and is preserved bit-exactly.
7. Statistics (`componentMin`/`componentMax`, and manifest-level statistics)
   ignore `NaN` and ignore cells rejected by the mask field. If **every** value
   in a brick is `NaN`/masked, the writer MUST store `componentMin = +inf` and
   `componentMax = -inf` for that brick, which readers treat as "no valid data".
8. A brick that fails its CRC MAY be marked unavailable while the rest of the
   case continues to load, provided the failure is surfaced in the validation
   report. A reader MUST NOT silently substitute zeros.

---

## 5. CVM — boundary / structure triangle mesh

### 5.1 Header — fixed 96 bytes

| Offset | Size | Type | Name |
| --- | --- | --- | --- |
| 0 | 8 | char[8] | `magic` = `CFDMESH1` |
| 8 | 2 | uint16 | `majorVersion` = 1 |
| 10 | 2 | uint16 | `minorVersion` = 0 |
| 12 | 4 | uint32 | `endianMarker` = `0x01020304` |
| 16 | 4 | uint32 | `flags` |
| 20 | 4 | uint32 | `headerBytes` = 96 |
| 24 | 8 | uint64 | `vertexCount` |
| 32 | 8 | uint64 | `triangleCount` |
| 40 | 8 | uint64 | `positionsOffset` |
| 48 | 8 | uint64 | `normalsOffset` |
| 56 | 8 | uint64 | `indicesOffset` |
| 64 | 8 | uint64 | `patchIdsOffset` |
| 72 | 8 | uint64 | `nodeIdsOffset` |
| 80 | 4 | uint32 | `headerCrc32c` |
| 84 | 12 | byte[12] | `reserved` = 0 |

`headerCrc32c` is CRC-32C over bytes `[0, 96)` with bytes `[80, 84)` zeroed.

### 5.2 Flags

```
bit 0  normals present
bit 1  patch IDs present
bit 2  node IDs present
bit 3  positions are float64 (otherwise float32)
```

An offset for an array whose presence bit is clear MUST be written as 0 and MUST
be ignored by readers.

### 5.3 Arrays

- `positions` — XYZ per vertex, float32 or float64 per flag bit 3
- `normals` — XYZ float32 per vertex, when present
- `indices` — 3 × uint32 per triangle. Every index MUST be `< vertexCount`;
  a reader MUST validate this and reject out-of-range indices.
- `patchIds` — 1 × uint32 per **triangle**, when present
- `nodeIds` — 1 × uint64 per **vertex**, when present

Triangle winding is **counter-clockwise when viewed from outside** the solid.

The shipped sample case MUST contain the named patches `inlet`, `outlet`,
`sideWalls`, and `cylinderWall`.

---

## 6. CVA — mesh-associated array (reserved for FEA)

CVA is specified and round-trip tested in 1.0, but no 1.0 UI renders every CVA
result type. Header is a fixed 96 bytes, magic `CFDARR1\0`, same endian marker,
same CRC rule.

It carries: frame index, physical time, element/vertex association, component
count (1 scalar, 3 vector, 6 symmetric tensor, 9 full tensor), storage type
(float16/float32/float64), optional compression, and per-frame plus global
statistics.

### 6.1 Header — fixed 96 bytes

| Offset | Size | Type | Name |
| --- | --- | --- | --- |
| 0 | 8 | char[8] | `magic` = `CFDARR1\0` |
| 8 | 2 | uint16 | `majorVersion` = 1 |
| 10 | 2 | uint16 | `minorVersion` = 0 |
| 12 | 4 | uint32 | `endianMarker` = `0x01020304` |
| 16 | 4 | uint32 | `flags` |
| 20 | 4 | uint32 | `headerBytes` = 96 |
| 24 | 4 | uint32 | `frameIndex` |
| 28 | 4 | uint32 | `fieldNumericId` |
| 32 | 8 | float64 | `simulationTime` |
| 40 | 8 | uint64 | `valueCount` (vertices or elements) |
| 48 | 1 | uint8 | `componentCount` — 1, 3, 6, or 9 |
| 49 | 1 | uint8 | `dataType` |
| 50 | 1 | uint8 | `association` |
| 51 | 1 | uint8 | `codec` |
| 52 | 4 | uint32 | `payloadCrc32c` |
| 56 | 8 | uint64 | `payloadOffset` |
| 64 | 8 | uint64 | `compressedBytes` |
| 72 | 8 | uint64 | `uncompressedBytes` |
| 80 | 4 | uint32 | `headerCrc32c` |
| 84 | 4 | uint32 | `reserved` = 0 |
| 88 | 8 | uint64 | `statisticsOffset` — 0 when absent |

`headerCrc32c` is CRC-32C over bytes `[0, 96)` with bytes `[80, 84)` zeroed —
byte-for-byte the same rule as CVM §5.1.

Two layout choices are deliberate and must not be "tidied":

- Bytes `[0, 24)` are identical in meaning and position to a CVM header, so one
  C++ routine can sniff and validate any CFDViz container.
- `headerCrc32c` sits at `[80, 84)` for the same reason.

### 6.2 Enumerations

```
dataType     1 = float16   2 = float32   4 = float64
             3 is RESERVED for CVF's uint8, which CVA does not offer.
             1 and 2 match CVF §4.2 so a shared C++ enum needs no translation.

association  0 = mesh-element   1 = mesh-vertex
             Matches CVF's convention: element-like 0, nodal/point-like 1.

flags        bit 0  per-frame statistics present
             bit 1  global statistics present
```

### 6.3 Payload rules

These mirror CVF §4.4 exactly, so both payload decoders behave the same way.

1. Components are **interleaved per entity**: `v0.x, v0.y, v0.z, v1.x, …`.
2. `uncompressedBytes == valueCount * componentCount * sizeof(dataType)`,
   verified **before** allocating (§1.5).
3. `payloadCrc32c` is CRC-32C over the **compressed** bytes as stored, so
   integrity is checkable without decompressing.
4. `NaN` is legal and preserved bit-exactly; statistics ignore it.

Symmetric 6-component tensors use the normative order `XX, YY, ZZ, XY, YZ, XZ`.
The other common Voigt order (`XX, YY, ZZ, YZ, XZ, XY`) is the classic way for
two solvers to silently disagree, so readers must not assume it. Full
9-component tensors are row-major: `XX, XY, XZ, YX, YY, YZ, ZX, ZY, ZZ`.

### 6.4 Statistics section

Present when `statisticsOffset != 0`. One section is `8 + 32 * componentCount`
bytes; the per-frame section comes first, then the global section when both
flags are set.

| Offset | Size | Type | Name |
| --- | --- | --- | --- |
| 0 | 8 | uint64 | `valueCount` |
| 8 | 8·C | float64[C] | `minimum` per component |
| 8 + 8·C | 8·C | float64[C] | `maximum` per component |
| 8 + 16·C | 8·C | float64[C] | `mean` per component |
| 8 + 24·C | 8·C | uint64[C] | `validCount` per component |

`validCount` counts non-NaN entries per component, so a partially invalid field
still reports honest statistics rather than silently averaging NaN.

### 6.5 Associating a CVA with a mesh

A `.cva` is **not declared in `manifest.fields[]`**, and this is deliberate.
`fields[]` describes grid storage: every entry names a `grid`, and §3.2 requires
1.0 to *reject* the `mesh-vertex` and `mesh-element` associations a CVA carries.
A CVA declared as a field would therefore be a manifest that 1.0 must refuse.

`manifest.structures[]` is the slot that binds a mesh to its results. Each entry
names a `mesh` by id and may name `displacementField`, `velocityField`,
`stressField` and `strainField`. It is **reserved in 1.0**: readers MUST parse
and validate it when present, and MUST NOT require it.

**Discovery convention.** Because `structures[]` is optional in 1.0, a case may
carry CVA files that nothing in the manifest points at. Both implementations
resolve these the same way, and a reader MUST follow it or the two will disagree
about what a case contains:

```
meshes/*.cva        relative to the case root, matched non-recursively
```

Each discovered path is recorded verbatim in `known_values.json` so the C++ and
Python readers can be compared file by file. `§1.3` path-traversal rules apply
unchanged: a discovered path that escapes the case root MUST be rejected, not
clamped.

**This is a 1.0 limitation, not the intended end state.** The better fix is a
first-class manifest slot for mesh-associated arrays, which is a breaking change
deferred to 1.1 — see the note in `structures[]` in `schema/manifest.schema.json`.
Until then, *discovery is normative*: a reader that only honours `structures[]`
will silently miss arrays that a conforming writer emitted.

---

## 7. Compression codecs — deviation from the original plan

**The original plan mandated zstd. This project ships zlib instead.** The reason
is concrete and was verified against the installed engine, not assumed:

> Unreal Engine 5.8 ships **no C++ zstd** that a runtime module can link. The
> only zstd in the installation is `ZstdSharp.dll`, a **.NET** assembly used by
> UnrealBuildTool and AutomationTool — unusable from a runtime C++ module.
> `Engine/Source/ThirdParty/` contains zstd *licence* files and a Boost header
> reference, but no linkable library.
>
> Verified: `find "$UE/Engine" -iname "*zstd*"` returns only `.dll`, `.tps`,
> `.LICENSE` and a Boost `.hpp`. `Engine/Source/ThirdParty/` contains `zlib`
> and no `zstd`.

Mandating zstd would mean the Unreal reader **could not decode the sample case
this project ships** — a release-blocking contradiction — or would require
vendoring and building zstd from source, adding a third-party dependency and
build complexity for no visualisation benefit.

Shipping default is therefore **`codec = 3` (zlib)**:

- Python: `zlib` — standard library, no dependency at all.
- Unreal: `FCompression::UncompressMemory(NAME_Zlib, ...)` — engine-native.

Normative details:

- zlib payloads use the **zlib container format (RFC 1950)**: 2-byte header plus
  trailing Adler-32. This is exactly what Python's `zlib.compress()` emits and
  what `NAME_Zlib` consumes. Raw DEFLATE (RFC 1951) is **not** used.
- Default compression level is **6**.
- `codec = 2` (lz4) is permitted; Unreal decodes it via `NAME_LZ4`. The Python
  writer emits it only when the optional `lz4` package is installed.
- `codec = 1` (zstd) is **reserved but unimplemented**. A 1.0 reader MUST reject
  it with exactly this user-facing message:
  `"CVF codec 'zstd' is reserved but not supported in CFDViz 1.0. Re-export this case with codec 'zlib'."`
  It MUST NOT crash, and MUST NOT silently fall back to another codec.

This deviation is recorded in `Docs/ADR/005-compression-codec.md`.

---

## 8. CRC-32C

All CRCs in this format are **CRC-32C (Castagnoli)**, not the CRC-32 used by
zip/zlib, and not Unreal's `FCrc::MemCrc32`.

```
polynomial (normal)     0x1EDC6F41
polynomial (reflected)  0x82F63B78
initial value           0xFFFFFFFF
input reflected         yes
output reflected        yes
final XOR               0xFFFFFFFF
```

**Mandatory self-check.** Both implementations MUST contain a test asserting:

```
CRC32C("123456789")  ==  0xE3069283
```

This is the standard check value. A CRC implementation that fails it is wrong,
and any file it produced is unreadable by a correct implementation. A
table-driven software implementation is sufficient and is required for
portability; hardware acceleration is an optional optimisation that MUST produce
identical results.

---

## 9. Cross-language verification bridge — `known_values.json`

Definition-of-done item 17 requires that the sample's known values match between
the Python and Unreal readers. Proving that by inspection is not acceptable, so
the generator emits a machine-checkable bridge file at the case root.

The **Python generator writes it**; the **Unreal automation test reads it** and
asserts every entry. Neither side may regenerate the other's expectations.

```json
{
  "formatVersion": "1.0.0",
  "caseId": "d7f46da7-6bd8-4d4b-9410-32d1ea776328",
  "crc32cCheck": "0xE3069283",
  "samples": [
    {
      "frame": 0,
      "field": "pressure",
      "voxel": [10, 20, 5],
      "component": 0,
      "value": -0.0234567,
      "bits": "0xBCC024DD"
    }
  ]
}
```

Rules:

1. `bits` is the **exact IEEE 754 bit pattern** of the stored value, as an
   8-digit uppercase hex string, for the field's own `dataType`
   (16-bit → 4 digits for `float16`, 2 digits for `uint8`).
2. Comparison is performed on `bits`, **not** on `value`. `value` exists for
   human readability only. This makes the check exact and immune to
   text-formatting and rounding differences between languages.
3. The sample set MUST include, at minimum:
   - a scalar field and a vector field,
   - the first and last stored frame,
   - a voxel inside the masked obstacle,
   - a voxel on a **partial edge brick**,
   - a voxel whose value is `NaN`, if the case contains one.
4. A reader that cannot reproduce every `bits` entry exactly has failed,
   regardless of how close `value` looks.

---

## 10. Validation report

`python -m cfdviz validate <case>` and the in-application validator MUST both
report, per case:

- manifest schema conformance
- every invariant in §3.1
- per-frame presence of every declared field file
- CVF header CRC and per-brick payload CRC results
- `uncompressedBytes` consistency (§4.4.4) for every brick
- declared vs. recomputed field statistics, with the discrepancy shown
- monotonic timeline
- mask coverage
- count of `NaN` and masked cells per field

A malformed or partially corrupted case MUST produce a specific, actionable
error message identifying the offending file and byte offset. It MUST NOT crash,
and MUST NOT report success.
