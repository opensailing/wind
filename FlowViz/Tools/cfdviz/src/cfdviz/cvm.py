"""CVM — CFDViz boundary / structure triangle mesh.

See ``Docs/CFDVIZ_FORMAT.md`` section 5, which is normative. A ``.cvm`` file is
a fixed 96-byte header followed by up to five flat arrays. It exists so the
Unreal side can draw the wind-tunnel walls and the obstacle geometry without
inferring surfaces from the voxel grid, and so patch identity (inlet / outlet /
sideWalls / cylinderWall) survives the trip from the solver.

Serialization is **manual, field by field** via :mod:`struct`. Nothing here
``memcpy``s a native struct, because C++ struct padding is compiler-dependent
and the Unreal reader must agree with these bytes exactly.

Layout written by :func:`write_cvm` (offsets are recorded in the header, so a
reader never assumes this order — but keeping it deterministic makes the two
implementations diffable byte-for-byte)::

    [0, 96)   header
    positions   float32[3] or float64[3] per vertex   (always present)
    normals     float32[3] per vertex                 (flag bit 0)
    indices     uint32[3]  per triangle               (always present)
    patchIds    uint32     per TRIANGLE               (flag bit 1)
    nodeIds     uint64     per VERTEX                 (flag bit 2)

Each array starts on an 8-byte boundary; the gap bytes are zero. Alignment is
not required by the format — readers must use the header offsets — but it lets
a C++ reader alias the buffer directly instead of copying.

Triangle winding is **counter-clockwise viewed from outside** the solid. The
mesh helpers in this module (:func:`make_box_mesh`, :func:`make_cylinder_mesh`)
assert that property from the geometry rather than trusting hand-written vertex
orders, since a flipped face is invisible in a wireframe and only shows up as a
black hole in the shaded Unreal view.
"""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Final, Mapping, Sequence

import numpy as np

from .crc32c import crc32c, self_check

__all__ = [
    "CVM_MAGIC",
    "CVM_HEADER_BYTES",
    "CVM_ENDIAN_MARKER",
    "CVM_MAJOR_VERSION",
    "CVM_MINOR_VERSION",
    "CVM_FLAG_NORMALS",
    "CVM_FLAG_PATCH_IDS",
    "CVM_FLAG_NODE_IDS",
    "CVM_FLAG_FLOAT64_POSITIONS",
    "CVM_KNOWN_FLAGS",
    "CVMError",
    "CVMFormatError",
    "CVMHeader",
    "CVMData",
    "write_cvm",
    "read_cvm",
    "make_box_mesh",
    "make_cylinder_mesh",
]

# A broken CRC silently produces files no correct reader will accept, so fail at
# import time rather than at the end of a long export.
self_check()

CVM_MAGIC: Final = b"CFDMESH1"
CVM_HEADER_BYTES: Final = 96
CVM_ENDIAN_MARKER: Final = 0x01020304
CVM_MAJOR_VERSION: Final = 1
CVM_MINOR_VERSION: Final = 0

CVM_FLAG_NORMALS: Final = 1 << 0
CVM_FLAG_PATCH_IDS: Final = 1 << 1
CVM_FLAG_NODE_IDS: Final = 1 << 2
CVM_FLAG_FLOAT64_POSITIONS: Final = 1 << 3

#: Every flag bit this version understands. A file setting anything else is
#: rejected: an unknown bit may change how an array is laid out, and reading it
#: as if the bit were clear would silently produce wrong geometry.
CVM_KNOWN_FLAGS: Final = (
    CVM_FLAG_NORMALS
    | CVM_FLAG_PATCH_IDS
    | CVM_FLAG_NODE_IDS
    | CVM_FLAG_FLOAT64_POSITIONS
)

#: ``magic, major, minor, endian, flags, headerBytes,``
#: ``vertexCount, triangleCount, positionsOffset, normalsOffset,``
#: ``indicesOffset, patchIdsOffset, nodeIdsOffset, headerCrc32c, reserved``
_HEADER_STRUCT: Final = struct.Struct("<8sHHIII7QI12s")
assert _HEADER_STRUCT.size == CVM_HEADER_BYTES

#: Byte range of ``headerCrc32c``, zeroed while the CRC is computed (spec 5.1).
_CRC_FIELD_SPAN: Final = (80, 84)

_POSITION_DTYPES: Final = {False: "<f4", True: "<f8"}
_INDEX_DTYPE: Final = "<u4"
_NORMAL_DTYPE: Final = "<f4"
_PATCH_ID_DTYPE: Final = "<u4"
_NODE_ID_DTYPE: Final = "<u8"

_ALIGNMENT: Final = 8


class CVMError(Exception):
    """Base class for every error raised by this module."""


