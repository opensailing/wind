"""Qualification gates for downloadable external-solver demo cases."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest

from cfdviz.__main__ import main
from cfdviz.fluidx3d import (
    FluidX3DImportParameters,
    fluidx3d_field,
    import_fluidx3d_case,
)
from cfdviz.representative import (
    RepresentativeCaseRequirements,
    qualify_representative_case,
)


def _write_vtk(path: Path, values: np.ndarray, *, vtk_type: str = "float") -> Path:
    values = np.asarray(values)
    nx, ny, nz, components = values.shape
    header = (
        "# vtk DataFile Version 3.0\n"
        f"FluidX3D {path.name}\n"
        "BINARY\n"
        "DATASET STRUCTURED_POINTS\n"
        f"DIMENSIONS {nx} {ny} {nz}\n"
        "ORIGIN 0 0 0\n"
        "SPACING 1 1 1\n"
        f"POINT_DATA {nx * ny * nz}\n"
        f"SCALARS data {vtk_type} {components}\n"
        "LOOKUP_TABLE default\n"
    ).encode("ascii")
    dtype = {"float": ">f4", "unsigned_char": "u1"}[vtk_type]
    disk = values.transpose(2, 1, 0, 3).astype(dtype)
    path.write_bytes(header + disk.tobytes(order="C"))
    return path


def _velocity(step: int, *, static: bool = False, flat_spanwise: bool = False) -> np.ndarray:
    x, y, z = np.indices((5, 4, 3), dtype=np.float64)
    time = 0.0 if static else step * 0.002
    spanwise = np.zeros_like(z) if flat_spanwise else 0.08 * z + 0.01 * x
    crossflow = -0.04 * y if flat_spanwise else -0.04 * y + 0.005 * z
    temporal_spanwise = 0.0 if flat_spanwise else time * (0.2 + 0.05 * z)
    return np.stack(
        (
            0.4 + 0.03 * x + time * (1.0 + 0.1 * y),
            crossflow,
            spanwise + temporal_spanwise,
        ),
        axis=-1,
    ).astype("<f4")


def _case(
    tmp_path: Path,
    *,
    static: bool = False,
    flat_spanwise: bool = False,
) -> Path:
    steps = (100, 110, 120)
    velocity_files = tuple(
        _write_vtk(
            tmp_path / f"u-{step:09d}.vtk",
            _velocity(step, static=static, flat_spanwise=flat_spanwise),
        )
        for step in steps
    )
    flags = np.zeros((5, 4, 3, 1), dtype="u1")
    flags[2, 2, 1, 0] = 0x01
    flag_files = tuple(
        _write_vtk(
            tmp_path / f"flags-{step:09d}.vtk",
            flags,
            vtk_type="unsigned_char",
        )
        for step in steps
    )
    return import_fluidx3d_case(
        FluidX3DImportParameters(
            output=tmp_path / "Representative.cfdviz",
            name="qualification fixture",
            fields=(fluidx3d_field("U", velocity_files),),
            flag_files=flag_files,
            source_time_step=0.01,
            solver_step_offset=0,
            source_units="lattice",
            source_axes=("+X", "+Y", "+Z"),
            solver_method="lattice-Boltzmann D3Q19 SRT",
            solver_commit="fixture-revision",
            solver_configuration="D3Q19 SRT FP16S",
            source_case="sphere-wake",
            codec="none",
        )
    )


def _requirements() -> RepresentativeCaseRequirements:
    return RepresentativeCaseRequirements(
        solver_name="FluidX3D",
        solver_revision="fixture-revision",
        minimum_active_dimensions=(5, 4, 3),
        minimum_frames=3,
        minimum_spanwise_velocity_ratio=0.01,
        minimum_spanwise_gradient_ratio=0.01,
        minimum_temporal_change_ratio=1e-5,
        maximum_feature_displacement_cells=10.0,
    )


def test_external_three_dimensional_temporal_case_passes_every_gate(tmp_path: Path):
    report = qualify_representative_case(_case(tmp_path), _requirements())

    assert report.ok, report.render()
    assert report.metrics["effectiveSpatialDimensions"] == 3
    assert report.metrics["minimumAdjacentVelocityChangeRatio"] > 0
    assert report.metrics["spanwiseVelocityRatio"] > 0.01
    assert report.metrics["spanwiseGradientRatio"] > 0.01
    assert report.metrics["knownValueSamples"] > 0
    assert report.metrics["caseBytes"] > 0
    assert len(report.metrics["sha256"]) == 64


def test_analytic_or_unpinned_provenance_cannot_pass_as_representative(tmp_path: Path):
    root = _case(tmp_path)
    manifest_path = root / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["provenance"]["sourceType"] = "analytic-generator"
    del manifest["provenance"]["sourceRevision"]
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

    report = qualify_representative_case(root, _requirements())

    assert not report.ok
    assert any("sourceType" in error for error in report.errors)
    assert any("source revision" in error for error in report.errors)


def test_static_frames_fail_the_temporal_change_gate(tmp_path: Path):
    report = qualify_representative_case(
        _case(tmp_path, static=True),
        _requirements(),
    )

    assert not report.ok
    assert any("adjacent velocity" in error for error in report.errors)


def test_flat_spanwise_data_fails_velocity_and_gradient_gates(tmp_path: Path):
    report = qualify_representative_case(
        _case(tmp_path, flat_spanwise=True),
        _requirements(),
    )

    assert not report.ok
    assert any("spanwise velocity" in error for error in report.errors)
    assert any("spanwise gradient" in error for error in report.errors)


def test_invalid_thresholds_are_rejected_before_reading_a_case():
    with pytest.raises(ValueError, match="minimum_active_dimensions"):
        RepresentativeCaseRequirements(
            solver_name="FluidX3D",
            solver_revision="revision",
            minimum_active_dimensions=(1, 0, 1),
        )


def test_cli_emits_machine_readable_qualification_evidence(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
):
    root = _case(tmp_path)

    status = main(
        [
            "qualify-representative",
            str(root),
            "--solver",
            "FluidX3D",
            "--revision",
            "fixture-revision",
            "--minimum-active-dimensions",
            "5",
            "4",
            "3",
            "--minimum-frames",
            "3",
            "--minimum-spanwise-velocity-ratio",
            "0.01",
            "--minimum-spanwise-gradient-ratio",
            "0.01",
            "--minimum-temporal-change-ratio",
            "0.00001",
            "--maximum-feature-displacement-cells",
            "10",
            "--json",
        ]
    )

    assert status == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload["ok"] is True
    assert payload["metrics"]["effectiveSpatialDimensions"] == 3
