"""End-to-end OpenFOAM sampled-set conversion to CFDViz 1.1."""

from __future__ import annotations

import json
from dataclasses import replace
from pathlib import Path

import numpy as np
import pytest

import cfdviz.__main__ as cfdviz_main
from cfdviz.__main__ import main
from cfdviz.case import validate_case
from cfdviz.cvf import read_cvf
from cfdviz.openfoam import (
    OpenFOAMError,
    OpenFOAMImportParameters,
    OpenFOAMLattice,
    import_openfoam_case,
    openfoam_field,
)


def _fields():
    return (
        openfoam_field("U", "vector"),
        openfoam_field("p", "scalar", pressure_kind="kinematic"),
        openfoam_field("Q", "scalar"),
        openfoam_field("vorticity", "vector"),
    )


def _header() -> str:
    return "x,y,z,U_x,U_y,U_z,p,Q,vorticity_x,vorticity_y,vorticity_z"


def _write_sequence(
    root: Path,
    *,
    lattice: OpenFOAMLattice,
    times: tuple[str, ...] = ("0", "0.5"),
    missing: tuple[int, int, int] | None = None,
) -> None:
    for frame, time in enumerate(times):
        directory = root / time
        directory.mkdir(parents=True)
        rows = [_header()]
        for i in range(lattice.points[0]):
            for j in range(lattice.points[1]):
                for k in range(lattice.points[2]):
                    if frame == 1 and missing == (i, j, k):
                        continue
                    x = lattice.origin[0] + i * lattice.spacing[0]
                    y = lattice.origin[1] + j * lattice.spacing[1]
                    z = lattice.origin[2] + k * lattice.spacing[2]
                    u = (1.0 + i + frame, 10.0 + j, 100.0 + k)
                    pressure = 1000.0 + 100.0 * i + 10.0 * j + k
                    q = 900.0 + 100.0 * i + 10.0 * j + k
                    vorticity = (2.0 + i, 20.0 + j, 200.0 + k)
                    values = (
                        x,
                        y,
                        z,
                        *u,
                        pressure,
                        q,
                        *vorticity,
                    )
                    rows.append(",".join(str(value) for value in values))
        (directory / "volume.csv").write_text(
            "\n".join(rows) + "\n",
            encoding="utf-8",
        )


def _parameters(
    tmp_path: Path,
    *,
    input_directory: Path,
    lattice: OpenFOAMLattice,
    source_axes: tuple[str, str, str] = ("+X", "+Y", "+Z"),
    force: bool = False,
) -> OpenFOAMImportParameters:
    return OpenFOAMImportParameters(
        output=tmp_path / "OpenFOAM.cfdviz",
        name="OpenFOAM sampled lattice",
        input_directory=input_directory,
        set_name="volume",
        format="csv",
        fields=_fields(),
        lattice=lattice,
        source_axes=source_axes,
        source_precision="float64",
        write_precision=12,
        sampling_method="uniform",
        solver_method="finite-volume",
        export_command="postProcess -func flowvizLattice",
        source_case="motorBike",
        solver_version="13",
        solver_commit="openfoam-revision",
        force=force,
    )


