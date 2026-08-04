"""CVA — CFDViz mesh-associated array (reserved for FEA).

See ``Docs/CFDVIZ_FORMAT.md`` section 6. CVA is *specified and round-trip
tested* in 1.0, but no 1.0 UI renders every CVA result type; the point of
shipping it now is that the on-disk contract is frozen and provable, so an FEA
result written today is still readable when the renderer catches up.

One ``.cva`` file holds **one array at one frame**: a value per mesh vertex or
per mesh element of the companion ``.cvm`` (section 5), with 1, 3, 6, or 9
components. Section 6 fixes the invariants — 96-byte header, magic
``CFDARR1\\0``, endian marker ``0x01020304``, the CRC rule of section 8, and the
symmetric-tensor component order — and this module fixes the remaining byte
layout. The Unreal reader is written against the table below.

Header — fixed 96 bytes, little-endian, manually serialized::

    Offset Size Type     Name
    0      8    char[8]  magic = "CFDARR1\\0"
    8      2    uint16   majorVersion = 1
    10     2    uint16   minorVersion = 0
    12     4    uint32   endianMarker = 0x01020304
    16     4    uint32   flags
    20     4    uint32   headerBytes = 96
    24     4    uint32   frameIndex
    28     4    uint32   fieldNumericId
    32     8    float64  simulationTime
    40     8    uint64   valueCount        entities (vertices or elements)
    48     1    uint8    componentCount    1 | 3 | 6 | 9
    49     1    uint8    dataType
    50     1    uint8    association
    51     1    uint8    codec
    52     4    uint32   payloadCrc32c
    56     8    uint64   payloadOffset
    64     8    uint64   compressedBytes
    72     8    uint64   uncompressedBytes
    80     4    uint32   headerCrc32c
    84     4    uint32   reserved = 0
    88     8    uint64   statisticsOffset  0 when no statistics are stored

The first 24 bytes are laid out exactly like a CVM header (magic, versions,
endian marker, flags, headerBytes) so a C++ reader can sniff and validate any
CFDViz container with one shared routine. ``headerCrc32c`` sits at ``[80, 84)``
for the same reason: it makes the CRC rule literally identical to CVM's —
CRC-32C over ``[0, 96)`` with ``[80, 84)`` zeroed.

Payload rules, mirroring CVF section 4.4 so the two payload decoders behave the
same way:

1. Components are **interleaved per entity**: ``v0.x, v0.y, v0.z, v1.x, ...``.
2. ``uncompressedBytes == valueCount * componentCount * sizeof(dataType)``,
   verified **before** allocating (spec 1.5).
3. ``payloadCrc32c`` is CRC-32C over the **compressed** bytes as stored, so
   integrity can be checked without decompressing.
4. ``NaN`` is legal and is preserved bit-exactly; statistics ignore it.

Symmetric 6-component tensors are stored in the normative order
``XX, YY, ZZ, XY, YZ, XZ`` (:data:`SYMMETRIC_TENSOR_COMPONENT_ORDER`). Full
9-component tensors are row-major ``XX, XY, XZ, YX, YY, YZ, ZX, ZY, ZZ``.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Final, Sequence

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
    "CVA_MAGIC",
    "CVA_HEADER_BYTES",
    "CVA_ENDIAN_MARKER",
    "CVA_MAJOR_VERSION",
    "CVA_MINOR_VERSION",
    "CVA_DTYPE_FLOAT16",
    "CVA_DTYPE_FLOAT32",
    "CVA_DTYPE_FLOAT64",
    "CVA_DTYPE_NAMES",
    "CVA_DTYPE_NUMPY",
    "CVA_ASSOC_ELEMENT",
    "CVA_ASSOC_VERTEX",
    "CVA_ASSOC_NAMES",
    "CVA_FLAG_FRAME_STATISTICS",
    "CVA_FLAG_GLOBAL_STATISTICS",
    "CVA_KNOWN_FLAGS",
    "VALID_COMPONENT_COUNTS",
    "SYMMETRIC_TENSOR_COMPONENT_ORDER",
    "FULL_TENSOR_COMPONENT_ORDER",
    "CVAError",
    "CVAFormatError",
    "ArrayStatistics",
    "CVAHeader",
    "CVAData",
    "write_cva",
    "read_cva",
]

# A wrong CRC produces files no correct reader accepts; fail at import, not at
# the end of a long export.
self_check()

CVA_MAGIC: Final = b"CFDARR1\x00"
CVA_HEADER_BYTES: Final = 96
CVA_ENDIAN_MARKER: Final = 0x01020304
CVA_MAJOR_VERSION: Final = 1
CVA_MINOR_VERSION: Final = 0

#: Storage type IDs. 1 and 2 deliberately match CVF (section 4.2) so the shared
#: C++ enum needs no per-container translation. 3 stays reserved for CVF's
#: ``uint8``, which CVA does not offer, and float64 takes the next free value.
CVA_DTYPE_FLOAT16: Final = 1
CVA_DTYPE_FLOAT32: Final = 2
CVA_DTYPE_FLOAT64: Final = 4

CVA_DTYPE_NAMES: Final[dict[int, str]] = {
    CVA_DTYPE_FLOAT16: "float16",
    CVA_DTYPE_FLOAT32: "float32",
    CVA_DTYPE_FLOAT64: "float64",
}

#: Explicit little-endian numpy dtype strings. Never rely on native byte order.
CVA_DTYPE_NUMPY: Final[dict[int, str]] = {
    CVA_DTYPE_FLOAT16: "<f2",
    CVA_DTYPE_FLOAT32: "<f4",
    CVA_DTYPE_FLOAT64: "<f8",
}

#: Association IDs, following CVF's convention that the volumetric/element-like
#: association is 0 and the nodal/point-like association is 1.
CVA_ASSOC_ELEMENT: Final = 0
CVA_ASSOC_VERTEX: Final = 1

CVA_ASSOC_NAMES: Final[dict[int, str]] = {
    CVA_ASSOC_ELEMENT: "mesh-element",
    CVA_ASSOC_VERTEX: "mesh-vertex",
}

CVA_FLAG_FRAME_STATISTICS: Final = 1 << 0
CVA_FLAG_GLOBAL_STATISTICS: Final = 1 << 1
CVA_KNOWN_FLAGS: Final = CVA_FLAG_FRAME_STATISTICS | CVA_FLAG_GLOBAL_STATISTICS

#: 1 scalar, 3 vector, 6 symmetric tensor, 9 full tensor (spec 6).
VALID_COMPONENT_COUNTS: Final[tuple[int, ...]] = (1, 3, 6, 9)

#: Normative order for the 6-component symmetric tensor (spec 6). Writing a
#: stress tensor in Voigt's other common order (XX, YY, ZZ, YZ, XZ, XY) is the
#: classic way to make two solvers silently disagree, so it is spelled out here
#: and asserted by the round-trip test.
SYMMETRIC_TENSOR_COMPONENT_ORDER: Final[tuple[str, ...]] = (
    "XX", "YY", "ZZ", "XY", "YZ", "XZ",
)

#: Row-major order for the 9-component full tensor.
FULL_TENSOR_COMPONENT_ORDER: Final[tuple[str, ...]] = (
    "XX", "XY", "XZ", "YX", "YY", "YZ", "ZX", "ZY", "ZZ",
)

#: ``magic, major, minor, endian, flags, headerBytes, frameIndex,``
#: ``fieldNumericId, simulationTime, valueCount, componentCount, dataType,``
#: ``association, codec, payloadCrc32c, payloadOffset, compressedBytes,``
#: ``uncompressedBytes, headerCrc32c, reserved, statisticsOffset``
_HEADER_STRUCT: Final = struct.Struct("<8sHHIIIIIdQBBBBIQQQIIQ")
assert _HEADER_STRUCT.size == CVA_HEADER_BYTES

#: Byte range of ``headerCrc32c``, zeroed while the header CRC is computed.
_CRC_FIELD_SPAN: Final = (80, 84)

_ALIGNMENT: Final = 8

_DTYPE_IDS_BY_NAME: Final[dict[str, int]] = {
    name: code for code, name in CVA_DTYPE_NAMES.items()
}
_ASSOC_IDS_BY_NAME: Final[dict[str, int]] = {
    "mesh-element": CVA_ASSOC_ELEMENT,
    "element": CVA_ASSOC_ELEMENT,
    "cell": CVA_ASSOC_ELEMENT,
    "mesh-vertex": CVA_ASSOC_VERTEX,
    "vertex": CVA_ASSOC_VERTEX,
    "node": CVA_ASSOC_VERTEX,
    "point": CVA_ASSOC_VERTEX,
}


class CVAError(Exception):
    """Base class for every error raised by this module."""


class CVAFormatError(CVAError):
    """A ``.cva`` file is malformed, truncated, or internally inconsistent.

    Carries the offending file and, where the problem is localised, the byte
    offset — spec section 10 requires a corrupt case to name both.

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


