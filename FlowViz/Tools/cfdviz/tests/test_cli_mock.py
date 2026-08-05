"""The three subcommands added for plan section 7: generate-mock, extract,
benchmark-read.

The existing CLI tests (``test_cli.py``) fix the conventions these follow, and
the conventions are what these tests are mostly about:

- a missing or unreadable input is ``rc = 1``, not a traceback and not a zero;
- a bad *invocation* is argparse's ``rc = 2``;
- nothing prints a stack trace, because an unreadable file is a fact about the
  input rather than a bug in the tool.

Where a test asserts a success path, there is a companion asserting the
matching failure path, so "it printed something and exited 0" cannot pass for
"it did the work".
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

from cfdviz.__main__ import main
from cfdviz.case import validate_case
from cfdviz.manifest import load_manifest


def run(capsys, *argv: str) -> tuple[int, str, str]:
    status = main(list(argv))
    captured = capsys.readouterr()
    return status, captured.out, captured.err


@pytest.fixture(scope="module")
def generated_case(tmp_path_factory) -> Path:
    """A tiny generated case, produced through the CLI itself.

    Going through ``main`` rather than the library keeps the fixture honest: if
    the subcommand's argument wiring is broken, every test in this module fails
    rather than only the one that checks the wiring.
    """
    root = tmp_path_factory.mktemp("cli") / "Tiny.cfdviz"
    status = main(
        [
            "generate-mock", "--output", str(root),
            "--dimensions", "24", "16", "4",
            "--frames", "3",
            "--brick-size", "16", "16", "4",
        ]
    )
    assert status == 0
    return root


# ---------------------------------------------------------------------------
# generate-mock
# ---------------------------------------------------------------------------

def test_generate_mock_writes_a_case_that_validates(generated_case: Path):
    report = validate_case(generated_case)
    assert report.ok, report.render()


def test_generate_mock_is_reachable_through_dash_m(tmp_path: Path):
    """The shipping invocation from BUILD.md, run as a real subprocess."""
    package = Path(__file__).resolve().parents[1]
    result = subprocess.run(
        [
            sys.executable, "-m", "cfdviz", "generate-mock",
            "--output", str(tmp_path / "Sub.cfdviz"),
            "--dimensions", "16", "8", "4", "--frames", "2",
            "--brick-size", "8", "8", "4",
        ],
        capture_output=True, text=True, cwd=package,
        env={"PYTHONPATH": str(package / "src"), "PATH": "/usr/bin:/bin"},
    )
    assert result.returncode == 0, result.stderr
    assert (tmp_path / "Sub.cfdviz" / "manifest.json").is_file()


def test_generate_mock_honours_every_documented_parameter(tmp_path: Path):
    """Each flag must actually reach the data, not merely be accepted.

    A parser that accepts ``--inlet-velocity`` and then ignores it is worse
    than one that rejects it, so the assertions are on the generated case
    rather than on the exit status.
    """
    root = tmp_path / "Custom.cfdviz"
    status = main(
        [
            "generate-mock", "--output", str(root),
            "--dimensions", "20", "12", "4",
            "--frames", "3",
            "--frame-interval", "0.05",
            "--domain", "6", "3", "0.8",
            "--origin", "1", "2", "3",
            "--inlet-velocity", "4.0",
            "--cylinder-radius", "0.4",
            "--shedding-frequency", "3.0",
            "--circulation", "1.5",
            "--core-radius", "0.2",
            "--spanwise-perturbation", "0.2",
            "--codec", "none",
            "--float16",
            "--seed", "7",
            "--brick-size", "8", "8", "4",
        ]
    )
    assert status == 0

    manifest = load_manifest(root)
    grid = manifest["grids"][0]
    assert grid["dimensions"] == [20, 12, 4]
    assert grid["origin"] == [1.0, 2.0, 3.0]
    assert grid["spacing"] == pytest.approx([6 / 20, 3 / 12, 0.8 / 4])
    assert manifest["timeline"]["times"] == pytest.approx([0.0, 0.05, 0.10])

    velocity = next(f for f in manifest["fields"] if f["id"] == "U")
    assert velocity["dataType"] == "float16"
    assert velocity["storage"]["codec"] == "none"

    parameters = json.loads(
        next(
            note.split("parameters = ", 1)[1]
            for note in manifest["provenance"]["notes"]
            if note.startswith("parameters = ")
        )
    )
    assert parameters["seed"] == 7
    assert parameters["inletVelocity"] == 4.0
    assert parameters["cylinderRadius"] == 0.4
    assert parameters["sheddingFrequency"] == 3.0
    assert parameters["circulation"] == 1.5
    assert parameters["coreRadius"] == 0.2
    assert parameters["spanwisePerturbation"] == 0.2


def test_generate_mock_low_res_differs_from_the_default_preset(tmp_path: Path):
    """``--low-res`` must select a genuinely smaller case, not just a flag."""
    root = tmp_path / "Low.cfdviz"
    assert main(["generate-mock", "--output", str(root), "--low-res"]) == 0
    manifest = load_manifest(root)
    assert manifest["grids"][0]["dimensions"] != [128, 64, 24]
    total = sum(p.stat().st_size for p in root.rglob("*") if p.is_file())
    assert total < 4 * 1024 * 1024


def test_generate_mock_rejects_a_degenerate_grid_without_a_traceback(
    capsys, tmp_path: Path
):
    """A two-cell axis cannot carry a second-order difference; say so."""
    status, out, err = run(
        capsys, "generate-mock", "--output", str(tmp_path / "Bad.cfdviz"),
        "--dimensions", "8", "2", "4",
    )
    assert status == 1
    assert "Traceback" not in (out + err)
    assert "internal error" not in (out + err)
    assert "3" in (out + err)


def test_generate_mock_rejects_an_unknown_codec(capsys, tmp_path: Path):
    with pytest.raises(SystemExit) as excinfo:
        main(
            ["generate-mock", "--output", str(tmp_path / "Bad.cfdviz"),
             "--codec", "brotli"]
        )
    assert excinfo.value.code == 2


def test_generate_mock_rejects_reserved_zstd(capsys, tmp_path: Path):
    """zstd is reserved in 1.0 (spec 7) and must not be writable."""
    with pytest.raises(SystemExit) as excinfo:
        main(
            ["generate-mock", "--output", str(tmp_path / "Bad.cfdviz"),
             "--codec", "zstd"]
        )
    assert excinfo.value.code == 2


# ---------------------------------------------------------------------------
# extract
# ---------------------------------------------------------------------------

def test_extract_writes_an_npy_that_round_trips(capsys, generated_case: Path,
                                                tmp_path: Path):
    destination = tmp_path / "U.npy"
    status, out, _ = run(
        capsys, "extract", str(generated_case),
        "--frame", "1", "--field", "U", "--output", str(destination),
    )
    assert status == 0

    values = np.load(destination)
    manifest = load_manifest(generated_case)
    grid = manifest["grids"][0]
    assert values.shape == (*grid["dimensions"], 3)

    from cfdviz.cvf import read_cvf
    entry = next(f for f in manifest["fields"] if f["id"] == "U")
    stored = read_cvf(
        generated_case / entry["storage"]["pathPattern"].format(frame=1)
    ).values
    assert values.dtype == stored.dtype
    assert np.array_equal(
        values.view(np.uint32 if values.itemsize == 4 else np.uint16),
        stored.view(np.uint32 if stored.itemsize == 4 else np.uint16),
    ), "extract altered the stored bits"


def test_extract_without_an_output_prints_a_summary(capsys, generated_case: Path):
    status, out, _ = run(
        capsys, "extract", str(generated_case), "--frame", "0", "--field",
        "pressure",
    )
    assert status == 0
    assert "pressure" in out
    assert "min" in out.lower() and "max" in out.lower()


def test_extract_prints_a_single_voxel(capsys, generated_case: Path):
    status, out, _ = run(
        capsys, "extract", str(generated_case), "--frame", "0", "--field", "U",
        "--voxel", "3", "4", "1",
    )
    assert status == 0

    from cfdviz.cvf import read_cvf
    manifest = load_manifest(generated_case)
    entry = next(f for f in manifest["fields"] if f["id"] == "U")
    expected = read_cvf(
        generated_case / entry["storage"]["pathPattern"].format(frame=0)
    ).values[3, 4, 1]
    for value in expected:
        assert f"{float(value):.6g}" in out


def test_extract_reports_a_masked_voxel_as_masked(capsys, generated_case: Path):
    """A NaN inside the obstacle must read as "masked", not as a number.

    Printing ``nan`` alone would be true but useless; the whole point of the
    mask is that the absence of data is a different fact from a value.
    """
    from cfdviz.cvf import read_cvf

    manifest = load_manifest(generated_case)
    mask_entry = next(f for f in manifest["fields"] if f["id"] == "validMask")
    mask = read_cvf(
        generated_case / mask_entry["storage"]["pathPattern"].format(frame=0)
    ).values[..., 0]
    rejected = np.argwhere(mask == 0)
    assert len(rejected), "the fixture masks nothing, so this cannot be tested"
    voxel = [str(int(v)) for v in rejected[0]]

    status, out, _ = run(
        capsys, "extract", str(generated_case), "--frame", "0", "--field", "U",
        "--voxel", *voxel,
    )
    assert status == 0
    assert "masked" in out.lower()


def test_extract_json_output_carries_exact_bit_patterns(capsys,
                                                        generated_case: Path):
    """Decimal text cannot express NaN; the bits always can (spec 9.1/9.2)."""
    status, out, _ = run(
        capsys, "extract", str(generated_case), "--frame", "0", "--field",
        "pressure", "--voxel", "2", "2", "1", "--json",
    )
    assert status == 0
    payload = json.loads(out)
    assert payload["field"] == "pressure"
    assert payload["frame"] == 0
    assert payload["voxel"] == [2, 2, 1]
    assert payload["bits"][0].startswith("0x")


def test_extract_on_a_missing_case_exits_one(capsys, tmp_path: Path):
    status, out, err = run(
        capsys, "extract", str(tmp_path / "nope.cfdviz"), "--frame", "0",
        "--field", "U",
    )
    assert status == 1
    assert "Traceback" not in (out + err)


def test_extract_on_an_unknown_field_names_the_available_ones(
    capsys, generated_case: Path
):
    status, out, err = run(
        capsys, "extract", str(generated_case), "--frame", "0", "--field",
        "temperature",
    )
    assert status == 1
    text = out + err
    assert "temperature" in text
    assert "pressure" in text, "the error does not list the fields that do exist"


def test_extract_on_an_out_of_range_frame_exits_one(capsys,
                                                    generated_case: Path):
    status, out, err = run(
        capsys, "extract", str(generated_case), "--frame", "999", "--field", "U",
    )
    assert status == 1
    assert "999" in (out + err)
    assert "Traceback" not in (out + err)


def test_extract_on_an_out_of_range_voxel_exits_one(capsys,
                                                    generated_case: Path):
    status, out, err = run(
        capsys, "extract", str(generated_case), "--frame", "0", "--field", "U",
        "--voxel", "9999", "0", "0",
    )
    assert status == 1
    assert "Traceback" not in (out + err)


# ---------------------------------------------------------------------------
# benchmark-read
# ---------------------------------------------------------------------------

def test_benchmark_read_reports_bytes_and_throughput(capsys,
                                                     generated_case: Path):
    status, out, _ = run(capsys, "benchmark-read", str(generated_case))
    assert status == 0
    lowered = out.lower()
    assert "mb/s" in lowered
    assert "frame" in lowered


def test_benchmark_read_json_output_is_machine_readable(capsys,
                                                        generated_case: Path):
    status, out, _ = run(capsys, "benchmark-read", str(generated_case), "--json")
    assert status == 0
    payload = json.loads(out)
    assert payload["frames"] == 3
    assert payload["decodedBytes"] > 0
    assert payload["seconds"] > 0.0
    assert payload["fields"], "no per-field timings were reported"
    assert {"U", "pressure", "validMask"} <= set(payload["fields"])


def test_benchmark_read_decodes_the_whole_case_not_just_the_headers(
    capsys, generated_case: Path
):
    """The reported byte count must be the decoded volume, not the file size.

    A benchmark that only opened headers would still print a plausible
    throughput. Comparing against the uncompressed volume size is what makes
    that impossible: it is far larger than the compressed files on disk.
    """
    status, out, _ = run(capsys, "benchmark-read", str(generated_case), "--json")
    payload = json.loads(out)

    manifest = load_manifest(generated_case)
    cells = int(np.prod(manifest["grids"][0]["dimensions"]))
    item = {"float16": 2, "float32": 4, "uint8": 1}
    expected = sum(
        cells * entry["componentCount"] * item[entry["dataType"]]
        for entry in manifest["fields"]
    ) * manifest["timeline"]["frameCount"]
    assert payload["decodedBytes"] == expected


def test_benchmark_read_can_be_limited_to_one_field(capsys,
                                                    generated_case: Path):
    status, out, _ = run(
        capsys, "benchmark-read", str(generated_case), "--field", "U", "--json"
    )
    assert status == 0
    payload = json.loads(out)
    assert set(payload["fields"]) == {"U"}


def test_benchmark_read_on_a_missing_case_exits_one(capsys, tmp_path: Path):
    status, out, err = run(capsys, "benchmark-read", str(tmp_path / "nope"))
    assert status == 1
    assert "Traceback" not in (out + err)


def test_benchmark_read_on_a_corrupt_case_exits_one(capsys,
                                                    generated_case: Path,
                                                    tmp_path: Path):
    """Corruption must not be reported as a fast read."""
    import shutil

    broken = tmp_path / "Broken.cfdviz"
    shutil.copytree(generated_case, broken)
    (broken / "frames" / "000000" / "U.cvf").write_bytes(b"not a cvf")

    status, out, err = run(capsys, "benchmark-read", str(broken))
    assert status == 1
    assert "U.cvf" in (out + err)
    assert "Traceback" not in (out + err)
