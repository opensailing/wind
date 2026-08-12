"""Checksum-verified installation of the representative demo archive."""

from __future__ import annotations

import hashlib
import json
import stat
import subprocess
import tarfile
from pathlib import Path

import numpy as np

from cfdviz.fluidx3d import (
    FluidX3DImportParameters,
    fluidx3d_field,
    import_fluidx3d_case,
)


REPO_ROOT = Path(__file__).resolve().parents[4]
DOWNLOADER = REPO_ROOT / "FlowViz" / "Tools" / "download_demo_case.sh"


def test_downloader_is_directly_executable():
    assert DOWNLOADER.stat().st_mode & stat.S_IXUSR


def _write_vtk(path: Path, values: np.ndarray, *, vtk_type: str = "float") -> Path:
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


def _external_case(root: Path) -> Path:
    steps = (0, 10)
    velocity_files = []
    flag_files = []
    x, y, z = np.indices((3, 3, 2), dtype=np.float32)
    flags = np.zeros((3, 3, 2, 1), dtype="u1")
    flags[1, 1, 1, 0] = 0x01
    for step in steps:
        velocity = np.stack(
            (
                0.3 + 0.02 * x + step * 0.001,
                0.03 * y,
                0.04 * z + 0.01 * x + step * 0.0002,
            ),
            axis=-1,
        ).astype("<f4")
        velocity_files.append(_write_vtk(root / f"u-{step:09d}.vtk", velocity))
        flag_files.append(
            _write_vtk(
                root / f"flags-{step:09d}.vtk",
                flags,
                vtk_type="unsigned_char",
            )
        )
    return import_fluidx3d_case(
        FluidX3DImportParameters(
            output=root / "Source.cfdviz",
            name="download fixture",
            fields=(fluidx3d_field("U", velocity_files),),
            flag_files=tuple(flag_files),
            source_time_step=0.01,
            solver_step_offset=0,
            source_units="lattice",
            source_axes=("+X", "+Y", "+Z"),
            solver_method="lattice-Boltzmann D3Q19 SRT",
            solver_commit="download-fixture-revision",
            solver_configuration="fixture",
            source_case="fixture",
            excluded_flag_bits=0x01,
            codec="none",
        )
    )


def _archive(tmp_path: Path) -> tuple[Path, Path]:
    case = _external_case(tmp_path)
    release = tmp_path / "stage" / "TinyRelease"
    (release / "case").mkdir(parents=True)
    (release / "altered-source" / "FluidX3D" / "src").mkdir(parents=True)
    (release / "recipe").mkdir()
    (release / "evidence").mkdir()
    for source in case.rglob("*"):
        target = release / "case" / source.relative_to(case)
        if source.is_dir():
            target.mkdir(exist_ok=True)
        else:
            target.write_bytes(source.read_bytes())
    (release / "altered-source" / "FluidX3D" / "LICENSE.md").write_text(
        "fixture license\n", encoding="utf-8"
    )
    (release / "altered-source" / "FluidX3D" / "src" / "setup.cpp").write_text(
        "// fixture altered setup\n", encoding="utf-8"
    )
    (release / "altered-source" / "FluidX3D" / "src" / "defines.hpp").write_text(
        "// fixture altered defines\n", encoding="utf-8"
    )
    (release / "recipe" / "sphere_wake_setup.cpp").write_text(
        "// fixture recipe\n", encoding="utf-8"
    )
    (release / "recipe" / "defines.patch").write_text(
        "fixture patch\n", encoding="utf-8"
    )
    (release / "recipe" / "README.md").write_text(
        "fixture recipe\n", encoding="utf-8"
    )
    (release / "evidence" / "qualification.json").write_text(
        "{}\n", encoding="utf-8"
    )
    (release / "evidence" / "known-values.txt").write_text(
        "fixture known values\n", encoding="utf-8"
    )
    (release / "generation.json").write_text("{}\n", encoding="utf-8")
    archive = tmp_path / "tiny.tar.gz"
    with tarfile.open(archive, "w:gz") as stream:
        stream.add(release, arcname="TinyRelease")
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    descriptor = tmp_path / "tiny.download.json"
    descriptor.write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "archiveName": archive.name,
                "archiveBytes": archive.stat().st_size,
                "archiveSha256": digest,
                "url": None,
                "releaseRoot": "TinyRelease",
                "casePath": "case",
                "caseDirectory": "Installed.cfdviz",
                "supportDirectory": "Installed.release",
                "qualification": {
                    "solverName": "FluidX3D",
                    "solverRevision": "download-fixture-revision",
                    "minimumActiveDimensions": [3, 3, 2],
                    "minimumFrames": 2,
                    "minimumSpanwiseVelocityRatio": 0.001,
                    "minimumSpanwiseGradientRatio": 0.001,
                    "minimumTemporalChangeRatio": 1e-6,
                    "maximumFeatureDisplacementCells": 10.0,
                },
            }
        ),
        encoding="utf-8",
    )
    return archive, descriptor