def _resolve_dtype(dtype: int | str | np.dtype) -> int:
    """Map a dtype name, numpy dtype, or numeric ID to a CVA dataType ID."""
    if isinstance(dtype, (int, np.integer)) and not isinstance(dtype, bool):
        code = int(dtype)
        if code in CVA_DTYPE_NAMES:
            return code
        raise CVAError(
            f"unknown CVA dataType ID {code}; supported: "
            f"{sorted(CVA_DTYPE_NAMES)} ({sorted(CVA_DTYPE_NAMES.values())})"
        )
    name = np.dtype(dtype).name if not isinstance(dtype, str) else dtype.strip().lower()
    if name in _DTYPE_IDS_BY_NAME:
        return _DTYPE_IDS_BY_NAME[name]
    raise CVAError(
        f"unsupported CVA storage type {dtype!r}; CVA stores "
        f"{sorted(_DTYPE_IDS_BY_NAME)} only"
    )


def _resolve_association(association: int | str) -> int:
    """Map ``'mesh-vertex'``/``'mesh-element'`` (or an ID) to an association ID."""
    if isinstance(association, (int, np.integer)) and not isinstance(association, bool):
        code = int(association)
        if code in CVA_ASSOC_NAMES:
            return code
        raise CVAError(
            f"unknown CVA association ID {code}; supported: "
            f"{CVA_ASSOC_ELEMENT} (mesh-element), {CVA_ASSOC_VERTEX} (mesh-vertex)"
        )
    key = str(association).strip().lower()
    if key in _ASSOC_IDS_BY_NAME:
        return _ASSOC_IDS_BY_NAME[key]
    raise CVAError(
        f"unknown CVA association {association!r}; expected 'mesh-vertex' or "
        "'mesh-element'"
    )


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------

