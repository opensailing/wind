"""FluidX3D volume exports converted into complete CFDViz cases."""

from __future__ import annotations

import json
from dataclasses import fields as dataclass_fields, replace
from pathlib import Path

import numpy as np
import pytest

from cfdviz.__main__ import main
from cfdviz.case import validate_case
from cfdviz.cvf import read_cvf
from cfdviz.fluidx3d import (
    FluidX3DError,
    FluidX3DImportParameters,
    fluidx3d_field,
    import_fluidx3d_case,
)
from cfdviz.manifest import load_manifest


def _write_vtk(
    path: Path,
    values: np.ndarray,
    *,
    origin=(-1.5, -1.0, -0.5),
    spacing=(0.5, 1.25, 2.0),
    vtk_type="float",
    title: str | None = None,
) -> Path:
    values = np.asarray(values)
    nx, ny, nz, components = values.shape
    header = (
        "# vtk DataFile Version 3.0\n"
        f"{title if title is not None else f'FluidX3D {path.name}'}\n"
        "BINARY\n"
        "DATASET STRUCTURED_POINTS\n"
        f"DIMENSIONS {nx} {ny} {nz}\n"
        f"ORIGIN {origin[0]} {origin[1]} {origin[2]}\n"
        f"SPACING {spacing[0]} {spacing[1]} {spacing[2]}\n"
        f"POINT_DATA {nx * ny * nz}\n"
        f"SCALARS data {vtk_type} {components}\n"
        "LOOKUP_TABLE default\n"
    ).encode("ascii")
    disk = values.transpose(2, 1, 0, 3)
    dtype = {"float": ">f4", "unsigned_char": "u1"}[vtk_type]
    path.write_bytes(header + disk.astype(dtype).tobytes(order="C"))
    return path


def _velocity(step: int) -> np.ndarray:
    x, y, z = np.indices((4, 3, 2), dtype=np.float64)
    return np.stack(
        (
            0.25 * x + step * 0.001,
            -0.5 * y,
            0.75 * z + 0.1 * x,
        ),
        axis=-1,
    ).astype("<f4")


def _flags() -> np.ndarray:
    flags = np.zeros((4, 3, 2, 1), dtype="u1")
    flags[1, 2, 1, 0] = 0x81  # TYPE_S plus an unrelated high bit.
    flags[2, 1, 0, 0] = 0x20  # TYPE_G free-surface gas cell.
    return flags


def _inputs(tmp_path: Path, *, steps=(0, 100)) -> tuple[list[Path], list[Path]]:
    velocity_files: list[Path] = []
    flag_files: list[Path] = []
    for step in steps:
        velocity_files.append(
            _write_vtk(tmp_path / f"u-{step:09d}.vtk", _velocity(step))
        )
        flag_files.append(
            _write_vtk(
                tmp_path / f"flags-{step:09d}.vtk",
                _flags(),
                vtk_type="unsigned_char",
            )
        )
    return velocity_files, flag_files


def _parameters(
    tmp_path: Path,
    *,
    steps=(0, 100),
    force=False,
) -> FluidX3DImportParameters:
    velocity_files, flag_files = _inputs(tmp_path, steps=steps)
    return FluidX3DImportParameters(
        output=tmp_path / "Imported.cfdviz",
        name="FluidX3D fixture",
        fields=(fluidx3d_field("U", velocity_files),),
        flag_files=tuple(flag_files),
        source_time_step=0.002,
        solver_step_offset=0,
        source_units="si",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
        length_scale=0.1,
        velocity_scale=2.0,
        length_unit="m",
        time_unit="s",
        mass_unit="kg",
        codec="none",
        brick_size=(3, 2, 2),
        solver_version="3.2",
        solver_commit="fixture-commit",
        solver_configuration="D3Q19 SRT",
        force=force,
    )


def test_imported_fluidx3d_case_validates_and_discloses_provenance(tmp_path: Path):
    root = import_fluidx3d_case(_parameters(tmp_path))

    report = validate_case(root)
    assert report.ok, report.render()
    manifest = load_manifest(root)
    assert manifest["case"]["quality"] == "external-solver-sample"
    assert manifest["case"]["solver"] == {
        "name": "FluidX3D",
        "version": "3.2",
        "method": "lattice-Boltzmann D3Q19",
        "commit": "fixture-commit",
        "configuration": "D3Q19 SRT",
    }
    provenance = manifest["provenance"]
    assert provenance["sourceType"] == "external-solver"
    assert provenance["exportCommand"] == "write_device_to_vtk"
    assert any("big-endian" in note for note in provenance["notes"])
    assert any("cell centres" in note for note in provenance["notes"])
    assert any("field identity" in note for note in provenance["notes"])
    assert (root / "known_values.json").is_file()


