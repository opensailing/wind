"""manifest.json — format spec section 3, and the path rule of section 1.3.

The schema in ``src/cfdviz/schema/`` names this module as "the authoritative
implementation" of the traversal check, so the tests here are the contract for
both.

Two things carry the weight:

* **Every section 3.1 invariant gets its own test that breaks exactly one
  thing.** A single "the good manifest validates" test cannot fail for the right
  reason; a battery of one-mutation-each tests can only pass if each check is
  independently real.
* **Path traversal is tested lexically**, with no file on disk, because
  section 1.3 requires the check to happen before any filesystem call — a check
  implemented via ``resolve()`` would pass a round-trip and still be wrong.
"""

from __future__ import annotations

import copy
import json
from pathlib import Path

import pytest

from conftest import sample_manifest
from cfdviz.manifest import (
    SCHEMA_PATH,
    ManifestError,
    is_safe_relative_path,
    load_manifest,
    load_schema,
    validate_manifest,
)


def good() -> dict:
    return sample_manifest()


# ---------------------------------------------------------------------------
# Path traversal — spec 1.3, lexical, before any filesystem call
# ---------------------------------------------------------------------------

@pytest.mark.parametrize(
    "path",
    [
        "frames/000000/U.cvf",
        "meshes/obstacle.cvm",
        "a.cvf",
        "deeply/nested/but/fine/x.cvf",
        "frames/{frame:06d}/U.cvf",
        "dots.in.name/v1.2/U.cvf",
        "..hidden/U.cvf",   # a leading '..' in a NAME is not a '..' SEGMENT
        "x..y/U.cvf",       # nor is one in the middle of a name
        "frames/trailing..",
    ],
)
def test_safe_paths_are_accepted(path):
    assert is_safe_relative_path(path) is True


@pytest.mark.parametrize(
    "path",
    [
        "../escape.cvf",
        "frames/../../escape.cvf",
        "frames/000000/../../../etc/passwd",
        "..",
        "a/..",
        "/absolute.cvf",
        "/etc/passwd",
        "C:/windows/system32",
        "c:\\windows\\system32",
        "frames\\000000\\U.cvf",   # backslash separates on Windows
        "..\\escape.cvf",
        "",
        "frames/\x00U.cvf",
        "frames/\nU.cvf",
    ],
)
def test_unsafe_paths_are_rejected(path):
    assert is_safe_relative_path(path) is False


def test_traversal_check_is_lexical_not_filesystem_based(tmp_path: Path):
    """Spec 1.3: "before any filesystem call".

    A path that does not escape *after* normalization can still escape during
    it — and a check built on ``Path.resolve`` would accept this one, because
    resolving lands back inside the root. The rule is about the literal path.
    """
    assert is_safe_relative_path("frames/000000/../../frames/000000/U.cvf") is False
    # Nothing exists on disk, and the answer is the same either way.
    assert is_safe_relative_path("../x") is False
    (tmp_path / "x").write_text("")
    assert is_safe_relative_path("../x") is False


def test_schema_pattern_and_the_function_agree():
    """The schema mirrors this function; a drift between them is a real bug."""
    jsonschema = pytest.importorskip("jsonschema")
    schema = load_schema()
    pattern = {"type": "string", **schema["$defs"]["relativePath"]}
    validator = jsonschema.Draft202012Validator(pattern)
    for path in ["frames/000000/U.cvf", "a.cvf", "meshes/o.cvm"]:
        assert validator.is_valid(path) is is_safe_relative_path(path) is True
    for path in ["../x", "/x", "C:/x", "a\\b", "a/../b", ""]:
        assert validator.is_valid(path) is is_safe_relative_path(path) is False


# ---------------------------------------------------------------------------
# Schema conformance
# ---------------------------------------------------------------------------

def test_schema_file_exists_and_is_the_declared_one():
    assert SCHEMA_PATH.is_file()
    schema = load_schema()
    assert schema["$id"] == "https://flowviz.dev/schema/cfdviz-1.0.schema.json"


def test_the_sample_manifest_validates():
    assert validate_manifest(good()) == []


def test_schema_validation_rejects_a_missing_required_property():
    pytest.importorskip("jsonschema")
    manifest = good()
    del manifest["units"]
    problems = validate_manifest(manifest)
    assert any("units" in p for p in problems)


def test_format_marker_must_be_exact():
    manifest = good()
    manifest["format"] = "cfdviz"
    assert any("format" in p for p in validate_manifest(manifest))


def test_unsupported_major_version_is_rejected():
    """Spec 1.4: unsupported MAJOR rejects."""
    manifest = good()
    manifest["version"] = "2.0.0"
    assert any("major version" in p for p in validate_manifest(manifest))


def test_newer_minor_version_is_accepted():
    """Spec 1.4: newer MINOR loads, provided everything required is present."""
    manifest = good()
    manifest["version"] = "1.7.0"
    assert validate_manifest(manifest) == []


def test_unknown_properties_are_ignored_silently():
    """Spec 1.4: unknown JSON object properties are ignored, not rejected."""
    manifest = good()
    manifest["somethingFrom1_1"] = {"nested": [1, 2, 3]}
    manifest["fields"][0]["futureHint"] = "ignore me"
    assert validate_manifest(manifest) == []


def test_unknown_value_of_a_required_enum_is_rejected():
    """Spec 1.4: unknown values of a *required* enum reject."""
    pytest.importorskip("jsonschema")
    manifest = good()
    manifest["fields"][0]["dataType"] = "float64"
    assert validate_manifest(manifest) != []