@dataclass(slots=True)
class ArrayStatistics:
    """Per-component statistics over a CVA array, ignoring ``NaN``.

    Serialized layout, ``C = componentCount`` (32 bytes per component plus an
    8-byte count)::

        0          uint64  valueCount    entities covered by these statistics
        8          float64 minimum[C]
        8 + 8C     float64 maximum[C]
        8 + 16C    float64 mean[C]
        8 + 24C    uint64  validCount[C] non-NaN samples per component

    Statistics are always float64 regardless of payload storage type: they are
    metadata, and widening costs 32 bytes per component while keeping a float16
    array's extremes exactly representable.

    Following CVF section 4.4.7, a component with no valid samples stores
    ``minimum = +inf`` and ``maximum = -inf`` — the "no valid data" sentinel a
    reader can test without a separate flag — and ``mean = NaN``.

    Attributes:
        minimum: ``(C,)`` float64 per-component minimum.
        maximum: ``(C,)`` float64 per-component maximum.
        mean: ``(C,)`` float64 per-component arithmetic mean of valid samples.
        valid_count: ``(C,)`` uint64 count of non-NaN samples per component.
        value_count: Number of entities these statistics cover. For frame
            statistics this equals the header's ``valueCount``; for global
            statistics it is the sum across the frames that were merged.
    """

    minimum: np.ndarray
    maximum: np.ndarray
    mean: np.ndarray
    valid_count: np.ndarray
    value_count: int

    @property
    def component_count(self) -> int:
        """Number of components these statistics describe."""
        return int(self.minimum.shape[0])

    @property
    def nan_count(self) -> np.ndarray:
        """``(C,)`` int64 count of NaN samples per component."""
        return self.value_count - np.asarray(self.valid_count, dtype=np.int64)

    @classmethod
    def from_values(cls, values: np.ndarray) -> ArrayStatistics:
        """Compute statistics from a ``(valueCount, componentCount)`` array.

        NaN is excluded (spec 1.7). ``numpy``'s ``nanmin``/``nanmean`` are
        deliberately avoided: they emit ``RuntimeWarning`` for an all-NaN
        component, and this package treats ``RuntimeWarning`` as an error, so
        the masked reduction is written out by hand.

        Args:
            values: 2-D array of stored values.

        Returns:
            Statistics over ``values``, in float64.
        """
        data = np.asarray(values, dtype=np.float64)
        if data.ndim != 2:
            raise CVAError(f"statistics input must be 2-D; got shape {data.shape}")
        value_count, components = data.shape

        valid = ~np.isnan(data)
        counts = valid.sum(axis=0).astype(np.uint64)

        # Neutral fills so the reductions never see NaN; components with no
        # valid samples fall back to the +inf / -inf sentinel below.
        minimum = np.where(valid, data, np.inf).min(axis=0) if value_count else np.full(
            components, np.inf, dtype=np.float64
        )
        maximum = np.where(valid, data, -np.inf).max(axis=0) if value_count else np.full(
            components, -np.inf, dtype=np.float64
        )
        totals = np.where(valid, data, 0.0).sum(axis=0)

        counts_f = counts.astype(np.float64)
        mean = np.full(components, np.nan, dtype=np.float64)
        nonempty = counts_f > 0.0
        mean[nonempty] = totals[nonempty] / counts_f[nonempty]
        minimum = np.where(nonempty, minimum, np.inf)
        maximum = np.where(nonempty, maximum, -np.inf)

        return cls(
            minimum=np.ascontiguousarray(minimum, dtype="<f8"),
            maximum=np.ascontiguousarray(maximum, dtype="<f8"),
            mean=np.ascontiguousarray(mean, dtype="<f8"),
            valid_count=np.ascontiguousarray(counts, dtype="<u8"),
            value_count=int(value_count),
        )

    def merge(self, other: ArrayStatistics) -> ArrayStatistics:
        """Combine two statistics blocks, e.g. to accumulate a global from frames.

        The mean is recovered exactly by re-weighting each side by its own valid
        count, so merging frame-by-frame gives the same answer as one pass over
        the concatenated data.

        Raises:
            CVAError: If the two blocks describe different component counts.
        """
        if self.component_count != other.component_count:
            raise CVAError(
                f"cannot merge statistics with {self.component_count} components into "
                f"{other.component_count} components"
            )
        a_counts = np.asarray(self.valid_count, dtype=np.float64)
        b_counts = np.asarray(other.valid_count, dtype=np.float64)
        total = a_counts + b_counts

        mean = np.full(self.component_count, np.nan, dtype=np.float64)
        nonempty = total > 0.0
        a_sum = np.where(a_counts > 0.0, np.nan_to_num(self.mean, nan=0.0) * a_counts, 0.0)
        b_sum = np.where(b_counts > 0.0, np.nan_to_num(other.mean, nan=0.0) * b_counts, 0.0)
        mean[nonempty] = (a_sum + b_sum)[nonempty] / total[nonempty]

        return ArrayStatistics(
            minimum=np.ascontiguousarray(
                np.minimum(self.minimum, other.minimum), dtype="<f8"
            ),
            maximum=np.ascontiguousarray(
                np.maximum(self.maximum, other.maximum), dtype="<f8"
            ),
            mean=np.ascontiguousarray(mean, dtype="<f8"),
            valid_count=np.ascontiguousarray(total.astype(np.uint64), dtype="<u8"),
            value_count=int(self.value_count) + int(other.value_count),
        )

    @staticmethod
    def section_bytes(component_count: int) -> int:
        """Serialized size of one statistics section for ``component_count``."""
        return 8 + 32 * int(component_count)

    def pack(self) -> bytes:
        """Serialize to ``8 + 32 * componentCount`` little-endian bytes."""
        components = self.component_count
        for name, array in (
            ("maximum", self.maximum),
            ("mean", self.mean),
            ("valid_count", self.valid_count),
        ):
            if np.asarray(array).shape != (components,):
                raise CVAError(
                    f"statistics field {name!r} has shape "
                    f"{np.asarray(array).shape}, expected ({components},)"
                )
        return b"".join(
            (
                struct.pack("<Q", int(self.value_count)),
                np.ascontiguousarray(self.minimum, dtype="<f8").tobytes(),
                np.ascontiguousarray(self.maximum, dtype="<f8").tobytes(),
                np.ascontiguousarray(self.mean, dtype="<f8").tobytes(),
                np.ascontiguousarray(self.valid_count, dtype="<u8").tobytes(),
            )
        )

    @classmethod
    def unpack(cls, data: bytes, component_count: int) -> ArrayStatistics:
        """Parse one statistics section.

        Args:
            data: At least ``section_bytes(component_count)`` bytes.
            component_count: Components the section describes.

        Raises:
            CVAError: If ``data`` is shorter than the section.
        """
        needed = cls.section_bytes(component_count)
        if len(data) < needed:
            raise CVAError(
                f"statistics section needs {needed} bytes for {component_count} "
                f"components, got {len(data)}"
            )
        (value_count,) = struct.unpack_from("<Q", data, 0)
        cursor = 8
        floats = []
        for _ in range(3):
            floats.append(
                np.frombuffer(data, dtype="<f8", count=component_count, offset=cursor).copy()
            )
            cursor += 8 * component_count
        counts = np.frombuffer(
            data, dtype="<u8", count=component_count, offset=cursor
        ).copy()
        return cls(
            minimum=floats[0],
            maximum=floats[1],
            mean=floats[2],
            valid_count=counts,
            value_count=int(value_count),
        )