def test_import_writes_valid_point_associated_case_with_honest_timeline(
    tmp_path: Path,
):
    lattice = OpenFOAMLattice(
        origin=(10.0, 20.0, 30.0),
        spacing=(2.0, 3.0, 5.0),
        points=(3, 2, 3),
        coordinate_tolerance=1.0e-9,
    )
    source = tmp_path / "postProcessing" / "flowvizLattice"
    _write_sequence(source, lattice=lattice, missing=(1, 1, 1))
    parameters = _parameters(
        tmp_path,
        input_directory=source,
        lattice=lattice,
    )

    output = import_openfoam_case(parameters)

    manifest = json.loads((output / "manifest.json").read_text("utf-8"))
    assert manifest["case"]["quality"] == "external-solver-sample"
    assert manifest["case"]["solver"] == {
        "name": "OpenFOAM",
        "version": "13",
        "method": "finite-volume",
        "commit": "openfoam-revision",
    }
    assert manifest["provenance"]["sourceType"] == "external-solver"
    assert manifest["provenance"]["sourceCase"] == "motorBike"
    assert manifest["provenance"]["exportCommand"] == parameters.export_command
    assert manifest["timeline"]["times"] == [0.0, 0.5]
    assert "steps" not in manifest["timeline"]
    assert "sampling" not in manifest["timeline"]
    assert manifest["grids"][0]["dimensions"] == [2, 1, 2]
    assert manifest["grids"][0]["origin"] == [10.0, 20.0, 30.0]
    assert manifest["grids"][0]["spacing"] == [2.0, 3.0, 5.0]
    assert all(field["association"] == "point" for field in manifest["fields"])
    assert manifest["qualityMetrics"]["activeCellCount"] == 4
    assert manifest["qualityMetrics"]["activeDimensions"] == [2, 1, 2]

    velocity = read_cvf(output / "frames/000000/U.cvf")
    mask = read_cvf(output / "frames/000001/validMask.cvf")
    q = read_cvf(output / "frames/000000/qCriterion.cvf")
    assert velocity.values.shape == (3, 2, 3, 3)
    assert mask.values.shape == (3, 2, 3, 1)
    assert mask.values[1, 1, 1, 0] == 0
    assert np.isnan(read_cvf(output / "frames/000001/U.cvf").values[1, 1, 1]).all()
    assert q.values[2, 1, 2, 0] == pytest.approx(1112.0)
    assert (output / "known_values.json").is_file()
    report = validate_case(output)
    assert report.ok, report.render()


