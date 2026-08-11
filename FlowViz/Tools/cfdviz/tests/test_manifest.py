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
import cfdviz.manifest as manifest_module
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


@pytest.fixture
def validate_fallback(monkeypatch):
    """Exercise the required validator path when jsonschema is not installed."""
    monkeypatch.setattr(manifest_module, "_schema_problems", lambda _manifest: [])
    return manifest_module.validate_manifest


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
    assert schema["$id"] == "https://flowviz.dev/schema/cfdviz-1.1.schema.json"


def test_the_sample_manifest_validates():
    assert validate_manifest(good()) == []


def test_the_synthetic_sample_is_not_labelled_as_external_solver_data():
    manifest = good()
    assert manifest["case"]["quality"] == "synthetic-correctness-fixture"
    assert manifest["provenance"]["sourceType"] == "synthetic"


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


def test_a_valid_1_0_manifest_keeps_its_legacy_empty_solver_strings():
    manifest = good()
    manifest["version"] = "1.0.0"
    manifest["case"]["solver"] = {"name": "", "version": "", "method": ""}
    assert validate_manifest(manifest) == []


def test_a_1_1_solver_property_must_not_be_empty():
    manifest = good()
    manifest["case"]["solver"]["method"] = ""
    assert any("method" in problem for problem in validate_manifest(manifest))


def test_unknown_properties_are_ignored_silently():
    """Spec 1.4: unknown JSON object properties are ignored, not rejected."""
    manifest = good()
    manifest["somethingFrom1_2"] = {"nested": [1, 2, 3]}
    manifest["case"]["solver"]["futureSolverHint"] = "ignore me"
    manifest["timeline"]["sampling"]["futureSamplingHint"] = "ignore me"
    manifest["qualityMetrics"]["futureQualityHint"] = "ignore me"
    manifest["fields"][-1]["phase"]["futurePhaseHint"] = "ignore me"
    manifest["provenance"]["futureProvenanceHint"] = "ignore me"
    assert validate_manifest(manifest) == []


def test_external_solver_revision_must_be_a_string():
    manifest = good()
    manifest["case"]["solver"]["commit"] = 123
    assert any("commit" in p for p in validate_manifest(manifest))


def test_provenance_source_revision_must_be_a_string():
    manifest = good()
    manifest["provenance"]["sourceRevision"] = ["not", "a", "revision"]
    assert any("sourceRevision" in p for p in validate_manifest(manifest))


@pytest.mark.parametrize("value", ["", 123])
def test_provenance_source_type_must_be_a_nonempty_string(value):
    manifest = good()
    manifest["provenance"]["sourceType"] = value
    assert any("sourceType" in p for p in validate_manifest(manifest))


def test_manual_fallback_enforces_solver_and_provenance_types(validate_fallback):
    manifest = good()
    manifest["case"]["solver"]["commit"] = 123
    manifest["provenance"]["sourceRevision"] = ["not", "a", "revision"]
    manifest["provenance"]["notes"] = [123]
    problems = validate_fallback(manifest)
    assert any("commit" in problem for problem in problems)
    assert any("sourceRevision" in problem for problem in problems)
    assert any("notes" in problem for problem in problems)


@pytest.mark.parametrize(
    "path",
    [
        ("case", "solver"),
        ("timeline", "sampling"),
        ("field", "phase"),
        ("qualityMetrics",),
        ("provenance",),
    ],
)
def test_explicit_null_optional_blocks_are_rejected_by_the_fallback(
    validate_fallback, path
):
    manifest = good()
    if path == ("field", "phase"):
        manifest["fields"][-1]["phase"] = None
    elif len(path) == 2:
        manifest[path[0]][path[1]] = None
    else:
        manifest[path[0]] = None
    assert any("object" in problem for problem in validate_fallback(manifest))


def test_external_solver_quality_requires_external_solver_provenance(
    validate_fallback,
):
    manifest = good()
    manifest["case"]["quality"] = "external-solver-sample"
    manifest["provenance"]["sourceType"] = "synthetic"
    assert any(
        "external-solver-sample" in problem and "sourceType" in problem
        for problem in validate_fallback(manifest)
    )


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