# ---------------------------------------------------------------------------
# Header
# ---------------------------------------------------------------------------

@dataclass(slots=True)
class CVAHeader:
    """The fixed 96-byte CVA header. See the module docstring for the layout."""

    value_count: int = 0
    component_count: int = 1
    data_type: int = CVA_DTYPE_FLOAT32
    association: int = CVA_ASSOC_VERTEX
    codec: int = CODEC_NONE
    frame_index: int = 0
    field_numeric_id: int = 0
    simulation_time: float = 0.0
    flags: int = 0
    payload_offset: int = 0
    compressed_bytes: int = 0
    uncompressed_bytes: int = 0
    statistics_offset: int = 0
    payload_crc32c: int = 0
    header_crc32c: int = 0
    major_version: int = CVA_MAJOR_VERSION
    minor_version: int = CVA_MINOR_VERSION

    @property
    def numpy_dtype(self) -> str:
        """Explicit little-endian numpy dtype string for the stored payload."""
        return CVA_DTYPE_NUMPY[self.data_type]

    @property
    def item_size(self) -> int:
        """Bytes per stored scalar."""
        return int(np.dtype(self.numpy_dtype).itemsize)

    @property
    def data_type_name(self) -> str:
        """Human-readable storage type, e.g. ``'float32'``."""
        return CVA_DTYPE_NAMES[self.data_type]

    @property
    def association_name(self) -> str:
        """``'mesh-vertex'`` or ``'mesh-element'``."""
        return CVA_ASSOC_NAMES[self.association]

    @property
    def codec_name(self) -> str:
        """Codec name, e.g. ``'zlib'``."""
        return CODEC_NAMES.get(self.codec, f"unknown({self.codec})")

    @property
    def has_frame_statistics(self) -> bool:
        """Whether a per-frame statistics section is stored."""
        return bool(self.flags & CVA_FLAG_FRAME_STATISTICS)

    @property
    def has_global_statistics(self) -> bool:
        """Whether a case-global statistics section is stored."""
        return bool(self.flags & CVA_FLAG_GLOBAL_STATISTICS)

    @property
    def expected_uncompressed_bytes(self) -> int:
        """``valueCount * componentCount * sizeof(dataType)`` (spec 4.4.4 analogue)."""
        return self.value_count * self.component_count * self.item_size

    def pack(self) -> bytes:
        """Serialize to exactly 96 bytes, computing and storing ``headerCrc32c``.

        Returns:
            The 96 header bytes, little-endian.
        """
        blank = _HEADER_STRUCT.pack(
            CVA_MAGIC,
            self.major_version,
            self.minor_version,
            CVA_ENDIAN_MARKER,
            self.flags,
            CVA_HEADER_BYTES,
            self.frame_index,
            self.field_numeric_id,
            float(self.simulation_time),
            self.value_count,
            self.component_count,
            self.data_type,
            self.association,
            self.codec,
            self.payload_crc32c,
            self.payload_offset,
            self.compressed_bytes,
            self.uncompressed_bytes,
            0,  # headerCrc32c, zero while checksumming
            0,  # reserved
            self.statistics_offset,
        )
        self.header_crc32c = crc32c(blank)
        lo, hi = _CRC_FIELD_SPAN
        return blank[:lo] + struct.pack("<I", self.header_crc32c) + blank[hi:]

    @classmethod
    def unpack(cls, data: bytes, *, path: Path | str | None = None,
               verify_crc: bool = True) -> CVAHeader:
        """Parse and validate 96 header bytes.

        Args:
            data: At least 96 bytes; trailing bytes are ignored.
            path: File the bytes came from, for error messages.
            verify_crc: Check ``headerCrc32c``. Only a validator that reports
                corruption instead of raising should pass ``False``.

        Returns:
            The parsed header.

        Raises:
            CVAFormatError: On short input, bad magic, foreign byte order,
                unsupported major version, wrong ``headerBytes``, unknown flag
                bits, an illegal enum value, or a header CRC mismatch.
            UnsupportedCodecError: If ``codec`` is the reserved zstd ID, with
                the exact message the spec mandates.
        """
        if len(data) < CVA_HEADER_BYTES:
            raise CVAFormatError(
                f"file is {len(data)} bytes, shorter than the {CVA_HEADER_BYTES}-byte "
                "CVA header",
                path=path,
                offset=0,
            )
        blob = bytes(data[:CVA_HEADER_BYTES])
        (
            magic,
            major,
            minor,
            endian,
            flags,
            header_bytes,
            frame_index,
            field_numeric_id,
            simulation_time,
            value_count,
            component_count,
            data_type,
            association,
            codec,
            payload_crc,
            payload_offset,
            compressed_bytes,
            uncompressed_bytes,
            stored_crc,
            reserved,
            statistics_offset,
        ) = _HEADER_STRUCT.unpack(blob)

        if magic != CVA_MAGIC:
            raise CVAFormatError(
                f"bad magic {magic!r}; expected {CVA_MAGIC!r}. This is not a CVA array "
                "file.",
                path=path,
                offset=0,
            )
        if endian != CVA_ENDIAN_MARKER:
            # Spec 1.1: reject foreign byte order, never byte-swap it.
            raise CVAFormatError(
                f"byte order not supported: endianMarker is 0x{endian:08X}, expected "
                f"0x{CVA_ENDIAN_MARKER:08X}. CFDViz is little-endian only.",
                path=path,
                offset=12,
            )
        if major != CVA_MAJOR_VERSION:
            raise CVAFormatError(
                f"unsupported major version {major}; this reader implements CVA "
                f"{CVA_MAJOR_VERSION}.x",
                path=path,
                offset=8,
            )
        if header_bytes != CVA_HEADER_BYTES:
            raise CVAFormatError(
                f"headerBytes is {header_bytes}, expected {CVA_HEADER_BYTES}",
                path=path,
                offset=20,
            )
        unknown = flags & ~CVA_KNOWN_FLAGS
        if unknown:
            raise CVAFormatError(
                f"unknown flag bits 0x{unknown:08X} set; this file uses a CVA feature "
                f"this reader does not implement (flags=0x{flags:08X})",
                path=path,
                offset=16,
            )
        if reserved != 0:
            raise CVAFormatError(
                "reserved header bytes [84, 88) are not zero", path=path, offset=84
            )
        if component_count not in VALID_COMPONENT_COUNTS:
            raise CVAFormatError(
                f"componentCount {component_count} is not one of "
                f"{VALID_COMPONENT_COUNTS} (1 scalar, 3 vector, 6 symmetric tensor, "
                "9 full tensor)",
                path=path,
                offset=48,
            )
        if data_type not in CVA_DTYPE_NAMES:
            raise CVAFormatError(
                f"unknown dataType {data_type}; CVA supports "
                f"{sorted(CVA_DTYPE_NAMES.values())}",
                path=path,
                offset=49,
            )
        if association not in CVA_ASSOC_NAMES:
            raise CVAFormatError(
                f"unknown association {association}; expected "
                f"{CVA_ASSOC_ELEMENT} (mesh-element) or {CVA_ASSOC_VERTEX} (mesh-vertex)",
                path=path,
                offset=50,
            )
        if codec == CODEC_ZSTD:
            # Spec 7 fixes this user-facing wording; the Unreal reader emits the
            # identical string. Never fall back to another codec.
            raise UnsupportedCodecError(ZSTD_REJECTION_MESSAGE)
        if codec not in CODEC_NAMES:
            # zstd is excluded from the suggestion list: it is a known ID, but
            # it was already rejected above and must never be recommended.
            usable = sorted(
                name for code, name in CODEC_NAMES.items() if code != CODEC_ZSTD
            )
            raise CVAFormatError(
                f"unknown codec ID {codec}; supported: {usable}",
                path=path,
                offset=51,
            )
        if not (flags & CVA_FLAG_FRAME_STATISTICS) and not (
            flags & CVA_FLAG_GLOBAL_STATISTICS
        ) and statistics_offset != 0:
            raise CVAFormatError(
                f"statisticsOffset is {statistics_offset} but no statistics flag is "
                "set; absent sections require offset 0",
                path=path,
                offset=88,
            )

        header = cls(
            value_count=value_count,
            component_count=component_count,
            data_type=data_type,
            association=association,
            codec=codec,
            frame_index=frame_index,
            field_numeric_id=field_numeric_id,
            simulation_time=simulation_time,
            flags=flags,
            payload_offset=payload_offset,
            compressed_bytes=compressed_bytes,
            uncompressed_bytes=uncompressed_bytes,
            statistics_offset=statistics_offset,
            payload_crc32c=payload_crc,
            header_crc32c=stored_crc,
            major_version=major,
            minor_version=minor,
        )

        if verify_crc:
            lo, hi = _CRC_FIELD_SPAN
            computed = crc32c(blob[:lo] + b"\x00" * (hi - lo) + blob[hi:])
            if computed != stored_crc:
                raise CVAFormatError(
                    f"header CRC-32C mismatch: stored 0x{stored_crc:08X}, computed "
                    f"0x{computed:08X}. The header is corrupt.",
                    path=path,
                    offset=lo,
                )
        return header