class CVMFormatError(CVMError):
    """A ``.cvm`` file is malformed, truncated, or internally inconsistent.

    Carries the offending file and, when the problem is localised, the byte
    offset — the spec (section 10) requires that a corrupt case name both.

    Attributes:
        path: File the problem was found in, if known.
        offset: Byte offset the problem was found at, if localised.
    """

    def __init__(self, message: str, *, path: Path | str | None = None,
                 offset: int | None = None) -> None:
        parts: list[str] = []
        if path is not None:
            parts.append(str(path))
        if offset is not None:
            parts.append(f"byte offset {offset}")
        prefix = ": ".join(parts)
        super().__init__(f"{prefix}: {message}" if prefix else message)
        self.path = Path(path) if path is not None else None
        self.offset = offset


def _align_up(value: int, alignment: int = _ALIGNMENT) -> int:
    """Round ``value`` up to the next multiple of ``alignment``."""
    remainder = value % alignment
    return value if remainder == 0 else value + (alignment - remainder)


@dataclass(slots=True)
class CVMHeader:
    """The fixed 96-byte CVM header (spec 5.1).

    Offsets belonging to an array whose presence bit is clear are 0 on write and
    ignored on read.
    """

    vertex_count: int = 0
    triangle_count: int = 0
    flags: int = 0
    positions_offset: int = 0
    normals_offset: int = 0
    indices_offset: int = 0
    patch_ids_offset: int = 0
    node_ids_offset: int = 0
    major_version: int = CVM_MAJOR_VERSION
    minor_version: int = CVM_MINOR_VERSION
    header_crc32c: int = 0

    # -- flag conveniences ------------------------------------------------
    @property
    def has_normals(self) -> bool:
        """Whether a per-vertex normal array is stored."""
        return bool(self.flags & CVM_FLAG_NORMALS)

    @property
    def has_patch_ids(self) -> bool:
        """Whether a per-triangle patch ID array is stored."""
        return bool(self.flags & CVM_FLAG_PATCH_IDS)

    @property
    def has_node_ids(self) -> bool:
        """Whether a per-vertex solver node ID array is stored."""
        return bool(self.flags & CVM_FLAG_NODE_IDS)

    @property
    def positions_are_float64(self) -> bool:
        """Whether positions are stored as float64 rather than float32."""
        return bool(self.flags & CVM_FLAG_FLOAT64_POSITIONS)

    @property
    def position_dtype(self) -> str:
        """Explicit little-endian numpy dtype string for the position array."""
        return _POSITION_DTYPES[self.positions_are_float64]

    # -- serialization ----------------------------------------------------
    def pack(self) -> bytes:
        """Serialize to exactly 96 bytes, computing and storing ``headerCrc32c``.

        The CRC covers ``[0, 96)`` with ``[80, 84)`` zeroed, so it is computed
        against a first pass in which the field is still zero, then patched in.

        Returns:
            The 96 header bytes, little-endian.
        """
        blank = _HEADER_STRUCT.pack(
            CVM_MAGIC,
            self.major_version,
            self.minor_version,
            CVM_ENDIAN_MARKER,
            self.flags,
            CVM_HEADER_BYTES,
            self.vertex_count,
            self.triangle_count,
            self.positions_offset,
            self.normals_offset,
            self.indices_offset,
            self.patch_ids_offset,
            self.node_ids_offset,
            0,  # headerCrc32c, zero while checksumming
            b"\x00" * 12,
        )
        self.header_crc32c = crc32c(blank)
        lo, hi = _CRC_FIELD_SPAN
        return blank[:lo] + struct.pack("<I", self.header_crc32c) + blank[hi:]

    @classmethod
    def unpack(cls, data: bytes, *, path: Path | str | None = None,
               verify_crc: bool = True) -> CVMHeader:
        """Parse and validate 96 header bytes.

        Args:
            data: At least 96 bytes; extra trailing bytes are ignored.
            path: File the bytes came from, for error messages.
            verify_crc: Check ``headerCrc32c``. Only a validator that wants to
                report the corruption instead of raising should pass ``False``.

        Returns:
            The parsed header.

        Raises:
            CVMFormatError: On short input, bad magic, foreign byte order,
                unsupported major version, wrong ``headerBytes``, unknown flag
                bits, or a header CRC mismatch.
        """
        if len(data) < CVM_HEADER_BYTES:
            raise CVMFormatError(
                f"file is {len(data)} bytes, shorter than the {CVM_HEADER_BYTES}-byte "
                "CVM header",
                path=path,
                offset=0,
            )
        blob = bytes(data[:CVM_HEADER_BYTES])
        (
            magic,
            major,
            minor,
            endian,
            flags,
            header_bytes,
            vertex_count,
            triangle_count,
            positions_offset,
            normals_offset,
            indices_offset,
            patch_ids_offset,
            node_ids_offset,
            stored_crc,
            reserved,
        ) = _HEADER_STRUCT.unpack(blob)

        if magic != CVM_MAGIC:
            raise CVMFormatError(
                f"bad magic {magic!r}; expected {CVM_MAGIC!r}. This is not a CVM mesh file.",
                path=path,
                offset=0,
            )
        if endian != CVM_ENDIAN_MARKER:
            # Spec 1.1/5.1: reject foreign byte order, never byte-swap it.
            raise CVMFormatError(
                f"byte order not supported: endianMarker is 0x{endian:08X}, expected "
                f"0x{CVM_ENDIAN_MARKER:08X}. CFDViz is little-endian only.",
                path=path,
                offset=12,
            )
        if major != CVM_MAJOR_VERSION:
            raise CVMFormatError(
                f"unsupported major version {major}; this reader implements CVM "
                f"{CVM_MAJOR_VERSION}.x",
                path=path,
                offset=8,
            )
        if header_bytes != CVM_HEADER_BYTES:
            raise CVMFormatError(
                f"headerBytes is {header_bytes}, expected {CVM_HEADER_BYTES}",
                path=path,
                offset=20,
            )
        unknown = flags & ~CVM_KNOWN_FLAGS
        if unknown:
            raise CVMFormatError(
                f"unknown flag bits 0x{unknown:08X} set; this file uses a CVM feature "
                f"this reader does not implement (flags=0x{flags:08X})",
                path=path,
                offset=16,
            )
        if reserved != b"\x00" * 12:
            raise CVMFormatError(
                "reserved header bytes [84, 96) are not zero", path=path, offset=84
            )

        header = cls(
            vertex_count=vertex_count,
            triangle_count=triangle_count,
            flags=flags,
            positions_offset=positions_offset,
            normals_offset=normals_offset,
            indices_offset=indices_offset,
            patch_ids_offset=patch_ids_offset,
            node_ids_offset=node_ids_offset,
            major_version=major,
            minor_version=minor,
            header_crc32c=stored_crc,
        )

        if verify_crc:
            lo, hi = _CRC_FIELD_SPAN
            computed = crc32c(blob[:lo] + b"\x00" * (hi - lo) + blob[hi:])
            if computed != stored_crc:
                raise CVMFormatError(
                    f"header CRC-32C mismatch: stored 0x{stored_crc:08X}, computed "
                    f"0x{computed:08X}. The header is corrupt.",
                    path=path,
                    offset=lo,
                )

        # Offsets for absent arrays must be 0 (spec 5.2) — a stale non-zero
        # offset behind a clear bit means the writer was buggy, and trusting it
        # later would read whatever happens to be at that offset.
        for name, offset, present, off_at in (
            ("normalsOffset", normals_offset, header.has_normals, 48),
            ("patchIdsOffset", patch_ids_offset, header.has_patch_ids, 64),
            ("nodeIdsOffset", node_ids_offset, header.has_node_ids, 72),
        ):
            if not present and offset != 0:
                raise CVMFormatError(
                    f"{name} is {offset} but its presence flag is clear; the spec "
                    "requires 0 for absent arrays",
                    path=path,
                    offset=off_at,
                )
        return header