def test_downloader_verifies_extracts_and_qualifies_before_installing(tmp_path: Path):
    archive, descriptor = _archive(tmp_path)

    result = subprocess.run(
        [
            "bash",
            str(DOWNLOADER),
            "--descriptor",
            str(descriptor),
            "--archive",
            str(archive),
        ],
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr + result.stdout
    assert (tmp_path / "Installed.cfdviz" / "manifest.json").is_file()
    assert (
        tmp_path
        / "Installed.release"
        / "altered-source"
        / "FluidX3D"
        / "LICENSE.md"
    ).is_file()
    assert "installed representative demo" in result.stdout


def test_license_only_release_is_rejected(tmp_path: Path):
    archive, descriptor = _archive(tmp_path)
    unpacked = tmp_path / "unpacked"
    with tarfile.open(archive, "r:gz") as stream:
        stream.extractall(unpacked)
    (unpacked / "TinyRelease" / "altered-source" / "FluidX3D" / "src" / "setup.cpp").unlink()
    with tarfile.open(archive, "w:gz") as stream:
        stream.add(unpacked / "TinyRelease", arcname="TinyRelease")
    payload = json.loads(descriptor.read_text(encoding="utf-8"))
    payload["archiveBytes"] = archive.stat().st_size
    payload["archiveSha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
    descriptor.write_text(json.dumps(payload), encoding="utf-8")

    result = subprocess.run(
        [
            "bash",
            str(DOWNLOADER),
            "--descriptor",
            str(descriptor),
            "--archive",
            str(archive),
        ],
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode != 0
    assert "altered-source/FluidX3D/src/setup.cpp" in result.stderr
    assert not (tmp_path / "Installed.cfdviz").exists()


def test_bad_archive_checksum_installs_nothing(tmp_path: Path):
    archive, descriptor = _archive(tmp_path)
    archive.write_bytes(archive.read_bytes() + b"corrupt")

    result = subprocess.run(
        [
            "bash",
            str(DOWNLOADER),
            "--descriptor",
            str(descriptor),
            "--archive",
            str(archive),
        ],
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode != 0
    assert "checksum" in result.stderr.lower() or "size" in result.stderr.lower()
    assert not (tmp_path / "Installed.cfdviz").exists()
    assert not (tmp_path / "Installed.release").exists()


def test_force_preserves_existing_install_when_qualification_fails(tmp_path: Path):
    archive, descriptor = _archive(tmp_path)
    payload = json.loads(descriptor.read_text(encoding="utf-8"))
    payload["qualification"]["solverRevision"] = "wrong-revision"
    descriptor.write_text(json.dumps(payload), encoding="utf-8")

    installed_case = tmp_path / "Installed.cfdviz"
    installed_support = tmp_path / "Installed.release"
    installed_case.mkdir()
    installed_support.mkdir()
    (installed_case / "old.txt").write_text("old case\n", encoding="utf-8")
    (installed_support / "old.txt").write_text("old support\n", encoding="utf-8")

    result = subprocess.run(
        [
            "bash",
            str(DOWNLOADER),
            "--descriptor",
            str(descriptor),
            "--archive",
            str(archive),
            "--force",
        ],
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode != 0
    assert (installed_case / "old.txt").read_text(encoding="utf-8") == "old case\n"
    assert (
        installed_support / "old.txt"
    ).read_text(encoding="utf-8") == "old support\n"