def test_vtk_points_are_reinterpreted_as_cells_with_a_half_spacing_shift(
    tmp_path: Path,
):
    root = import_fluidx3d_case(_parameters(tmp_path))
    manifest = load_manifest(root)
    grid = manifest["grids"][0]

    assert grid["dimensions"] == [4, 3, 2]
    assert grid["spacing"] == pytest.approx([0.05, 0.125, 0.2])
    assert grid["origin"] == pytest.approx([-0.175, -0.1625, -0.15])
    field = next(entry for entry in manifest["fields"] if entry["id"] == "U")
    assert field["association"] == "cell"

    source_origin = np.array([-1.5, -1.0, -0.5]) * 0.1
    cell_index = np.array([3, 2, 1])
    centre = np.array(grid["origin"]) + np.array(grid["spacing"]) * (
        cell_index + 0.5
    )
    assert centre == pytest.approx(
        source_origin + np.array(grid["spacing"]) * cell_index
    )


def test_axis_order_scaling_and_solid_mask_reach_the_stored_payload(tmp_path: Path):
    root = import_fluidx3d_case(_parameters(tmp_path))
    velocity = read_cvf(root / "frames/000000/U.cvf").values
    mask = read_cvf(root / "frames/000000/validMask.cvf").values[..., 0]

    assert velocity.shape == (4, 3, 2, 3)
    assert velocity[3, 1, 0] == pytest.approx(_velocity(0)[3, 1, 0] * 2.0)
    # Grid validity is not field validity: solid and gas cells remain usable by
    # force and phase fields, while velocity marks both unsupported sites NaN.
    assert mask[1, 2, 1] == 1
    assert mask[2, 1, 0] == 1
    assert np.isnan(velocity[1, 2, 1]).all()
    assert np.isnan(velocity[2, 1, 0]).all()
    assert mask.sum() == 24


def test_timeline_uses_filename_steps_and_declares_exact_source_cadence(
    tmp_path: Path,
):
    root = import_fluidx3d_case(_parameters(tmp_path))
    timeline = load_manifest(root)["timeline"]

    assert timeline["steps"] == [0, 100]
    assert timeline["times"] == pytest.approx([0.0, 0.2])
    assert timeline["sampling"]["sourceTimeStep"] == 0.002
    assert timeline["sampling"]["storedStepStride"] == 100
    assert timeline["sampling"]["maxFeatureDisplacementCells"] > 0


def test_single_frame_import_does_not_fabricate_a_sampling_stride(tmp_path: Path):
    root = import_fluidx3d_case(_parameters(tmp_path, steps=(6000,)))
    timeline = load_manifest(root)["timeline"]

    assert timeline["steps"] == [6000]
    assert timeline["times"] == pytest.approx([12.0])
    assert "sampling" not in timeline


def test_quality_metrics_are_recomputed_from_stored_velocity(tmp_path: Path):
    root = import_fluidx3d_case(_parameters(tmp_path))
    manifest = load_manifest(root)
    metrics = manifest["qualityMetrics"]

    stored = []
    masks = []
    for frame in range(2):
        stored.append(read_cvf(root / f"frames/{frame:06d}/U.cvf").values)
        masks.append(
            read_cvf(root / f"frames/{frame:06d}/validMask.cvf").values[..., 0]
            != 0
        )
    selected = np.concatenate(
        [
            values[mask][np.isfinite(values[mask]).all(axis=1)]
            for values, mask in zip(stored, masks)
        ],
        axis=0,
    )
    assert metrics["velocityComponentRms"] == pytest.approx(
        np.sqrt(np.mean(selected.astype(np.float64) ** 2, axis=0))
    )
    assert metrics["activeCellCount"] == 22
    assert metrics["activeDimensions"] == [4, 3, 2]
    assert metrics["effectiveSpatialDimensions"] == 3
    assert metrics["spanwiseGradientRms"] > 0
    assert metrics["temporalFrameCount"] == 2


def test_phi_preset_declares_the_volume_fraction_orientation(tmp_path: Path):
    velocity_files, flag_files = _inputs(tmp_path, steps=(0,))
    phi = np.zeros((4, 3, 2, 1), dtype="<f4")
    phi[:, :, 1, 0] = 1.0
    phi_file = _write_vtk(tmp_path / "phi-000000000.vtk", phi)
    parameters = FluidX3DImportParameters(
        output=tmp_path / "Phase.cfdviz",
        name="phase",
        fields=(
            fluidx3d_field("U", velocity_files),
            fluidx3d_field("phi", (phi_file,)),
        ),
        flag_files=tuple(flag_files),
        source_time_step=1.0,
        solver_step_offset=0,
        source_units="lattice",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
        codec="none",
    )

    root = import_fluidx3d_case(parameters)
    phase = next(
        field["phase"]
        for field in load_manifest(root)["fields"]
        if field["id"] == "phi"
    )

    assert phase == {
        "representation": "volume-fraction",
        "primaryPhase": "liquid",
        "secondaryPhase": "gas",
        "interfaceValue": 0.5,
        "inside": "greater-than-interface",
    }