@dataclass(slots=True)
class CVMData:
    """A decoded CVM mesh.

    Attributes:
        positions: ``(vertexCount, 3)`` float32 or float64, canonical coordinates.
        indices: ``(triangleCount, 3)`` uint32, CCW viewed from outside.
        normals: ``(vertexCount, 3)`` float32, or ``None``.
        patch_ids: ``(triangleCount,)`` uint32 — one per TRIANGLE — or ``None``.
        node_ids: ``(vertexCount,)`` uint64 — one per VERTEX — or ``None``.
        header: The header exactly as it was stored on disk.
        path: Where the mesh was read from, for diagnostics.
    """

    positions: np.ndarray
    indices: np.ndarray
    normals: np.ndarray | None = None
    patch_ids: np.ndarray | None = None
    node_ids: np.ndarray | None = None
    header: CVMHeader = field(default_factory=CVMHeader)
    path: Path | None = None

    @property
    def vertex_count(self) -> int:
        """Number of vertices."""
        return int(self.positions.shape[0])

    @property
    def triangle_count(self) -> int:
        """Number of triangles."""
        return int(self.indices.shape[0])


# ---------------------------------------------------------------------------
# Input coercion
# ---------------------------------------------------------------------------

def _as_vec3_array(values: object, dtype: str, name: str) -> np.ndarray:
    """Coerce ``values`` to a contiguous ``(n, 3)`` array of ``dtype``.

    ``astype(..., copy=False)`` is deliberate: when the caller already supplies
    the exact little-endian dtype the bytes pass through untouched, which is how
    NaN payloads survive bit-exactly.
    """
    array = np.asarray(values)
    if array.ndim != 2 or array.shape[1] != 3:
        raise CVMError(f"{name} must have shape (n, 3); got {array.shape}")
    if not np.issubdtype(array.dtype, np.floating) and not np.issubdtype(
        array.dtype, np.integer
    ):
        raise CVMError(f"{name} must be numeric; got dtype {array.dtype}")
    return np.ascontiguousarray(array.astype(dtype, copy=False))


