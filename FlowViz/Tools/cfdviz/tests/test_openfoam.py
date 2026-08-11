"""OpenFOAM sampled-set tables converted without spatial guesswork."""

from __future__ import annotations

import gzip
from decimal import Decimal
from pathlib import Path

import numpy as np
import pytest

from cfdviz.openfoam import (
    OpenFOAMError,
    OpenFOAMLattice,
    discover_sampled_set_frames,
    openfoam_field,
    read_sampled_set,
    scatter_sampled_set,
)


def _fields():
    return (
        openfoam_field("U", "vector"),
        openfoam_field("p", "scalar", pressure_kind="kinematic"),
        openfoam_field("Q", "scalar"),
        openfoam_field("vorticity", "vector"),
    )


def _csv(rows: list[str]) -> str:
    return "\n".join(
        [
            "x,y,z,U_x,U_y,U_z,p,Q,vorticity_x,vorticity_y,vorticity_z",
            *rows,
            "",
        ]
    )


def test_csv_parser_requires_explicit_scalar_vector_schema(tmp_path: Path):
    path = tmp_path / "volume.csv"
    path.write_text(
        _csv(["10,20,30,1,2,3,100000,0.25,-1,0,1"]),
        encoding="utf-8",
    )

    table = read_sampled_set(path, fields=_fields(), format="csv")

    assert table.coordinates.tolist() == [[10.0, 20.0, 30.0]]
    assert table.values["U"].tolist() == [[1.0, 2.0, 3.0]]
    assert table.values["pressure"].tolist() == [[100000.0]]
    assert table.values["qCriterion"].tolist() == [[0.25]]
    assert table.values["vorticity"].tolist() == [[-1.0, 0.0, 1.0]]
    assert table.row_numbers == (2,)


def test_raw_parser_ignores_segment_blank_lines_and_supports_gzip(tmp_path: Path):
    path = tmp_path / "volume.xy.gz"
    payload = (
        "# x y z U_x U_y U_z p Q vorticity_x vorticity_y vorticity_z\n"
        "10 20 30 1 2 3 100000 0.25 -1 0 1\n\n\n"
        "12 20 30 2 2 3 99990 0.20 -1 0 1\n"
    )
    with gzip.open(path, "wt", encoding="utf-8", newline="") as stream:
        stream.write(payload)

    table = read_sampled_set(path, fields=_fields(), format="raw")

    assert table.coordinates.shape == (2, 3)
    assert table.row_numbers == (2, 5)


@pytest.mark.parametrize(
    "header",
    [
        "x,U_x,U_y,U_z,p,Q,vorticity_x,vorticity_y,vorticity_z",
        "distance,x,y,z,U_x,U_y,U_z,p,Q,vorticity_x,vorticity_y,vorticity_z",
    ],
)
def test_parser_rejects_non_xyz_coordinate_modes(tmp_path: Path, header: str):
    path = tmp_path / "volume.csv"
    path.write_text(header + "\n1,2,3,4,5,6,7,8,9,10,11\n", encoding="utf-8")

    with pytest.raises(OpenFOAMError, match="axis xyz"):
        read_sampled_set(path, fields=_fields(), format="csv")


def test_parser_rejects_shifted_or_silently_missing_columns(tmp_path: Path):
    path = tmp_path / "volume.csv"
    path.write_text(
        "x,y,z,U_x,U_y,U_z,Q,vorticity_x,vorticity_y,vorticity_z\n"
        "10,20,30,1,2,3,0.25,-1,0,1\n",
        encoding="utf-8",
    )

    with pytest.raises(OpenFOAMError, match="header.*p"):
        read_sampled_set(path, fields=_fields(), format="csv")


def test_discovery_sorts_decimal_time_directories_numerically(tmp_path: Path):
    root = tmp_path / "postProcessing" / "flowvizLattice"
    for name in ("0.2", "1e-05", "0"):
        directory = root / name
        directory.mkdir(parents=True)
        (directory / "volume.csv").write_text(_csv([]), encoding="utf-8")
    (root / "latestTime").mkdir()

    frames = discover_sampled_set_frames(root, set_name="volume", format="csv")

    assert [frame.time for frame in frames] == [
        Decimal("0"),
        Decimal("1e-05"),
        Decimal("0.2"),
    ]


def test_discovery_rejects_numerically_duplicate_time_directories(tmp_path: Path):
    root = tmp_path / "samples"
    for name in ("1", "1.0"):
        directory = root / name
        directory.mkdir(parents=True)
        (directory / "volume.csv").write_text(_csv([]), encoding="utf-8")

    with pytest.raises(OpenFOAMError, match="same numeric time"):
        discover_sampled_set_frames(root, set_name="volume", format="csv")