def test_every_field_must_have_the_same_solver_steps(tmp_path: Path):
    velocity_files, flag_files = _inputs(tmp_path, steps=(0, 100))
    density_file = _write_vtk(
        tmp_path / "rho-000000000.vtk",
        np.ones((4, 3, 2, 1), dtype="<f4"),
    )
    parameters = FluidX3DImportParameters(
        output=tmp_path / "Bad.cfdviz",
        name="bad",
        fields=(
            fluidx3d_field("U", velocity_files),
            fluidx3d_field("density", (density_file,)),
        ),
        flag_files=tuple(flag_files),
        source_time_step=1.0,
        solver_step_offset=0,
        source_units="lattice",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
    )

    with pytest.raises(FluidX3DError, match="density.*steps.*U"):
        import_fluidx3d_case(parameters)


def test_grid_mismatch_is_rejected_before_any_case_is_claimed_valid(tmp_path: Path):
    velocity_files, flag_files = _inputs(tmp_path, steps=(0, 100))
    bad = _write_vtk(
        tmp_path / "rho-000000100.vtk",
        np.ones((4, 3, 2, 1), dtype="<f4"),
        spacing=(0.5, 1.5, 2.0),
    )
    good = _write_vtk(
        tmp_path / "rho-000000000.vtk",
        np.ones((4, 3, 2, 1), dtype="<f4"),
    )
    parameters = FluidX3DImportParameters(
        output=tmp_path / "BadGrid.cfdviz",
        name="bad-grid",
        fields=(
            fluidx3d_field("U", velocity_files),
            fluidx3d_field("density", (good, bad)),
        ),
        flag_files=tuple(flag_files),
        source_time_step=1.0,
        solver_step_offset=0,
        source_units="lattice",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
    )

    with pytest.raises(FluidX3DError, match="rho-000000100.*grid.*does not match"):
        import_fluidx3d_case(parameters)


def test_nonempty_output_is_preserved_unless_force_is_explicit(tmp_path: Path):
    parameters = _parameters(tmp_path)
    parameters.output.mkdir()
    stale = parameters.output / "keep.txt"
    stale.write_text("keep", encoding="utf-8")

    with pytest.raises(FluidX3DError, match="not empty.*force"):
        import_fluidx3d_case(parameters)

    assert stale.read_text(encoding="utf-8") == "keep"


def test_force_removes_stale_frames_from_an_earlier_import(tmp_path: Path):
    parameters = _parameters(tmp_path)
    parameters.output.mkdir()
    stale = parameters.output / "frames" / "000099" / "U.cvf"
    stale.parent.mkdir(parents=True)
    stale.write_bytes(b"old")
    forced = replace(parameters, force=True)

    root = import_fluidx3d_case(forced)

    assert not stale.exists()
    assert validate_case(root).ok


def test_manifest_json_never_contains_nonstandard_nan_tokens(tmp_path: Path):
    root = import_fluidx3d_case(_parameters(tmp_path))
    text = (root / "manifest.json").read_text(encoding="utf-8")

    def reject_constant(token: str):
        raise AssertionError(f"manifest contains non-standard JSON token {token}")

    # parse_constant sees bare NaN/Infinity tokens but not those words in prose.
    json.loads(text, parse_constant=reject_constant)


def test_import_is_byte_deterministic_for_the_same_inputs(tmp_path: Path):
    parameters = _parameters(tmp_path)
    first_parameters = replace(parameters, output=tmp_path / "First.cfdviz")
    second_parameters = replace(parameters, output=tmp_path / "Second.cfdviz")

    first = import_fluidx3d_case(first_parameters)
    second = import_fluidx3d_case(second_parameters)
    first_files = {
        path.relative_to(first): path.read_bytes()
        for path in first.rglob("*")
        if path.is_file()
    }
    second_files = {
        path.relative_to(second): path.read_bytes()
        for path in second.rglob("*")
        if path.is_file()
    }

    assert first_files == second_files


def test_explicit_all_valid_assumption_is_disclosed_and_produces_all_valid_mask(
    tmp_path: Path,
):
    velocity_files, _ = _inputs(tmp_path, steps=(0,))
    parameters = FluidX3DImportParameters(
        output=tmp_path / "NoFlags.cfdviz",
        name="no-flags",
        fields=(fluidx3d_field("U", velocity_files),),
        source_time_step=1.0,
        solver_step_offset=0,
        source_units="lattice",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
        assume_all_cells_valid=True,
        codec="none",
    )

    root = import_fluidx3d_case(parameters)
    mask = read_cvf(root / "frames/000000/validMask.cvf").values[..., 0]
    notes = load_manifest(root)["provenance"]["notes"]

    assert np.all(mask == 1)
    assert any("No FluidX3D flags sequence" in note for note in notes)


