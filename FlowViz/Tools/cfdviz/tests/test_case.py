"""Case-level validation and the cross-language bridge — spec sections 9 and 10.

Section 10 is unusually specific about what failure looks like: a malformed case
"MUST produce a specific, actionable error message identifying the offending
file and byte offset. It MUST NOT crash, and MUST NOT report success."

So every test here is differential. Each one breaks one thing in an otherwise
valid case and requires that

1. validation reports failure (not just "doesn't crash"), and
2. the report names the offending file.

The `valid_case` fixture is validated clean in its own test, which is what stops
the whole file from passing because the case builder was broken.
"""

from __future__ import annotations

import json
import struct
from pathlib import Path

import numpy as np
import pytest

from conftest import build_case, sample_manifest
from cfdviz.case import (
    ValidationReport,
    build_known_values,
    validate_case,
    verify_known_values,
)
from cfdviz.crc32c import CHECK_VALUE


# ---------------------------------------------------------------------------
# The baseline: without this, every test below is meaningless
# ---------------------------------------------------------------------------

def test_a_valid_case_validates_clean(valid_case: Path):
    report = validate_case(valid_case)
    assert report.ok, report.render()
    assert report.errors == []


def test_report_counts_what_it_checked(valid_case: Path):
    """A report that checked nothing would also have zero errors."""
    report = validate_case(valid_case)
    assert report.checked_files >= 6   # 3 fields x 2 frames
    assert report.checked_bricks >= 6


# ---------------------------------------------------------------------------
# Structural failures
# ---------------------------------------------------------------------------

def test_a_missing_manifest_is_reported_not_raised(tmp_path: Path):
    root = tmp_path / "Empty.cfdviz"
    root.mkdir()
    report = validate_case(root)
    assert not report.ok
    assert any("manifest.json" in e for e in report.errors)


def test_a_case_root_that_is_not_a_directory_is_reported(tmp_path: Path):
    path = tmp_path / "notacase.txt"
    path.write_text("hello")
    report = validate_case(path)
    assert not report.ok
    assert any("directory" in e for e in report.errors)


