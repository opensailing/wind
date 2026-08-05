"""CVF — CFDViz bricked volume field.

See ``Docs/CFDVIZ_FORMAT.md`` section 4. One ``.cvf`` file holds **one field at
one frame** and is independently readable: the brick directory permits random
brick access, region-of-interest loading, and empty-space skipping without
decoding the whole volume.

Serialization is manual, field by field. Nothing here ``memcpy``s a struct,
because C++ padding is compiler-dependent and the Unreal reader has to agree
byte for byte.

Header — fixed 128 bytes, little-endian (spec 4.1)::

    Offset Size Type        Name
    0      8    char[8]     magic = "CFDVOL1\\0"
    8      4    uint32      headerBytes = 128
    12     2    uint16      majorVersion = 1
    14     2    uint16      minorVersion = 0
    16     4    uint32      endianMarker = 0x01020304
    20     4    uint32      flags
    24     4    uint32      frameIndex
    28     4    uint32      fieldNumericId
    32     8    float64     simulationTime
    40     4    uint32      dimensionX        grid CELL counts, per the manifest
    44     4    uint32      dimensionY
    48     4    uint32      dimensionZ
    52     2    uint16      brickSizeX
    54     2    uint16      brickSizeY
    56     2    uint16      brickSizeZ
    58     1    uint8       componentCount
    59     1    uint8       dataType
    60     1    uint8       association
    61     1    uint8       codec
    62     2    uint16      reserved = 0
    64     8    uint64      brickCount
    72     8    uint64      directoryOffset
    80     8    uint64      payloadOffset
    88     16   float32[4]  backgroundValue
    104    4    uint32      headerCrc32c
    108    20   byte[20]    reserved = 0

Brick directory — 80 bytes per entry (spec 4.3)::

    Offset Size Type        Name
    0      4    uint32      brickIndexX
    4      4    uint32      brickIndexY
    8      4    uint32      brickIndexZ
    12     2    uint16      validSizeX
    14     2    uint16      validSizeY
    16     2    uint16      validSizeZ
    18     2    uint16      flags
    20     8    uint64      absolutePayloadOffset
    28     4    uint32      compressedBytes
    32     4    uint32      uncompressedBytes
    36     16   float32[4]  componentMin
    52     16   float32[4]  componentMax
    68     4    uint32      payloadCrc32c
    72     8    byte[8]     reserved = 0

Payload rules (spec 4.4), all enforced here:

1. Within a brick **X varies fastest**, then Y, then Z.
2. Components are **interleaved per voxel**.
3. Edge bricks are **not padded**; a partial brick stores exactly
   ``validSizeX * validSizeY * validSizeZ`` voxels.
4. ``uncompressedBytes == validSizeX * validSizeY * validSizeZ *
   componentCount * sizeof(dataType)``, verified **before allocating**.
5. A brick whose every voxel equals ``backgroundValue`` MAY be omitted; an
   absent brick evaluates to ``backgroundValue`` everywhere.
6. ``NaN`` is legal and is preserved bit-exactly.
7. Statistics ignore ``NaN`` and masked cells. An entirely invalid brick stores
   ``componentMin = +inf`` and ``componentMax = -inf``.

The half-cell offset of spec 3.2 is the format's most common source of error, so
it is explicit here: ``dimensionX/Y/Z`` are always the **cell** counts declared
in the manifest, and the number of stored values per axis is derived from
``association`` — ``n`` for ``cell``, ``n + 1`` for ``point``. The derived shape
is :attr:`CVFHeader.value_extent`.
"""

from __future__ import annotations

import os
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Final, Iterator, Sequence

import numpy as np

from .codecs import (
    CODEC_NAMES,
    CODEC_NONE,
    CODEC_ZSTD,
    DEFAULT_CODEC,
    ZSTD_REJECTION_MESSAGE,
    CodecError,
    UnsupportedCodecError,
    compress,
    decompress,
)
from .crc32c import crc32c, self_check

__all__ = [
    "CVF_MAGIC",
    "CVF_HEADER_BYTES",
    "CVF_DIRECTORY_ENTRY_BYTES",
    "CVF_ENDIAN_MARKER",
    "CVF_MAJOR_VERSION",
    "CVF_MINOR_VERSION",
    "CVF_DTYPE_FLOAT16",
    "CVF_DTYPE_FLOAT32",
    "CVF_DTYPE_UINT8",
    "CVF_DTYPE_NAMES",
    "CVF_DTYPE_NUMPY",
    "CVF_ASSOC_CELL",
    "CVF_ASSOC_POINT",
    "CVF_ASSOC_NAMES",
    "CVF_FLAG_SPARSE",
    "CVF_KNOWN_FLAGS",
    "MAX_COMPONENT_COUNT",
    "MAX_BRICK_SIZE",
    "DEFAULT_BRICK_SIZE",
    "CVFError",
    "CVFFormatError",
    "CVFBrickEntry",
    "CVFHeader",
    "CVFData",
    "CVFReader",
    "write_cvf",
    "read_cvf",
]

# A wrong CRC produces files that no correct reader accepts. Fail at import
# rather than at the end of a long export.
self_check()

CVF_MAGIC: Final = b"CFDVOL1\x00"
CVF_HEADER_BYTES: Final = 128
CVF_DIRECTORY_ENTRY_BYTES: Final = 80
CVF_ENDIAN_MARKER: Final = 0x01020304
CVF_MAJOR_VERSION: Final = 1
CVF_MINOR_VERSION: Final = 0

#: Storage type IDs (spec 4.2). CVA reuses 1 and 2 so a shared C++ enum needs no
#: per-container translation; CVA's float64 (4) is deliberately *not* accepted
#: here, because spec 3.3 forbids float64 field storage in 1.0.
CVF_DTYPE_FLOAT16: Final = 1
CVF_DTYPE_FLOAT32: Final = 2
CVF_DTYPE_UINT8: Final = 3

CVF_DTYPE_NAMES: Final[dict[int, str]] = {
    CVF_DTYPE_FLOAT16: "float16",
    CVF_DTYPE_FLOAT32: "float32",
    CVF_DTYPE_UINT8: "uint8",
}

#: Explicit little-endian numpy dtype strings. Never rely on native byte order.
CVF_DTYPE_NUMPY: Final[dict[int, str]] = {
    CVF_DTYPE_FLOAT16: "<f2",
    CVF_DTYPE_FLOAT32: "<f4",
    CVF_DTYPE_UINT8: "u1",
}

#: Association IDs (spec 4.2). The reserved associations of spec 3.2 —
#: ``mesh-vertex``, ``mesh-element``, ``integration-point``, ``face``,
#: ``particle`` — have no CVF ID and are rejected.
CVF_ASSOC_CELL: Final = 0
CVF_ASSOC_POINT: Final = 1

CVF_ASSOC_NAMES: Final[dict[int, str]] = {
    CVF_ASSOC_CELL: "cell",
    CVF_ASSOC_POINT: "point",
}

#: Header flag: bricks equal to ``backgroundValue`` may be absent from the
#: directory (spec 4.4.5). With this bit clear a reader may assume the directory
#: covers the whole volume, which lets it skip the background prefill.
CVF_FLAG_SPARSE: Final = 1 << 0
CVF_KNOWN_FLAGS: Final = CVF_FLAG_SPARSE

#: ``backgroundValue``, ``componentMin`` and ``componentMax`` are all
#: ``float32[4]``, which caps a CVF at four components.
MAX_COMPONENT_COUNT: Final = 4

#: ``brickSizeX/Y/Z`` are uint16.
MAX_BRICK_SIZE: Final = 0xFFFF