def test_flags_grid_spacing_must_match_the_velocity_grid(tmp_path: Path):
    velocity_files, flag_files = _inputs(tmp_path, steps=(0,))
    _write_vtk(
        flag_files[0],
        _flags(),
        spacing=(0.5, 1.5, 2.0),
        vtk_type="unsigned_char",
    )
    parameters = FluidX3DImportParameters(
        output=tmp_path / "BadFlags.cfdviz",
        name="bad-flags",
        fields=(fluidx3d_field("U", velocity_files),),
        flag_files=tuple(flag_files),
        source_time_step=1.0,
        solver_step_offset=0,
        source_units="lattice",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
        codec="none",
    )

    with pytest.raises(FluidX3DError, match="flags-000000000.*grid.*U"):
        import_fluidx3d_case(parameters)


def test_nonuniform_stored_steps_do_not_claim_fixed_sampling(tmp_path: Path):
    root = import_fluidx3d_case(_parameters(tmp_path, steps=(0, 100, 250)))

    timeline = load_manifest(root)["timeline"]
    assert timeline["steps"] == [0, 100, 250]
    assert "sampling" not in timeline


def test_import_fluidx3d_cli_writes_a_valid_case(capsys, tmp_path: Path):
    velocity_files, flag_files = _inputs(tmp_path)
    output = tmp_path / "Cli.cfdviz"

    status = main(
        [
            "import-fluidx3d",
            "--output",
            str(output),
            "--name",
            "CLI fixture",
            "--field",
            "U",
            *(str(path) for path in velocity_files),
            "--flags",
            *(str(path) for path in flag_files),
            "--source-time-step",
            "0.002",
            "--solver-step-offset",
            "0",
            "--source-units",
            "si",
            "--source-axes",
            "+X",
            "+Y",
            "+Z",
            "--solver-method",
            "lattice-Boltzmann D3Q19",
            "--length-scale",
            "0.1",
            "--velocity-scale",
            "2",
            "--length-unit",
            "m",
            "--time-unit",
            "s",
            "--mass-unit",
            "kg",
            "--codec",
            "none",
        ]
    )
    captured = capsys.readouterr()

    assert status == 0, captured.err
    assert "FluidX3D" in captured.out
    assert validate_case(output).ok


def test_import_fluidx3d_cli_reports_input_errors_without_a_traceback(
    capsys,
    tmp_path: Path,
):
    bad = _write_vtk(tmp_path / "velocity.vtk", _velocity(0))

    status = main(
        [
            "import-fluidx3d",
            "--output",
            str(tmp_path / "BadCli.cfdviz"),
            "--name",
            "bad",
            "--field",
            "U",
            str(bad),
            "--source-time-step",
            "1",
            "--solver-step-offset",
            "0",
            "--source-units",
            "lattice",
            "--source-axes",
            "+X",
            "+Y",
            "+Z",
            "--solver-method",
            "lattice-Boltzmann D3Q19",
            "--assume-all-cells-valid",
        ]
    )
    captured = capsys.readouterr()
    text = captured.out + captured.err

    assert status != 0
    assert "solverStep" in text
    assert "Traceback" not in text
    assert "internal error" not in text


def test_field_source_exposes_only_explicit_caller_inputs(tmp_path: Path):
    source = fluidx3d_field("U", (tmp_path / "u-000000000.vtk",))

    assert [entry.name for entry in dataclass_fields(type(source))] == [
        "field_id",
        "files",
        "value_scale",
        "unit",
    ]


def test_field_unit_override_must_be_a_nonempty_string(tmp_path: Path):
    with pytest.raises(FluidX3DError, match="unit.*non-empty string"):
        fluidx3d_field(
            "density",
            (tmp_path / "rho-000000000.vtk",),
            unit=7,
        )


def test_field_mapping_requires_the_canonical_fluidx3d_filename_prefix(
    tmp_path: Path,
):
    path = _write_vtk(tmp_path / "rho-000000000.vtk", _velocity(0))
    parameters = FluidX3DImportParameters(
        output=tmp_path / "WrongPrefix.cfdviz",
        name="wrong-prefix",
        fields=(fluidx3d_field("U", (path,)),),
        source_time_step=1.0,
        solver_step_offset=0,
        source_units="lattice",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
        assume_all_cells_valid=True,
        codec="none",
    )

    with pytest.raises(FluidX3DError, match="field 'U'.*prefix.*'u'"):
        import_fluidx3d_case(parameters)


def test_source_unit_mode_and_solver_method_are_required(tmp_path: Path):
    velocity_files, _ = _inputs(tmp_path, steps=(0,))
    common = {
        "output": tmp_path / "Required.cfdviz",
        "name": "required",
        "fields": (fluidx3d_field("U", velocity_files),),
        "source_time_step": 1.0,
        "solver_step_offset": 0,
        "source_axes": ("+X", "+Y", "+Z"),
        "solver_method": "lattice-Boltzmann D3Q19",
    }

    with pytest.raises(TypeError, match="source_units"):
        FluidX3DImportParameters(**common)

    common["source_units"] = "lattice"
    del common["solver_method"]
    with pytest.raises(TypeError, match="solver_method"):
        FluidX3DImportParameters(**common)


