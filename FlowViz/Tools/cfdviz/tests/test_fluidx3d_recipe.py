"""The tracked, reproducible recipe for the real FluidX3D demo dataset."""

from __future__ import annotations

import json
import re
import subprocess
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[4]
RECIPE = REPO_ROOT / "FlowViz" / "Tools" / "fluidx3d"
GENERATOR = REPO_ROOT / "FlowViz" / "Tools" / "generate_fluidx3d_sample.sh"
RUN_DEMO = REPO_ROOT / "FlowViz" / "run_demo.command"
README = REPO_ROOT / "FlowViz" / "README.md"
DESCRIPTOR = (
    REPO_ROOT / "FlowViz" / "Samples" / "FluidX3DSphereWake.download.json"
)
REVISION = "024e48c23256a31346cf458fba76deae4aca7869"


def test_setup_is_a_three_dimensional_temporal_sphere_wake():
    source = (RECIPE / "sphere_wake_setup.cpp").read_text(encoding="utf-8")

    assert "LBM lbm(192u, 96u, 96u" in source
    assert "const float Diameter = 24.0f" in source
    assert "const float ReynoldsNumber = 200.0f" in source
    assert "const float InletVelocity = 0.06f" in source
    assert "sphere(" in source and "TYPE_S" in source
    assert "TYPE_E" in source
    assert "lbm.run(6000u)" in source
    assert "StoredFrames = 40u" in source
    assert "StoredStepStride = 10u" in source
    assert "lbm.u.write_device_to_vtk(\"\", false)" in source
    assert "lbm.flags.write_device_to_vtk(\"\", false)" in source


def test_defines_patch_selects_the_reviewed_headless_configuration():
    patch = (RECIPE / "defines.patch").read_text(encoding="utf-8")

    for enabled in ("D3Q19", "SRT", "FP16S", "EQUILIBRIUM_BOUNDARIES"):
        assert re.search(rf"^\+#define {enabled}(?:\s|$)", patch, re.MULTILINE)
    assert re.search(r"^\+//#define BENCHMARK", patch, re.MULTILINE)
    assert "INTERACTIVE_GRAPHICS" not in "\n".join(
        line for line in patch.splitlines() if line.startswith("+")
    )


def test_generator_never_builds_in_or_modifies_the_source_checkout():
    script = GENERATOR.read_text(encoding="utf-8")

    assert f'REVISION="{REVISION}"' in script
    assert 'git clone --no-local "${SOURCE}" "${CHECKOUT}"' in script
    assert 'git -C "${CHECKOUT}" checkout --detach "${REVISION}"' in script
    assert 'cp "${SETUP_SOURCE}" "${CHECKOUT}/src/setup.cpp"' in script
    assert 'git -C "${CHECKOUT}" apply "${DEFINES_PATCH}"' in script
    assert 'make -C "${CHECKOUT}" macOS' in script
    assert 'git -C "${SOURCE}" rev-parse --git-dir' in script
    assert '[[ -d "${SOURCE}/.git" ]]' not in script
    assert 'git -C "${SOURCE}"' not in script.replace(
        'git -C "${SOURCE}" rev-parse --git-dir',
        "",
    ).replace(
        'git clone --no-local "${SOURCE}" "${CHECKOUT}"',
        "",
    )


def test_generator_requires_flags_and_runs_all_acceptance_tools():
    script = GENERATOR.read_text(encoding="utf-8")

    assert '--flags "${VTK_DIR}"/flags-*.vtk' in script
    assert "--assume-all-cells-valid" not in script
    assert "qualify-representative" in script
    assert "known-values" in script and "--check" in script
    assert " validate " in script
    assert " inspect " in script
    assert " benchmark-read " in script
    assert "altered-source" in script
    assert "sha256" in script


def test_download_descriptor_pins_layout_and_qualification_requirements():
    payload = json.loads(DESCRIPTOR.read_text(encoding="utf-8"))

    assert payload["releaseRoot"] == "FluidX3DSphereWake"
    assert payload["casePath"] == "case"
    assert payload["caseDirectory"] == "FluidX3DSphereWake.cfdviz"
    assert payload["supportDirectory"] == "FluidX3DSphereWake.release"
    assert payload["qualification"] == {
        "solverName": "FluidX3D",
        "solverRevision": REVISION,
        "minimumActiveDimensions": [192, 96, 96],
        "minimumFrames": 40,
        "minimumSpanwiseVelocityRatio": 0.005,
        "minimumSpanwiseGradientRatio": 0.005,
        "minimumTemporalChangeRatio": 0.00001,
        "maximumFeatureDisplacementCells": 2.0,
    }

    script = GENERATOR.read_text(encoding="utf-8")
    for key in ("releaseRoot", "casePath", "supportDirectory", "qualification"):
        assert f'"{key}"' in script


def test_primary_demo_launches_the_representative_external_solver_case():
    script = RUN_DEMO.read_text(encoding="utf-8")

    assert "FluidX3DSphereWake.cfdviz" in script
    assert "-field=U" in script
    assert "MockCylinderWake.cfdviz" not in script
    assert "generate_fluidx3d_sample.sh" in script
    assert script.index('[[ -f "$SOURCE_CASE/manifest.json" ]]') < script.index(
        '[[ -f "$PACKAGED_CASE/manifest.json" ]]'
    )


def test_generator_force_deletes_only_paths_it_marked(tmp_path: Path):
    source = tmp_path / "FluidX3D"
    source.mkdir()
    subprocess.run(["git", "init", "-q", str(source)], check=True)
    unrelated = tmp_path / "unrelated"
    unrelated.mkdir()
    marker = unrelated / "keep.txt"
    marker.write_text("keep\n", encoding="utf-8")

    result = subprocess.run(
        [
            "bash",
            str(GENERATOR),
            "--source",
            str(source),
            "--work-dir",
            str(unrelated),
            "--output",
            str(tmp_path / "case.cfdviz"),
            "--force",
        ],
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode != 0
    assert "refusing to replace unowned work directory" in result.stderr
    assert marker.read_text(encoding="utf-8") == "keep\n"


def test_packaging_docs_stage_sample_before_uat_codesigning():
    readme = README.read_text(encoding="utf-8")
    command = readme.index('RunUAT.sh" BuildCookRun')
    staging = readme.index("Generate or install the representative case before packaging")

    assert staging < command
    assert "cp -R Samples/FluidX3DSphereWake.cfdviz" not in readme[command:]


def test_generator_uses_a_bash32_safe_force_argument():
    script = GENERATOR.read_text(encoding="utf-8")

    assert 'CONVERT_FORCE=()' not in script
    assert '"${CONVERT_FORCE[@]}"' not in script
    assert 'if [[ "${FORCE}" -eq 1 ]]; then' in script


def test_release_evidence_is_scrubbed_and_archive_timestamp_is_stable():
    script = GENERATOR.read_text(encoding="utf-8")

    assert 'payload["root"] = "case"' in script
    assert 'payload["case"] = "case"' in script
    assert 'text.replace(output, "case")' in script
    assert "gzip -n -1" in script