def _as_index_array(values: object, vertex_count: int) -> np.ndarray:
    """Coerce triangle indices to ``(n, 3)`` uint32, validating every index.

    Range checks happen on the *original* integers, before the cast: casting a
    negative int64 to uint32 wraps it into a huge in-file value that then looks
    plausible, so checking afterwards would be checking the wrong number.
    """
    array = np.asarray(values)
    if array.ndim != 2 or array.shape[1] != 3:
        raise CVMError(f"indices must have shape (n, 3); got {array.shape}")
    if not np.issubdtype(array.dtype, np.integer):
        raise CVMError(f"indices must be an integer array; got dtype {array.dtype}")
    if array.size:
        lo = int(array.min())
        hi = int(array.max())
        if lo < 0:
            raise CVMError(f"indices contain a negative value ({lo})")
        if hi >= vertex_count:
            bad = int(np.argmax(array.reshape(-1) >= vertex_count))
            raise CVMError(
                f"triangle index {hi} at flat position {bad} is >= vertexCount "
                f"{vertex_count}; every index must address an existing vertex"
            )
    return np.ascontiguousarray(array.astype(_INDEX_DTYPE, copy=False))


def _as_uint_array(values: object, dtype: str, count: int, name: str,
                   plural: str, singular: str) -> np.ndarray:
    """Coerce ``values`` to a 1-D unsigned array of exactly ``count`` elements.

    ``plural``/``singular`` name the entity the array is associated with, so the
    length error can say *which* association was violated. Mixing up the
    per-triangle ``patchIds`` and the per-vertex ``nodeIds`` is the easy mistake
    here, and a bare "length mismatch" would not point at it.
    """
    array = np.asarray(values)
    if array.ndim != 1:
        raise CVMError(f"{name} must be 1-D; got shape {array.shape}")
    if not np.issubdtype(array.dtype, np.integer):
        raise CVMError(f"{name} must be an integer array; got dtype {array.dtype}")
    if array.shape[0] != count:
        raise CVMError(
            f"{name} has {array.shape[0]} entries but there are {count} {plural}; "
            f"{name} is one value per {singular}"
        )
    if array.size:
        lo = int(array.min())
        hi = int(array.max())
        limit = int(np.iinfo(dtype).max)
        if lo < 0:
            raise CVMError(f"{name} contains a negative value ({lo})")
        if hi > limit:
            raise CVMError(f"{name} value {hi} exceeds the {dtype} maximum {limit}")
    return np.ascontiguousarray(array.astype(dtype, copy=False))


# ---------------------------------------------------------------------------
# Writing
# ---------------------------------------------------------------------------

def write_cvm(
    path: Path | str,
    *,
    positions: object,
    indices: object,
    normals: object | None = None,
    patch_ids: object | None = None,
    node_ids: object | None = None,
    use_float64: bool = False,
) -> CVMHeader:
    """Write a CVM triangle mesh.

    Args:
        path: Destination ``.cvm`` file. Parent directories are created.
        positions: ``(vertexCount, 3)`` XYZ in canonical coordinates (spec 2).
        indices: ``(triangleCount, 3)`` vertex indices, CCW viewed from outside.
            Every index must be ``< vertexCount``.
        normals: Optional ``(vertexCount, 3)`` normals, stored as float32.
        patch_ids: Optional ``(triangleCount,)`` uint32 — one per **triangle**.
        node_ids: Optional ``(vertexCount,)`` uint64 — one per **vertex**.
        use_float64: Store positions as float64 instead of float32 (flag bit 3).

    Returns:
        The :class:`CVMHeader` that was written, with final offsets filled in.

    Raises:
        CVMError: If an array has the wrong shape, wrong length for its
            association, or contains an out-of-range triangle index.
    """
    path = Path(path)
    position_dtype = _POSITION_DTYPES[bool(use_float64)]
    pos = _as_vec3_array(positions, position_dtype, "positions")
    vertex_count = int(pos.shape[0])

    idx = _as_index_array(indices, vertex_count)
    triangle_count = int(idx.shape[0])

    nrm: np.ndarray | None = None
    if normals is not None:
        nrm = _as_vec3_array(normals, _NORMAL_DTYPE, "normals")
        if nrm.shape[0] != vertex_count:
            raise CVMError(
                f"normals has {nrm.shape[0]} entries but there are {vertex_count} "
                "vertices; normals are one per vertex"
            )

    pid: np.ndarray | None = None
    if patch_ids is not None:
        pid = _as_uint_array(
            patch_ids, _PATCH_ID_DTYPE, triangle_count, "patch_ids",
            "triangles", "triangle",
        )

    nid: np.ndarray | None = None
    if node_ids is not None:
        nid = _as_uint_array(
            node_ids, _NODE_ID_DTYPE, vertex_count, "node_ids",
            "vertices", "vertex",
        )

    flags = 0
    if nrm is not None:
        flags |= CVM_FLAG_NORMALS
    if pid is not None:
        flags |= CVM_FLAG_PATCH_IDS
    if nid is not None:
        flags |= CVM_FLAG_NODE_IDS
    if use_float64:
        flags |= CVM_FLAG_FLOAT64_POSITIONS

    header = CVMHeader(
        vertex_count=vertex_count, triangle_count=triangle_count, flags=flags
    )

    # Lay the arrays out first so the header can carry final offsets, then emit
    # header and payload in one pass.
    chunks: list[bytes] = []
    cursor = CVM_HEADER_BYTES

    def place(array: np.ndarray) -> int:
        nonlocal cursor
        padding = _align_up(cursor) - cursor
        if padding:
            chunks.append(b"\x00" * padding)
            cursor += padding
        offset = cursor
        blob = array.tobytes()
        chunks.append(blob)
        cursor += len(blob)
        return offset

    header.positions_offset = place(pos)
    if nrm is not None:
        header.normals_offset = place(nrm)
    header.indices_offset = place(idx)
    if pid is not None:
        header.patch_ids_offset = place(pid)
    if nid is not None:
        header.node_ids_offset = place(nid)

    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(header.pack())
        for chunk in chunks:
            handle.write(chunk)
    return header