def test_source_unit_mode_derives_honest_default_units(tmp_path: Path):
    velocity_files, _ = _inputs(tmp_path, steps=(0,))
    parameters = FluidX3DImportParameters(
        output=tmp_path / "SI.cfdviz",
        name="si",
        fields=(fluidx3d_field("U", velocity_files),),
        source_time_step=1.0,
        solver_step_offset=0,
        source_units="si",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
        assume_all_cells_valid=True,
        codec="none",
    )

    root = import_fluidx3d_case(parameters)

    assert load_manifest(root)["units"] == {
        "length": "m",
        "time": "s",
        "mass": "kg",
        "temperature": "K",
    }
    assert any(
        "sourceUnits='si'" in note
        for note in load_manifest(root)["provenance"]["notes"]
    )


def test_non_fluidx3d_vtk_title_is_rejected(tmp_path: Path):
    path = _write_vtk(
        tmp_path / "u-000000000.vtk",
        _velocity(0),
        title="generic VTK export",
    )
    parameters = FluidX3DImportParameters(
        output=tmp_path / "Generic.cfdviz",
        name="generic",
        fields=(fluidx3d_field("U", (path,)),),
        source_time_step=1.0,
        solver_step_offset=0,
        source_units="lattice",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
        assume_all_cells_valid=True,
        codec="none",
    )

    with pytest.raises(FluidX3DError, match=r"u-000000000\.vtk.*title.*FluidX3D"):
        import_fluidx3d_case(parameters)


def test_axis_mapping_reorders_grid_samples_and_vector_components(tmp_path: Path):
    source = np.zeros((2, 3, 4, 3), dtype="<f4")
    source[...] = (1.0, 2.0, 3.0)
    path = _write_vtk(
        tmp_path / "u-000000000.vtk",
        source,
        origin=(10.0, 20.0, 30.0),
        spacing=(1.0, 2.0, 3.0),
    )
    parameters = FluidX3DImportParameters(
        output=tmp_path / "Mapped.cfdviz",
        name="mapped",
        fields=(fluidx3d_field("U", (path,)),),
        source_time_step=1.0,
        solver_step_offset=0,
        source_units="lattice",
        source_axes=("+Z", "+Y", "-X"),
        solver_method="lattice-Boltzmann D3Q27",
        assume_all_cells_valid=True,
        codec="none",
    )

    root = import_fluidx3d_case(parameters)
    manifest = load_manifest(root)
    grid = manifest["grids"][0]
    velocity = read_cvf(root / "frames/000000/U.cvf").values

    assert grid["dimensions"] == [4, 3, 2]
    assert grid["spacing"] == pytest.approx([3.0, 2.0, 1.0])
    assert grid["origin"] == pytest.approx([-40.5, 19.0, 9.5])
    assert velocity.shape == (4, 3, 2, 3)
    assert velocity[0, 0, 0] == pytest.approx([-3.0, 2.0, 1.0])
    assert manifest["case"]["solver"]["method"] == "lattice-Boltzmann D3Q27"


def test_gas_cells_are_invalid_for_velocity_but_retained_for_phase(tmp_path: Path):
    velocity_files, flag_files = _inputs(tmp_path, steps=(0,))
    phi = np.full((4, 3, 2, 1), 0.75, dtype="<f4")
    phi[2, 1, 0, 0] = 0.25
    phi_path = _write_vtk(tmp_path / "phi-000000000.vtk", phi)
    parameters = FluidX3DImportParameters(
        output=tmp_path / "PhaseValidity.cfdviz",
        name="phase-validity",
        fields=(
            fluidx3d_field("U", velocity_files),
            fluidx3d_field("phi", (phi_path,)),
        ),
        flag_files=tuple(flag_files),
        source_time_step=1.0,
        solver_step_offset=0,
        source_units="lattice",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
        codec="none",
    )

    root = import_fluidx3d_case(parameters)
    velocity = read_cvf(root / "frames/000000/U.cvf").values
    stored_phi = read_cvf(root / "frames/000000/phi.cvf").values[..., 0]
    mask = read_cvf(root / "frames/000000/validMask.cvf").values[..., 0]

    assert mask[2, 1, 0] == 1
    assert np.isnan(velocity[2, 1, 0]).all()
    assert stored_phi[2, 1, 0] == pytest.approx(0.25)
    assert np.isnan(stored_phi[1, 2, 1])


def test_force_values_are_retained_only_on_solid_cells(tmp_path: Path):
    velocity_files, flag_files = _inputs(tmp_path, steps=(0,))
    force = np.ones((4, 3, 2, 3), dtype="<f4")
    force[1, 2, 1] = (9.0, 8.0, 7.0)
    force_path = _write_vtk(tmp_path / "F-000000000.vtk", force)
    parameters = FluidX3DImportParameters(
        output=tmp_path / "Force.cfdviz",
        name="force",
        fields=(
            fluidx3d_field("U", velocity_files),
            fluidx3d_field("force", (force_path,)),
        ),
        flag_files=tuple(flag_files),
        source_time_step=1.0,
        solver_step_offset=0,
        source_units="lattice",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
        force_interpretation="boundary",
        codec="none",
    )

    root = import_fluidx3d_case(parameters)
    stored_force = read_cvf(root / "frames/000000/force.cvf").values

    assert stored_force[1, 2, 1] == pytest.approx([9.0, 8.0, 7.0])
    assert np.isnan(stored_force[0, 0, 0]).all()


