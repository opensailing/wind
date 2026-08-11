"""FluidX3D legacy binary VTK input.

The fixture bytes in this module follow ``Memory_Container<T>::write_vtk``
directly.  They are not written by the reader under test: a shared wrong idea
about byte order or axis order must not be able to round-trip successfully.
"""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np
import pytest

from conftest import assert_bits_equal, ramp
from cfdviz.vtk_legacy import VTKError, read_structured_points


def _vtk_bytes(
    values: np.ndarray,
    *,
    origin=(-1.5, -1.0, -0.5),
    spacing=(0.5, 1.25, 2.0),
    vtk_type="float",
    mode="BINARY",
    dataset="STRUCTURED_POINTS",
    point_count: int | None = None,
) -> bytes:
    """Build the exact one-array layout emitted by FluidX3D."""
    values = np.asarray(values)
    nx, ny, nz, components = values.shape
    count = nx * ny * nz if point_count is None else point_count
    header = (
        "# vtk DataFile Version 3.0\n"
        "FluidX3D test-field\n"
        f"{mode}\n"
        f"DATASET {dataset}\n"
        f"DIMENSIONS {nx} {ny} {nz}\n"
        f"ORIGIN {origin[0]} {origin[1]} {origin[2]}\n"
        f"SPACING {spacing[0]} {spacing[1]} {spacing[2]}\n"
        f"POINT_DATA {count}\n"
        f"SCALARS data {vtk_type} {components}\n"
        "LOOKUP_TABLE default\n"
    ).encode("ascii")

    disk_order = values.transpose(2, 1, 0, 3)
    disk_dtype = {
        "float": ">f4",
        "double": ">f8",
        "unsigned_char": "u1",
    }[vtk_type]
    return header + disk_order.astype(disk_dtype).tobytes(order="C")


def _write(path: Path, blob: bytes) -> Path:
    path.write_bytes(blob)
    return path


def test_fluidx3d_header_is_parsed_at_the_payload_boundary(tmp_path: Path):
    values = ramp((4, 3, 2, 3), "<f4")
    blob = _vtk_bytes(
        values,
        origin=(-9.55000020e1, -4.75e1, -4.75e1),
        spacing=(1.0, 1.0, 1.0),
    )
    path = _write(tmp_path / "u-000006000.vtk", blob)

    header, actual = read_structured_points(path)

    assert header.title == "FluidX3D test-field"
    assert header.dimensions == (4, 3, 2)
    assert header.origin == pytest.approx((-95.500002, -47.5, -47.5))
    assert header.spacing == (1.0, 1.0, 1.0)
    assert header.point_count == 24
    assert header.field_name == "data"
    assert header.scalar_type == "float"
    assert header.component_count == 3
    assert header.header_bytes == blob.index(b"LOOKUP_TABLE default\n") + len(
        b"LOOKUP_TABLE default\n"
    )
    assert actual.shape == (4, 3, 2, 3)


def test_big_endian_aos_payload_becomes_little_endian_xyz_components(
    tmp_path: Path,
):
    expected = ramp((4, 3, 2, 3), "<f4")
    path = _write(tmp_path / "u-000000123.vtk", _vtk_bytes(expected))

    _, actual = read_structured_points(path)

    assert actual.dtype == np.dtype("<f4")
    assert_bits_equal(actual, expected)
    assert actual[3, 2, 1, 2] == expected[3, 2, 1, 2]


def test_nan_payload_bits_survive_the_endian_conversion(tmp_path: Path):
    expected = np.array([0x7FC12345, 0x80000000], dtype="<u4").view("<f4")
    expected = expected.reshape(2, 1, 1, 1)
    header = _vtk_bytes(np.zeros_like(expected))
    boundary = header.index(b"LOOKUP_TABLE default\n") + len(
        b"LOOKUP_TABLE default\n"
    )
    payload = b"".join(
        struct.pack(">I", bits) for bits in (0x7FC12345, 0x80000000)
    )
    path = _write(tmp_path / "awkward.vtk", header[:boundary] + payload)

    _, actual = read_structured_points(path)

    assert_bits_equal(actual, expected)