#: 32^3 voxels is a good default: one float32 scalar brick is 128 KiB, which
#: streams well and keeps the directory small for typical LBM grids.
DEFAULT_BRICK_SIZE: Final[tuple[int, int, int]] = (32, 32, 32)

#: ``magic, headerBytes, major, minor, endian, flags, frameIndex,``
#: ``fieldNumericId, simulationTime, dimX, dimY, dimZ, brickX, brickY, brickZ,``
#: ``componentCount, dataType, association, codec, reserved, brickCount,``
#: ``directoryOffset, payloadOffset, background[4], headerCrc32c, reserved[20]``
_HEADER_STRUCT: Final = struct.Struct("<8sIHHIIIIdIIIHHHBBBBHQQQ4fI20s")
assert _HEADER_STRUCT.size == CVF_HEADER_BYTES

#: ``brickIndexX/Y/Z, validSizeX/Y/Z, flags, absolutePayloadOffset,``
#: ``compressedBytes, uncompressedBytes, componentMin[4], componentMax[4],``
#: ``payloadCrc32c, reserved[8]``
_ENTRY_STRUCT: Final = struct.Struct("<IIIHHHHQII4f4fI8s")
assert _ENTRY_STRUCT.size == CVF_DIRECTORY_ENTRY_BYTES

#: Byte range of ``headerCrc32c``, zeroed while the header CRC is computed.
_CRC_FIELD_SPAN: Final = (104, 108)

_DTYPE_IDS_BY_NAME: Final[dict[str, int]] = {
    name: code for code, name in CVF_DTYPE_NAMES.items()
}
_ASSOC_IDS_BY_NAME: Final[dict[str, int]] = {
    "cell": CVF_ASSOC_CELL,
    "point": CVF_ASSOC_POINT,
}

#: Associations spec 3.2 reserves for a future version. Named explicitly so the
#: error tells the caller *why* the value is refused instead of "unknown".
_RESERVED_ASSOCIATIONS: Final[tuple[str, ...]] = (
    "mesh-vertex",
    "mesh-element",
    "integration-point",
    "face",
    "particle",
)


class CVFError(Exception):
    """Base class for every error raised by this module."""