def test_compression_level_is_rejected_when_the_codec_ignores_it(tmp_path: Path):
    parameters = _parameters(tmp_path)

    with pytest.raises(FluidX3DError, match="level.*zlib"):
        replace(parameters, codec="lz4", level=4)


def test_oversized_solver_step_is_rejected_with_its_source_path(tmp_path: Path):
    digits = "9" * 240
    path = _write_vtk(tmp_path / f"u-{digits}.vtk", _velocity(0))
    parameters = FluidX3DImportParameters(
        output=tmp_path / "HugeStep.cfdviz",
        name="huge-step",
        fields=(fluidx3d_field("U", (path,)),),
        source_time_step=1.0,
        solver_step_offset=0,
        source_units="lattice",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
        assume_all_cells_valid=True,
        codec="none",
    )

    with pytest.raises(FluidX3DError, match=rf"{digits}.*exactly"):
        import_fluidx3d_case(parameters)


@pytest.mark.parametrize(
    ("field_id", "value", "float_type", "message"),
    [
        ("U", np.inf, "float32", "infinity after scaling"),
        ("phi", 1.5, "float32", "outside \\[0, 1\\]"),
        ("U", 1.0e10, "float16", "overflows float16"),
    ],
)
def test_field_conversion_errors_name_path_step_and_field(
    tmp_path: Path,
    field_id: str,
    value: float,
    float_type: str,
    message: str,
):
    velocity = _velocity(0)
    velocity_path = _write_vtk(tmp_path / "u-000000000.vtk", velocity)
    fields = [fluidx3d_field("U", (velocity_path,))]
    target_path = velocity_path
    if field_id == "phi":
        values = np.full((4, 3, 2, 1), value, dtype="<f4")
        target_path = _write_vtk(tmp_path / "phi-000000000.vtk", values)
        fields.append(fluidx3d_field("phi", (target_path,)))
    else:
        velocity[0, 0, 0, 0] = value
        target_path = _write_vtk(velocity_path, velocity)
    parameters = FluidX3DImportParameters(
        output=tmp_path / "BadValue.cfdviz",
        name="bad-value",
        fields=tuple(fields),
        source_time_step=1.0,
        solver_step_offset=0,
        source_units="lattice",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
        assume_all_cells_valid=True,
        codec="none",
        float_type=float_type,
    )

    with pytest.raises(
        FluidX3DError,
        match=rf"{target_path.name}.*step 0.*field '{field_id}'.*{message}",
    ):
        import_fluidx3d_case(parameters)


def test_float32_nan_payload_bits_survive_identity_conversion(tmp_path: Path):
    velocity = _velocity(0)
    signaling_nan_bits = np.array([0x7FA12345], dtype="<u4")
    velocity[0, 0, 0, 0] = signaling_nan_bits.view("<f4")[0]
    path = _write_vtk(tmp_path / "u-000000000.vtk", velocity)
    parameters = FluidX3DImportParameters(
        output=tmp_path / "NanBits.cfdviz",
        name="nan-bits",
        fields=(fluidx3d_field("U", (path,)),),
        source_time_step=1.0,
        solver_step_offset=0,
        source_units="lattice",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
        assume_all_cells_valid=True,
        codec="none",
        float_type="float32",
    )

    root = import_fluidx3d_case(parameters)
    stored = read_cvf(root / "frames/000000/U.cvf").values

    assert stored[0, 0, 0, 0:1].view("<u4")[0] == signaling_nan_bits[0]


def test_packed_moving_boundary_flag_is_not_treated_as_solid(tmp_path: Path):
    velocity = _velocity(0)
    velocity[0, 0, 0] = (4.0, 5.0, 6.0)
    flags = _flags()
    flags[0, 0, 0, 0] = 0x03  # TYPE_MS, not TYPE_S.
    velocity_path = _write_vtk(tmp_path / "u-000000000.vtk", velocity)
    flag_path = _write_vtk(
        tmp_path / "flags-000000000.vtk",
        flags,
        vtk_type="unsigned_char",
    )
    parameters = FluidX3DImportParameters(
        output=tmp_path / "MovingBoundary.cfdviz",
        name="moving-boundary",
        fields=(fluidx3d_field("U", (velocity_path,)),),
        flag_files=(flag_path,),
        source_time_step=1.0,
        solver_step_offset=0,
        source_units="lattice",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
        codec="none",
    )

    root = import_fluidx3d_case(parameters)
    stored = read_cvf(root / "frames/000000/U.cvf").values

    assert stored[0, 0, 0] == pytest.approx([4.0, 5.0, 6.0])