@dataclass(slots=True)
class CVAData:
    """A decoded CVA array.

    Attributes:
        values: ``(valueCount, componentCount)`` in the file's storage dtype,
            bit-exact — including ``NaN``. Not widened, so a reader can hash it
            against the writer's bytes.
        header: The header exactly as stored on disk.
        frame_statistics: Per-frame statistics, or ``None``.
        global_statistics: Case-global statistics, or ``None``.
        path: Where the array was read from, for diagnostics.
    """

    values: np.ndarray
    header: CVAHeader = field(default_factory=CVAHeader)
    frame_statistics: ArrayStatistics | None = None
    global_statistics: ArrayStatistics | None = None
    path: Path | None = None

    @property
    def value_count(self) -> int:
        """Number of entities (vertices or elements)."""
        return int(self.values.shape[0])

    @property
    def component_count(self) -> int:
        """Components per entity."""
        return int(self.values.shape[1])

    @property
    def frame_index(self) -> int:
        """Frame this array belongs to."""
        return self.header.frame_index

    @property
    def simulation_time(self) -> float:
        """Physical time of this frame, in the manifest's time unit."""
        return self.header.simulation_time

    @property
    def component_names(self) -> tuple[str, ...]:
        """Normative component labels for this array's component count."""
        if self.component_count == 1:
            return ("",)
        if self.component_count == 3:
            return ("X", "Y", "Z")
        if self.component_count == 6:
            return SYMMETRIC_TENSOR_COMPONENT_ORDER
        return FULL_TENSOR_COMPONENT_ORDER