class CVFFormatError(CVFError):
    """A ``.cvf`` file is malformed, truncated, or internally inconsistent.

    Carries the offending file and, where the problem is localised, the byte
    offset — spec section 10 requires a corrupt case to name both.

    Attributes:
        path: File the problem was found in, if known.
        offset: Byte offset the problem was found at, if localised.
        detail: The message *without* the path and offset prefix. A caller that
            knows the file by a better name — a case-relative path, say — can
            re-render the error without the absolute path appearing twice.
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
        self.detail = message


def _ceil_div(value: int, divisor: int) -> int:
    """Integer ceiling division, for whole-number brick counts."""
    return -(-value // divisor)


def _resolve_dtype(dtype: int | str | np.dtype) -> int:
    """Map a dtype name, numpy dtype, or numeric ID to a CVF ``dataType`` ID.

    ``float64`` is called out by name rather than lumped in with "unsupported":
    it is a real solver precision that spec 3.3 refuses for *storage*, and a
    caller who asks for it deserves to be told which rule they hit.
    """
    if isinstance(dtype, (int, np.integer)) and not isinstance(dtype, bool):
        code = int(dtype)
        if code in CVF_DTYPE_NAMES:
            return code
        raise CVFError(
            f"unknown CVF dataType ID {code}; supported: {sorted(CVF_DTYPE_NAMES)} "
            f"({sorted(CVF_DTYPE_NAMES.values())})"
        )
    name = dtype.strip().lower() if isinstance(dtype, str) else np.dtype(dtype).name
    if name in _DTYPE_IDS_BY_NAME:
        return _DTYPE_IDS_BY_NAME[name]
    if name in ("float64", "double", "f8"):
        raise CVFError(
            "float64 field storage is not supported in CFDViz 1.0 (spec 3.3); store "
            "float32, float16, or uint8 and record the solver precision in "
            "manifest field.solverPrecision"
        )
    raise CVFError(
        f"unsupported CVF storage type {dtype!r}; CVF stores "
        f"{sorted(_DTYPE_IDS_BY_NAME)} only"
    )


def _resolve_association(association: int | str) -> int:
    """Map ``'cell'``/``'point'`` (or an ID) to a CVF association ID."""
    if isinstance(association, (int, np.integer)) and not isinstance(association, bool):
        code = int(association)
        if code in CVF_ASSOC_NAMES:
            return code
        raise CVFError(
            f"unknown CVF association ID {code}; expected {CVF_ASSOC_CELL} (cell) or "
            f"{CVF_ASSOC_POINT} (point)"
        )
    key = str(association).strip().lower()
    if key in _ASSOC_IDS_BY_NAME:
        return _ASSOC_IDS_BY_NAME[key]
    if key in _RESERVED_ASSOCIATIONS:
        raise CVFError(
            f"association {key!r} is reserved for a future version and must be "
            "rejected in CFDViz 1.0 (spec 3.2); only 'cell' and 'point' are supported"
        )
    raise CVFError(
        f"unknown CVF association {association!r}; expected 'cell' or 'point'"
    )


def _bit_view(array: np.ndarray) -> np.ndarray:
    """Reinterpret ``array`` as unsigned integers of the same width.

    Background-brick omission must compare *bit patterns*, not values: ``NaN``
    never equals itself, so a value comparison would store every brick of a
    NaN-backgrounded field, and ``-0.0 == 0.0`` would omit bricks whose sign bit
    a reader would then get wrong.
    """
    array = np.ascontiguousarray(array)
    return array.view({1: "u1", 2: "<u2", 4: "<u4", 8: "<u8"}[array.dtype.itemsize])


# ---------------------------------------------------------------------------
# Directory entry
# ---------------------------------------------------------------------------

@dataclass(slots=True)
class CVFBrickEntry:
    """One 80-byte brick directory entry (spec 4.3).

    Attributes:
        brick_index_x: Brick coordinate along X, in bricks, not voxels.
        brick_index_y: Brick coordinate along Y.
        brick_index_z: Brick coordinate along Z.
        valid_size_x: Voxels actually stored along X. Edge bricks are smaller
            than ``brickSizeX`` and are never padded (spec 4.4.3).
        valid_size_y: Voxels actually stored along Y.
        valid_size_z: Voxels actually stored along Z.
        flags: Per-brick flags. No bits are defined in 1.0.
        absolute_payload_offset: File offset of the stored payload.
        compressed_bytes: Payload size as stored.
        uncompressed_bytes: Payload size after decoding (spec 4.4.4).
        component_min: ``float32[4]`` per-component minimum, ignoring NaN and
            masked cells. ``+inf`` means "no valid data" (spec 4.4.7).
        component_max: ``float32[4]`` per-component maximum. ``-inf`` means "no
            valid data".
        payload_crc32c: CRC-32C over the **compressed** bytes as stored, so
            integrity can be checked without decompressing.
    """

    brick_index_x: int = 0
    brick_index_y: int = 0
    brick_index_z: int = 0
    valid_size_x: int = 0
    valid_size_y: int = 0
    valid_size_z: int = 0
    flags: int = 0
    absolute_payload_offset: int = 0
    compressed_bytes: int = 0
    uncompressed_bytes: int = 0
    component_min: np.ndarray = field(
        default_factory=lambda: np.full(4, np.inf, dtype="<f4")
    )
    component_max: np.ndarray = field(
        default_factory=lambda: np.full(4, -np.inf, dtype="<f4")
    )
    payload_crc32c: int = 0

    @property
    def brick_index(self) -> tuple[int, int, int]:
        """``(x, y, z)`` brick coordinate."""
        return (self.brick_index_x, self.brick_index_y, self.brick_index_z)

    @property
    def valid_size(self) -> tuple[int, int, int]:
        """``(x, y, z)`` voxel counts actually stored in this brick."""
        return (self.valid_size_x, self.valid_size_y, self.valid_size_z)

    @property
    def voxel_count(self) -> int:
        """Voxels stored in this brick."""
        return self.valid_size_x * self.valid_size_y * self.valid_size_z

    def expected_uncompressed_bytes(self, component_count: int, item_size: int) -> int:
        """The normative product of spec 4.4.4."""
        return self.voxel_count * component_count * item_size

    def pack(self) -> bytes:
        """Serialize to exactly 80 bytes."""
        minimum = np.asarray(self.component_min, dtype=np.float64).reshape(-1)
        maximum = np.asarray(self.component_max, dtype=np.float64).reshape(-1)
        return _ENTRY_STRUCT.pack(
            self.brick_index_x,
            self.brick_index_y,
            self.brick_index_z,
            self.valid_size_x,
            self.valid_size_y,
            self.valid_size_z,
            self.flags,
            self.absolute_payload_offset,
            self.compressed_bytes,
            self.uncompressed_bytes,
            *(float(v) for v in minimum),
            *(float(v) for v in maximum),
            self.payload_crc32c,
            b"\x00" * 8,
        )

    @classmethod
    def unpack(cls, data: bytes, *, path: Path | str | None = None,
               offset: int = 0) -> CVFBrickEntry:
        """Parse one 80-byte entry.

        Args:
            data: Exactly (or at least) 80 bytes.
            path: File the bytes came from, for error messages.
            offset: File offset of this entry, for error messages.

        Raises:
            CVFFormatError: On short input, non-zero reserved bytes, or flag
                bits this version does not implement.
        """
        if len(data) < CVF_DIRECTORY_ENTRY_BYTES:
            raise CVFFormatError(
                f"brick directory entry is {len(data)} bytes, shorter than the "
                f"{CVF_DIRECTORY_ENTRY_BYTES}-byte entry",
                path=path,
                offset=offset,
            )
        (
            bx, by, bz,
            vsx, vsy, vsz,
            flags,
            payload_offset,
            compressed_bytes,
            uncompressed_bytes,
            min0, min1, min2, min3,
            max0, max1, max2, max3,
            payload_crc,
            reserved,
        ) = _ENTRY_STRUCT.unpack_from(data, 0)

        if reserved != b"\x00" * 8:
            raise CVFFormatError(
                "reserved brick directory bytes [72, 80) are not zero",
                path=path,
                offset=offset + 72,
            )
        if flags != 0:
            # Spec 1.4: a newer minor version is accepted only when every
            # construct is *understood*. An unknown brick flag could change how
            # the payload decodes, so it is refused rather than ignored.
            raise CVFFormatError(
                f"brick flags 0x{flags:04X} are set; this file uses a per-brick "
                "feature this reader does not implement",
                path=path,
                offset=offset + 18,
            )

        return cls(
            brick_index_x=bx,
            brick_index_y=by,
            brick_index_z=bz,
            valid_size_x=vsx,
            valid_size_y=vsy,
            valid_size_z=vsz,
            flags=flags,
            absolute_payload_offset=payload_offset,
            compressed_bytes=compressed_bytes,
            uncompressed_bytes=uncompressed_bytes,
            component_min=np.array([min0, min1, min2, min3], dtype="<f4"),
            component_max=np.array([max0, max1, max2, max3], dtype="<f4"),
            payload_crc32c=payload_crc,
        )


# ---------------------------------------------------------------------------
# Header
# ---------------------------------------------------------------------------

@dataclass(slots=True)
class CVFHeader:
    """The fixed 128-byte CVF header. See the module docstring for the layout."""

    dimension_x: int = 1
    dimension_y: int = 1
    dimension_z: int = 1
    brick_size_x: int = DEFAULT_BRICK_SIZE[0]
    brick_size_y: int = DEFAULT_BRICK_SIZE[1]
    brick_size_z: int = DEFAULT_BRICK_SIZE[2]
    component_count: int = 1
    data_type: int = CVF_DTYPE_FLOAT32
    association: int = CVF_ASSOC_CELL
    codec: int = CODEC_NONE
    frame_index: int = 0
    field_numeric_id: int = 0
    simulation_time: float = 0.0
    flags: int = 0
    brick_count: int = 0
    directory_offset: int = CVF_HEADER_BYTES
    payload_offset: int = CVF_HEADER_BYTES
    background_value: np.ndarray = field(
        default_factory=lambda: np.zeros(4, dtype="<f4")
    )
    header_crc32c: int = 0
    major_version: int = CVF_MAJOR_VERSION
    minor_version: int = CVF_MINOR_VERSION

    # -- derived ------------------------------------------------------------

    @property
    def dimensions(self) -> tuple[int, int, int]:
        """Grid **cell** counts, exactly as the manifest declares them."""
        return (self.dimension_x, self.dimension_y, self.dimension_z)

    @property
    def brick_size(self) -> tuple[int, int, int]:
        """Nominal brick extent in voxels. Edge bricks are smaller."""
        return (self.brick_size_x, self.brick_size_y, self.brick_size_z)

    @property
    def value_extent(self) -> tuple[int, int, int]:
        """Stored values per axis, derived from ``association`` (spec 3.2).

        ``cell`` stores ``n`` values per axis, located at cell centres;
        ``point`` stores ``n + 1``, located at cell corners. Getting this
        half-cell offset wrong is the single most common visualisation error in
        this format, so it is derived in exactly one place.
        """
        if self.association == CVF_ASSOC_POINT:
            return (self.dimension_x + 1, self.dimension_y + 1, self.dimension_z + 1)
        return self.dimensions

    @property
    def brick_grid(self) -> tuple[int, int, int]:
        """Bricks per axis needed to tile :attr:`value_extent`."""
        extent = self.value_extent
        return (
            _ceil_div(extent[0], self.brick_size_x),
            _ceil_div(extent[1], self.brick_size_y),
            _ceil_div(extent[2], self.brick_size_z),
        )

    @property
    def numpy_dtype(self) -> str:
        """Explicit little-endian numpy dtype string for the stored payload."""
        return CVF_DTYPE_NUMPY[self.data_type]

    @property
    def item_size(self) -> int:
        """Bytes per stored scalar."""
        return int(np.dtype(self.numpy_dtype).itemsize)

    @property
    def data_type_name(self) -> str:
        """Human-readable storage type, e.g. ``'float32'``."""
        return CVF_DTYPE_NAMES[self.data_type]

    @property
    def association_name(self) -> str:
        """``'cell'`` or ``'point'``."""
        return CVF_ASSOC_NAMES[self.association]

    @property
    def codec_name(self) -> str:
        """Codec name, e.g. ``'zlib'``."""
        return CODEC_NAMES.get(self.codec, f"unknown({self.codec})")

    @property
    def is_sparse(self) -> bool:
        """Whether background bricks may be absent from the directory."""
        return bool(self.flags & CVF_FLAG_SPARSE)

    @property
    def value_count(self) -> int:
        """Total stored values, components included."""
        extent = self.value_extent
        return extent[0] * extent[1] * extent[2] * self.component_count

    # -- serialization ------------------------------------------------------

    def pack(self) -> bytes:
        """Serialize to exactly 128 bytes, computing ``headerCrc32c``.

        The CRC rule is spec 4.1's: CRC-32C over ``[0, 128)`` with the four
        bytes of the CRC field itself zeroed.
        """
        background = np.asarray(self.background_value, dtype=np.float64).reshape(-1)
        if background.size != 4:
            raise CVFError(
                f"backgroundValue must have 4 slots, got {background.size}"
            )
        blank = _HEADER_STRUCT.pack(
            CVF_MAGIC,
            CVF_HEADER_BYTES,
            self.major_version,
            self.minor_version,
            CVF_ENDIAN_MARKER,
            self.flags,
            self.frame_index,
            self.field_numeric_id,
            float(self.simulation_time),
            self.dimension_x,
            self.dimension_y,
            self.dimension_z,
            self.brick_size_x,
            self.brick_size_y,
            self.brick_size_z,
            self.component_count,
            self.data_type,
            self.association,
            self.codec,
            0,  # reserved [62, 64)
            self.brick_count,
            self.directory_offset,
            self.payload_offset,
            *(float(v) for v in background),
            0,  # headerCrc32c, zero while checksumming
            b"\x00" * 20,  # reserved [108, 128)
        )
        self.header_crc32c = crc32c(blank)
        lo, hi = _CRC_FIELD_SPAN
        return blank[:lo] + struct.pack("<I", self.header_crc32c) + blank[hi:]

    @classmethod
    def unpack(cls, data: bytes, *, path: Path | str | None = None,
               verify_crc: bool = True) -> CVFHeader:
        """Parse and validate 128 header bytes.

        Structural checks run before the CRC so that a file which is simply not
        a CVF reports *that*, rather than a checksum mismatch nobody can act on.
        Everything else — including every field the CRC is the only witness for
        — is caught by the CRC check at the end.

        Args:
            data: At least 128 bytes; trailing bytes are ignored.
            path: File the bytes came from, for error messages.
            verify_crc: Check ``headerCrc32c``. Only a validator that reports
                corruption instead of raising should pass ``False``.

        Raises:
            CVFFormatError: On short input, bad magic, foreign byte order,
                unsupported major version, wrong ``headerBytes``, unknown flag
                bits, non-zero reserved bytes, an illegal enum value, or a
                header CRC mismatch.
            UnsupportedCodecError: If ``codec`` is the reserved zstd ID, with
                the exact message spec 7 mandates.
        """
        if len(data) < CVF_HEADER_BYTES:
            raise CVFFormatError(
                f"file is {len(data)} bytes, shorter than the {CVF_HEADER_BYTES}-byte "
                "CVF header",
                path=path,
                offset=0,
            )
        blob = bytes(data[:CVF_HEADER_BYTES])
        (
            magic,
            header_bytes,
            major,
            minor,
            endian,
            flags,
            frame_index,
            field_numeric_id,
            simulation_time,
            dim_x, dim_y, dim_z,
            brick_x, brick_y, brick_z,
            component_count,
            data_type,
            association,
            codec,
            reserved_short,
            brick_count,
            directory_offset,
            payload_offset,
            bg0, bg1, bg2, bg3,
            stored_crc,
            reserved_tail,
        ) = _HEADER_STRUCT.unpack(blob)

        if magic != CVF_MAGIC:
            raise CVFFormatError(
                f"bad magic {magic!r}; expected {CVF_MAGIC!r}. This is not a CVF volume "
                "file.",
                path=path,
                offset=0,
            )
        if endian != CVF_ENDIAN_MARKER:
            # Spec 1.1: reject foreign byte order, never byte-swap it.
            raise CVFFormatError(
                f"byte order not supported: endianMarker is 0x{endian:08X}, expected "
                f"0x{CVF_ENDIAN_MARKER:08X}. CFDViz is little-endian only.",
                path=path,
                offset=16,
            )
        if major != CVF_MAJOR_VERSION:
            raise CVFFormatError(
                f"unsupported major version {major}; this reader implements CVF "
                f"{CVF_MAJOR_VERSION}.x",
                path=path,
                offset=12,
            )
        if header_bytes != CVF_HEADER_BYTES:
            raise CVFFormatError(
                f"headerBytes is {header_bytes}, expected {CVF_HEADER_BYTES}",
                path=path,
                offset=8,
            )
        unknown = flags & ~CVF_KNOWN_FLAGS
        if unknown:
            raise CVFFormatError(
                f"unknown flag bits 0x{unknown:08X} set; this file uses a CVF feature "
                f"this reader does not implement (flags=0x{flags:08X})",
                path=path,
                offset=20,
            )
        if reserved_short != 0:
            raise CVFFormatError(
                "reserved header bytes [62, 64) are not zero", path=path, offset=62
            )
        if reserved_tail != b"\x00" * 20:
            raise CVFFormatError(
                "reserved header bytes [108, 128) are not zero", path=path, offset=108
            )
        if min(dim_x, dim_y, dim_z) < 1:
            raise CVFFormatError(
                f"grid dimension ({dim_x}, {dim_y}, {dim_z}) has a component below 1; "
                "every dimension must be at least 1 (spec 3.1)",
                path=path,
                offset=40,
            )
        if min(brick_x, brick_y, brick_z) < 1:
            raise CVFFormatError(
                f"brickSize ({brick_x}, {brick_y}, {brick_z}) has a component below 1; "
                "a brick must be at least one voxel on every axis",
                path=path,
                offset=52,
            )
        if not 1 <= component_count <= MAX_COMPONENT_COUNT:
            raise CVFFormatError(
                f"componentCount {component_count} is outside 1..{MAX_COMPONENT_COUNT}; "
                "the header's float32[4] statistics slots cap a CVF at "
                f"{MAX_COMPONENT_COUNT} components",
                path=path,
                offset=58,
            )
        if data_type not in CVF_DTYPE_NAMES:
            raise CVFFormatError(
                f"unknown dataType {data_type}; CVF supports "
                f"{sorted(CVF_DTYPE_NAMES.values())} (float64 field storage is not "
                "supported in 1.0, spec 3.3)",
                path=path,
                offset=59,
            )
        if association not in CVF_ASSOC_NAMES:
            raise CVFFormatError(
                f"unknown association {association}; expected {CVF_ASSOC_CELL} (cell) "
                f"or {CVF_ASSOC_POINT} (point). Associations such as "
                f"{list(_RESERVED_ASSOCIATIONS)} are reserved and rejected in 1.0 "
                "(spec 3.2).",
                path=path,
                offset=60,
            )
        if codec == CODEC_ZSTD:
            # Spec 7 fixes this user-facing wording; the Unreal reader emits the
            # identical string. Never fall back to another codec.
            raise UnsupportedCodecError(ZSTD_REJECTION_MESSAGE)
        if codec not in CODEC_NAMES:
            usable = sorted(
                name for code, name in CODEC_NAMES.items() if code != CODEC_ZSTD
            )
            raise CVFFormatError(
                f"unknown codec ID {codec}; supported: {usable}",
                path=path,
                offset=61,
            )

        header = cls(
            dimension_x=dim_x,
            dimension_y=dim_y,
            dimension_z=dim_z,
            brick_size_x=brick_x,
            brick_size_y=brick_y,
            brick_size_z=brick_z,
            component_count=component_count,
            data_type=data_type,
            association=association,
            codec=codec,
            frame_index=frame_index,
            field_numeric_id=field_numeric_id,
            simulation_time=simulation_time,
            flags=flags,
            brick_count=brick_count,
            directory_offset=directory_offset,
            payload_offset=payload_offset,
            background_value=np.array([bg0, bg1, bg2, bg3], dtype="<f4"),
            header_crc32c=stored_crc,
            major_version=major,
            minor_version=minor,
        )

        if verify_crc:
            lo, hi = _CRC_FIELD_SPAN
            computed = crc32c(blob[:lo] + b"\x00" * (hi - lo) + blob[hi:])
            if computed != stored_crc:
                raise CVFFormatError(
                    f"header CRC-32C mismatch: stored 0x{stored_crc:08X}, computed "
                    f"0x{computed:08X}. The header is corrupt.",
                    path=path,
                    offset=lo,
                )
        return header


@dataclass(slots=True)
class CVFData:
    """A fully decoded CVF volume.

    Attributes:
        values: ``(X, Y, Z, componentCount)`` in the file's storage dtype,
            bit-exact — including ``NaN``. Indexing is ``[x, y, z, c]``
            regardless of the payload's X-fastest byte order.
        header: The header exactly as stored on disk.
        directory: The brick directory, in file order.
        path: Where the volume was read from, for diagnostics.
    """

    values: np.ndarray
    header: CVFHeader = field(default_factory=CVFHeader)
    directory: tuple[CVFBrickEntry, ...] = ()
    path: Path | None = None

    @property
    def component_count(self) -> int:
        """Components per voxel."""
        return int(self.values.shape[3])

    @property
    def frame_index(self) -> int:
        """Frame this volume belongs to."""
        return self.header.frame_index

    @property
    def simulation_time(self) -> float:
        """Physical time of this frame, in the manifest's time unit."""
        return self.header.simulation_time