def test_temperature_excludes_exact_gas_cells(tmp_path: Path):
    velocity_files, flag_files = _inputs(tmp_path, steps=(0,))
    temperature = np.full((4, 3, 2, 1), 2.0, dtype="<f4")
    path = _write_vtk(tmp_path / "T-000000000.vtk", temperature)
    parameters = FluidX3DImportParameters(
        output=tmp_path / "Temperature.cfdviz",
        name="temperature",
        fields=(
            fluidx3d_field("U", velocity_files),
            fluidx3d_field("temperature", (path,)),
        ),
        flag_files=tuple(flag_files),
        source_time_step=1.0,
        solver_step_offset=0,
        source_units="lattice",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
        codec="none",
    )

    root = import_fluidx3d_case(parameters)
    stored = read_cvf(root / "frames/000000/temperature.cvf").values[..., 0]

    assert np.isnan(stored[2, 1, 0])


def test_force_interpretation_is_explicit_and_preserves_volume_force(tmp_path: Path):
    velocity_files, flag_files = _inputs(tmp_path, steps=(0,))
    force = np.ones((4, 3, 2, 3), dtype="<f4")
    force[0, 0, 0] = (4.0, 5.0, 6.0)
    path = _write_vtk(tmp_path / "F-000000000.vtk", force)
    common = {
        "output": tmp_path / "VolumeForce.cfdviz",
        "name": "volume-force",
        "fields": (
            fluidx3d_field("U", velocity_files),
            fluidx3d_field("force", (path,)),
        ),
        "flag_files": tuple(flag_files),
        "source_time_step": 1.0,
        "solver_step_offset": 0,
        "source_units": "lattice",
        "source_axes": ("+X", "+Y", "+Z"),
        "solver_method": "lattice-Boltzmann D3Q19",
        "codec": "none",
    }

    with pytest.raises(FluidX3DError, match="force_interpretation"):
        FluidX3DImportParameters(**common)

    parameters = FluidX3DImportParameters(
        **common,
        force_interpretation="volume",
    )
    root = import_fluidx3d_case(parameters)
    stored = read_cvf(root / "frames/000000/force.cvf").values

    assert stored[0, 0, 0] == pytest.approx([4.0, 5.0, 6.0])
    assert np.isnan(stored[1, 2, 1]).all()
    assert np.isnan(stored[2, 1, 0]).all()


def test_force_interpretation_changes_case_identity_and_provenance(tmp_path: Path):
    velocity_files, flag_files = _inputs(tmp_path, steps=(0,))
    force_path = _write_vtk(
        tmp_path / "F-000000000.vtk",
        np.ones((4, 3, 2, 3), dtype="<f4"),
    )
    parameters = FluidX3DImportParameters(
        output=tmp_path / "BoundaryForce.cfdviz",
        name="force-identity",
        fields=(
            fluidx3d_field("U", velocity_files),
            fluidx3d_field("force", (force_path,)),
        ),
        flag_files=tuple(flag_files),
        source_time_step=1.0,
        solver_step_offset=0,
        source_units="lattice",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
        force_interpretation="boundary",
        codec="none",
    )

    boundary = import_fluidx3d_case(parameters)
    volume = import_fluidx3d_case(
        replace(
            parameters,
            output=tmp_path / "VolumeForce.cfdviz",
            force_interpretation="volume",
        )
    )
    boundary_manifest = load_manifest(boundary)
    volume_manifest = load_manifest(volume)

    assert boundary_manifest["case"]["id"] != volume_manifest["case"]["id"]
    boundary_notes = "\n".join(boundary_manifest["provenance"]["notes"])
    volume_notes = "\n".join(volume_manifest["provenance"]["notes"])
    assert "force is retained only on exact solid cells" in boundary_notes
    assert "force is retained only on active fluid cells" in volume_notes
    assert "force outside solids" not in volume_notes


def test_scaling_uses_wide_precision_before_float32_storage(tmp_path: Path):
    velocity_files, _ = _inputs(tmp_path, steps=(0,))
    density = np.full((4, 3, 2, 1), 1.0e30, dtype="<f4")
    path = _write_vtk(tmp_path / "rho-000000000.vtk", density)
    parameters = FluidX3DImportParameters(
        output=tmp_path / "WideScale.cfdviz",
        name="wide-scale",
        fields=(
            fluidx3d_field("U", velocity_files),
            fluidx3d_field("density", (path,), value_scale=1.0e-50),
        ),
        source_time_step=1.0,
        solver_step_offset=0,
        source_units="lattice",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
        assume_all_cells_valid=True,
        codec="none",
    )

    root = import_fluidx3d_case(parameters)
    stored = read_cvf(root / "frames/000000/density.cvf").values

    assert stored[0, 0, 0, 0] == pytest.approx(1.0e-20, rel=1.0e-6)