# ---------------------------------------------------------------------------
# Reading
# ---------------------------------------------------------------------------

def _read_array(
    blob: bytes,
    *,
    path: Path,
    name: str,
    offset: int,
    count: int,
    dtype: str,
    per_element: int,
    header_offset: int,
) -> np.ndarray:
    """Bounds-check a span against the real file size, then decode it.

    Spec 1.5: every offset and length is validated against the file on disk
    *before* anything is allocated, so a hostile size field cannot make the
    reader reserve gigabytes.
    """
    itemsize = np.dtype(dtype).itemsize
    nbytes = count * per_element * itemsize
    if offset < CVM_HEADER_BYTES and nbytes > 0:
        raise CVMFormatError(
            f"{name} offset {offset} overlaps the {CVM_HEADER_BYTES}-byte header",
            path=path,
            offset=header_offset,
        )
    if offset > len(blob) or nbytes > len(blob) - offset:
        raise CVMFormatError(
            f"{name} needs {nbytes} bytes at offset {offset}, but the file is only "
            f"{len(blob)} bytes; the file is truncated or the header is corrupt",
            path=path,
            offset=header_offset,
        )
    array = np.frombuffer(blob, dtype=dtype, count=count * per_element, offset=offset)
    array = array.copy()  # detach from the read-only file buffer
    return array.reshape(count, per_element) if per_element > 1 else array


def read_cvm(path: Path | str, *, verify_crc: bool = True) -> CVMData:
    """Read a CVM triangle mesh, validating it against the file on disk.

    Args:
        path: The ``.cvm`` file.
        verify_crc: Verify the header CRC-32C. Leave this on outside validators.

    Returns:
        The decoded :class:`CVMData`. Arrays whose presence bit is clear are
        ``None``; their offsets are ignored entirely.

    Raises:
        CVMFormatError: If the file is missing, truncated, has a bad magic,
            foreign byte order, an unsupported version, a CRC mismatch, or a
            triangle index that does not address an existing vertex. The message
            always names the file and, where meaningful, the byte offset.
    """
    path = Path(path)
    try:
        blob = path.read_bytes()
    except OSError as exc:
        raise CVMFormatError(f"cannot read mesh: {exc}", path=path) from exc

    header = CVMHeader.unpack(blob, path=path, verify_crc=verify_crc)
    vertex_count = header.vertex_count
    triangle_count = header.triangle_count

    positions = _read_array(
        blob,
        path=path,
        name="positions",
        offset=header.positions_offset,
        count=vertex_count,
        dtype=header.position_dtype,
        per_element=3,
        header_offset=40,
    )
    indices = _read_array(
        blob,
        path=path,
        name="indices",
        offset=header.indices_offset,
        count=triangle_count,
        dtype=_INDEX_DTYPE,
        per_element=3,
        header_offset=56,
    )

    normals = None
    if header.has_normals:
        normals = _read_array(
            blob,
            path=path,
            name="normals",
            offset=header.normals_offset,
            count=vertex_count,
            dtype=_NORMAL_DTYPE,
            per_element=3,
            header_offset=48,
        )
    patch_ids = None
    if header.has_patch_ids:
        patch_ids = _read_array(
            blob,
            path=path,
            name="patchIds",
            offset=header.patch_ids_offset,
            count=triangle_count,
            dtype=_PATCH_ID_DTYPE,
            per_element=1,
            header_offset=64,
        )
    node_ids = None
    if header.has_node_ids:
        node_ids = _read_array(
            blob,
            path=path,
            name="nodeIds",
            offset=header.node_ids_offset,
            count=vertex_count,
            dtype=_NODE_ID_DTYPE,
            per_element=1,
            header_offset=72,
        )

    # Spec 5.3: every index must be < vertexCount. Checked after decoding but
    # before the caller can index with it, which is where it would crash.
    if indices.size:
        worst = int(indices.max())
        if worst >= vertex_count:
            flat = indices.reshape(-1)
            first = int(np.argmax(flat >= vertex_count))
            bad_offset = header.indices_offset + first * np.dtype(_INDEX_DTYPE).itemsize
            raise CVMFormatError(
                f"triangle index {int(flat[first])} at triangle {first // 3} is >= "
                f"vertexCount {vertex_count}; the index buffer is corrupt",
                path=path,
                offset=bad_offset,
            )

    return CVMData(
        positions=positions,
        indices=indices,
        normals=normals,
        patch_ids=patch_ids,
        node_ids=node_ids,
        header=header,
        path=path,
    )