# ---------------------------------------------------------------------------
# Writing
# ---------------------------------------------------------------------------

def _to_storage(values: object, dtype_id: int) -> np.ndarray:
    """Coerce ``values`` to a contiguous array in the stored dtype.

    A finite input that becomes infinite (or wraps) in the target type is
    rejected rather than stored: spec 1.6 forbids silent clamping and
    rescaling, and turning 1e30 into ``inf`` because someone chose float16
    storage is exactly the data loss that must surface at export time rather
    than in a plot six months later. ``NaN`` is not overflow and passes through
    untouched (spec 1.7).
    """
    target = np.dtype(CVF_DTYPE_NUMPY[dtype_id])
    source = np.asarray(values)
    if not (
        np.issubdtype(source.dtype, np.floating)
        or np.issubdtype(source.dtype, np.integer)
    ):
        raise CVFError(f"values must be numeric; got dtype {source.dtype}")
    if source.dtype == target:
        return np.ascontiguousarray(source)

    def _report(bad: np.ndarray) -> None:
        flat = int(np.argmax(bad.reshape(-1)))
        raise CVFError(
            f"value {source.reshape(-1)[flat]!r} at flat index {flat} overflows "
            f"{CVF_DTYPE_NAMES[dtype_id]} storage. Choose a wider storage type; "
            "CFDViz must not silently clamp or rescale values (spec 1.6)."
        )

    if np.issubdtype(target, np.integer):
        info = np.iinfo(target)
        with np.errstate(invalid="ignore"):
            finite = np.isfinite(source) if np.issubdtype(
                source.dtype, np.floating
            ) else np.ones(source.shape, dtype=bool)
            out_of_range = ~finite | (source < info.min) | (source > info.max)
        if bool(np.any(out_of_range)):
            _report(out_of_range)
        with np.errstate(invalid="ignore"):
            out = source.astype(target, copy=True)
        return np.ascontiguousarray(out)

    # errstate: the overflow is detected explicitly below with a message that
    # names the value, which beats numpy's context-free RuntimeWarning.
    with np.errstate(over="ignore", invalid="ignore"):
        out = source.astype(target, copy=True)
    with np.errstate(invalid="ignore"):
        overflowed = np.isfinite(source) & ~np.isfinite(out)
    if bool(np.any(overflowed)):
        _report(overflowed)
    return np.ascontiguousarray(out)