@pytest.mark.parametrize("value", [float("inf"), float("-inf")])
def test_infinity_is_not_a_legal_time_without_jsonschema(validate_fallback, value):
    manifest = good()
    manifest["timeline"]["times"] = [0.0, value]
    assert any("finite" in problem for problem in validate_fallback(manifest))


def test_frame_count_must_match_times():
    manifest = good()
    manifest["timeline"]["frameCount"] = 3
    assert any("frameCount" in p for p in validate_manifest(manifest))


def test_frame_count_must_match_steps_when_present():
    manifest = good()
    manifest["timeline"]["steps"] = [0]
    assert any("steps" in p for p in validate_manifest(manifest))


def test_timeline_sampling_must_be_an_object_when_present():
    manifest = good()
    manifest["timeline"]["sampling"] = []
    assert any("timeline.sampling" in p for p in validate_manifest(manifest))


def test_source_time_step_must_be_positive():
    manifest = good()
    manifest["timeline"]["sampling"]["sourceTimeStep"] = 0.0
    assert any("sourceTimeStep" in p for p in validate_manifest(manifest))


def test_stored_step_stride_must_match_declared_steps():
    manifest = good()
    manifest["timeline"]["steps"] = [0, 99]
    assert any("storedStepStride" in p for p in validate_manifest(manifest))


def test_source_time_step_and_stride_must_match_stored_times():
    manifest = good()
    manifest["timeline"]["sampling"]["sourceTimeStep"] = 0.004
    assert any("sourceTimeStep" in p for p in validate_manifest(manifest))


def test_max_feature_displacement_must_be_finite_and_non_negative():
    manifest = good()
    manifest["timeline"]["sampling"]["maxFeatureDisplacementCells"] = -0.01
    assert any(
        "maxFeatureDisplacementCells" in p for p in validate_manifest(manifest)
    )


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


def test_uint32_field_and_patch_ids_remain_valid():
    manifest = good()
    manifest["fields"][0]["numericId"] = 3_000_000_000
    manifest["meshes"][0]["patches"][0]["id"] = 3_000_000_001
    assert validate_manifest(manifest) == []


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


def test_phase_interpretation_requires_a_scalar_field():
    manifest = good()
    manifest["fields"][-1]["components"] = ["r", "g"]
    manifest["fields"][-1]["componentCount"] = 2
    assert any("phase" in p and "scalar" in p for p in validate_manifest(manifest))


def test_phase_interpretation_requires_floating_point_storage():
    manifest = good()
    manifest["fields"][-1]["dataType"] = "uint8"
    assert any("phase" in p and "floating" in p for p in validate_manifest(manifest))


def test_volume_fraction_interface_value_must_be_in_unit_interval():
    manifest = good()
    manifest["fields"][-1]["phase"]["interfaceValue"] = 1.01
    assert any("interfaceValue" in p for p in validate_manifest(manifest))


def test_unknown_phase_representation_is_rejected():
    manifest = good()
    manifest["fields"][-1]["phase"]["representation"] = "particle-id"
    assert any("representation" in p for p in validate_manifest(manifest))


def test_unknown_phase_inside_convention_is_rejected():
    manifest = good()
    manifest["fields"][-1]["phase"]["inside"] = "sideways"
    assert any("inside" in p for p in validate_manifest(manifest))


def test_phase_interface_value_must_be_finite():
    manifest = good()
    manifest["fields"][-1]["phase"]["interfaceValue"] = float("nan")
    assert any("interfaceValue" in p for p in validate_manifest(manifest))


@pytest.mark.parametrize(
    "key,value",
    [
        ("primaryPhase", 123),
        ("primaryPhase", ""),
        ("secondaryPhase", []),
        ("secondaryPhase", ""),
    ],
)
def test_phase_names_are_nonempty_strings_without_jsonschema(
    validate_fallback, key, value
):
    manifest = good()
    manifest["fields"][-1]["phase"][key] = value
    assert any(key in problem for problem in validate_fallback(manifest))


def test_phase_interpretation_is_optional():
    manifest = good()
    del manifest["fields"][-1]["phase"]
    assert validate_manifest(manifest) == []


def test_quality_metrics_are_optional():
    manifest = good()
    del manifest["qualityMetrics"]
    assert validate_manifest(manifest) == []


