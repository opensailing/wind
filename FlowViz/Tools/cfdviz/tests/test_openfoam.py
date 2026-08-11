"""OpenFOAM sampled-set tables converted without spatial guesswork."""

from __future__ import annotations

import gzip
from decimal import Decimal
from pathlib import Path

import numpy as np
import pytest

import cfdviz.openfoam as openfoam_module
from cfdviz.openfoam import (
    OpenFOAMError,
    OpenFOAMFieldSpec,
    OpenFOAMLattice,
    OpenFOAMSampledSet,
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


def test_raw_parser_rejects_ambiguous_truncated_field_labels(tmp_path: Path):
    fields = (
        openfoam_field("U", "vector"),
        openfoam_field(
            "longScalarOne",
            "scalar",
            field_id="longScalarOne",
            unit="1",
            semantic="custom",
        ),
        openfoam_field(
            "longScalarTwo",
            "scalar",
            field_id="longScalarTwo",
            unit="1",
            semantic="custom",
        ),
    )
    path = tmp_path / "volume.xy"
    path.write_text(
        "# x y z U_x U_y U_z longScalar... longScalar...\n"
        "0 0 0 1 2 3 4 5\n",
        encoding="utf-8",
    )

    with pytest.raises(OpenFOAMError, match="ambiguous truncated"):
        read_sampled_set(path, fields=fields, format="raw")


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
    for name in ("0.2", "1e-05", "0", "-0.1"):
        directory = root / name
        directory.mkdir(parents=True)
        (directory / "volume.csv").write_text(_csv([]), encoding="utf-8")
    (root / "latestTime").mkdir()

    frames = discover_sampled_set_frames(root, set_name="volume", format="csv")

    assert [frame.time for frame in frames] == [
        Decimal("-0.1"),
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


@pytest.mark.parametrize("separator", ["\n", "\0"])
def test_public_parser_rejects_invalid_csv_separator(
    tmp_path: Path,
    separator: str,
):
    path = tmp_path / "volume.csv"
    path.write_text(_csv([]), encoding="utf-8")

    with pytest.raises(OpenFOAMError, match="CSV separator"):
        read_sampled_set(
            path,
            fields=_fields(),
            format="csv",
            separator=separator,
        )


def test_parser_checks_on_disk_bound_before_loading_file(
    tmp_path: Path,
    monkeypatch,
):
    path = tmp_path / "volume.csv"
    path.write_text(_csv(["10,20,30,1,2,3,100000,0.25,-1,0,1"]), encoding="utf-8")

    def fail_read_bytes(_path):
        raise AssertionError("oversized input was read before its size was checked")

    monkeypatch.setattr(Path, "read_bytes", fail_read_bytes)
    with pytest.raises(OpenFOAMError, match="on-disk input.*above limit"):
        read_sampled_set(
            path,
            fields=_fields(),
            format="csv",
            max_input_bytes=path.stat().st_size - 1,
        )


def test_gzip_bound_streams_without_full_decompress_allocation(
    tmp_path: Path,
    monkeypatch,
):
    path = tmp_path / "volume.csv.gz"
    payload = _csv(["10,20,30,1,2,3,100000,0.25,-1,0,1"] * 100)
    with gzip.open(path, "wt", encoding="utf-8", newline="") as stream:
        stream.write(payload)

    def fail_decompress(_raw):
        raise AssertionError("gzip.decompress allocated the full expanded payload")

    monkeypatch.setattr(openfoam_module.gzip, "decompress", fail_decompress)
    with pytest.raises(OpenFOAMError, match="decompressed input.*above limit"):
        read_sampled_set(
            path,
            fields=_fields(),
            format="csv",
            max_input_bytes=path.stat().st_size,
        )


def test_parser_translates_truncated_gzip_errors(tmp_path: Path):
    path = tmp_path / "truncated.csv.gz"
    path.write_bytes(gzip.compress(_csv([]).encode("utf-8"))[:-4])

    with pytest.raises(OpenFOAMError, match="cannot read sampled-set text"):
        read_sampled_set(path, fields=_fields(), format="csv")


def test_parser_translates_csv_field_limit_errors(tmp_path: Path):
    path = tmp_path / "oversized-field.csv"
    oversized = "9" * (openfoam_module.csv.field_size_limit() + 1)
    path.write_text(
        "x,y,z,U_x,U_y,U_z,p,Q,vorticity_x,vorticity_y,vorticity_z\n"
        f"{oversized},20,30,1,2,3,100000,0.25,-1,0,1\n",
        encoding="utf-8",
    )

    with pytest.raises(OpenFOAMError, match="cannot parse sampled-set CSV"):
        read_sampled_set(path, fields=_fields(), format="csv")


def test_parser_enforces_explicit_row_budget(tmp_path: Path):
    path = tmp_path / "too-many-rows.csv"
    path.write_text(
        _csv(
            [
                "10,20,30,1,2,3,100000,0.25,-1,0,1",
                "12,20,30,2,2,3,99990,0.20,-1,0,1",
            ]
        ),
        encoding="utf-8",
    )

    with pytest.raises(OpenFOAMError, match="row count.*above limit"):
        read_sampled_set(
            path,
            fields=_fields(),
            format="csv",
            max_rows=1,
        )


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


@pytest.mark.parametrize(
    "row",
    [
        "10,20,30,nan,nan,nan,100000,0.25,-1,0,1",
        "10,20,30,1,2,3,nan,0.25,-1,0,1",
    ],
)
def test_scatter_rejects_mixed_missingness_between_fields(
    tmp_path: Path,
    row: str,
):
    path = tmp_path / "volume.csv"
    path.write_text(_csv([row]), encoding="utf-8")
    table = read_sampled_set(path, fields=_fields(), format="csv")
    lattice = OpenFOAMLattice(
        origin=(10.0, 20.0, 30.0),
        spacing=(2.0, 3.0, 5.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )

    with pytest.raises(OpenFOAMError, match="missing.*some.*fields"):
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
        ({"points": (2_147_483_648, 2, 2)}, "point counts"),
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


def test_public_scatter_enforces_default_lattice_budget_before_allocation():
    fields = _fields()
    table = OpenFOAMSampledSet(
        path=Path("oversized.csv"),
        coordinates=np.empty((0, 3), dtype=np.float64),
        values={
            field.field_id: np.empty((0, field.component_count), dtype=np.float64)
            for field in fields
        },
        row_numbers=(),
    )
    lattice = OpenFOAMLattice(
        origin=(0.0, 0.0, 0.0),
        spacing=(1.0, 1.0, 1.0),
        points=(1001, 1001, 101),
        coordinate_tolerance=1.0e-9,
    )

    with pytest.raises(OpenFOAMError, match="max_lattice_points"):
        scatter_sampled_set(
            table,
            lattice=lattice,
            fields=fields,
            source_precision="float64",
            write_precision=12,
        )


def test_public_scatter_enforces_default_dense_byte_budget_before_allocation():
    fields = _fields()
    table = OpenFOAMSampledSet(
        path=Path("oversized.csv"),
        coordinates=np.empty((0, 3), dtype=np.float64),
        values={
            field.field_id: np.empty((0, field.component_count), dtype=np.float64)
            for field in fields
        },
        row_numbers=(),
    )
    lattice = OpenFOAMLattice(
        origin=(0.0, 0.0, 0.0),
        spacing=(1.0, 1.0, 1.0),
        points=(464, 464, 464),
        coordinate_tolerance=1.0e-9,
    )

    with pytest.raises(OpenFOAMError, match="max_dense_bytes"):
        scatter_sampled_set(
            table,
            lattice=lattice,
            fields=fields,
            source_precision="float64",
            write_precision=12,
        )


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


@pytest.mark.parametrize(
    ("component_count", "components"),
    [
        (1, ("scalar",)),
        (3, ("u", "v", "w")),
    ],
)
def test_public_field_schema_requires_canonical_component_names(
    component_count: int,
    components: tuple[str, ...],
):
    with pytest.raises(OpenFOAMError, match="canonical component names"):
        OpenFOAMFieldSpec(
            source_name="custom",
            field_id="custom",
            component_count=component_count,
            components=components,
            unit="1",
            semantic="custom",
            vector_kind="true" if component_count == 3 else None,
        )


def test_scalar_field_rejects_a_vector_transform_kind():
    with pytest.raises(OpenFOAMError, match="scalar.*vector_kind"):
        openfoam_field("Q", "scalar", vector_kind="pseudo")


@pytest.mark.parametrize(
    ("source_name", "kind", "kwargs"),
    [
        ("Q", "vector", {"vector_kind": "true"}),
        ("U", "scalar", {}),
        ("p", "vector", {"pressure_kind": "dynamic", "vector_kind": "true"}),
    ],
)
def test_standard_fields_reject_the_wrong_scalar_vector_kind(
    source_name: str,
    kind: str,
    kwargs: dict,
):
    with pytest.raises(OpenFOAMError, match="must be declared as"):
        openfoam_field(source_name, kind, **kwargs)


@pytest.mark.parametrize("empty", ["field_id", "unit", "semantic"])
def test_custom_field_empty_overrides_raise_openfoam_error(empty: str):
    arguments = {
        "field_id": "custom",
        "unit": "1",
        "semantic": "custom",
    }
    arguments[empty] = ""

    with pytest.raises(OpenFOAMError, match=empty.replace("_", " ")):
        openfoam_field("custom", "scalar", **arguments)


@pytest.mark.parametrize(
    ("source_name", "kwargs", "message"),
    [
        (
            "p_rgh",
            {"pressure_kind": "kinematic", "unit": "Pa"},
            "unit.*pressure_kind",
        ),
        (
            "alpha.water",
            {"secondary_phase": "air", "semantic": "temperature"},
            "semantic.*volume-fraction",
        ),
    ],
)
def test_pressure_and_phase_interpretations_reject_conflicting_overrides(
    source_name: str,
    kwargs: dict,
    message: str,
):
    with pytest.raises(OpenFOAMError, match=message):
        openfoam_field(source_name, "scalar", **kwargs)


def test_field_schema_requires_pressure_and_phase_interpretation():
    with pytest.raises(OpenFOAMError, match="pressure_kind"):
        openfoam_field("p", "scalar")
    with pytest.raises(OpenFOAMError, match="pressure_kind"):
        openfoam_field("p_rgh", "scalar")
    with pytest.raises(OpenFOAMError, match="secondary_phase"):
        openfoam_field("alpha.water", "scalar")
    with pytest.raises(OpenFOAMError, match="must differ"):
        openfoam_field("alpha.water", "scalar", secondary_phase="water")

    dynamic_pressure = openfoam_field("p", "scalar", pressure_kind="dynamic")
    dynamic_reduced_pressure = openfoam_field(
        "p_rgh",
        "scalar",
        pressure_kind="dynamic",
    )
    water = openfoam_field(
        "alpha.water",
        "scalar",
        secondary_phase="air",
    )

    assert dynamic_pressure.field_id == "pressure"
    assert dynamic_pressure.unit == "Pa"
    assert dynamic_reduced_pressure.field_id == "p_rgh"
    assert dynamic_reduced_pressure.unit == "Pa"
    assert water.phase == {
        "representation": "volume-fraction",
        "primaryPhase": "water",
        "secondaryPhase": "air",
        "interfaceValue": 0.5,
        "inside": "greater-than-interface",
    }