# ---------------------------------------------------------------------------
# Writing
# ---------------------------------------------------------------------------

def _to_storage(values: object, dtype_id: int) -> np.ndarray:
    """Coerce ``values`` to a contiguous 2-D array in the stored dtype.

    A finite input that becomes infinite in the target type is rejected rather
    than stored: spec 1.6 forbids silent clamping, and turning 1e300 into
    ``inf`` because someone chose float16 storage is exactly the kind of data
    loss that must surface at export time, not in a plot six months later.
    ``NaN`` is not overflow and passes through untouched (spec 1.7).
    """
    target = CVA_DTYPE_NUMPY[dtype_id]
    source = np.asarray(values)
    if source.ndim == 1:
        source = source.reshape(-1, 1)
    if source.ndim != 2:
        raise CVAError(
            f"values must be 1-D or 2-D (valueCount, componentCount); got shape "
            f"{source.shape}"
        )
    if not np.issubdtype(source.dtype, np.floating) and not np.issubdtype(
        source.dtype, np.integer
    ):
        raise CVAError(f"values must be numeric; got dtype {source.dtype}")

    if source.dtype == np.dtype(target):
        return np.ascontiguousarray(source)

    # errstate: the overflow is detected explicitly below with a message that
    # names the value, which beats numpy's context-free RuntimeWarning.
    with np.errstate(over="ignore", invalid="ignore"):
        out = source.astype(target, copy=True)
    overflowed = np.isfinite(source) & ~np.isfinite(out)
    if bool(overflowed.any()):
        flat = int(np.argmax(overflowed.reshape(-1)))
        row, column = divmod(flat, source.shape[1])
        raise CVAError(
            f"value {source.reshape(-1)[flat]!r} at entity {row}, component {column} "
            f"overflows {CVA_DTYPE_NAMES[dtype_id]} storage and would become infinite. "
            "Choose a wider storage type; CFDViz must not silently clamp values."
        )
    return np.ascontiguousarray(out)


