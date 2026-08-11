"""Read FluidX3D's legacy binary VTK structured-point exports.

FluidX3D writes one field per file using VTK 3.0 ``STRUCTURED_POINTS``.  The
payload is big-endian, X-fastest, and component-interleaved even though CFDViz
stores little-endian arrays in an ``(X, Y, Z, C)`` numpy shape.  This module is
only the byte-level reader; physical field identity and units belong to the
FluidX3D converter because the VTK header calls every array ``data``.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path
from typing import Final

import numpy as np

__all__ = [
    "VTK_HEADER_LIMIT",
    "VTKError",
    "VTKLegacyHeader",
    "read_structured_points",
]

VTK_HEADER_LIMIT: Final = 4096
_MAX_DIMENSION: Final = 2_147_483_647
_MAX_COMPONENTS: Final = 4

_VTK_DTYPES: Final[dict[str, str]] = {
    "char": "i1",
    "unsigned_char": "u1",
    "short": ">i2",
    "unsigned_short": ">u2",
    "int": ">i4",
    "unsigned_int": ">u4",
    "long": ">i8",
    "unsigned_long": ">u8",
    "float": ">f4",
    "double": ">f8",
}


class VTKError(Exception):
    """A legacy VTK input is unsupported, malformed, or truncated."""


@dataclass(frozen=True)
class VTKLegacyHeader:
    """The declarations preceding one structured-points payload."""

    title: str
    dimensions: tuple[int, int, int]
    origin: tuple[float, float, float]
    spacing: tuple[float, float, float]
    point_count: int
    field_name: str
    scalar_type: str
    component_count: int
    header_bytes: int

    @property
    def value_count(self) -> int:
        return self.point_count * self.component_count

    @property
    def payload_bytes(self) -> int:
        return self.value_count * np.dtype(_VTK_DTYPES[self.scalar_type]).itemsize


def _problem(path: Path, message: str) -> VTKError:
    return VTKError(f"{path}: {message}")


def _tokens(
    line: str,
    *,
    path: Path,
    keyword: str,
    value_count: int,
) -> list[str]:
    parts = line.split()
    if not parts or parts[0] != keyword or len(parts) != value_count + 1:
        raise _problem(
            path,
            f"expected '{keyword}' followed by {value_count} value(s), got {line!r}",
        )
    return parts[1:]


def _integer(token: str, *, path: Path, label: str) -> int:
    try:
        value = int(token, 10)
    except ValueError as exc:
        raise _problem(path, f"{label} value {token!r} is not an integer") from exc
    return value


def _vector3(line: str, *, path: Path, keyword: str) -> tuple[float, float, float]:
    values: list[float] = []
    for token in _tokens(
        line,
        path=path,
        keyword=keyword,
        value_count=3,
    ):
        try:
            value = float(token)
        except ValueError as exc:
            raise _problem(path, f"{keyword} value {token!r} is not numeric") from exc
        if not math.isfinite(value):
            raise _problem(path, f"{keyword} must contain only finite values")
        values.append(value)
    return (values[0], values[1], values[2])


def _header_lines(prefix: bytes, *, path: Path) -> tuple[list[str], int]:
    raw_lines = prefix.splitlines(keepends=True)
    if len(raw_lines) < 10:
        if len(prefix) >= VTK_HEADER_LIMIT:
            detail = f"header exceeds the {VTK_HEADER_LIMIT}-byte safety limit"
        else:
            detail = "header ended before LOOKUP_TABLE"
        raise _problem(path, detail)

    selected = raw_lines[:10]
    if any(not line.endswith((b"\n", b"\r")) for line in selected):
        raise _problem(path, "header ended before LOOKUP_TABLE")

    header_bytes = sum(len(line) for line in selected)
    try:
        lines = [line.rstrip(b"\r\n").decode("ascii") for line in selected]
    except UnicodeDecodeError as exc:
        raise _problem(path, "header is not ASCII") from exc
    return lines, header_bytes


def _parse_header(prefix: bytes, *, path: Path) -> VTKLegacyHeader:
    lines, header_bytes = _header_lines(prefix, path=path)

    if lines[0] != "# vtk DataFile Version 3.0":
        raise _problem(
            path,
            "expected '# vtk DataFile Version 3.0' as the first line",
        )
    title = lines[1]
    if not title:
        raise _problem(path, "VTK title must not be empty")
    if lines[2] != "BINARY":
        raise _problem(path, f"FluidX3D input must be BINARY, got {lines[2]!r}")

    dataset = _tokens(
        lines[3],
        path=path,
        keyword="DATASET",
        value_count=1,
    )[0]
    if dataset != "STRUCTURED_POINTS":
        raise _problem(
            path,
            "FluidX3D volume input must use DATASET STRUCTURED_POINTS, "
            f"got {dataset!r}",
        )

    dimension_tokens = _tokens(
        lines[4],
        path=path,
        keyword="DIMENSIONS",
        value_count=3,
    )
    dimensions = tuple(
        _integer(token, path=path, label="DIMENSIONS")
        for token in dimension_tokens
    )
    if any(value < 1 or value > _MAX_DIMENSION for value in dimensions):
        raise _problem(
            path,
            f"DIMENSIONS must be integers in [1, {_MAX_DIMENSION}], got {dimensions}",
        )

    origin = _vector3(lines[5], path=path, keyword="ORIGIN")
    spacing = _vector3(lines[6], path=path, keyword="SPACING")
    if any(value <= 0 for value in spacing):
        raise _problem(path, f"SPACING must contain positive values, got {spacing}")

    point_count_token = _tokens(
        lines[7],
        path=path,
        keyword="POINT_DATA",
        value_count=1,
    )[0]
    point_count = _integer(
        point_count_token,
        path=path,
        label="POINT_DATA",
    )
    implied_count = math.prod(dimensions)
    if point_count != implied_count:
        raise _problem(
            path,
            f"POINT_DATA {point_count} but dimensions imply {implied_count}",
        )

    scalar_parts = lines[8].split()
    if len(scalar_parts) not in (3, 4) or scalar_parts[0] != "SCALARS":
        raise _problem(
            path,
            "expected 'SCALARS <name> <type> [componentCount]'",
        )
    field_name = scalar_parts[1]
    scalar_type = scalar_parts[2]
    if scalar_type not in _VTK_DTYPES:
        supported = ", ".join(sorted(_VTK_DTYPES))
        raise _problem(
            path,
            f"unsupported VTK scalar type {scalar_type!r}; supported: {supported}",
        )
    component_count = (
        _integer(scalar_parts[3], path=path, label="SCALARS component count")
        if len(scalar_parts) == 4
        else 1
    )
    if not 1 <= component_count <= _MAX_COMPONENTS:
        raise _problem(
            path,
            f"SCALARS component count must be in [1, {_MAX_COMPONENTS}], "
            f"got {component_count}",
        )

    lookup = _tokens(
        lines[9],
        path=path,
        keyword="LOOKUP_TABLE",
        value_count=1,
    )[0]
    if not lookup:
        raise _problem(path, "LOOKUP_TABLE name must not be empty")

    return VTKLegacyHeader(
        title=title,
        dimensions=(dimensions[0], dimensions[1], dimensions[2]),
        origin=origin,
        spacing=spacing,
        point_count=point_count,
        field_name=field_name,
        scalar_type=scalar_type,
        component_count=component_count,
        header_bytes=header_bytes,
    )


def _little_endian(flat: np.ndarray) -> np.ndarray:
    if flat.dtype.itemsize == 1:
        return flat.copy()
    return flat.byteswap().view(flat.dtype.newbyteorder("<"))


def read_structured_points(
    path: Path | str,
    *,
    max_payload_bytes: int | None = None,
) -> tuple[VTKLegacyHeader, np.ndarray]:
    """Read one FluidX3D VTK field as ``(X, Y, Z, C)``.

    File size and an optional payload cap are checked before numpy allocates the
    declared array.  The returned multi-byte dtype is explicitly little-endian;
    byte swapping, rather than numeric conversion, preserves NaN payload bits.
    """
    source = Path(path)
    try:
        file_size = source.stat().st_size
    except OSError as exc:
        raise _problem(source, str(exc)) from exc

    try:
        with source.open("rb") as stream:
            prefix = stream.read(VTK_HEADER_LIMIT)
    except OSError as exc:
        raise _problem(source, str(exc)) from exc

    header = _parse_header(prefix, path=source)
    expected_size = header.header_bytes + header.payload_bytes
    if file_size != expected_size:
        raise _problem(
            source,
            f"expected {expected_size} bytes from the header, found {file_size}",
        )
    if max_payload_bytes is not None:
        if max_payload_bytes < 0:
            raise ValueError("max_payload_bytes must be non-negative")
        if header.payload_bytes > max_payload_bytes:
            raise _problem(
                source,
                f"payload requires {header.payload_bytes} bytes, exceeding the "
                f"{max_payload_bytes}-byte limit",
            )

    disk_dtype = np.dtype(_VTK_DTYPES[header.scalar_type])
    try:
        flat = np.fromfile(
            source,
            dtype=disk_dtype,
            count=header.value_count,
            offset=header.header_bytes,
        )
    except (OSError, ValueError) as exc:
        raise _problem(source, f"could not read payload: {exc}") from exc
    if flat.size != header.value_count:
        raise _problem(
            source,
            f"payload decoded {flat.size} values; expected {header.value_count}",
        )

    little = _little_endian(flat)
    nx, ny, nz = header.dimensions
    values = little.reshape(
        (nz, ny, nx, header.component_count),
        order="C",
    ).transpose(2, 1, 0, 3)
    return header, values.copy(order="C")