def test_scatter_reconstructs_declared_point_lattice_and_preserves_missing_points(
    tmp_path: Path,
):
    path = tmp_path / "volume.csv"
    path.write_text(
        _csv(
            [
                "12,23,30,4,5,6,40,0.4,-4,0,4",
                "10,20,30,1,2,3,10,0.1,-1,0,1",
                "12,20,30,2,3,4,20,0.2,-2,0,2",
            ]
        ),
        encoding="utf-8",
    )
    table = read_sampled_set(path, fields=_fields(), format="csv")
    lattice = OpenFOAMLattice(
        origin=(10.0, 20.0, 30.0),
        spacing=(2.0, 3.0, 5.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )

    scattered = scatter_sampled_set(
        table,
        lattice=lattice,
        fields=_fields(),
        source_precision="float64",
        write_precision=12,
    )

    assert scattered.values["U"].shape == (2, 2, 2, 3)
    assert scattered.values["U"][1, 1, 0] == pytest.approx([4.0, 5.0, 6.0])
    assert scattered.valid_mask[1, 1, 0] == 1
    assert scattered.valid_mask[0, 1, 1] == 0
    assert np.isnan(scattered.values["pressure"][0, 1, 1, 0])


@pytest.mark.parametrize(
    ("extra_row", "message"),
    [
        ("10,20,30,9,9,9,90,0.9,-9,0,9", "duplicate.*lattice index"),
        ("10.4,20,30,9,9,9,90,0.9,-9,0,9", "off lattice.*tolerance"),
    ],
)
def test_scatter_rejects_duplicate_and_off_lattice_rows(
    tmp_path: Path,
    extra_row: str,
    message: str,
):
    path = tmp_path / "volume.csv"
    path.write_text(
        _csv(
            [
                "10,20,30,1,2,3,10,0.1,-1,0,1",
                extra_row,
            ]
        ),
        encoding="utf-8",
    )
    table = read_sampled_set(path, fields=_fields(), format="csv")
    lattice = OpenFOAMLattice(
        origin=(10.0, 20.0, 30.0),
        spacing=(2.0, 3.0, 5.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )

    with pytest.raises(OpenFOAMError, match=message):
        scatter_sampled_set(
            table,
            lattice=lattice,
            fields=_fields(),
            source_precision="float64",
            write_precision=12,
        )


def test_openfoam_invalid_location_sentinel_becomes_missing_point(tmp_path: Path):
    path = tmp_path / "volume.csv"
    sentinel = "1.79769e+307"
    path.write_text(
        _csv(["10,20,30," + ",".join([sentinel] * 8)]),
        encoding="utf-8",
    )
    table = read_sampled_set(path, fields=_fields(), format="csv")
    lattice = OpenFOAMLattice(
        origin=(10.0, 20.0, 30.0),
        spacing=(2.0, 3.0, 5.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )

    scattered = scatter_sampled_set(
        table,
        lattice=lattice,
        fields=_fields(),
        source_precision="float64",
        write_precision=6,
    )

    assert scattered.valid_mask[0, 0, 0] == 0
    assert np.isnan(scattered.values["U"][0, 0, 0]).all()


def test_csv_parser_supports_an_explicit_custom_separator(tmp_path: Path):
    path = tmp_path / "volume.csv"
    path.write_text(
        _csv(["10,20,30,1,2,3,100000,0.25,-1,0,1"]).replace(",", ";"),
        encoding="utf-8",
    )

    table = read_sampled_set(
        path,
        fields=_fields(),
        format="csv",
        separator=";",
    )

    assert table.values["U"].tolist() == [[1.0, 2.0, 3.0]]


def test_parser_bounds_both_compressed_and_decompressed_input(tmp_path: Path):
    path = tmp_path / "volume.csv.gz"
    payload = _csv(["10,20,30,1,2,3,100000,0.25,-1,0,1"] * 100)
    with gzip.open(path, "wt", encoding="utf-8", newline="") as stream:
        stream.write(payload)

    with pytest.raises(OpenFOAMError, match="above limit"):
        read_sampled_set(
            path,
            fields=_fields(),
            format="csv",
            max_input_bytes=path.stat().st_size - 1,
        )
    with pytest.raises(OpenFOAMError, match="decompressed.*above limit"):
        read_sampled_set(
            path,
            fields=_fields(),
            format="csv",
            max_input_bytes=path.stat().st_size,
        )


@pytest.mark.parametrize(
    ("value", "message"),
    [
        ("not-a-number", "non-numeric"),
        ("inf", "infinity"),
        ("-inf", "infinity"),
    ],
)
def test_parser_rejects_malformed_and_infinite_values(
    tmp_path: Path,
    value: str,
    message: str,
):
    path = tmp_path / "volume.csv"
    path.write_text(
        _csv([f"10,20,30,{value},2,3,100000,0.25,-1,0,1"]),
        encoding="utf-8",
    )

    with pytest.raises(OpenFOAMError, match=message):
        read_sampled_set(path, fields=_fields(), format="csv")


def test_scatter_rejects_a_partially_missing_vector(tmp_path: Path):
    path = tmp_path / "volume.csv"
    path.write_text(
        _csv(["10,20,30,1,nan,3,100000,0.25,-1,0,1"]),
        encoding="utf-8",
    )
    table = read_sampled_set(path, fields=_fields(), format="csv")
    lattice = OpenFOAMLattice(
        origin=(10.0, 20.0, 30.0),
        spacing=(2.0, 3.0, 5.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )

    with pytest.raises(OpenFOAMError, match="only some NaN components"):
        scatter_sampled_set(
            table,
            lattice=lattice,
            fields=_fields(),
            source_precision="float64",
            write_precision=12,
        )


def test_scatter_rejects_sentinel_mixed_with_ordinary_values(tmp_path: Path):
    path = tmp_path / "volume.csv"
    sentinel = "1.79769e+307"
    path.write_text(
        _csv([f"10,20,30,{sentinel},{sentinel},{sentinel},100000,0.25,-1,0,1"]),
        encoding="utf-8",
    )
    table = read_sampled_set(path, fields=_fields(), format="csv")
    lattice = OpenFOAMLattice(
        origin=(10.0, 20.0, 30.0),
        spacing=(2.0, 3.0, 5.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )

    with pytest.raises(OpenFOAMError, match="sentinel.*mixed"):
        scatter_sampled_set(
            table,
            lattice=lattice,
            fields=_fields(),
            source_precision="float64",
            write_precision=12,
        )


@pytest.mark.parametrize(
    ("changes", "message"),
    [
        ({"origin": (True, 20.0, 30.0)}, "origin"),
        ({"spacing": (2.0, 0.0, 5.0)}, "spacing"),
        ({"spacing": (2.0, True, 5.0)}, "spacing"),
        ({"points": (2, 1, 2)}, "point counts"),
        ({"points": (2, 2.5, 2)}, "point counts"),
        ({"points": (2, True, 2)}, "point counts"),
        ({"coordinate_tolerance": False}, "coordinate_tolerance"),
        ({"coordinate_tolerance": -1.0}, "coordinate_tolerance"),
        ({"coordinate_tolerance": 1.0}, "coordinate_tolerance"),
    ],
)
def test_lattice_rejects_invalid_geometry(changes: dict, message: str):
    arguments = {
        "origin": (10.0, 20.0, 30.0),
        "spacing": (2.0, 3.0, 5.0),
        "points": (2, 2, 2),
        "coordinate_tolerance": 1.0e-9,
    }
    arguments.update(changes)

    with pytest.raises(OpenFOAMError, match=message):
        OpenFOAMLattice(**arguments)


def test_scatter_rejects_coordinates_outside_declared_lattice(tmp_path: Path):
    path = tmp_path / "volume.csv"
    path.write_text(
        _csv(["14,20,30,1,2,3,100000,0.25,-1,0,1"]),
        encoding="utf-8",
    )
    table = read_sampled_set(path, fields=_fields(), format="csv")
    lattice = OpenFOAMLattice(
        origin=(10.0, 20.0, 30.0),
        spacing=(2.0, 3.0, 5.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )

    with pytest.raises(OpenFOAMError, match="outside lattice"):
        scatter_sampled_set(
            table,
            lattice=lattice,
            fields=_fields(),
            source_precision="float64",
            write_precision=12,
        )


def test_discovery_rejects_decimal_times_that_collapse_to_one_float(tmp_path: Path):
    root = tmp_path / "samples"
    for name in ("1", "1.0000000000000001"):
        directory = root / name
        directory.mkdir(parents=True)
        (directory / "volume.csv").write_text(_csv([]), encoding="utf-8")

    with pytest.raises(OpenFOAMError, match="cannot be represented distinctly"):
        discover_sampled_set_frames(root, set_name="volume", format="csv")


def test_field_schema_requires_pressure_and_phase_interpretation():
    with pytest.raises(OpenFOAMError, match="pressure_kind"):
        openfoam_field("p", "scalar")
    with pytest.raises(OpenFOAMError, match="secondary_phase"):
        openfoam_field("alpha.water", "scalar")

    dynamic_pressure = openfoam_field("p", "scalar", pressure_kind="dynamic")
    water = openfoam_field(
        "alpha.water",
        "scalar",
        secondary_phase="air",
    )

    assert dynamic_pressure.field_id == "pressure"
    assert dynamic_pressure.unit == "Pa"
    assert water.phase == {
        "representation": "volume-fraction",
        "primaryPhase": "water",
        "secondaryPhase": "air",
        "interfaceValue": 0.5,
        "inside": "greater-than-interface",
    }