def test_a_manifest_invariant_violation_is_reported(tmp_path: Path):
    manifest = sample_manifest()
    root = build_case(tmp_path / "Bad.cfdviz", manifest)
    manifest["timeline"]["times"] = [1.0, 0.5]
    (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")

    report = validate_case(root)
    assert not report.ok
    assert any("monoton" in e for e in report.errors)


def test_a_missing_frame_file_is_reported_with_its_path(valid_case: Path):
    """Spec 10: per-frame presence of every declared field file."""
    missing = valid_case / "frames" / "000001" / "U.cvf"
    missing.unlink()

    report = validate_case(valid_case)
    assert not report.ok
    assert any("frames/000001/U.cvf" in e for e in report.errors)


def test_a_corrupt_header_is_reported_with_file_and_offset(valid_case: Path):
    """Spec 10: name the offending file AND the byte offset."""
    path = valid_case / "frames" / "000000" / "pressure.cvf"
    blob = bytearray(path.read_bytes())
    struct.pack_into("<I", blob, 24, 4242)  # frameIndex, leaving a stale CRC
    path.write_bytes(bytes(blob))

    report = validate_case(valid_case)
    assert not report.ok
    joined = " ".join(report.errors)
    assert "pressure.cvf" in joined
    assert "byte offset" in joined
    assert "CRC" in joined


def test_a_corrupt_brick_payload_is_reported(valid_case: Path):
    """Spec 4.4.8: surfaced in the report, never silently zero-filled."""
    path = valid_case / "frames" / "000000" / "U.cvf"
    blob = bytearray(path.read_bytes())
    payload_offset = struct.unpack_from("<Q", blob, 80)[0]
    blob[payload_offset] ^= 0xFF
    path.write_bytes(bytes(blob))

    report = validate_case(valid_case)
    assert not report.ok
    assert any("payload CRC" in e for e in report.errors)


def test_a_truncated_field_file_is_reported_not_crashed(valid_case: Path):
    path = valid_case / "frames" / "000000" / "pressure.cvf"
    path.write_bytes(path.read_bytes()[:100])

    report = validate_case(valid_case)
    assert not report.ok
    assert any("pressure.cvf" in e for e in report.errors)


def test_a_field_file_whose_header_contradicts_the_manifest_is_reported(
    valid_case: Path,
):
    """A .cvf must agree with the manifest entry that points at it."""
    manifest = sample_manifest()
    manifest["grids"][0]["dimensions"] = [8, 8, 8]  # the files say 4x3x2
    (valid_case / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")

    report = validate_case(valid_case)
    assert not report.ok
    assert any("dimension" in e for e in report.errors)


def test_a_wrong_field_numeric_id_is_reported(valid_case: Path):
    manifest = sample_manifest()
    manifest["fields"][0]["numericId"] = 77
    (valid_case / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")

    report = validate_case(valid_case)
    assert not report.ok
    assert any("numericId" in e or "fieldNumericId" in e for e in report.errors)


def test_a_header_frame_index_disagreeing_with_its_directory_is_reported(
    valid_case: Path,
):
    """frames/000001/ must contain files whose header says frameIndex 1.

    A file filed under the wrong frame directory reads back as the wrong
    instant in time, which is invisible in a still image and obvious only as a
    stutter in an animation.
    """
    from conftest import tamper_cvf

    path = valid_case / "frames" / "000001" / "pressure.cvf"
    path.write_bytes(tamper_cvf(path.read_bytes(), 24, "<I", 7))

    report = validate_case(valid_case)
    assert not report.ok
    joined = " ".join(report.errors)
    assert "frames/000001/pressure.cvf" in joined
    assert "frameIndex" in joined


def test_a_frame_time_disagreeing_with_the_timeline_is_reported(valid_case: Path):
    manifest = sample_manifest()
    manifest["timeline"]["times"] = [0.0, 99.0]  # the file says 0.5
    (valid_case / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")

    report = validate_case(valid_case)
    assert not report.ok
    assert any("simulationTime" in e for e in report.errors)


def test_declared_statistics_are_recomputed_and_the_discrepancy_shown(
    tmp_path: Path,
):
    """Spec 10: "declared vs. recomputed field statistics, with the discrepancy
    shown"."""
    manifest = sample_manifest()
    manifest["fields"][0]["statistics"] = {
        "globalComponentMin": [-999.0],
        "globalComponentMax": [999.0],
    }
    root = build_case(tmp_path / "Stats.cfdviz", manifest)

    report = validate_case(root)
    assert not report.ok
    joined = " ".join(report.errors)
    assert "statistics" in joined
    assert "-999" in joined, "the report must show the declared value"


@pytest.mark.parametrize("bogus", [float("nan"), float("inf"), float("-inf")])
def test_a_non_finite_declared_statistic_is_reported_as_such(
    tmp_path: Path, bogus: float
):
    """NaN and infinity are legal *payload* values but never legal statistics.

    Spec 4.4.7 gives all-NaN bricks the sentinel min=+inf/max=-inf, so an
    infinity leaking into a manifest usually means an all-NaN field was
    summarised and written out verbatim. Diagnosed by name, because "declared
    12.75 but got inf" would send the reader looking in the wrong place.
    """
    manifest = sample_manifest()
    manifest["fields"][0]["statistics"] = {"globalComponentMin": [bogus]}
    root = build_case(tmp_path / "NonFinite.cfdviz", manifest)
    # json.dumps writes a bare NaN/Infinity token, which is not legal JSON;
    # the manifest is written with allow_nan so the *validator* is what fails.
    (root / "manifest.json").write_text(
        json.dumps(manifest), encoding="utf-8"
    )

    report = validate_case(root)
    assert not report.ok
    assert any("finite" in e for e in report.errors), report.errors


def test_correct_declared_statistics_pass(tmp_path: Path):
    """Proves the statistics check is not simply always-fail."""
    manifest = sample_manifest()
    root = build_case(tmp_path / "Stats2.cfdviz", manifest)
    recomputed = validate_case(root).statistics["pressure"]

    manifest["fields"][0]["statistics"] = {
        "globalComponentMin": [float(recomputed.minimum[0])],
        "globalComponentMax": [float(recomputed.maximum[0])],
    }
    root2 = build_case(tmp_path / "Stats3.cfdviz", manifest)
    report = validate_case(root2)
    assert report.ok, report.render()


# ---------------------------------------------------------------------------
# NaN, mask coverage, and counting — spec 10
# ---------------------------------------------------------------------------

def test_report_counts_nan_and_masked_cells_per_field(valid_case: Path):
    report = validate_case(valid_case)
    stats = report.statistics["pressure"]
    # The sample writes exactly one NaN per frame, over two frames.
    assert stats.nan_count == 2
    # One masked cell per frame, from validMask[0,0,0] = 0.
    assert stats.masked_count == 2
    assert report.statistics["U"].nan_count == 0


def test_statistics_exclude_nan_and_masked_cells(valid_case: Path):
    """Spec 1.7 and 4.4.7, checked against an independent recomputation."""
    from cfdviz.cvf import read_cvf

    report = validate_case(valid_case)
    stats = report.statistics["pressure"]

    mask = read_cvf(valid_case / "frames/000000/validMask.cvf").values[..., 0] != 0
    expected_max = -np.inf
    for frame in (0, 1):
        values = read_cvf(
            valid_case / f"frames/{frame:06d}/pressure.cvf"
        ).values[..., 0]
        with np.errstate(invalid="ignore"):
            usable = values[mask & ~np.isnan(values)]
        expected_max = max(expected_max, float(usable.max()))
    assert float(stats.maximum[0]) == pytest.approx(expected_max)


def test_mask_coverage_is_reported(valid_case: Path):
    """Spec 10: "mask coverage"."""
    report = validate_case(valid_case)
    # 4*3*2 = 24 cells, one of them masked out, per frame.
    assert report.statistics["pressure"].valid_count == 2 * (24 - 1) - 2


def test_a_mask_field_of_the_wrong_shape_is_reported(tmp_path: Path):
    from cfdviz.cvf import write_cvf

    root = build_case(tmp_path / "BadMask.cfdviz")
    write_cvf(
        root / "frames/000000/validMask.cvf",
        values=np.ones((2, 2, 2, 1), dtype="u1"),
        dtype="uint8",
        codec=0,
        brick_size=(4, 4, 4),
        field_numeric_id=3,
    )
    report = validate_case(root)
    assert not report.ok
    assert any("validMask" in e for e in report.errors)


# ---------------------------------------------------------------------------
# known_values.json — spec 9
# ---------------------------------------------------------------------------

def test_known_values_has_the_required_top_level_fields(valid_case: Path):
    bridge = build_known_values(valid_case)
    assert bridge["formatVersion"] == "1.0.0"
    assert bridge["caseId"] == sample_manifest()["case"]["id"]
    assert bridge["crc32cCheck"] == f"0x{CHECK_VALUE:08X}"
    assert bridge["samples"]


def test_known_values_bits_are_the_exact_stored_bit_pattern(valid_case: Path):
    """Spec 9.2: comparison is on ``bits``, never on ``value``."""
    from cfdviz.cvf import read_cvf

    manifest = sample_manifest()
    by_id = {f["id"]: f for f in manifest["fields"]}
    for sample in build_known_values(valid_case)["samples"]:
        entry = by_id[sample["field"]]
        path = valid_case / entry["storage"]["pathPattern"].format(
            frame=sample["frame"]
        )
        values = read_cvf(path).values
        x, y, z = sample["voxel"]
        stored = values[x, y, z, sample["component"]]
        raw = np.asarray(stored).tobytes()
        expected = int.from_bytes(raw, "little")
        digits = 2 * len(raw)
        assert sample["bits"] == f"0x{expected:0{digits}X}"


def test_known_values_bit_width_follows_the_field_data_type(valid_case: Path):
    """Spec 9.1: 4 hex digits for float16, 2 for uint8, 8 for float32."""
    widths = {"pressure": 8, "U": 8, "validMask": 2}
    for sample in build_known_values(valid_case)["samples"]:
        body = sample["bits"].removeprefix("0x")
        assert len(body) == widths[sample["field"]], sample
        assert body == body.upper(), "hex digits must be uppercase"


def test_known_values_covers_the_required_sample_set(valid_case: Path):
    """Spec 9.3 enumerates a minimum sample set; each item is required."""
    samples = build_known_values(valid_case)["samples"]
    fields = {s["field"] for s in samples}
    frames = {s["frame"] for s in samples}

    assert "pressure" in fields, "a scalar field is required"
    assert "U" in fields, "a vector field is required"
    assert {0, 1} <= frames, "the first and last stored frame are required"
    assert any(s.get("note") == "masked" for s in samples), "a masked voxel is required"
    assert any(s.get("note") == "partial-edge-brick" for s in samples)
    assert any(s.get("note") == "nan" for s in samples), "the case has a NaN"


def test_known_values_records_a_nan_by_its_bits_not_its_value(valid_case: Path):
    samples = build_known_values(valid_case)["samples"]
    nan_sample = next(s for s in samples if s.get("note") == "nan")
    # JSON has no NaN token, so value is null and bits carry the truth.
    assert nan_sample["value"] is None
    assert nan_sample["bits"] == "0x7FC00000"


def test_known_values_is_strict_json_with_no_nan_token(valid_case: Path):
    """``json.dumps`` will happily emit bare ``NaN``, which is not JSON.

    Python's own parser accepts it on the way back in, so a round-trip through
    this package would never notice. Unreal's parser does not, and the bridge
    file is precisely the thing Unreal has to read — so the check is that the
    serialized text is parseable with ``parse_constant`` set to reject.
    """
    from cfdviz.case import write_known_values

    text = (write_known_values(valid_case)).read_text(encoding="utf-8")
    for token in ("NaN", "Infinity", "-Infinity"):
        assert token not in text, f"{token} is not a JSON literal"

    def reject(constant: str) -> None:
        raise AssertionError(f"non-JSON constant {constant!r} in the bridge file")

    json.loads(text, parse_constant=reject)


def test_verify_known_values_accepts_the_case_it_was_built_from(valid_case: Path):
    bridge = build_known_values(valid_case)
    assert verify_known_values(valid_case, bridge) == []


def test_verify_known_values_detects_a_changed_voxel(valid_case: Path):
    """The check that proves the bridge can fail: mutate one stored bit."""
    from cfdviz.cvf import read_cvf, write_cvf

    bridge = build_known_values(valid_case)
    sample = next(s for s in bridge["samples"] if s["field"] == "U")
    path = valid_case / f"frames/{sample['frame']:06d}/U.cvf"
    values = read_cvf(path).values.copy()
    x, y, z = sample["voxel"]
    values[x, y, z, sample["component"]] += np.float32(1.0)
    write_cvf(path, values=values, brick_size=(4, 4, 4), field_numeric_id=2,
              simulation_time=float(sample["frame"]) * 0.5,
              frame_index=sample["frame"], codec=3)

    problems = verify_known_values(valid_case, bridge)
    assert problems
    assert any("bits" in p for p in problems)


def test_verify_known_values_rejects_a_wrong_crc_check_constant(valid_case: Path):
    """Spec 9: the bridge carries the CRC check value; a mismatch means the two
    sides do not share a CRC implementation and nothing else can be trusted."""
    bridge = build_known_values(valid_case)
    bridge["crc32cCheck"] = "0xCBF43926"  # zlib's CRC-32 check value
    problems = verify_known_values(valid_case, bridge)
    assert any("crc32cCheck" in p for p in problems)


def test_write_known_values_lands_at_the_case_root(valid_case: Path):
    from cfdviz.case import write_known_values

    path = write_known_values(valid_case)
    assert path == valid_case / "known_values.json"
    loaded = json.loads(path.read_text(encoding="utf-8"))
    assert loaded["crc32cCheck"] == f"0x{CHECK_VALUE:08X}"
    assert verify_known_values(valid_case, loaded) == []


def test_validate_checks_known_values_when_present(valid_case: Path):
    from cfdviz.case import write_known_values

    write_known_values(valid_case)
    assert validate_case(valid_case).ok

    path = valid_case / "known_values.json"
    bridge = json.loads(path.read_text(encoding="utf-8"))
    bridge["samples"][0]["bits"] = "0xDEADBEEF"
    path.write_text(json.dumps(bridge), encoding="utf-8")

    report = validate_case(valid_case)
    assert not report.ok
    assert any("known_values" in e or "bits" in e for e in report.errors)


# ---------------------------------------------------------------------------
# Mesh samples — spec 5, carried in the bridge so the Unreal mesh reader is
# verified against something other than two implementations reading the same
# sentence.
# ---------------------------------------------------------------------------

def test_the_sample_mesh_has_unequal_vertex_and_triangle_counts():
    """The property that makes every association test below able to fail.

    ``patchIds`` is per-triangle, ``nodeIds`` per-vertex. If the counts were
    equal, a reader that swapped them would produce correctly-sized arrays of
    wrong numbers and no length check could tell.
    """
    from conftest import SAMPLE_TRIANGLE_COUNT, SAMPLE_VERTEX_COUNT, sample_mesh

    assert SAMPLE_VERTEX_COUNT != SAMPLE_TRIANGLE_COUNT
    geometry = sample_mesh()
    assert geometry["positions"].shape[0] == SAMPLE_VERTEX_COUNT
    assert geometry["indices"].shape[0] == SAMPLE_TRIANGLE_COUNT
    assert geometry["patch_ids"].shape[0] == SAMPLE_TRIANGLE_COUNT
    assert geometry["node_ids"].shape[0] == SAMPLE_VERTEX_COUNT


def test_known_values_samples_mesh_vertex_positions_by_bits(valid_case: Path):
    from cfdviz.cvm import read_cvm

    mesh_samples = build_known_values(valid_case)["meshSamples"]
    positions = [s for s in mesh_samples if s["kind"] == "position"]
    assert positions, "vertex positions must be sampled"

    stored = read_cvm(valid_case / "meshes/obstacle.cvm").positions
    for sample in positions:
        actual = stored[sample["vertex"]][sample["component"]]
        raw = np.asarray(actual).tobytes()
        assert sample["bits"] == f"0x{int.from_bytes(raw, 'little'):0{2 * len(raw)}X}"


def test_known_values_samples_triangles_with_corner_order_preserved(
    valid_case: Path,
):
    """Spec 5: winding is CCW seen from outside, so corner ORDER is data.

    Sampling the index triple as a set would let a reader rotate or reverse each
    triangle — flipping its facing — and still pass.
    """
    from cfdviz.cvm import read_cvm

    mesh_samples = build_known_values(valid_case)["meshSamples"]
    triangles = [s for s in mesh_samples if s["kind"] == "triangle"]
    assert triangles, "triangles must be sampled"

    stored = read_cvm(valid_case / "meshes/obstacle.cvm").indices
    for sample in triangles:
        expected = [int(v) for v in stored[sample["triangle"]]]
        assert sample["indices"] == expected, "corner order must match exactly"

    # And the fixture must actually be order-sensitive, or the check above is
    # satisfied by any permutation.
    assert any(
        s["indices"] != sorted(s["indices"]) for s in triangles
    ), "no sampled triangle has a non-sorted corner order; the check cannot fail"


def test_known_values_samples_patch_ids_per_triangle(valid_case: Path):
    from conftest import SAMPLE_TRIANGLE_COUNT
    from cfdviz.cvm import read_cvm

    mesh_samples = build_known_values(valid_case)["meshSamples"]
    patches = [s for s in mesh_samples if s["kind"] == "patchId"]
    assert patches, "patchIds must be sampled"

    stored = read_cvm(valid_case / "meshes/obstacle.cvm").patch_ids
    for sample in patches:
        assert sample["triangle"] < SAMPLE_TRIANGLE_COUNT
        assert sample["value"] == int(stored[sample["triangle"]])

    # The LAST triangle must be among them. Bounded by triangleCount alone, a
    # sampler that walked the vertex count would still emit in-range indices
    # (there are fewer triangles than vertices) and every value would check out.
    assert max(s["triangle"] for s in patches) == SAMPLE_TRIANGLE_COUNT - 1


def test_known_values_samples_node_ids_per_vertex(valid_case: Path):
    from conftest import SAMPLE_VERTEX_COUNT
    from cfdviz.cvm import read_cvm

    mesh_samples = build_known_values(valid_case)["meshSamples"]
    nodes = [s for s in mesh_samples if s["kind"] == "nodeId"]
    assert nodes, "nodeIds must be sampled"

    stored = read_cvm(valid_case / "meshes/obstacle.cvm").node_ids
    for sample in nodes:
        assert sample["vertex"] < SAMPLE_VERTEX_COUNT
        assert sample["value"] == int(stored[sample["vertex"]])

    # The mirror of the patchId case, and the one that actually bites: a sampler
    # that walked triangleCount here would stop at vertex 1 of 5, leave the tail
    # of the array unsampled, and report nothing wrong.
    assert max(s["vertex"] for s in nodes) == SAMPLE_VERTEX_COUNT - 1


def test_samples_reach_both_ends_of_every_axis(valid_case: Path):
    """Sampling only index 0 would satisfy every per-value check above.

    Off-by-one errors in a reader's loop bounds live at the ends, so the first
    and last index of each sampled axis must appear.
    """
    from conftest import SAMPLE_TRIANGLE_COUNT, SAMPLE_VERTEX_COUNT

    bridge = build_known_values(valid_case)
    mesh_samples = bridge["meshSamples"]

    for kind, key, count in (
        ("position", "vertex", SAMPLE_VERTEX_COUNT),
        ("nodeId", "vertex", SAMPLE_VERTEX_COUNT),
        ("triangle", "triangle", SAMPLE_TRIANGLE_COUNT),
        ("patchId", "triangle", SAMPLE_TRIANGLE_COUNT),
    ):
        indices = {s[key] for s in mesh_samples if s["kind"] == kind}
        assert indices, f"{kind} must be sampled"
        assert min(indices) == 0, f"{kind} never samples the first entry"
        assert max(indices) == count - 1, f"{kind} never samples the last entry"

    entities = {s["entity"] for s in bridge["arraySamples"] if s["kind"] == "value"}
    assert min(entities) == 0
    assert max(entities) == SAMPLE_VERTEX_COUNT - 1


def test_mesh_samples_record_both_counts(valid_case: Path):
    """A reader that swapped the two associations would disagree here first."""
    from conftest import SAMPLE_TRIANGLE_COUNT, SAMPLE_VERTEX_COUNT

    mesh = build_known_values(valid_case)["meshes"][0]
    assert mesh["id"] == "obstacle"
    assert mesh["vertexCount"] == SAMPLE_VERTEX_COUNT
    assert mesh["triangleCount"] == SAMPLE_TRIANGLE_COUNT


def test_verify_known_values_detects_a_swapped_patch_and_node_association(
    valid_case: Path,
):
    """The differential test the unequal counts exist to make possible."""
    from conftest import sample_mesh
    from cfdviz.cvm import write_cvm

    bridge = build_known_values(valid_case)
    assert verify_known_values(valid_case, bridge) == []

    geometry = sample_mesh()
    # patchIds now carries what nodeIds should, truncated to triangle count.
    write_cvm(
        valid_case / "meshes/obstacle.cvm",
        positions=geometry["positions"],
        indices=geometry["indices"],
        patch_ids=geometry["node_ids"][: len(geometry["patch_ids"])].astype("<u4"),
        node_ids=geometry["node_ids"],
    )
    problems = verify_known_values(valid_case, bridge)
    assert problems
    assert any("patchId" in p for p in problems)


def test_verify_known_values_detects_a_moved_vertex(valid_case: Path):
    from conftest import sample_mesh
    from cfdviz.cvm import write_cvm

    bridge = build_known_values(valid_case)
    geometry = sample_mesh()
    geometry["positions"][0, 0] += np.float32(0.5)
    write_cvm(
        valid_case / "meshes/obstacle.cvm",
        positions=geometry["positions"],
        indices=geometry["indices"],
        patch_ids=geometry["patch_ids"],
        node_ids=geometry["node_ids"],
    )
    problems = verify_known_values(valid_case, bridge)
    assert any("position" in p for p in problems)


def test_verify_known_values_detects_a_reversed_triangle(valid_case: Path):
    """Reversing a triangle keeps every vertex and flips the facing."""
    from conftest import sample_mesh
    from cfdviz.cvm import write_cvm

    bridge = build_known_values(valid_case)
    geometry = sample_mesh()
    geometry["indices"][0] = geometry["indices"][0][::-1]
    write_cvm(
        valid_case / "meshes/obstacle.cvm",
        positions=geometry["positions"],
        indices=geometry["indices"],
        patch_ids=geometry["patch_ids"],
        node_ids=geometry["node_ids"],
    )
    problems = verify_known_values(valid_case, bridge)
    assert any("triangle" in p for p in problems)


# ---------------------------------------------------------------------------
# Mesh array samples — spec 6
# ---------------------------------------------------------------------------

def test_known_values_samples_array_payload_by_entity_and_component(
    valid_case: Path,
):
    from cfdviz.cva import read_cva

    array_samples = build_known_values(valid_case)["arraySamples"]
    payload = [s for s in array_samples if s["kind"] == "value"]
    assert payload, "array payload must be sampled"

    for sample in payload:
        stored = read_cva(valid_case / sample["path"]).values
        actual = stored[sample["entity"]][sample["component"]]
        raw = np.asarray(actual).tobytes()
        assert sample["bits"] == f"0x{int.from_bytes(raw, 'little'):0{2 * len(raw)}X}"


def test_array_samples_pin_the_symmetric_tensor_component_order(valid_case: Path):
    """Spec 6.3: XX YY ZZ XY YZ XZ, not the other common Voigt order.

    The wrong order permutes shear components into each other and every value
    stays physically plausible, so only an exact per-component comparison
    catches it. Recording the component *name* alongside the index is what makes
    the bridge assert the order rather than merely the values.
    """
    from cfdviz.cva import SYMMETRIC_TENSOR_COMPONENT_ORDER

    array_samples = build_known_values(valid_case)["arraySamples"]
    payload = [s for s in array_samples if s["kind"] == "value"]
    named = {s["component"]: s["componentName"] for s in payload}

    assert len(named) == 6, "every component of the tensor must be sampled"
    for index, name in named.items():
        assert name == SYMMETRIC_TENSOR_COMPONENT_ORDER[index]
    assert tuple(named[i] for i in range(6)) == ("XX", "YY", "ZZ", "XY", "YZ", "XZ")


def test_known_values_samples_array_statistics_per_component(valid_case: Path):
    from cfdviz.cva import read_cva

    array_samples = build_known_values(valid_case)["arraySamples"]
    stats = [s for s in array_samples if s["kind"] == "statistics"]
    assert stats, "array statistics must be sampled"

    for sample in stats:
        stored = read_cva(valid_case / sample["path"]).frame_statistics
        index = sample["component"]
        assert sample["validCount"] == int(stored.valid_count[index])
        for key, array in (("minimum", stored.minimum),
                           ("maximum", stored.maximum),
                           ("mean", stored.mean)):
            raw = np.float64(array[index]).tobytes()
            assert sample[key]["bits"] == f"0x{int.from_bytes(raw, 'little'):016X}"


def test_a_nan_component_reports_a_lower_valid_count(valid_case: Path):
    """The single integer that catches "NaN quietly became zero".

    Component 3 of the sample array carries one NaN; every other component is
    fully valid. If a reader coerced NaN to 0.0 the count would come back equal
    to its neighbours' and the payload bits alone would not say so.
    """
    from conftest import SAMPLE_VERTEX_COUNT

    array_samples = build_known_values(valid_case)["arraySamples"]
    stats = {
        s["component"]: s
        for s in array_samples
        if s["kind"] == "statistics" and s["frame"] == 0
    }
    assert stats[3]["validCount"] == SAMPLE_VERTEX_COUNT - 1
    for index in (0, 1, 2, 4, 5):
        assert stats[index]["validCount"] == SAMPLE_VERTEX_COUNT
    assert stats[3]["validCount"] < stats[0]["validCount"]


def test_verify_known_values_detects_nan_coerced_to_zero(valid_case: Path):
    """Differential: rewrite the array with the NaN replaced by 0.0."""
    from conftest import sample_array_values
    from cfdviz.cva import write_cva

    bridge = build_known_values(valid_case)
    assert verify_known_values(valid_case, bridge) == []

    values = sample_array_values(0)
    values[2, 3] = np.float32(0.0)
    write_cva(
        valid_case / "meshes/obstacle.wallShearStress.000000.cva",
        values=values, frame_index=0, simulation_time=0.0,
        association="mesh-vertex", field_numeric_id=10,
    )
    problems = verify_known_values(valid_case, bridge)
    assert problems
    assert any("validCount" in p for p in problems), problems


def test_verify_known_values_detects_a_permuted_tensor_component_order(
    valid_case: Path,
):
    """Swapping two shear components leaves every value plausible."""
    from conftest import sample_array_values
    from cfdviz.cva import write_cva

    bridge = build_known_values(valid_case)
    values = sample_array_values(0)
    # XY <-> YZ: the exact confusion the two Voigt conventions produce.
    values[:, [3, 4]] = values[:, [4, 3]]
    write_cva(
        valid_case / "meshes/obstacle.wallShearStress.000000.cva",
        values=values, frame_index=0, simulation_time=0.0,
        association="mesh-vertex", field_numeric_id=10,
    )
    problems = verify_known_values(valid_case, bridge)
    assert problems


def test_the_nan_itself_is_sampled_in_the_payload(valid_case: Path):
    """``validCount`` alone leaves the NaN's own bits unpinned.

    The statistic says "one value was not finite"; only a payload sample says
    *which* value and that it arrived as a quiet NaN rather than as a signalling
    one or as some other non-finite bit pattern.
    """
    array_samples = build_known_values(valid_case)["arraySamples"]
    payload = [
        s for s in array_samples
        if s["kind"] == "value" and s["frame"] == 0
        and s["entity"] == 2 and s["component"] == 3
    ]
    assert payload, "the entity carrying the NaN must be sampled"
    sample = payload[0]
    assert sample["value"] is None, "JSON cannot express NaN; value must be null"
    bits = int(sample["bits"], 16)
    assert np.isnan(np.frombuffer(bits.to_bytes(4, "little"), dtype="<f4")[0])


def test_verify_known_values_detects_an_altered_payload_value(valid_case: Path):
    """The payload comparison must fire on its own, not ride on the statistics.

    Changing a value also moves that component's min/max/mean, and those problem
    strings mention "bits" too — so a loose substring assertion here stays green
    with the payload branch deleted. The check must name the entity, which only
    the payload branch does.
    """
    from conftest import sample_array_values
    from cfdviz.cva import write_cva

    bridge = build_known_values(valid_case)
    values = sample_array_values(0)
    values[0, 0] = np.float32(99.5)
    write_cva(
        valid_case / "meshes/obstacle.wallShearStress.000000.cva",
        values=values, frame_index=0, simulation_time=0.0,
        association="mesh-vertex", field_numeric_id=10,
    )
    problems = verify_known_values(valid_case, bridge)
    assert any("entity 0" in p and "bits" in p for p in problems), problems


def test_verify_known_values_rejects_a_wrong_component_name(valid_case: Path):
    """The bridge asserts the Voigt order, so a renamed component must fail."""
    bridge = build_known_values(valid_case)
    for sample in bridge["arraySamples"]:
        if sample["component"] == 3:
            sample["componentName"] = "YZ"
            break
    problems = verify_known_values(valid_case, bridge)
    assert any("XY" in p for p in problems), problems


@pytest.mark.parametrize("key", ["minimum", "maximum", "mean"])
def test_verify_known_values_rejects_a_wrong_statistic(valid_case: Path, key: str):
    bridge = build_known_values(valid_case)
    for sample in bridge["arraySamples"]:
        if sample["kind"] == "statistics":
            sample[key]["bits"] = "0x0000000000000000"
            break
    problems = verify_known_values(valid_case, bridge)
    assert any(key in p for p in problems), problems


def test_verify_known_values_rejects_a_wrong_mesh_count(valid_case: Path):
    bridge = build_known_values(valid_case)
    bridge["meshes"][0]["vertexCount"] += 1
    problems = verify_known_values(valid_case, bridge)
    assert any("vertexCount" in p for p in problems), problems


def test_verify_known_values_rejects_an_unknown_sample_kind(valid_case: Path):
    """Silently skipping an unrecognised kind turns a typo into false coverage."""
    bridge = build_known_values(valid_case)
    bridge["meshSamples"].append({"kind": "patchID", "mesh": "obstacle", "triangle": 0})
    bridge["arraySamples"].append({
        "kind": "values",
        "path": "meshes/obstacle.wallShearStress.000000.cva",
        "component": 0,
    })
    problems = verify_known_values(valid_case, bridge)
    assert sum("unknown" in p for p in problems) == 2, problems


@pytest.mark.parametrize(
    ("kind", "key", "index"),
    [
        # 3 is a valid VERTEX index but out of range for 2 triangles: the exact
        # shape of a bridge built by a reader that swapped the associations.
        ("patchId", "triangle", 3),
        ("nodeId", "vertex", 9),
        ("position", "vertex", 9),
        ("triangle", "triangle", 9),
    ],
)
def test_verify_known_values_reports_an_out_of_range_index(
    valid_case: Path, kind: str, key: str, index: int
):
    """Out of range must be a problem string, not an IndexError out of the tool."""
    bridge = build_known_values(valid_case)
    sample = next(s for s in bridge["meshSamples"] if s["kind"] == kind)
    sample[key] = index
    problems = verify_known_values(valid_case, bridge)
    assert any("range" in p for p in problems), problems


def test_verify_known_values_reports_an_out_of_range_entity(valid_case: Path):
    bridge = build_known_values(valid_case)
    sample = next(s for s in bridge["arraySamples"] if s["kind"] == "value")
    sample["entity"] = 99
    problems = verify_known_values(valid_case, bridge)
    assert any("range" in p for p in problems), problems


def test_mesh_and_array_samples_survive_a_write_read_round_trip(valid_case: Path):
    """The bridge on disk must verify, not just the in-memory object."""
    from cfdviz.case import write_known_values

    path = write_known_values(valid_case)
    loaded = json.loads(path.read_text(encoding="utf-8"))
    assert loaded["meshSamples"] and loaded["arraySamples"]
    assert verify_known_values(valid_case, loaded) == []


# ---------------------------------------------------------------------------
# Report rendering
# ---------------------------------------------------------------------------

def test_report_renders_every_error(valid_case: Path):
    (valid_case / "frames" / "000000" / "U.cvf").unlink()
    report = validate_case(valid_case)
    text = report.render()
    for error in report.errors:
        assert error in text
    assert "FAIL" in text


def test_a_clean_report_says_so(valid_case: Path):
    text = validate_case(valid_case).render()
    assert "OK" in text
    assert "FAIL" not in text


def test_report_is_falsy_when_it_has_errors(valid_case: Path):
    assert bool(validate_case(valid_case)) is True
    (valid_case / "manifest.json").unlink()
    assert bool(validate_case(valid_case)) is False


def test_empty_report_is_not_ok_by_default():
    """A blank report must not read as success."""
    report = ValidationReport(root=Path("."))
    report.error("something went wrong")
    assert not report.ok