def test_lattice_temperature_is_not_labeled_kelvin(tmp_path: Path):
    velocity_files, _ = _inputs(tmp_path, steps=(0,))
    temperature = _write_vtk(
        tmp_path / "T-000000000.vtk",
        np.ones((4, 3, 2, 1), dtype="<f4"),
    )
    parameters = FluidX3DImportParameters(
        output=tmp_path / "LatticeTemperature.cfdviz",
        name="lattice-temperature",
        fields=(
            fluidx3d_field("U", velocity_files),
            fluidx3d_field("temperature", (temperature,)),
        ),
        source_time_step=1.0,
        solver_step_offset=0,
        source_units="lattice",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
        assume_all_cells_valid=True,
        codec="none",
    )

    root = import_fluidx3d_case(parameters)
    manifest = load_manifest(root)
    field = next(item for item in manifest["fields"] if item["id"] == "temperature")

    assert manifest["units"]["temperature"] == "lattice-temperature"
    assert field["unit"] == "lattice-temperature"


def test_flag_dependent_options_require_a_flags_sequence(tmp_path: Path):
    velocity_files, _ = _inputs(tmp_path, steps=(0,))
    common = {
        "output": tmp_path / "FlagsRequired.cfdviz",
        "name": "flags-required",
        "fields": (fluidx3d_field("U", velocity_files),),
        "source_time_step": 1.0,
        "solver_step_offset": 0,
        "source_units": "lattice",
        "source_axes": ("+X", "+Y", "+Z"),
        "solver_method": "lattice-Boltzmann D3Q19",
        "assume_all_cells_valid": True,
    }

    with pytest.raises(FluidX3DError, match="excluded_flag_bits.*flags"):
        FluidX3DImportParameters(**common, excluded_flag_bits=0x01)


def test_phi_scaling_and_custom_velocity_units_are_rejected(tmp_path: Path):
    velocity_files, flag_files = _inputs(tmp_path, steps=(0,))
    phi = _write_vtk(
        tmp_path / "phi-000000000.vtk",
        np.ones((4, 3, 2, 1), dtype="<f4"),
    )
    common = {
        "output": tmp_path / "SemanticScale.cfdviz",
        "name": "semantic-scale",
        "flag_files": tuple(flag_files),
        "source_time_step": 1.0,
        "solver_step_offset": 0,
        "source_units": "lattice",
        "source_axes": ("+X", "+Y", "+Z"),
        "solver_method": "lattice-Boltzmann D3Q19",
    }

    with pytest.raises(FluidX3DError, match="phi.*value_scale"):
        FluidX3DImportParameters(
            **common,
            fields=(
                fluidx3d_field("U", velocity_files),
                fluidx3d_field("phi", (phi,), value_scale=0.5),
            ),
        )
    with pytest.raises(FluidX3DError, match="U.*unit"):
        FluidX3DImportParameters(
            **common,
            fields=(fluidx3d_field("U", velocity_files, unit="km/h"),),
        )


def test_zlib_level_and_force_flag_are_validated_at_construction(tmp_path: Path):
    parameters = _parameters(tmp_path)

    with pytest.raises(FluidX3DError, match="zlib.*level.*0.*9"):
        replace(parameters, codec="zlib", level=10)
    with pytest.raises(FluidX3DError, match="force must be a boolean"):
        replace(parameters, force="false")


@pytest.mark.parametrize("bits", ["1", 1.5, True])
def test_excluded_flag_bits_requires_an_integer_byte(tmp_path: Path, bits):
    parameters = _parameters(tmp_path)

    with pytest.raises(FluidX3DError, match="excluded_flag_bits.*integer.*0.*255"):
        replace(parameters, excluded_flag_bits=bits)


def test_solver_step_offset_recovers_fluidx3d_filename_rollover(tmp_path: Path):
    velocity_files, _ = _inputs(tmp_path, steps=(5,))
    parameters = FluidX3DImportParameters(
        output=tmp_path / "Offset.cfdviz",
        name="offset",
        fields=(fluidx3d_field("U", velocity_files),),
        source_time_step=0.25,
        solver_step_offset=1_000_000_000,
        source_units="lattice",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
        assume_all_cells_valid=True,
        codec="none",
    )

    root = import_fluidx3d_case(parameters)
    timeline = load_manifest(root)["timeline"]

    assert timeline["steps"] == [1_000_000_005]
    assert timeline["times"] == [250_000_001.25]


def test_sequence_spanning_filename_rollover_is_rejected(tmp_path: Path):
    before = _write_vtk(tmp_path / "u-999999999.vtk", _velocity(0))
    after = _write_vtk(tmp_path / "u-000000000.vtk", _velocity(1))
    parameters = FluidX3DImportParameters(
        output=tmp_path / "CrossRollover.cfdviz",
        name="cross-rollover",
        fields=(fluidx3d_field("U", (before, after)),),
        source_time_step=0.25,
        solver_step_offset=0,
        source_units="lattice",
        source_axes=("+X", "+Y", "+Z"),
        solver_method="lattice-Boltzmann D3Q19",
        assume_all_cells_valid=True,
        codec="none",
    )

    with pytest.raises(FluidX3DError, match="nine-digit.*rollover"):
        import_fluidx3d_case(parameters)