def write_cva(
    path: Path | str,
    *,
    values: object,
    frame_index: int = 0,
    simulation_time: float = 0.0,
    association: int | str = CVA_ASSOC_VERTEX,
    field_numeric_id: int = 0,
    dtype: int | str | np.dtype = "float32",
    codec: int = DEFAULT_CODEC,
    level: int | None = None,
    compute_statistics: bool = True,
    global_statistics: ArrayStatistics | None = None,
) -> CVAHeader:
    """Write one mesh-associated array for one frame.

    Args:
        path: Destination ``.cva`` file. Parent directories are created.
        values: ``(valueCount,)`` or ``(valueCount, componentCount)``. The
            component count must be 1, 3, 6, or 9; a 6-component array is
            interpreted in the normative order
            :data:`SYMMETRIC_TENSOR_COMPONENT_ORDER`.
        frame_index: Frame this array belongs to.
        simulation_time: Physical time of the frame, in manifest time units.
        association: ``'mesh-vertex'`` / ``'mesh-element'`` or the numeric ID.
        field_numeric_id: The manifest ``numericId`` of the field.
        dtype: Storage type — ``'float16'``, ``'float32'``, ``'float64'``, or a
            ``CVA_DTYPE_*`` constant.
        codec: Payload codec; defaults to zlib (spec 7). zstd is rejected.
        level: Codec level; ``None`` uses the documented default.
        compute_statistics: Compute and store per-frame statistics over the
            **stored** (post-quantization) values, so a validator recomputing
            them from the file gets the identical numbers.
        global_statistics: Optional case-global statistics, typically built by
            merging every frame's :class:`ArrayStatistics`.

    Returns:
        The :class:`CVAHeader` that was written, with final offsets and CRCs.

    Raises:
        CVAError: On an illegal component count, non-numeric values, or a value
            that would overflow the chosen storage type.
        UnsupportedCodecError: If ``codec`` is zstd or an unavailable codec.
    """
    path = Path(path)
    dtype_id = _resolve_dtype(dtype)
    association_id = _resolve_association(association)

    stored = _to_storage(values, dtype_id)
    value_count, component_count = stored.shape
    if component_count not in VALID_COMPONENT_COUNTS:
        raise CVAError(
            f"componentCount {component_count} is not one of {VALID_COMPONENT_COUNTS} "
            "(1 scalar, 3 vector, 6 symmetric tensor, 9 full tensor)"
        )

    frame_stats: ArrayStatistics | None = None
    if compute_statistics:
        # Deliberately over the STORED values: a float16 export must declare the
        # extremes a reader will actually find in the file.
        frame_stats = ArrayStatistics.from_values(stored)
    if global_statistics is not None and (
        global_statistics.component_count != component_count
    ):
        raise CVAError(
            f"global_statistics describes {global_statistics.component_count} "
            f"components but the array has {component_count}"
        )

    raw = stored.tobytes()
    payload = compress(raw, codec, level)
    # A codec that grows the data is pointless; fall back so the file is never
    # larger than the values it holds.
    effective_codec = codec
    if codec != CODEC_NONE and len(payload) >= len(raw):
        payload = raw
        effective_codec = CODEC_NONE

    flags = 0
    if frame_stats is not None:
        flags |= CVA_FLAG_FRAME_STATISTICS
    if global_statistics is not None:
        flags |= CVA_FLAG_GLOBAL_STATISTICS

    header = CVAHeader(
        value_count=int(value_count),
        component_count=int(component_count),
        data_type=dtype_id,
        association=association_id,
        codec=effective_codec,
        frame_index=int(frame_index),
        field_numeric_id=int(field_numeric_id),
        simulation_time=float(simulation_time),
        flags=flags,
        compressed_bytes=len(payload),
        uncompressed_bytes=len(raw),
        payload_crc32c=crc32c(payload),  # over the COMPRESSED bytes, as stored
    )

    chunks: list[bytes] = []
    cursor = CVA_HEADER_BYTES

    def place(blob: bytes) -> int:
        nonlocal cursor
        padding = _align_up(cursor) - cursor
        if padding:
            chunks.append(b"\x00" * padding)
            cursor += padding
        offset = cursor
        chunks.append(blob)
        cursor += len(blob)
        return offset

    stats_blob = b""
    if frame_stats is not None:
        stats_blob += frame_stats.pack()
    if global_statistics is not None:
        stats_blob += global_statistics.pack()
    if stats_blob:
        header.statistics_offset = place(stats_blob)
    header.payload_offset = place(payload)

    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(header.pack())
        for chunk in chunks:
            handle.write(chunk)
    return header


# ---------------------------------------------------------------------------
# Reading
# ---------------------------------------------------------------------------