def _normalize_values(values: object) -> np.ndarray:
    """Return ``values`` as a 4-D ``(X, Y, Z, C)`` array.

    A 3-D input is a scalar field and gains a length-1 component axis, so the
    rest of the module never has to special-case scalars.
    """
    array = np.asarray(values)
    if array.ndim == 3:
        array = array.reshape(array.shape + (1,))
    if array.ndim != 4:
        raise CVFError(
            f"values must be 3-D (X, Y, Z) or 4-D (X, Y, Z, components); got shape "
            f"{array.shape}"
        )
    return array


def _background_slots(background_value: object) -> np.ndarray:
    """Expand ``background_value`` into the header's ``float32[4]`` slots.

    A scalar fills every slot so a single-component field's background is
    unambiguous no matter which slot a reader looks at; a shorter sequence is
    zero-padded.
    """
    array = np.asarray(background_value, dtype="<f4").reshape(-1)
    if array.size == 1:
        return np.full(4, array[0], dtype="<f4")
    if array.size > 4:
        raise CVFError(
            f"backgroundValue has {array.size} components; the header stores at most "
            f"{MAX_COMPONENT_COUNT}"
        )
    slots = np.zeros(4, dtype="<f4")
    slots[: array.size] = array
    return slots


def _brick_ranges(extent: int, brick_size: int) -> Iterator[tuple[int, int, int]]:
    """Yield ``(index, start, stop)`` for each brick along one axis."""
    for index in range(_ceil_div(extent, brick_size)):
        start = index * brick_size
        yield index, start, min(start + brick_size, extent)


def _brick_statistics(sub: np.ndarray, sub_mask: np.ndarray | None,
                      component_count: int) -> tuple[np.ndarray, np.ndarray]:
    """Per-component min/max over one brick, ignoring NaN and masked cells.

    Spec 4.4.7. A component with no valid sample gets ``min = +inf`` and
    ``max = -inf`` — the "no valid data" sentinel a reader can test without a
    separate flag — and so do the unused slots of a field with fewer than four
    components, because a plausible-looking zero there is worse than an obvious
    sentinel.

    ``numpy``'s ``nanmin``/``nanmax`` are deliberately avoided: they emit a
    ``RuntimeWarning`` for an all-NaN input, and this package promotes
    ``RuntimeWarning`` to an error.
    """
    # errstate: widening a *signalling* NaN raises FE_INVALID, which numpy
    # reports as "invalid value encountered in cast". Payload NaN is legal data
    # (spec 1.7), and this is a statistics computation over a copy — the stored
    # bits are untouched — so the flag carries no information here.
    with np.errstate(invalid="ignore"):
        data = np.ascontiguousarray(sub).astype(np.float64).reshape(-1, component_count)
    with np.errstate(invalid="ignore"):
        valid = ~np.isnan(data)
    if sub_mask is not None:
        valid = valid & np.ascontiguousarray(sub_mask).reshape(-1, 1)

    minimum = np.full(4, np.inf, dtype=np.float64)
    maximum = np.full(4, -np.inf, dtype=np.float64)
    if data.shape[0]:
        # np.where keeps the reduction away from NaN entirely, so an all-invalid
        # component falls out as the +inf / -inf sentinel with no special case.
        minimum[:component_count] = np.where(valid, data, np.inf).min(axis=0)
        maximum[:component_count] = np.where(valid, data, -np.inf).max(axis=0)
    return minimum.astype("<f4"), maximum.astype("<f4")