# ---------------------------------------------------------------------------
# Mesh construction helpers
# ---------------------------------------------------------------------------

#: Canonical face keys accepted by :func:`make_box_mesh`.
_BOX_FACES: Final = ("-x", "+x", "-y", "+y", "-z", "+z")

#: Aliases -> the faces they set. Group keys are applied before single-face keys
#: so ``{"sideWalls": 3, "-y": 9}`` means the same thing regardless of dict order.
_BOX_GROUP_ALIASES: Final[dict[str, tuple[str, ...]]] = {
    "all": _BOX_FACES,
    "default": _BOX_FACES,
    "sidewalls": ("-y", "+y", "-z", "+z"),
    "walls": ("-y", "+y", "-z", "+z"),
}
_BOX_SINGLE_ALIASES: Final[dict[str, str]] = {
    "-x": "-x", "+x": "+x", "-y": "-y", "+y": "+y", "-z": "-z", "+z": "+z",
    "xmin": "-x", "xmax": "+x", "ymin": "-y", "ymax": "+y", "zmin": "-z", "zmax": "+z",
    "inlet": "-x", "outlet": "+x",
    "floor": "-z", "ground": "-z", "ceiling": "+z", "roof": "+z", "sky": "+z",
}


def _resolve_box_patch_ids(patch_id_map: Mapping[str, int] | None) -> dict[str, int]:
    """Expand a caller's face/patch mapping into one patch ID per box face."""
    resolved = {face: 0 for face in _BOX_FACES}
    if not patch_id_map:
        return resolved

    groups: list[tuple[tuple[str, ...], int]] = []
    singles: list[tuple[str, int]] = []
    for raw_key, value in patch_id_map.items():
        key = str(raw_key).strip().lower()
        if key in _BOX_GROUP_ALIASES:
            groups.append((_BOX_GROUP_ALIASES[key], int(value)))
        elif key in _BOX_SINGLE_ALIASES:
            singles.append((_BOX_SINGLE_ALIASES[key], int(value)))
        else:
            accepted = sorted(set(_BOX_GROUP_ALIASES) | set(_BOX_SINGLE_ALIASES))
            raise CVMError(
                f"unknown box face key {raw_key!r} in patch_id_map. Accepted keys: "
                f"{accepted}"
            )
    for faces, value in groups:
        for face in faces:
            resolved[face] = value
    for face, value in singles:
        resolved[face] = value
    return resolved


def _emit_quad(
    positions: list[tuple[float, float, float]],
    indices: list[tuple[int, int, int]],
    normals: list[tuple[float, float, float]],
    corners: Sequence[Sequence[float]],
    expected_normal: Sequence[float],
) -> None:
    """Append a planar quad as two CCW triangles with flat per-vertex normals.

    The normal is derived from the emitted winding and then checked against
    ``expected_normal``. Deriving rather than hard-coding is the point: it makes
    it impossible for the vertex order and the shading normal to disagree, and
    the check catches a face that was typed in clockwise.
    """
    base = len(positions)
    a, b, c, d = (np.asarray(corner, dtype=np.float64) for corner in corners)
    normal = np.cross(b - a, c - a)
    length = float(np.linalg.norm(normal))
    if length == 0.0:
        raise CVMError("degenerate quad: the three leading corners are collinear")
    normal = normal / length
    if float(np.dot(normal, np.asarray(expected_normal, dtype=np.float64))) <= 0.0:
        raise CVMError(
            f"quad winding is inside-out: derived normal {normal.tolist()} opposes the "
            f"expected outward normal {list(expected_normal)}"
        )
    for corner in (a, b, c, d):
        positions.append((float(corner[0]), float(corner[1]), float(corner[2])))
        normals.append((float(normal[0]), float(normal[1]), float(normal[2])))
    # CCW viewed from outside, matching the corner order.
    indices.append((base + 0, base + 1, base + 2))
    indices.append((base + 0, base + 2, base + 3))