def test_quality_metrics_grid_must_name_a_declared_grid():
    manifest = good()
    manifest["qualityMetrics"]["grid"] = "nonexistent"
    assert any("qualityMetrics.grid" in p for p in validate_manifest(manifest))


def test_quality_references_are_case_sensitive():
    manifest = good()
    manifest["qualityMetrics"]["grid"] = "Main"
    manifest["qualityMetrics"]["velocityField"] = "u"
    problems = validate_manifest(manifest)
    assert any("qualityMetrics.grid" in problem for problem in problems)
    assert any("velocityField" in problem for problem in problems)


def test_zero_effective_dimensions_agree_with_a_single_active_cell():
    manifest = good()
    manifest["grids"][0]["dimensions"] = [1, 1, 1]
    manifest["qualityMetrics"]["activeDimensions"] = [1, 1, 1]
    manifest["qualityMetrics"]["activeCellCount"] = 1
    manifest["qualityMetrics"]["effectiveSpatialDimensions"] = 0
    assert validate_manifest(manifest) == []


def test_zero_frame_quality_evidence_agrees_with_an_empty_timeline():
    manifest = good()
    manifest["timeline"]["frameCount"] = 0
    manifest["timeline"]["times"] = []
    manifest["timeline"]["steps"] = []
    del manifest["timeline"]["sampling"]
    manifest["qualityMetrics"]["temporalFrameCount"] = 0
    assert validate_manifest(manifest) == []


def test_grid_dimension_leaves_room_for_point_value_extent(validate_fallback):
    manifest = good()
    manifest["grids"][0]["dimensions"][0] = 2_147_483_646
    assert validate_manifest(manifest) == []
    assert validate_fallback(manifest) == []

    manifest["grids"][0]["dimensions"][0] = 2_147_483_647
    for validate in (validate_manifest, validate_fallback):
        problems = validate(manifest)
        assert any("dimensions" in problem for problem in problems)


def test_quality_metrics_velocity_must_name_a_vector_field_on_its_grid():
    manifest = good()
    manifest["qualityMetrics"]["velocityField"] = "pressure"
    assert any("velocityField" in p for p in validate_manifest(manifest))


def test_quality_velocity_component_rms_has_three_components():
    manifest = good()
    manifest["qualityMetrics"]["velocityComponentRms"] = [1.0, 0.25]
    assert any("velocityComponentRms" in p for p in validate_manifest(manifest))


def test_quality_spanwise_gradient_must_be_finite_and_non_negative():
    manifest = good()
    manifest["qualityMetrics"]["spanwiseGradientRms"] = float("nan")
    assert any("spanwiseGradientRms" in p for p in validate_manifest(manifest))


def test_quality_active_dimensions_must_fit_the_declared_grid():
    manifest = good()
    manifest["qualityMetrics"]["activeDimensions"] = [5, 3, 2]
    assert any("activeDimensions" in p for p in validate_manifest(manifest))


def test_quality_effective_dimensions_must_match_active_dimensions():
    manifest = good()
    manifest["qualityMetrics"]["effectiveSpatialDimensions"] = 2
    assert any(
        "effectiveSpatialDimensions" in p for p in validate_manifest(manifest)
    )


@pytest.mark.parametrize(
    "key,value",
    [
        ("effectiveSpatialDimensions", 3.0),
        ("effectiveSpatialDimensions", True),
        ("temporalFrameCount", 2.0),
        ("temporalFrameCount", True),
    ],
)
def test_quality_integer_evidence_is_strict_without_jsonschema(
    validate_fallback, key, value
):
    manifest = good()
    manifest["qualityMetrics"][key] = value
    assert any(key in problem for problem in validate_fallback(manifest))


def test_quality_active_cell_count_must_fit_active_dimensions():
    manifest = good()
    manifest["qualityMetrics"]["activeCellCount"] = 25
    assert any("activeCellCount" in p for p in validate_manifest(manifest))


def test_quality_temporal_frame_count_must_match_the_timeline():
    manifest = good()
    manifest["qualityMetrics"]["temporalFrameCount"] = 3
    assert any("temporalFrameCount" in p for p in validate_manifest(manifest))


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