def write_cvf(
    path: Path | str,
    *,
    values: object,
    brick_size: Sequence[int] | int = DEFAULT_BRICK_SIZE,
    frame_index: int = 0,
    field_numeric_id: int = 0,
    simulation_time: float = 0.0,
    association: int | str = CVF_ASSOC_CELL,
    dtype: int | str | np.dtype = "float32",
    codec: int = DEFAULT_CODEC,
    level: int | None = None,
    background_value: object = 0.0,
    dimensions: Sequence[int] | None = None,
    mask: object = None,
    omit_background_bricks: bool = False,
) -> CVFHeader:
    """Write one field at one frame as a bricked volume.

    Args:
        path: Destination ``.cvf`` file. Parent directories are created.
        values: ``(X, Y, Z)`` or ``(X, Y, Z, components)``. Indexing is
            ``[x, y, z, c]``; this function handles the X-fastest byte order.
        brick_size: Brick extent in voxels, as a scalar or ``(x, y, z)``. Edge
            bricks are clipped, never padded.
        frame_index: Frame this volume belongs to.
        field_numeric_id: The manifest ``numericId`` of the field.
        simulation_time: Physical time of the frame, in manifest time units.
        association: ``'cell'`` or ``'point'`` (or the numeric ID). This is what
            fixes the half-cell offset of spec 3.2.
        dtype: Storage type — ``'float16'``, ``'float32'``, ``'uint8'``, or a
            ``CVF_DTYPE_*`` constant. ``float64`` is rejected (spec 3.3).
        codec: Payload codec; defaults to zlib (spec 7). zstd is rejected.
        level: Codec level; ``None`` uses the documented default.
        background_value: Value an omitted brick evaluates to. A scalar fills
            all four header slots.
        dimensions: Grid **cell** counts. Defaults to the value shape for
            ``cell`` association and to the value shape minus one per axis for
            ``point``. When given, it is checked against the value shape, which
            is what catches an off-by-one half-cell error at export time.
        mask: Optional ``(X, Y, Z)`` boolean array; ``False`` marks a cell that
            statistics must ignore (spec 4.4.7). Masked values are still
            **stored** — masking affects statistics, not the payload.
        omit_background_bricks: Drop bricks whose every voxel matches
            ``background_value`` bit for bit (spec 4.4.5).

    Returns:
        The :class:`CVFHeader` that was written, with final offsets and CRCs.

    Raises:
        CVFError: On an illegal shape, component count, brick size, dtype, or a
            value that would overflow the chosen storage type.
        UnsupportedCodecError: If ``codec`` is zstd or an unavailable codec.
    """
    path = Path(path)
    dtype_id = _resolve_dtype(dtype)
    association_id = _resolve_association(association)

    array = _normalize_values(values)
    extent = (int(array.shape[0]), int(array.shape[1]), int(array.shape[2]))
    component_count = int(array.shape[3])

    if not 1 <= component_count <= MAX_COMPONENT_COUNT:
        raise CVFError(
            f"componentCount {component_count} is outside 1..{MAX_COMPONENT_COUNT}; "
            "the header's backgroundValue and the directory's componentMin/Max are "
            f"float32[{MAX_COMPONENT_COUNT}], which caps a CVF at "
            f"{MAX_COMPONENT_COUNT} components"
        )
    if min(extent) < 1:
        raise CVFError(
            f"value extent {extent} has a component below 1; every dimension must be "
            "at least 1 (spec 3.1)"
        )

    if dimensions is None:
        if association_id == CVF_ASSOC_POINT:
            cells = tuple(n - 1 for n in extent)
            if min(cells) < 1:
                raise CVFError(
                    f"point association needs at least 2 values per axis to describe a "
                    f"grid, but the array is {extent}; pass dimensions= explicitly if "
                    "that is really intended"
                )
            dims = cells
        else:
            dims = extent
    else:
        dims = tuple(int(n) for n in dimensions)
        if len(dims) != 3:
            raise CVFError(f"dimensions must have 3 components; got {dimensions!r}")
        if min(dims) < 1:
            raise CVFError(
                f"grid dimensions {dims} has a component below 1; every dimension must "
                "be at least 1 (spec 3.1)"
            )

    expected_extent = (
        tuple(n + 1 for n in dims) if association_id == CVF_ASSOC_POINT else dims
    )
    if extent != expected_extent:
        association_name = CVF_ASSOC_NAMES[association_id]
        raise CVFError(
            f"{association_name} association on grid dimensions {dims} requires "
            f"{expected_extent} values per axis, but the array is {extent}. "
            "cell values sit at cell centres and point values at cell corners "
            "(spec 3.2); this half-cell offset is the format's most common bug."
        )

    if isinstance(brick_size, (int, np.integer)):
        bricks = (int(brick_size),) * 3
    else:
        bricks = tuple(int(n) for n in brick_size)
    if len(bricks) != 3:
        raise CVFError(f"brick_size must have 3 components; got {brick_size!r}")
    if min(bricks) < 1:
        raise CVFError(
            f"brick_size {bricks} has a component below 1; a brick must be at least "
            "one voxel on every axis"
        )
    if max(bricks) > MAX_BRICK_SIZE:
        raise CVFError(
            f"brick_size {bricks} exceeds {MAX_BRICK_SIZE}; brickSizeX/Y/Z are uint16"
        )

    stored = _to_storage(array, dtype_id)
    background = _background_slots(background_value)
    # The comparison basis for brick omission has to be the value a *reader*
    # will materialise: the float32 header slot narrowed to the storage type.
    background_storage = _to_storage(
        background[:component_count], dtype_id
    )
    background_bits = _bit_view(background_storage)

    mask_array: np.ndarray | None = None
    if mask is not None:
        mask_array = np.ascontiguousarray(np.asarray(mask, dtype=bool))
        if mask_array.shape != extent:
            raise CVFError(
                f"mask shape {mask_array.shape} does not match the value extent "
                f"{extent}"
            )

    item_size = int(np.dtype(CVF_DTYPE_NUMPY[dtype_id]).itemsize)

    entries: list[CVFBrickEntry] = []
    payloads: list[bytes] = []
    for kz, z0, z1 in _brick_ranges(extent[2], bricks[2]):
        for jy, y0, y1 in _brick_ranges(extent[1], bricks[1]):
            for ix, x0, x1 in _brick_ranges(extent[0], bricks[0]):
                sub = stored[x0:x1, y0:y1, z0:z1, :]
                if omit_background_bricks:
                    voxels = _bit_view(sub).reshape(-1, component_count)
                    if bool(np.all(voxels == background_bits)):
                        continue

                sub_mask = None if mask_array is None else mask_array[x0:x1, y0:y1, z0:z1]
                minimum, maximum = _brick_statistics(sub, sub_mask, component_count)

                # X fastest, then Y, then Z, components interleaved (spec 4.4.1
                # and 4.4.2): a C-contiguous (Z, Y, X, C) buffer is exactly that.
                raw = np.ascontiguousarray(sub.transpose(2, 1, 0, 3)).tobytes()
                blob = compress(raw, codec, level)

                entries.append(
                    CVFBrickEntry(
                        brick_index_x=ix,
                        brick_index_y=jy,
                        brick_index_z=kz,
                        valid_size_x=x1 - x0,
                        valid_size_y=y1 - y0,
                        valid_size_z=z1 - z0,
                        flags=0,
                        compressed_bytes=len(blob),
                        uncompressed_bytes=len(raw),
                        component_min=minimum,
                        component_max=maximum,
                        payload_crc32c=crc32c(blob),  # over the STORED bytes
                    )
                )
                payloads.append(blob)

    directory_offset = CVF_HEADER_BYTES
    payload_offset = directory_offset + len(entries) * CVF_DIRECTORY_ENTRY_BYTES
    cursor = payload_offset
    for entry, blob in zip(entries, payloads):
        entry.absolute_payload_offset = cursor
        cursor += len(blob)

    header = CVFHeader(
        dimension_x=dims[0],
        dimension_y=dims[1],
        dimension_z=dims[2],
        brick_size_x=bricks[0],
        brick_size_y=bricks[1],
        brick_size_z=bricks[2],
        component_count=component_count,
        data_type=dtype_id,
        association=association_id,
        codec=codec,
        frame_index=int(frame_index),
        field_numeric_id=int(field_numeric_id),
        simulation_time=float(simulation_time),
        flags=CVF_FLAG_SPARSE if omit_background_bricks else 0,
        brick_count=len(entries),
        directory_offset=directory_offset,
        payload_offset=payload_offset,
        background_value=background,
    )

    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(header.pack())
        for entry in entries:
            handle.write(entry.pack())
        for blob in payloads:
            handle.write(blob)
    return header