def make_box_mesh(
    min_corner: Sequence[float],
    max_corner: Sequence[float],
    patch_id_map: Mapping[str, int] | None = None,
    *,
    inward: bool = False,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Build an axis-aligned box as 24 vertices / 12 triangles.

    Each face gets its own four vertices so the normals stay flat — a shared
    8-vertex box would average three face normals per corner and shade the
    domain walls like a rounded balloon.

    Args:
        min_corner: ``(x0, y0, z0)``, the lower corner in canonical coordinates.
        max_corner: ``(x1, y1, z1)``, strictly greater than ``min_corner``.
        patch_id_map: Patch ID per face. Accepts the semantic keys ``"inlet"``
            (the -X face), ``"outlet"`` (+X) and ``"sideWalls"`` (the four Y/Z
            faces), as well as literal face keys ``"-x"``, ``"+x"``, ``"-y"``,
            ``"+y"``, ``"-z"``, ``"+z"``. Group keys are applied first, so a
            specific face always overrides ``"sideWalls"``. Faces not named get
            patch ID 0.
        inward: Flip winding and normals to face the box interior. Use this when
            the box is a *domain boundary* being viewed from inside the fluid
            rather than a solid obstacle.

    Returns:
        ``(positions, indices, normals, patch_ids)`` — float64 ``(24, 3)``,
        uint32 ``(12, 3)``, float64 ``(24, 3)``, uint32 ``(12,)``.

    Raises:
        CVMError: If the box is degenerate or ``patch_id_map`` names an
            unknown face.
    """
    lo = np.asarray(min_corner, dtype=np.float64)
    hi = np.asarray(max_corner, dtype=np.float64)
    if lo.shape != (3,) or hi.shape != (3,):
        raise CVMError("min_corner and max_corner must each have 3 components")
    if not np.all(hi > lo):
        raise CVMError(
            f"box is degenerate or inverted: min_corner={lo.tolist()} is not strictly "
            f"less than max_corner={hi.tolist()} on every axis"
        )

    x0, y0, z0 = (float(v) for v in lo)
    x1, y1, z1 = (float(v) for v in hi)

    # Corner order per face is CCW viewed from OUTSIDE; _emit_quad verifies it.
    faces: tuple[tuple[str, tuple[float, float, float], tuple[tuple[float, float, float], ...]], ...] = (
        ("-x", (-1.0, 0.0, 0.0), ((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0))),
        ("+x", (1.0, 0.0, 0.0), ((x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1))),
        ("-y", (0.0, -1.0, 0.0), ((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1))),
        ("+y", (0.0, 1.0, 0.0), ((x0, y1, z0), (x0, y1, z1), (x1, y1, z1), (x1, y1, z0))),
        ("-z", (0.0, 0.0, -1.0), ((x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (x1, y0, z0))),
        ("+z", (0.0, 0.0, 1.0), ((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1))),
    )

    patch_by_face = _resolve_box_patch_ids(patch_id_map)
    positions: list[tuple[float, float, float]] = []
    indices: list[tuple[int, int, int]] = []
    normals: list[tuple[float, float, float]] = []
    patch_ids: list[int] = []

    for face, outward, corners in faces:
        _emit_quad(positions, indices, normals, corners, outward)
        patch_ids.extend([patch_by_face[face]] * 2)  # two triangles per face

    pos = np.asarray(positions, dtype=np.float64)
    idx = np.asarray(indices, dtype=np.uint32)
    nrm = np.asarray(normals, dtype=np.float64)
    pid = np.asarray(patch_ids, dtype=np.uint32)
    if inward:
        idx = idx[:, ::-1].copy()
        nrm = -nrm
    return pos, idx, nrm, pid


_AXIS_INDEX: Final[dict[str, int]] = {"x": 0, "y": 1, "z": 2}
#: Right-handed basis per axis: ``u x v == w``, so increasing theta winds CCW
#: about the axis and the derived normals come out pointing outward.
_AXIS_BASIS: Final[dict[int, tuple[int, int]]] = {0: (1, 2), 1: (2, 0), 2: (0, 1)}


def _resolve_axis(axis: str | int) -> int:
    """Map ``'x'``/``'y'``/``'z'`` or ``0``/``1``/``2`` to an axis index."""
    if isinstance(axis, str):
        key = axis.strip().lower()
        if key in _AXIS_INDEX:
            return _AXIS_INDEX[key]
        raise CVMError(f"unknown axis {axis!r}; expected 'x', 'y', 'z', or 0, 1, 2")
    index = int(axis)
    if index not in (0, 1, 2):
        raise CVMError(f"axis index {index} out of range; expected 0, 1, or 2")
    return index


def make_cylinder_mesh(
    center: Sequence[float],
    radius: float,
    height: float,
    axis: str | int = "z",
    segments: int = 32,
    patch_id: int = 0,
    *,
    capped: bool = True,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Build a closed circular cylinder as a triangle mesh.

    The side wall carries smooth radial normals so the classic flow-past-a-
    cylinder obstacle shades as a curve rather than a prism. Caps use their own
    vertices with flat axial normals, because sharing them with the wall would
    smear the rim into a bevel.

    Args:
        center: Midpoint of the cylinder axis in canonical coordinates.
        radius: Cylinder radius; must be > 0.
        height: Total extent along ``axis``; must be > 0.
        axis: Axis the cylinder extends along — ``'x'``, ``'y'``, ``'z'`` or
            ``0``, ``1``, ``2``. Defaults to ``'z'`` (upright in a Z-up world).
        segments: Number of circumferential subdivisions; must be >= 3.
        patch_id: Patch ID applied to every triangle (e.g. ``cylinderWall``).
        capped: Emit end caps. Pass ``False`` when the cylinder spans the whole
            domain and its ends are buried in the boundary walls.

    Returns:
        ``(positions, indices, normals, patch_ids)`` — float64 ``(n, 3)``,
        uint32 ``(m, 3)``, float64 ``(n, 3)``, uint32 ``(m,)``. Winding is CCW
        viewed from outside.

    Raises:
        CVMError: If ``radius``/``height`` are not positive, ``segments`` < 3,
            or ``axis`` is not an axis.
    """
    axis_index = _resolve_axis(axis)
    centre = np.asarray(center, dtype=np.float64)
    if centre.shape != (3,):
        raise CVMError("center must have 3 components")
    radius = float(radius)
    height = float(height)
    segments = int(segments)
    if radius <= 0.0:
        raise CVMError(f"radius must be > 0; got {radius}")
    if height <= 0.0:
        raise CVMError(f"height must be > 0; got {height}")
    if segments < 3:
        raise CVMError(f"segments must be >= 3 to enclose a volume; got {segments}")

    u_index, v_index = _AXIS_BASIS[axis_index]
    u = np.zeros(3, dtype=np.float64)
    v = np.zeros(3, dtype=np.float64)
    w = np.zeros(3, dtype=np.float64)
    u[u_index] = 1.0
    v[v_index] = 1.0
    w[axis_index] = 1.0
    half = 0.5 * height

    angles = [2.0 * math.pi * i / segments for i in range(segments)]
    radial = [math.cos(a) * u + math.sin(a) * v for a in angles]

    positions: list[np.ndarray] = []
    normals: list[np.ndarray] = []
    indices: list[tuple[int, int, int]] = []

    # --- side wall: one ring at each end, shared between adjacent quads -----
    bottom_ring = [centre + radius * r - half * w for r in radial]
    top_ring = [centre + radius * r + half * w for r in radial]
    bottom_base = len(positions)
    for r, p in zip(radial, bottom_ring):
        positions.append(p)
        normals.append(r)
    top_base = len(positions)
    for r, p in zip(radial, top_ring):
        positions.append(p)
        normals.append(r)
    for i in range(segments):
        j = (i + 1) % segments
        b_i, b_j = bottom_base + i, bottom_base + j
        t_i, t_j = top_base + i, top_base + j
        # Increasing theta with u x v == w makes this order CCW from outside.
        indices.append((b_i, b_j, t_j))
        indices.append((b_i, t_j, t_i))

    if capped:
        # +axis cap: fan around a dedicated centre vertex, CCW seen from +w.
        cap_centre = len(positions)
        positions.append(centre + half * w)
        normals.append(w.copy())
        rim_base = len(positions)
        for p in top_ring:
            positions.append(p)
            normals.append(w.copy())
        for i in range(segments):
            j = (i + 1) % segments
            indices.append((cap_centre, rim_base + i, rim_base + j))

        # -axis cap: reversed order, so CCW seen from -w.
        cap_centre = len(positions)
        positions.append(centre - half * w)
        normals.append(-w)
        rim_base = len(positions)
        for p in bottom_ring:
            positions.append(p)
            normals.append(-w)
        for i in range(segments):
            j = (i + 1) % segments
            indices.append((cap_centre, rim_base + j, rim_base + i))

    pos = np.asarray(positions, dtype=np.float64)
    nrm = np.asarray(normals, dtype=np.float64)
    idx = np.asarray(indices, dtype=np.uint32)
    pid = np.full(idx.shape[0], int(patch_id), dtype=np.uint32)
    return pos, idx, nrm, pid


def outward_winding_is_consistent(
    positions: np.ndarray, indices: np.ndarray, normals: np.ndarray
) -> bool:
    """Return whether every triangle's geometric normal agrees with its shading normal.

    A cheap sanity check for generated geometry: it catches inside-out faces,
    which are invisible in wireframe and show up in Unreal only as holes in a
    shaded surface. Degenerate (zero-area) triangles are ignored.
    """
    pos = np.asarray(positions, dtype=np.float64)
    idx = np.asarray(indices)
    nrm = np.asarray(normals, dtype=np.float64)
    if idx.size == 0:
        return True
    a = pos[idx[:, 0]]
    b = pos[idx[:, 1]]
    c = pos[idx[:, 2]]
    face = np.cross(b - a, c - a)
    shading = nrm[idx[:, 0]] + nrm[idx[:, 1]] + nrm[idx[:, 2]]
    dots = np.einsum("ij,ij->i", face, shading)
    areas = np.linalg.norm(face, axis=1)
    meaningful = areas > 0.0
    return bool(np.all(dots[meaningful] > 0.0))
