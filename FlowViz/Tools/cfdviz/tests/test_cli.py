"""The ``cfdviz`` console script — spec section 10.

``pyproject.toml`` declares ``cfdviz = "cfdviz.__main__:main"``, so ``main`` is
the shipping entry point and is tested as one: called with an argv list,
returning a process exit status, writing to stdout/stderr.

The load-bearing behaviour is the exit code. "MUST NOT report success" is only
enforceable if a broken case makes the process exit non-zero, so every failure
test asserts on the status, not on the wording alone.
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

import pytest

from conftest import build_case, sample_manifest
from cfdviz.__main__ import main


def run(capsys, *argv: str) -> tuple[int, str, str]:
    """Invoke the CLI and capture (status, stdout, stderr)."""
    status = main(list(argv))
    captured = capsys.readouterr()
    return status, captured.out, captured.err


# ---------------------------------------------------------------------------
# Entry point plumbing
# ---------------------------------------------------------------------------

def test_the_console_script_target_exists():
    """pyproject declares cfdviz.__main__:main; make sure it really is there."""
    import cfdviz.__main__ as entry

    assert callable(entry.main)


def test_module_is_runnable_with_dash_m():
    import subprocess
    import sys

    root = Path(__file__).resolve().parents[1]
    result = subprocess.run(
        [sys.executable, "-m", "cfdviz", "--version"],
        capture_output=True, text=True, cwd=root,
        env={"PYTHONPATH": str(root / "src"), "PATH": "/usr/bin:/bin"},
    )
    assert result.returncode == 0, result.stderr
    assert "1.0" in result.stdout


def test_no_arguments_prints_usage_and_fails(capsys):
    status, out, err = run(capsys)
    assert status != 0
    assert "usage" in (out + err).lower()


def test_unknown_subcommand_fails(capsys):
    with pytest.raises(SystemExit) as excinfo:
        main(["frobnicate"])
    assert excinfo.value.code != 0


def test_version_reports_the_format_version(capsys):
    status, out, _ = run(capsys, "--version")
    assert status == 0
    assert "1.0.0" in out


# ---------------------------------------------------------------------------
# validate
# ---------------------------------------------------------------------------

def test_validate_a_good_case_exits_zero(capsys, valid_case: Path):
    status, out, _ = run(capsys, "validate", str(valid_case))
    assert status == 0
    assert "OK" in out


def test_validate_a_broken_case_exits_nonzero(capsys, valid_case: Path):
    (valid_case / "frames" / "000000" / "U.cvf").unlink()
    status, out, err = run(capsys, "validate", str(valid_case))
    assert status != 0, "a corrupt case must not report success (spec 10)"
    assert "U.cvf" in (out + err)


def test_validate_a_missing_case_exits_nonzero_without_a_traceback(
    capsys, tmp_path: Path
):
    status, out, err = run(capsys, "validate", str(tmp_path / "nope.cfdviz"))
    assert status != 0
    assert "Traceback" not in (out + err)


def test_validate_reports_the_byte_offset_of_a_corrupt_header(
    capsys, valid_case: Path
):
    path = valid_case / "frames" / "000000" / "pressure.cvf"
    blob = bytearray(path.read_bytes())
    struct.pack_into("<I", blob, 24, 4242)
    path.write_bytes(bytes(blob))

    status, out, err = run(capsys, "validate", str(valid_case))
    assert status != 0
    text = out + err
    assert "pressure.cvf" in text and "byte offset" in text


def test_validate_json_output_is_machine_readable(capsys, valid_case: Path):
    status, out, _ = run(capsys, "validate", str(valid_case), "--json")
    assert status == 0
    payload = json.loads(out)
    assert payload["ok"] is True
    assert payload["errors"] == []


def test_validate_json_output_on_failure_lists_the_errors(capsys, valid_case: Path):
    (valid_case / "manifest.json").unlink()
    status, out, _ = run(capsys, "validate", str(valid_case), "--json")
    assert status != 0
    payload = json.loads(out)
    assert payload["ok"] is False
    assert payload["errors"]


# ---------------------------------------------------------------------------
# info
# ---------------------------------------------------------------------------

def test_info_summarizes_a_case(capsys, valid_case: Path):
    status, out, _ = run(capsys, "info", str(valid_case))
    assert status == 0
    assert "pressure" in out and "U" in out
    assert "4x3x2" in out.replace(" ", "")


def test_info_on_a_single_cvf_file_prints_header_facts(capsys, valid_case: Path):
    status, out, _ = run(capsys, "info", str(valid_case / "frames/000000/U.cvf"))
    assert status == 0
    assert "float32" in out
    assert "zlib" in out or "none" in out


def test_info_on_a_corrupt_file_exits_nonzero(capsys, valid_case: Path):
    path = valid_case / "frames/000000/U.cvf"
    path.write_bytes(b"not a cvf file at all")
    status, out, err = run(capsys, "info", str(path))
    assert status != 0
    assert "Traceback" not in (out + err)


def test_info_on_a_missing_path_exits_nonzero(capsys, tmp_path: Path):
    """Naming a file that isn't there is a failure, not a no-op success."""
    status, out, err = run(capsys, "info", str(tmp_path / "absent.cvf"))
    assert status != 0
    assert "absent.cvf" in (out + err)