# ---------------------------------------------------------------------------
# Reading
# ---------------------------------------------------------------------------

class CVFReader:
    """Random-access reader for one ``.cvf`` volume.

    The header and the brick directory are read and fully validated up front;
    brick payloads are read and decoded on demand, which is what makes
    region-of-interest loading and empty-space skipping worth having.

    Every offset, length, and count is checked against the real file size
    **before** any buffer is allocated (spec 1.5), so a hostile ``brickCount``
    or ``uncompressedBytes`` becomes an error rather than an allocation.

    Use as a context manager::

        with CVFReader(path) as reader:
            brick = reader.read_brick(reader.find_brick(1, 0, 0))
    """

    def __init__(self, path: Path | str, *, verify_crc: bool = True) -> None:
        """Open and validate a volume.

        Args:
            path: The ``.cvf`` file.
            verify_crc: Verify the header CRC and every brick payload CRC.
                Leave this on outside validators that want to *report*
                corruption rather than raise on it.

        Raises:
            CVFFormatError: If the file is missing, truncated, or inconsistent.
            UnsupportedCodecError: If the payload uses the reserved zstd codec.
        """
        self.path = Path(path)
        self.verify_crc = bool(verify_crc)
        try:
            self._handle = open(self.path, "rb")
        except OSError as exc:
            raise CVFFormatError(f"cannot read volume: {exc}", path=self.path) from exc
        try:
            self._size = os.fstat(self._handle.fileno()).st_size
            self.header = CVFHeader.unpack(
                self._handle.read(CVF_HEADER_BYTES),
                path=self.path,
                verify_crc=self.verify_crc,
            )
            self.directory: tuple[CVFBrickEntry, ...] = self._read_directory()
            self._by_index = {entry.brick_index: entry for entry in self.directory}
        except BaseException:
            self._handle.close()
            raise

    # -- lifecycle ----------------------------------------------------------

    def __enter__(self) -> CVFReader:
        return self

    def __exit__(self, *exc_info: object) -> None:
        self.close()

    def close(self) -> None:
        """Close the underlying file handle. Idempotent."""
        if not self._handle.closed:
            self._handle.close()

    def __repr__(self) -> str:  # pragma: no cover - diagnostics only
        header = self.header
        return (
            f"<CVFReader {self.path.name} {header.data_type_name} "
            f"{header.value_extent} x{header.component_count} "
            f"{header.brick_count} bricks>"
        )

    # -- directory ----------------------------------------------------------

    def _read_directory(self) -> tuple[CVFBrickEntry, ...]:
        """Read and validate every brick directory entry."""
        header = self.header
        grid = header.brick_grid
        grid_total = grid[0] * grid[1] * grid[2]

        # Bound the count against the volume's own geometry before it is ever
        # multiplied into a byte length or an allocation (spec 1.5).
        if header.brick_count > grid_total:
            raise CVFFormatError(
                f"brickCount {header.brick_count} exceeds the {grid_total} bricks a "
                f"{header.value_extent} volume in {header.brick_size} bricks can hold; "
                "the directory is inconsistent with the header",
                path=self.path,
                offset=64,
            )
        if header.directory_offset < CVF_HEADER_BYTES:
            raise CVFFormatError(
                f"directoryOffset {header.directory_offset} overlaps the "
                f"{CVF_HEADER_BYTES}-byte header",
                path=self.path,
                offset=72,
            )
        needed = header.brick_count * CVF_DIRECTORY_ENTRY_BYTES
        if (
            header.directory_offset > self._size
            or needed > self._size - header.directory_offset
        ):
            raise CVFFormatError(
                f"brick directory needs {needed} bytes at offset "
                f"{header.directory_offset}, but the file is only {self._size} bytes; "
                "it is truncated or the header is corrupt",
                path=self.path,
                offset=72,
            )

        self._handle.seek(header.directory_offset)
        blob = self._handle.read(needed)
        if len(blob) != needed:
            raise CVFFormatError(
                f"brick directory is truncated: read {len(blob)} of {needed} bytes",
                path=self.path,
                offset=header.directory_offset,
            )

        item_size = header.item_size
        entries: list[CVFBrickEntry] = []
        seen: set[tuple[int, int, int]] = set()
        for i in range(header.brick_count):
            base = header.directory_offset + i * CVF_DIRECTORY_ENTRY_BYTES
            entry = CVFBrickEntry.unpack(
                blob[i * CVF_DIRECTORY_ENTRY_BYTES : (i + 1) * CVF_DIRECTORY_ENTRY_BYTES],
                path=self.path,
                offset=base,
            )
            self._validate_entry(entry, base, grid, item_size, seen)
            seen.add(entry.brick_index)
            entries.append(entry)
        return tuple(entries)

    def _validate_entry(self, entry: CVFBrickEntry, base: int,
                        grid: tuple[int, int, int], item_size: int,
                        seen: set[tuple[int, int, int]]) -> None:
        """Check one entry against the header and the file, before allocating."""
        header = self.header
        index = entry.brick_index
        if any(index[axis] >= grid[axis] for axis in range(3)):
            raise CVFFormatError(
                f"brick index {index} is outside the {grid} brick grid of this volume",
                path=self.path,
                offset=base,
            )
        if index in seen:
            raise CVFFormatError(
                f"duplicate directory entry for brick {index}; which copy wins would be "
                "undefined",
                path=self.path,
                offset=base,
            )

        extent = header.value_extent
        brick = header.brick_size
        expected = tuple(
            min(brick[axis], extent[axis] - index[axis] * brick[axis])
            for axis in range(3)
        )
        if entry.valid_size != expected:
            raise CVFFormatError(
                f"brick {index} declares validSize {entry.valid_size} but its position "
                f"in a {extent} volume with {brick} bricks requires {expected}; edge "
                "bricks are clipped, never padded (spec 4.4.3)",
                path=self.path,
                offset=base + 12,
            )

        # Spec 4.4.4: prove the declared size before allocating anything. This is
        # the primary defence against a malicious size field.
        product = entry.expected_uncompressed_bytes(header.component_count, item_size)
        if entry.uncompressed_bytes != product:
            raise CVFFormatError(
                f"brick {index} declares uncompressedBytes {entry.uncompressed_bytes}, "
                f"but validSize {entry.valid_size} x componentCount "
                f"{header.component_count} x sizeof({header.data_type_name}) = "
                f"{product}",
                path=self.path,
                offset=base + 32,
            )
        if entry.absolute_payload_offset < CVF_HEADER_BYTES and entry.compressed_bytes:
            raise CVFFormatError(
                f"brick {index} payload offset {entry.absolute_payload_offset} overlaps "
                f"the {CVF_HEADER_BYTES}-byte header",
                path=self.path,
                offset=base + 20,
            )
        if (
            entry.absolute_payload_offset > self._size
            or entry.compressed_bytes > self._size - entry.absolute_payload_offset
        ):
            raise CVFFormatError(
                f"brick {index} payload needs {entry.compressed_bytes} bytes at offset "
                f"{entry.absolute_payload_offset}, but the file is only {self._size} "
                "bytes; it is truncated or the directory is corrupt",
                path=self.path,
                offset=base + 20,
            )
        if (
            header.codec == CODEC_NONE
            and entry.compressed_bytes != entry.uncompressed_bytes
        ):
            raise CVFFormatError(
                f"brick {index} uses codec 'none' but compressedBytes "
                f"{entry.compressed_bytes} != uncompressedBytes "
                f"{entry.uncompressed_bytes}",
                path=self.path,
                offset=base + 28,
            )

    # -- access -------------------------------------------------------------

    def find_brick(self, x: int, y: int, z: int) -> CVFBrickEntry | None:
        """Look up a brick by its brick-grid coordinate.

        Returns:
            The entry, or ``None`` if the brick is absent from the directory. An
            absent brick is not an error: spec 4.4.5 says it evaluates to
            ``backgroundValue`` everywhere.
        """
        return self._by_index.get((int(x), int(y), int(z)))

    def read_brick(self, entry: CVFBrickEntry) -> np.ndarray:
        """Decode one brick.

        Args:
            entry: A directory entry, typically from :meth:`find_brick`.

        Returns:
            ``(validSizeX, validSizeY, validSizeZ, componentCount)`` in the
            file's storage dtype, indexed ``[x, y, z, c]``.

        Raises:
            CVFFormatError: If the payload is truncated or fails its CRC.
            UnsupportedCodecError: For the reserved zstd codec.
        """
        if entry is None:
            raise CVFError(
                "read_brick was given None; an absent brick evaluates to "
                "backgroundValue and has no payload to read (spec 4.4.5)"
            )
        header = self.header
        self._handle.seek(entry.absolute_payload_offset)
        blob = self._handle.read(entry.compressed_bytes)
        if len(blob) != entry.compressed_bytes:
            raise CVFFormatError(
                f"brick {entry.brick_index} payload is truncated: read {len(blob)} of "
                f"{entry.compressed_bytes} bytes",
                path=self.path,
                offset=entry.absolute_payload_offset,
            )
        if self.verify_crc:
            computed = crc32c(blob)
            if computed != entry.payload_crc32c:
                raise CVFFormatError(
                    f"brick {entry.brick_index} payload CRC-32C mismatch: stored "
                    f"0x{entry.payload_crc32c:08X}, computed 0x{computed:08X}. The "
                    "payload is corrupt.",
                    path=self.path,
                    offset=entry.absolute_payload_offset,
                )
        try:
            raw = decompress(blob, header.codec, entry.uncompressed_bytes)
        except UnsupportedCodecError:
            raise
        except CodecError as exc:
            raise CVFFormatError(
                f"brick {entry.brick_index} could not be decoded with codec "
                f"'{header.codec_name}': {exc}",
                path=self.path,
                offset=entry.absolute_payload_offset,
            ) from exc

        # The payload is (Z, Y, X, C) in memory order — X fastest, components
        # interleaved — so transposing back gives the [x, y, z, c] indexing
        # every caller expects.
        flat = np.frombuffer(
            raw,
            dtype=header.numpy_dtype,
            count=entry.voxel_count * header.component_count,
        )
        brick = flat.reshape(
            entry.valid_size_z,
            entry.valid_size_y,
            entry.valid_size_x,
            header.component_count,
        ).transpose(2, 1, 0, 3)
        return np.ascontiguousarray(brick)

    def background_array(self, shape: tuple[int, int, int]) -> np.ndarray:
        """An array of ``shape`` filled with ``backgroundValue`` per component."""
        header = self.header
        out = np.empty(shape + (header.component_count,), dtype=header.numpy_dtype)
        fill = _to_storage(
            header.background_value[: header.component_count], header.data_type
        )
        for component in range(header.component_count):
            out[..., component] = fill[component]
        return out

    def read_region(self, lo: Sequence[int], hi: Sequence[int]) -> np.ndarray:
        """Decode the half-open voxel region ``[lo, hi)``.

        Only the bricks overlapping the region are read. Bricks absent from the
        directory contribute ``backgroundValue`` (spec 4.4.5).

        Args:
            lo: Inclusive lower voxel corner, in :attr:`CVFHeader.value_extent`
                coordinates.
            hi: Exclusive upper voxel corner.

        Returns:
            ``(hi - lo, componentCount)`` in the file's storage dtype, indexed
            ``[x, y, z, c]``.

        Raises:
            CVFError: If the region is empty or leaves the volume.
        """
        header = self.header
        extent = header.value_extent
        low = tuple(int(v) for v in lo)
        high = tuple(int(v) for v in hi)
        if len(low) != 3 or len(high) != 3:
            raise CVFError(
                f"read_region needs 3-component corners; got {lo!r} and {hi!r}"
            )
        if any(low[axis] < 0 for axis in range(3)) or any(
            high[axis] > extent[axis] for axis in range(3)
        ):
            raise CVFError(
                f"region [{low}, {high}) is outside the value extent {extent}"
            )
        if any(low[axis] >= high[axis] for axis in range(3)):
            raise CVFError(
                f"region [{low}, {high}) is empty: every lo component must be "
                f"strictly below its hi component"
            )

        shape = tuple(high[axis] - low[axis] for axis in range(3))
        out = self.background_array(shape)

        brick = header.brick_size
        for kz in range(low[2] // brick[2], (high[2] - 1) // brick[2] + 1):
            for jy in range(low[1] // brick[1], (high[1] - 1) // brick[1] + 1):
                for ix in range(low[0] // brick[0], (high[0] - 1) // brick[0] + 1):
                    entry = self.find_brick(ix, jy, kz)
                    if entry is None:
                        continue  # absent brick == backgroundValue, already filled
                    data = self.read_brick(entry)
                    origin = (ix * brick[0], jy * brick[1], kz * brick[2])
                    starts = tuple(
                        max(low[axis], origin[axis]) for axis in range(3)
                    )
                    stops = tuple(
                        min(high[axis], origin[axis] + entry.valid_size[axis])
                        for axis in range(3)
                    )
                    out[
                        starts[0] - low[0] : stops[0] - low[0],
                        starts[1] - low[1] : stops[1] - low[1],
                        starts[2] - low[2] : stops[2] - low[2],
                        :,
                    ] = data[
                        starts[0] - origin[0] : stops[0] - origin[0],
                        starts[1] - origin[1] : stops[1] - origin[1],
                        starts[2] - origin[2] : stops[2] - origin[2],
                        :,
                    ]
        return out

    def read_all(self) -> np.ndarray:
        """Decode the whole volume as ``(X, Y, Z, componentCount)``."""
        return self.read_region((0, 0, 0), self.header.value_extent)


def read_cvf(path: Path | str, verify_crc: bool = True) -> CVFData:
    """Read a whole volume, validating it against the file on disk.

    Args:
        path: The ``.cvf`` file.
        verify_crc: Verify the header CRC and every brick payload CRC. Leave
            this on outside validators that want to report corruption rather
            than raise on it.

    Returns:
        The decoded :class:`CVFData`. ``values`` keeps the file's storage dtype,
        so ``NaN`` payloads compare bit-exactly against what was written.

    Raises:
        CVFFormatError: If the file is missing, truncated, has a bad magic,
            foreign byte order, an unsupported version, an illegal enum value,
            an inconsistent size, or a CRC mismatch. The message always names
            the file and, where meaningful, the byte offset.
        UnsupportedCodecError: If the payload uses the reserved zstd codec.
    """
    with CVFReader(path, verify_crc=verify_crc) as reader:
        return CVFData(
            values=reader.read_all(),
            header=reader.header,
            directory=reader.directory,
            path=reader.path,
        )