def test_signed_axes_transform_spatial_samples_true_vectors_and_pseudovectors(
    tmp_path: Path,
):
    lattice = OpenFOAMLattice(
        origin=(10.0, 20.0, 30.0),
        spacing=(2.0, 3.0, 5.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )
    source = tmp_path / "postProcessing" / "flowvizLattice"
    _write_sequence(source, lattice=lattice, times=("0",))
    parameters = _parameters(
        tmp_path,
        input_directory=source,
        lattice=lattice,
        source_axes=("+Z", "-Y", "-X"),
    )

    output = import_openfoam_case(parameters)

    manifest = json.loads((output / "manifest.json").read_text("utf-8"))
    assert manifest["grids"][0]["dimensions"] == [1, 1, 1]
    assert manifest["grids"][0]["origin"] == [-35.0, -23.0, 10.0]
    assert manifest["grids"][0]["spacing"] == [5.0, 3.0, 2.0]
    velocity = read_cvf(output / "frames/000000/U.cvf").values
    vorticity = read_cvf(output / "frames/000000/vorticity.cvf").values
    q = read_cvf(output / "frames/000000/qCriterion.cvf").values
    # canonical [0,0,0] comes from source [0,1,1] after the two signed flips.
    assert velocity[0, 0, 0] == pytest.approx([-101.0, -11.0, 1.0])
    # det(M)=-1, so the pseudovector gets the extra orientation sign.
    assert vorticity[0, 0, 0] == pytest.approx([201.0, 21.0, -2.0])
    # Native OpenFOAM Q is retained rather than replaced by a derived value.
    assert q[0, 0, 0, 0] == pytest.approx(911.0)
    report = validate_case(output)
    assert report.ok, report.render()


def test_positive_determinant_signed_axes_transform_pseudovectors_as_vectors(
    tmp_path: Path,
):
    lattice = OpenFOAMLattice(
        origin=(10.0, 20.0, 30.0),
        spacing=(2.0, 3.0, 5.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )
    source = tmp_path / "postProcessing" / "flowvizLattice"
    _write_sequence(source, lattice=lattice, times=("0",))
    parameters = _parameters(
        tmp_path,
        input_directory=source,
        lattice=lattice,
        source_axes=("-Y", "+Z", "-X"),
    )

    output = import_openfoam_case(parameters)

    manifest = json.loads((output / "manifest.json").read_text("utf-8"))
    assert manifest["grids"][0]["origin"] == [-35.0, -12.0, 20.0]
    assert manifest["grids"][0]["spacing"] == [5.0, 2.0, 3.0]
    velocity = read_cvf(output / "frames/000000/U.cvf").values
    vorticity = read_cvf(output / "frames/000000/vorticity.cvf").values
    # det(M)=+1, so a pseudovector receives the same component transform as U.
    assert velocity[0, 0, 0] == pytest.approx([-101.0, -2.0, 10.0])
    assert vorticity[0, 0, 0] == pytest.approx([-201.0, -3.0, 20.0])


def _write_u_scalar_sequence(
    root: Path,
    *,
    lattice: OpenFOAMLattice,
    scalar_name: str,
    scalar_value: float,
    velocity_x: float = 1.0,
) -> None:
    directory = root / "0"
    directory.mkdir(parents=True)
    rows = [f"x,y,z,U_x,U_y,U_z,{scalar_name}"]
    for i in range(lattice.points[0]):
        for j in range(lattice.points[1]):
            for k in range(lattice.points[2]):
                x = lattice.origin[0] + i * lattice.spacing[0]
                y = lattice.origin[1] + j * lattice.spacing[1]
                z = lattice.origin[2] + k * lattice.spacing[2]
                rows.append(
                    f"{x},{y},{z},{velocity_x},{10 + j},{100 + k},{scalar_value}"
                )
    (directory / "volume.csv").write_text(
        "\n".join(rows) + "\n",
        encoding="utf-8",
    )


def _write_u_scalar_fields_sequence(
    root: Path,
    *,
    lattice: OpenFOAMLattice,
    scalar_values: dict[str, float],
) -> None:
    directory = root / "0"
    directory.mkdir(parents=True)
    scalar_names = list(scalar_values)
    rows = [",".join(("x", "y", "z", "U_x", "U_y", "U_z", *scalar_names))]
    for i in range(lattice.points[0]):
        for j in range(lattice.points[1]):
            for k in range(lattice.points[2]):
                x = lattice.origin[0] + i * lattice.spacing[0]
                y = lattice.origin[1] + j * lattice.spacing[1]
                z = lattice.origin[2] + k * lattice.spacing[2]
                values = (
                    x,
                    y,
                    z,
                    1.0 + i,
                    10.0 + j,
                    100.0 + k,
                    *(scalar_values[name] for name in scalar_names),
                )
                rows.append(",".join(str(value) for value in values))
    (directory / "volume.csv").write_text(
        "\n".join(rows) + "\n",
        encoding="utf-8",
    )


def test_import_preserves_volume_fraction_interpretation_and_range(tmp_path: Path):
    lattice = OpenFOAMLattice(
        origin=(0.0, 0.0, 0.0),
        spacing=(1.0, 1.0, 1.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )
    source = tmp_path / "phase"
    _write_u_scalar_sequence(
        source,
        lattice=lattice,
        scalar_name="alpha.water",
        scalar_value=0.75,
    )
    fields = (
        openfoam_field("U", "vector"),
        openfoam_field("alpha.water", "scalar", secondary_phase="air"),
    )
    parameters = replace(
        _parameters(tmp_path, input_directory=source, lattice=lattice),
        fields=fields,
    )

    output = import_openfoam_case(parameters)

    manifest = json.loads((output / "manifest.json").read_text("utf-8"))
    alpha = next(field for field in manifest["fields"] if field["id"] == "alpha.water")
    assert alpha["phase"] == {
        "representation": "volume-fraction",
        "primaryPhase": "water",
        "secondaryPhase": "air",
        "interfaceValue": 0.5,
        "inside": "greater-than-interface",
    }

    bad_source = tmp_path / "bad-phase"
    _write_u_scalar_sequence(
        bad_source,
        lattice=lattice,
        scalar_name="alpha.water",
        scalar_value=1.01,
    )
    bad = replace(
        parameters,
        output=tmp_path / "BadPhase.cfdviz",
        input_directory=bad_source,
    )
    with pytest.raises(OpenFOAMError, match=r"volume fraction.*outside \[0, 1\]"):
        import_openfoam_case(bad)
    assert not bad.output.exists()


def test_import_rejects_values_that_overflow_selected_storage(tmp_path: Path):
    lattice = OpenFOAMLattice(
        origin=(0.0, 0.0, 0.0),
        spacing=(1.0, 1.0, 1.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )
    source = tmp_path / "overflow"
    _write_u_scalar_sequence(
        source,
        lattice=lattice,
        scalar_name="Q",
        scalar_value=1.0,
        velocity_x=70000.0,
    )
    parameters = replace(
        _parameters(tmp_path, input_directory=source, lattice=lattice),
        fields=(
            openfoam_field("U", "vector"),
            openfoam_field("Q", "scalar"),
        ),
        float_type="float16",
    )

    with pytest.raises(OpenFOAMError, match="70000.*overflows float16"):
        import_openfoam_case(parameters)

    assert not parameters.output.exists()


def test_import_raw_sampled_set_writes_a_valid_case(tmp_path: Path):
    lattice = OpenFOAMLattice(
        origin=(0.0, 0.0, 0.0),
        spacing=(1.0, 1.0, 1.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )
    source = tmp_path / "raw-samples"
    _write_sequence(source, lattice=lattice, times=("0",))
    csv_path = source / "0" / "volume.csv"
    lines = csv_path.read_text("utf-8").splitlines()
    (source / "0" / "volume.xy").write_text(
        "# " + lines[0].replace(",", " ") + "\n"
        + "\n\n".join(line.replace(",", " ") for line in lines[1:])
        + "\n",
        encoding="utf-8",
    )
    csv_path.unlink()
    parameters = replace(
        _parameters(tmp_path, input_directory=source, lattice=lattice),
        format="raw",
    )

    output = import_openfoam_case(parameters)

    assert read_cvf(output / "frames/000000/U.cvf").values[1, 1, 1] == pytest.approx(
        [2.0, 11.0, 101.0]
    )
    report = validate_case(output)
    assert report.ok, report.render()


def test_import_is_byte_deterministic_for_identical_sources(tmp_path: Path):
    lattice = OpenFOAMLattice(
        origin=(0.0, 0.0, 0.0),
        spacing=(1.0, 1.0, 1.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )
    source = tmp_path / "samples"
    _write_sequence(source, lattice=lattice, times=("0",))
    first = _parameters(tmp_path, input_directory=source, lattice=lattice)
    second = replace(first, output=tmp_path / "Second.cfdviz")

    import_openfoam_case(first)
    import_openfoam_case(second)

    first_files = {
        path.relative_to(first.output): path.read_bytes()
        for path in first.output.rglob("*")
        if path.is_file()
    }
    second_files = {
        path.relative_to(second.output): path.read_bytes()
        for path in second.output.rglob("*")
        if path.is_file()
    }
    assert first_files == second_files


def test_import_identity_ignores_source_row_order_and_blank_lines(tmp_path: Path):
    lattice = OpenFOAMLattice(
        origin=(0.0, 0.0, 0.0),
        spacing=(1.0, 1.0, 1.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )
    first_source = tmp_path / "ordered"
    second_source = tmp_path / "reordered"
    _write_sequence(first_source, lattice=lattice, times=("0",))
    lines = (first_source / "0" / "volume.csv").read_text("utf-8").splitlines()
    (second_source / "0").mkdir(parents=True)
    (second_source / "0" / "volume.csv").write_text(
        "\n\n" + lines[0] + "\n\n" + "\n\n".join(reversed(lines[1:])) + "\n",
        encoding="utf-8",
    )
    first = _parameters(tmp_path, input_directory=first_source, lattice=lattice)
    second = replace(
        first,
        output=tmp_path / "Reordered.cfdviz",
        input_directory=second_source,
    )

    import_openfoam_case(first)
    import_openfoam_case(second)

    first_files = {
        path.relative_to(first.output): path.read_bytes()
        for path in first.output.rglob("*")
        if path.is_file()
    }
    second_files = {
        path.relative_to(second.output): path.read_bytes()
        for path in second.output.rglob("*")
        if path.is_file()
    }
    assert first_files == second_files


def test_import_preserves_nonempty_output_without_force(tmp_path: Path):
    lattice = OpenFOAMLattice(
        origin=(0.0, 0.0, 0.0),
        spacing=(1.0, 1.0, 1.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )
    source = tmp_path / "samples"
    _write_sequence(source, lattice=lattice, times=("0",))
    parameters = _parameters(tmp_path, input_directory=source, lattice=lattice)
    parameters.output.mkdir()
    stale = parameters.output / "stale.txt"
    stale.write_text("keep", encoding="utf-8")

    with pytest.raises(OpenFOAMError, match="not empty.*force"):
        import_openfoam_case(parameters)

    assert stale.read_text("utf-8") == "keep"


@pytest.mark.parametrize(
    ("changes", "message"),
    [
        ({"source_axes": ("+X", "+Y", 1)}, "source_axes"),
        ({"fields": (object(),)}, "fields.*OpenFOAMFieldSpec"),
        ({"separator": "\n"}, "separator"),
        ({"format": "raw", "separator": ";"}, "separator.*raw"),
        ({"codec": "zstd"}, "zstd"),
        (
            {
                "fields": (
                    openfoam_field(
                        "vorticity",
                        "vector",
                        field_id="U",
                    ),
                )
            },
            "U field.*velocity.*true vector",
        ),
    ],
)
def test_import_parameters_reject_malformed_public_inputs(
    tmp_path: Path,
    changes: dict,
    message: str,
):
    lattice = OpenFOAMLattice(
        origin=(0.0, 0.0, 0.0),
        spacing=(1.0, 1.0, 1.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )
    source = tmp_path / "samples"
    parameters = _parameters(
        tmp_path,
        input_directory=source,
        lattice=lattice,
    )

    with pytest.raises(OpenFOAMError, match=message):
        replace(parameters, **changes)


def test_import_parameters_reject_output_nested_in_source(tmp_path: Path):
    lattice = OpenFOAMLattice(
        origin=(0.0, 0.0, 0.0),
        spacing=(1.0, 1.0, 1.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )
    source = tmp_path / "samples"
    parameters = _parameters(tmp_path, input_directory=source, lattice=lattice)

    with pytest.raises(OpenFOAMError, match="output.*inside input"):
        replace(parameters, output=source / "0")


def test_import_parameters_bound_dense_lattice_allocation(tmp_path: Path):
    lattice = OpenFOAMLattice(
        origin=(0.0, 0.0, 0.0),
        spacing=(1.0, 1.0, 1.0),
        points=(1001, 1001, 101),
        coordinate_tolerance=1.0e-9,
    )

    with pytest.raises(OpenFOAMError, match="max_lattice_points"):
        _parameters(
            tmp_path,
            input_directory=tmp_path / "samples",
            lattice=lattice,
        )


def _cli_arguments(
    *,
    source: Path,
    output: Path,
    fields: tuple[tuple[str, str], ...],
    extra: tuple[str, ...] = (),
) -> list[str]:
    arguments = [
        "import-openfoam",
        "--output",
        str(output),
        "--name",
        "CLI fixture",
        "--input-directory",
        str(source),
        "--set-name",
        "volume",
        "--format",
        "csv",
    ]
    for source_name, kind in fields:
        arguments.extend(("--field", source_name, kind))
    arguments.extend(
        (
            "--lattice-origin",
            "0",
            "0",
            "0",
            "--lattice-spacing",
            "1",
            "1",
            "1",
            "--lattice-points",
            "2",
            "2",
            "2",
            "--coordinate-tolerance",
            "1e-9",
            "--source-axes",
            "+X",
            "+Y",
            "+Z",
            "--source-precision",
            "float64",
            "--write-precision",
            "12",
            "--sampling-method",
            "uniform",
            "--solver-method",
            "finite-volume",
            "--export-command",
            "postProcess -func flowvizLattice",
            "--codec",
            "none",
            *extra,
        )
    )
    return arguments


def test_import_openfoam_cli_writes_a_valid_case(capsys, tmp_path: Path):
    lattice = OpenFOAMLattice(
        origin=(10.0, 20.0, 30.0),
        spacing=(2.0, 3.0, 5.0),
        points=(3, 2, 3),
        coordinate_tolerance=1.0e-9,
    )
    source = tmp_path / "postProcessing" / "flowvizLattice"
    _write_sequence(source, lattice=lattice)
    output = tmp_path / "Cli.cfdviz"

    status = main(
        [
            "import-openfoam",
            "--output",
            str(output),
            "--name",
            "CLI fixture",
            "--input-directory",
            str(source),
            "--set-name",
            "volume",
            "--format",
            "csv",
            "--field",
            "U",
            "vector",
            "--field",
            "p",
            "scalar",
            "--field",
            "Q",
            "scalar",
            "--field",
            "vorticity",
            "vector",
            "--pressure-kind",
            "kinematic",
            "--lattice-origin",
            "10",
            "20",
            "30",
            "--lattice-spacing",
            "2",
            "3",
            "5",
            "--lattice-points",
            "3",
            "2",
            "3",
            "--coordinate-tolerance",
            "1e-9",
            "--source-axes",
            "+X",
            "+Y",
            "+Z",
            "--source-precision",
            "float64",
            "--write-precision",
            "12",
            "--sampling-method",
            "uniform",
            "--solver-method",
            "finite-volume",
            "--export-command",
            "postProcess -func flowvizLattice",
            "--source-case",
            "motorBike",
            "--solver-version",
            "13",
            "--codec",
            "none",
        ]
    )
    captured = capsys.readouterr()

    assert status == 0, captured.err
    assert "OpenFOAM" in captured.out
    assert validate_case(output).ok


def test_import_openfoam_cli_reports_field_interpretation_errors_without_traceback(
    capsys,
    tmp_path: Path,
):
    lattice = OpenFOAMLattice(
        origin=(0.0, 0.0, 0.0),
        spacing=(1.0, 1.0, 1.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )
    source = tmp_path / "samples"
    _write_sequence(source, lattice=lattice, times=("0",))

    status = main(
        [
            "import-openfoam",
            "--output",
            str(tmp_path / "BadCli.cfdviz"),
            "--name",
            "bad",
            "--input-directory",
            str(source),
            "--set-name",
            "volume",
            "--format",
            "csv",
            "--field",
            "U",
            "vector",
            "--field",
            "p",
            "scalar",
            "--field",
            "Q",
            "scalar",
            "--field",
            "vorticity",
            "vector",
            "--lattice-origin",
            "0",
            "0",
            "0",
            "--lattice-spacing",
            "1",
            "1",
            "1",
            "--lattice-points",
            "2",
            "2",
            "2",
            "--coordinate-tolerance",
            "1e-9",
            "--source-axes",
            "+X",
            "+Y",
            "+Z",
            "--source-precision",
            "float64",
            "--write-precision",
            "12",
            "--sampling-method",
            "uniform",
            "--solver-method",
            "finite-volume",
            "--export-command",
            "postProcess -func flowvizLattice",
        ]
    )
    captured = capsys.readouterr()
    text = captured.out + captured.err

    assert status != 0
    assert "pressure_kind" in text
    assert "Traceback" not in text
    assert "internal error" not in text


def test_import_openfoam_cli_parses_custom_field_overrides(capsys, tmp_path: Path):
    lattice = OpenFOAMLattice(
        origin=(0.0, 0.0, 0.0),
        spacing=(1.0, 1.0, 1.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )
    source = tmp_path / "custom"
    _write_u_scalar_fields_sequence(
        source,
        lattice=lattice,
        scalar_values={"Tcustom": 300.0},
    )
    output = tmp_path / "CustomCli.cfdviz"

    status = main(
        _cli_arguments(
            source=source,
            output=output,
            fields=(("U", "vector"), ("Tcustom", "scalar")),
            extra=(
                "--field-id",
                "Tcustom=temperature",
                "--field-unit",
                "Tcustom=K",
                "--field-semantic",
                "Tcustom=temperature",
            ),
        )
    )
    captured = capsys.readouterr()

    assert status == 0, captured.err
    manifest = json.loads((output / "manifest.json").read_text("utf-8"))
    temperature = next(field for field in manifest["fields"] if field["id"] == "temperature")
    assert temperature["unit"] == "K"
    assert temperature["semantic"] == "temperature"


def test_import_openfoam_cli_maps_secondary_phase_per_source_field(
    capsys,
    tmp_path: Path,
):
    lattice = OpenFOAMLattice(
        origin=(0.0, 0.0, 0.0),
        spacing=(1.0, 1.0, 1.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )
    source = tmp_path / "phases"
    _write_u_scalar_fields_sequence(
        source,
        lattice=lattice,
        scalar_values={"alpha.water": 0.75, "alpha.oil": 0.25},
    )
    output = tmp_path / "PhaseCli.cfdviz"

    status = main(
        _cli_arguments(
            source=source,
            output=output,
            fields=(
                ("U", "vector"),
                ("alpha.water", "scalar"),
                ("alpha.oil", "scalar"),
            ),
            extra=(
                "--secondary-phase",
                "alpha.water=air",
                "--secondary-phase",
                "alpha.oil=water",
            ),
        )
    )
    captured = capsys.readouterr()

    assert status == 0, captured.err
    manifest = json.loads((output / "manifest.json").read_text("utf-8"))
    phase_fields = {
        field["id"]: field["phase"]
        for field in manifest["fields"]
        if "phase" in field
    }
    assert phase_fields["alpha.water"]["secondaryPhase"] == "air"
    assert phase_fields["alpha.oil"]["secondaryPhase"] == "water"


def test_import_openfoam_cli_applies_pressure_kind_to_p_rgh(capsys, tmp_path: Path):
    lattice = OpenFOAMLattice(
        origin=(0.0, 0.0, 0.0),
        spacing=(1.0, 1.0, 1.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )
    source = tmp_path / "reduced-pressure"
    _write_u_scalar_fields_sequence(
        source,
        lattice=lattice,
        scalar_values={"p_rgh": 101325.0},
    )
    output = tmp_path / "ReducedPressure.cfdviz"

    status = main(
        _cli_arguments(
            source=source,
            output=output,
            fields=(("U", "vector"), ("p_rgh", "scalar")),
            extra=("--pressure-kind", "dynamic"),
        )
    )
    captured = capsys.readouterr()

    assert status == 0, captured.err
    manifest = json.loads((output / "manifest.json").read_text("utf-8"))
    reduced = next(field for field in manifest["fields"] if field["id"] == "p_rgh")
    assert reduced["unit"] == "Pa"


@pytest.mark.parametrize(
    "extra",
    [
        ("--pressure-kind", "kinematic"),
        ("--secondary-phase", "alpha.water=air"),
    ],
)
def test_import_openfoam_cli_rejects_unused_field_interpretations(
    capsys,
    tmp_path: Path,
    extra: tuple[str, ...],
):
    lattice = OpenFOAMLattice(
        origin=(0.0, 0.0, 0.0),
        spacing=(1.0, 1.0, 1.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )
    source = tmp_path / "velocity-only"
    _write_u_scalar_fields_sequence(source, lattice=lattice, scalar_values={})

    status = main(
        _cli_arguments(
            source=source,
            output=tmp_path / "UnusedInterpretation.cfdviz",
            fields=(("U", "vector"),),
            extra=extra,
        )
    )
    captured = capsys.readouterr()
    text = captured.out + captured.err

    assert status != 0
    assert "does not apply" in text or "not supplied by --field" in text
    assert "internal error" not in text


def test_import_openfoam_cli_forwards_lattice_point_budget(capsys, tmp_path: Path):
    lattice = OpenFOAMLattice(
        origin=(0.0, 0.0, 0.0),
        spacing=(1.0, 1.0, 1.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )
    source = tmp_path / "budget"
    _write_u_scalar_fields_sequence(source, lattice=lattice, scalar_values={})

    status = main(
        _cli_arguments(
            source=source,
            output=tmp_path / "Budget.cfdviz",
            fields=(("U", "vector"),),
            extra=("--max-lattice-points", "7"),
        )
    )
    captured = capsys.readouterr()

    assert status != 0
    assert "max_lattice_points" in captured.err


def test_import_openfoam_cli_keeps_success_after_summary_failure(
    capsys,
    monkeypatch,
    tmp_path: Path,
):
    lattice = OpenFOAMLattice(
        origin=(0.0, 0.0, 0.0),
        spacing=(1.0, 1.0, 1.0),
        points=(2, 2, 2),
        coordinate_tolerance=1.0e-9,
    )
    source = tmp_path / "summary"
    _write_u_scalar_fields_sequence(source, lattice=lattice, scalar_values={})
    output = tmp_path / "Summary.cfdviz"

    def fail_summary(_root):
        raise OSError("summary read failed")

    monkeypatch.setattr(cfdviz_main, "load_manifest", fail_summary)
    status = main(
        _cli_arguments(
            source=source,
            output=output,
            fields=(("U", "vector"),),
        )
    )
    captured = capsys.readouterr()

    assert status == 0
    assert output.is_dir()
    assert "summary unavailable" in captured.err