def test_an_unreadable_case_does_not_print_a_traceback(capsys, valid_case: Path):
    """Spec 10: report the problem, do not crash.

    A traceback on stdout/stderr means an exception escaped ``main`` — the tool
    would still exit non-zero, so only inspecting the status would miss it.
    """
    (valid_case / "manifest.json").write_text("{ not json", encoding="utf-8")
    for argv in (["validate", str(valid_case)],
                 ["info", str(valid_case)],
                 ["known-values", str(valid_case)]):
        status, out, err = run(capsys, *argv)
        assert status != 0, argv
        assert "Traceback" not in (out + err), argv
        assert "internal error" not in (out + err), (
            f"{argv} escaped as an unexpected exception rather than a reported one"
        )


# ---------------------------------------------------------------------------
# known-values
# ---------------------------------------------------------------------------

def test_known_values_writes_the_bridge_file(capsys, valid_case: Path):
    status, _, _ = run(capsys, "known-values", str(valid_case))
    assert status == 0
    bridge = json.loads((valid_case / "known_values.json").read_text("utf-8"))
    assert bridge["crc32cCheck"] == "0xE3069283"


def test_known_values_check_mode_passes_on_a_matching_bridge(
    capsys, valid_case: Path
):
    run(capsys, "known-values", str(valid_case))
    status, out, _ = run(capsys, "known-values", str(valid_case), "--check")
    assert status == 0


def test_known_values_check_mode_fails_on_a_mismatched_bridge(
    capsys, valid_case: Path
):
    run(capsys, "known-values", str(valid_case))
    path = valid_case / "known_values.json"
    bridge = json.loads(path.read_text("utf-8"))
    bridge["samples"][0]["bits"] = "0xDEADBEEF"
    path.write_text(json.dumps(bridge), encoding="utf-8")

    status, out, err = run(capsys, "known-values", str(valid_case), "--check")
    assert status != 0
    assert "bits" in (out + err)


def test_known_values_reports_mesh_and_array_sample_counts(
    capsys, valid_case: Path
):
    """The counts printed must cover every sample the bridge actually holds.

    Printing only ``len(bridge["samples"])`` reads as full coverage while the
    mesh and array samples go unmentioned — a reader who saw "17 samples match"
    would have no way to tell whether the geometry was checked at all.
    """
    _, write_out, _ = run(capsys, "known-values", str(valid_case))
    _, check_out, _ = run(capsys, "known-values", str(valid_case), "--check")

    bridge = json.loads((valid_case / "known_values.json").read_text("utf-8"))
    total = (
        len(bridge["samples"])
        + len(bridge["meshSamples"])
        + len(bridge["arraySamples"])
    )
    assert total > len(bridge["samples"]), "the fixture must have mesh/array samples"
    for label, text in (("write", write_out), ("check", check_out)):
        # The pytest tmpdir is named after this test, so it contains the word
        # "mesh". Searching the whole line would match the path and pass no
        # matter what the tool printed; only the part after the path counts.
        tail = text.rsplit("known_values.json", 1)[-1]
        assert str(total) in tail, f"{label} mode does not report all {total} samples"
        assert f"{len(bridge['meshSamples'])} mesh" in tail, (
            f"{label} mode does not break out the mesh sample count: {tail!r}"
        )
        assert f"{len(bridge['arraySamples'])} array" in tail, (
            f"{label} mode does not break out the array sample count: {tail!r}"
        )


def test_known_values_check_mode_fails_on_a_mismatched_mesh_sample(
    capsys, valid_case: Path
):
    """A geometry mismatch must reach the exit status, not just the voxel path."""
    run(capsys, "known-values", str(valid_case))
    path = valid_case / "known_values.json"
    bridge = json.loads(path.read_text("utf-8"))
    triangle = next(s for s in bridge["meshSamples"] if s["kind"] == "triangle")
    triangle["indices"] = list(reversed(triangle["indices"]))
    path.write_text(json.dumps(bridge), encoding="utf-8")

    status, out, err = run(capsys, "known-values", str(valid_case), "--check")
    assert status != 0
    assert "triangle" in (out + err)


def test_known_values_check_mode_does_not_rewrite_the_file(
    capsys, valid_case: Path
):
    """--check must not paper over a mismatch by regenerating it."""
    run(capsys, "known-values", str(valid_case))
    path = valid_case / "known_values.json"
    bridge = json.loads(path.read_text("utf-8"))
    bridge["samples"][0]["bits"] = "0xDEADBEEF"
    before = json.dumps(bridge)
    path.write_text(before, encoding="utf-8")

    run(capsys, "known-values", str(valid_case), "--check")
    assert path.read_text("utf-8") == before


# ---------------------------------------------------------------------------
# The zstd rejection has to reach the user, verbatim
# ---------------------------------------------------------------------------

def test_a_zstd_file_is_rejected_through_the_cli_with_the_exact_message(
    capsys, valid_case: Path
):
    from conftest import tamper_cvf
    from cfdviz.codecs import CODEC_ZSTD, ZSTD_REJECTION_MESSAGE

    path = valid_case / "frames/000000/pressure.cvf"
    path.write_bytes(tamper_cvf(path.read_bytes(), 61, "<B", CODEC_ZSTD))

    status, out, err = run(capsys, "validate", str(valid_case))
    assert status != 0
    assert ZSTD_REJECTION_MESSAGE in (out + err)