# ---------------------------------------------------------------------------
# Section 3.1 invariants — one broken thing per test
# ---------------------------------------------------------------------------

def test_timeline_must_be_strictly_increasing():
    manifest = good()
    manifest["timeline"]["times"] = [0.0, 0.0]
    assert any("monoton" in p for p in validate_manifest(manifest))


def test_a_decreasing_timeline_is_rejected():
    manifest = good()
    manifest["timeline"]["times"] = [1.0, 0.5]
    assert any("monoton" in p for p in validate_manifest(manifest))


def test_nan_is_not_a_legal_time():
    manifest = good()
    manifest["timeline"]["times"] = [0.0, float("nan")]
    assert any("monoton" in p or "NaN" in p for p in validate_manifest(manifest))


def test_frame_count_must_match_times():
    manifest = good()
    manifest["timeline"]["frameCount"] = 3
    assert any("frameCount" in p for p in validate_manifest(manifest))


def test_frame_count_must_match_steps_when_present():
    manifest = good()
    manifest["timeline"]["steps"] = [0]
    assert any("steps" in p for p in validate_manifest(manifest))


def test_field_grid_must_name_a_declared_grid():
    manifest = good()
    manifest["fields"][0]["grid"] = "nonexistent"
    assert any("grid" in p for p in validate_manifest(manifest))


def test_mask_field_must_name_a_declared_field():
    manifest = good()
    manifest["grids"][0]["maskField"] = "nonexistent"
    assert any("maskField" in p for p in validate_manifest(manifest))


def test_duplicate_field_numeric_ids_are_rejected():
    manifest = good()
    manifest["fields"][1]["numericId"] = manifest["fields"][0]["numericId"]
    assert any("numericId" in p for p in validate_manifest(manifest))


def test_duplicate_field_ids_are_rejected():
    manifest = good()
    manifest["fields"][1]["id"] = manifest["fields"][0]["id"]
    assert any("duplicate field id" in p for p in validate_manifest(manifest))


def test_duplicate_grid_ids_are_rejected():
    manifest = good()
    manifest["grids"].append(copy.deepcopy(manifest["grids"][0]))
    assert any("duplicate grid id" in p for p in validate_manifest(manifest))


def test_component_count_must_match_components():
    manifest = good()
    manifest["fields"][1]["componentCount"] = 2  # but components has 3 names
    assert any("componentCount" in p for p in validate_manifest(manifest))


def test_unsupported_data_type_is_rejected():
    manifest = good()
    manifest["fields"][0]["dataType"] = "int32"
    assert any("dataType" in p for p in validate_manifest(manifest))


def test_reserved_association_is_rejected():
    """Spec 3.2: mesh-vertex and friends are reserved and MUST be rejected."""
    manifest = good()
    manifest["fields"][0]["association"] = "mesh-vertex"
    assert any("association" in p for p in validate_manifest(manifest))


def test_grid_dimension_below_one_is_rejected():
    manifest = good()
    manifest["grids"][0]["dimensions"] = [4, 0, 2]
    assert any("dimensions" in p for p in validate_manifest(manifest))


def test_non_positive_spacing_is_rejected():
    manifest = good()
    manifest["grids"][0]["spacing"] = [0.1, 0.0, 0.1]
    assert any("spacing" in p for p in validate_manifest(manifest))


def test_negative_spacing_is_rejected():
    manifest = good()
    manifest["grids"][0]["spacing"] = [0.1, -0.1, 0.1]
    assert any("spacing" in p for p in validate_manifest(manifest))


def test_path_pattern_traversal_is_rejected():
    manifest = good()
    manifest["fields"][0]["storage"]["pathPattern"] = "../../etc/passwd"
    assert any("pathPattern" in p for p in validate_manifest(manifest))


def test_mesh_path_traversal_is_rejected():
    manifest = good()
    manifest["meshes"] = [{"id": "m", "path": "../outside.cvm"}]
    assert any("path" in p for p in validate_manifest(manifest))


def test_each_invariant_test_above_starts_from_a_valid_manifest():
    """The differential guard: if ``good()`` were already broken, every test
    above would 'pass' for the wrong reason."""
    assert validate_manifest(good()) == []


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------

def test_load_manifest_reads_utf8_without_bom(tmp_path: Path):
    path = tmp_path / "manifest.json"
    path.write_text(json.dumps(good()), encoding="utf-8")
    assert load_manifest(path)["format"] == "CFDViz"


def test_load_manifest_rejects_a_byte_order_mark(tmp_path: Path):
    """Spec 1.2: UTF-8 without a BOM. A BOM makes strict JSON parsers fail."""
    path = tmp_path / "manifest.json"
    path.write_bytes(b"\xef\xbb\xbf" + json.dumps(good()).encode("utf-8"))
    with pytest.raises(ManifestError, match="byte-order mark"):
        load_manifest(path)


def test_load_manifest_reports_the_offending_file_on_bad_json(tmp_path: Path):
    path = tmp_path / "manifest.json"
    path.write_text("{ not json", encoding="utf-8")
    with pytest.raises(ManifestError) as excinfo:
        load_manifest(path)
    assert "manifest.json" in str(excinfo.value)


def test_load_manifest_reports_a_missing_file(tmp_path: Path):
    with pytest.raises(ManifestError, match="cannot read"):
        load_manifest(tmp_path / "absent.json")


def test_a_json_array_is_not_a_manifest(tmp_path: Path):
    path = tmp_path / "manifest.json"
    path.write_text("[]", encoding="utf-8")
    with pytest.raises(ManifestError, match="object"):
        load_manifest(path)