def test_unsigned_char_flags_need_no_byte_swap(tmp_path: Path):
    expected = ramp((4, 3, 2, 1), "u1")
    path = _write(
        tmp_path / "flags-000000123.vtk",
        _vtk_bytes(expected, vtk_type="unsigned_char"),
    )

    header, actual = read_structured_points(path)

    assert header.scalar_type == "unsigned_char"
    assert actual.dtype == np.dtype("u1")
    assert_bits_equal(actual, expected)


def test_truncated_payload_is_rejected_before_reading(tmp_path: Path):
    blob = _vtk_bytes(ramp((4, 3, 2, 3), "<f4"))
    path = _write(tmp_path / "truncated.vtk", blob[:-4])

    with pytest.raises(VTKError, match=r"truncated\.vtk.*expected .* bytes.*found"):
        read_structured_points(path)


def test_dimensions_that_cannot_fit_the_file_are_rejected_before_allocation(
    tmp_path: Path,
):
    blob = _vtk_bytes(ramp((4, 3, 2, 1), "<f4"))
    blob = blob.replace(b"DIMENSIONS 4 3 2", b"DIMENSIONS 2147483647 3 2")
    blob = blob.replace(b"POINT_DATA 24", b"POINT_DATA 12884901882")
    path = _write(tmp_path / "lying-dimensions.vtk", blob)

    with pytest.raises(VTKError, match=r"lying-dimensions\.vtk.*expected .* bytes.*found"):
        read_structured_points(path)


def test_ascii_vtk_is_rejected_with_the_required_binary_mode(tmp_path: Path):
    path = _write(
        tmp_path / "ascii.vtk",
        _vtk_bytes(ramp((2, 2, 2, 1), "<f4"), mode="ASCII"),
    )

    with pytest.raises(VTKError, match=r"ascii\.vtk.*BINARY.*ASCII"):
        read_structured_points(path)


def test_non_structured_dataset_is_rejected_by_name(tmp_path: Path):
    path = _write(
        tmp_path / "mesh.vtk",
        _vtk_bytes(
            ramp((2, 2, 2, 1), "<f4"),
            dataset="POLYDATA",
        ),
    )

    with pytest.raises(VTKError, match=r"mesh\.vtk.*STRUCTURED_POINTS.*POLYDATA"):
        read_structured_points(path)


def test_point_count_must_equal_the_declared_dimensions(tmp_path: Path):
    path = _write(
        tmp_path / "point-count.vtk",
        _vtk_bytes(ramp((4, 3, 2, 1), "<f4"), point_count=23),
    )

    with pytest.raises(VTKError, match=r"POINT_DATA 23.*dimensions imply 24"):
        read_structured_points(path)


@pytest.mark.parametrize(
    "origin,spacing,problem",
    [
        ((float("nan"), 0.0, 0.0), (1.0, 1.0, 1.0), "ORIGIN"),
        ((0.0, 0.0, 0.0), (1.0, float("inf"), 1.0), "SPACING"),
        ((0.0, 0.0, 0.0), (1.0, 0.0, 1.0), "SPACING"),
    ],
)
def test_nonfinite_or_nonpositive_geometry_is_rejected(
    tmp_path: Path, origin, spacing, problem
):
    path = _write(
        tmp_path / "geometry.vtk",
        _vtk_bytes(
            ramp((2, 2, 2, 1), "<f4"),
            origin=origin,
            spacing=spacing,
        ),
    )

    with pytest.raises(VTKError, match=problem):
        read_structured_points(path)


def test_trailing_bytes_after_the_single_field_are_rejected(tmp_path: Path):
    path = _write(
        tmp_path / "trailing.vtk",
        _vtk_bytes(ramp((2, 2, 2, 1), "<f4")) + b"junk",
    )

    with pytest.raises(VTKError, match=r"trailing\.vtk.*expected .* bytes.*found"):
        read_structured_points(path)