def read_cva(path: Path | str, *, verify_crc: bool = True) -> CVAData:
    """Read a mesh-associated array, validating it against the file on disk.

    Every offset and size is checked against the real file length before any
    buffer is allocated (spec 1.5), and ``uncompressedBytes`` is checked against
    ``valueCount * componentCount * sizeof(dataType)`` before decompression —
    the primary defence against a hostile size field.

    Args:
        path: The ``.cva`` file.
        verify_crc: Verify the header CRC and the payload CRC. Leave this on
            outside validators that want to report corruption rather than raise.

    Returns:
        The decoded :class:`CVAData`. ``values`` keeps the file's storage dtype,
        so ``NaN`` payloads compare bit-exactly against what was written.

    Raises:
        CVAFormatError: If the file is missing, truncated, has a bad magic,
            foreign byte order, an unsupported version, an illegal enum value,
            an inconsistent size, or a CRC mismatch. The message always names
            the file and, where meaningful, the byte offset.
        UnsupportedCodecError: If the payload uses the reserved zstd codec.
        CodecError: If decompression fails.
    """
    path = Path(path)
    try:
        blob = path.read_bytes()
    except OSError as exc:
        raise CVAFormatError(f"cannot read array: {exc}", path=path) from exc

    header = CVAHeader.unpack(blob, path=path, verify_crc=verify_crc)

    # Spec 4.4.4 analogue: prove the declared size before allocating anything.
    expected = header.expected_uncompressed_bytes
    if header.uncompressed_bytes != expected:
        raise CVAFormatError(
            f"uncompressedBytes is {header.uncompressed_bytes}, but valueCount "
            f"{header.value_count} x componentCount {header.component_count} x "
            f"sizeof({header.data_type_name}) = {expected}. The header is inconsistent.",
            path=path,
            offset=72,
        )
    if header.payload_offset < CVA_HEADER_BYTES and header.compressed_bytes > 0:
        raise CVAFormatError(
            f"payloadOffset {header.payload_offset} overlaps the "
            f"{CVA_HEADER_BYTES}-byte header",
            path=path,
            offset=56,
        )
    if header.payload_offset > len(blob) or header.compressed_bytes > len(blob) - header.payload_offset:
        raise CVAFormatError(
            f"payload needs {header.compressed_bytes} bytes at offset "
            f"{header.payload_offset}, but the file is only {len(blob)} bytes; it is "
            "truncated or the header is corrupt",
            path=path,
            offset=56,
        )
    if header.codec == CODEC_NONE and header.compressed_bytes != header.uncompressed_bytes:
        raise CVAFormatError(
            f"codec is 'none' but compressedBytes {header.compressed_bytes} != "
            f"uncompressedBytes {header.uncompressed_bytes}",
            path=path,
            offset=64,
        )

    payload = blob[header.payload_offset : header.payload_offset + header.compressed_bytes]
    if verify_crc:
        computed = crc32c(payload)
        if computed != header.payload_crc32c:
            raise CVAFormatError(
                f"payload CRC-32C mismatch: stored 0x{header.payload_crc32c:08X}, "
                f"computed 0x{computed:08X}. The payload is corrupt.",
                path=path,
                offset=header.payload_offset,
            )

    try:
        raw = decompress(payload, header.codec, header.uncompressed_bytes)
    except (CodecError, UnsupportedCodecError) as exc:
        if isinstance(exc, UnsupportedCodecError):
            raise
        raise CVAFormatError(
            f"payload could not be decoded with codec '{header.codec_name}': {exc}",
            path=path,
            offset=header.payload_offset,
        ) from exc

    values = np.frombuffer(
        raw,
        dtype=header.numpy_dtype,
        count=header.value_count * header.component_count,
    ).reshape(header.value_count, header.component_count).copy()

    frame_stats, global_stats = _read_statistics(blob, header, path)

    return CVAData(
        values=values,
        header=header,
        frame_statistics=frame_stats,
        global_statistics=global_stats,
        path=path,
    )


def _read_statistics(
    blob: bytes, header: CVAHeader, path: Path
) -> tuple[ArrayStatistics | None, ArrayStatistics | None]:
    """Decode the optional statistics sections, bounds-checked against the file."""
    if not header.has_frame_statistics and not header.has_global_statistics:
        return None, None

    section = ArrayStatistics.section_bytes(header.component_count)
    sections = int(header.has_frame_statistics) + int(header.has_global_statistics)
    needed = section * sections
    offset = header.statistics_offset
    if offset < CVA_HEADER_BYTES:
        raise CVAFormatError(
            f"statisticsOffset {offset} overlaps the {CVA_HEADER_BYTES}-byte header",
            path=path,
            offset=88,
        )
    if offset > len(blob) or needed > len(blob) - offset:
        raise CVAFormatError(
            f"statistics need {needed} bytes at offset {offset}, but the file is only "
            f"{len(blob)} bytes; it is truncated or the header is corrupt",
            path=path,
            offset=88,
        )

    cursor = offset
    frame_stats = None
    if header.has_frame_statistics:
        frame_stats = ArrayStatistics.unpack(
            blob[cursor : cursor + section], header.component_count
        )
        cursor += section
        if frame_stats.value_count != header.value_count:
            raise CVAFormatError(
                f"frame statistics cover {frame_stats.value_count} entities but the "
                f"header declares valueCount {header.value_count}",
                path=path,
                offset=offset,
            )
    global_stats = None
    if header.has_global_statistics:
        global_stats = ArrayStatistics.unpack(
            blob[cursor : cursor + section], header.component_count
        )
    return frame_stats, global_stats


def merge_statistics(blocks: Sequence[ArrayStatistics]) -> ArrayStatistics:
    """Fold per-frame statistics into one case-global block.

    Args:
        blocks: One or more per-frame :class:`ArrayStatistics`, all with the
            same component count.

    Returns:
        The combined statistics, suitable for ``write_cva(global_statistics=...)``.

    Raises:
        CVAError: If ``blocks`` is empty or the component counts disagree.
    """
    if not blocks:
        raise CVAError("merge_statistics needs at least one statistics block")
    combined = blocks[0]
    for block in blocks[1:]:
        combined = combined.merge(block)
    return combined
