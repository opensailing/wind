"""The low-resolution sample case that is committed to source control.

``test_mock.py`` proves the *generator* is correct. This module proves the
*artefact* in ``FlowViz/Samples/`` is the one that generator produces and is
still readable — which is a separate fact, and the one the Unreal side actually
depends on. Nothing in the renderer can display anything until a case exists on
disk, and this is that case.

Committed binaries rot in ways source does not: a regenerated case that nobody
re-committed, a partial ``git add`` that dropped the frames, a merge that took
the manifest from one side and the payloads from the other. Every check here is
about the bytes that are checked in, not about the code that made them.

The tests skip rather than fail when the sample is absent, so the Python package
stays testable when vendored on its own. The skip message says how to produce it.
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest

from cfdviz.case import validate_case, verify_known_values
from cfdviz.manifest import field_by_id, frame_path, load_manifest
from cfdviz.mock import MOCK_DISCLAIMER, low_resolution_parameters

#: ``tests`` -> ``cfdviz`` -> ``Tools`` -> ``FlowViz`` -> repository root.
_REPO_ROOT = Path(__file__).resolve().parents[4]

SAMPLE = _REPO_ROOT / "FlowViz" / "Samples" / "MockCylinderWake.cfdviz"

#: The ceiling this sample exists to stay under. Plan section 7 asks for "a few
#: MB at most"; 4 MiB is that, rounded to a number a reviewer can check by eye
#: with ``du``. Raising it is a decision, not an accident, and this constant is
#: where that decision has to be written down.
SIZE_LIMIT_BYTES = 4 * 1024 * 1024


pytestmark = pytest.mark.skipif(
    not SAMPLE.is_dir(),
    reason=(
        f"{SAMPLE} is absent; regenerate with "
        "python -m cfdviz generate-mock --low-res --output "
        "FlowViz/Samples/MockCylinderWake.cfdviz"
    ),
)


def sample_bytes() -> int:
    return sum(path.stat().st_size for path in SAMPLE.rglob("*") if path.is_file())


def test_the_committed_sample_validates():
    """The whole point: this case must load, byte for byte, CRC for CRC.

    ``validate_case`` re-decodes every brick and re-checks every checksum, so a
    truncated or half-committed payload fails here rather than in Unreal.
    """
    report = validate_case(SAMPLE)
    assert report.ok, report.render()


def test_the_committed_sample_matches_its_own_known_values():
    """spec 9's cross-implementation bridge, checked against the stored file.

    ``known_values.json`` is what the C++ reader is graded against. If it were
    regenerated from different data than the ``.cvf`` files beside it, the
    Unreal tests would be measuring agreement with a stale answer key.
    """
    bridge = json.loads((SAMPLE / "known_values.json").read_text(encoding="utf-8"))
    assert bridge["samples"], "the bridge declares no samples to check"
    problems = verify_known_values(SAMPLE, bridge)
    assert not problems, "\n".join(problems)


def test_the_committed_sample_is_the_low_resolution_preset():
    """The artefact must be what ``--low-res`` produces, not a stale variant.

    Comparing the case id is what makes this cheap and exact: the generator
    derives it as a uuid5 over every parameter, so any drift between the
    committed bytes and the current preset changes it.
    """
    manifest = load_manifest(SAMPLE)
    expected = low_resolution_parameters()
    assert manifest["case"]["id"] == expected.case_id, (
        "the committed sample was generated from different parameters than "
        "low_resolution_parameters() currently defines; regenerate it with "
        "python -m cfdviz generate-mock --low-res"
    )
    assert manifest["grids"][0]["dimensions"] == list(expected.dimensions)
    assert manifest["timeline"]["frameCount"] == expected.frame_count


def test_the_committed_sample_fits_in_source_control():
    """A repository is not a data store. Exact size is reported on failure."""
    total = sample_bytes()
    assert total <= SIZE_LIMIT_BYTES, (
        f"the committed sample is {total} bytes "
        f"({total / 1024 / 1024:.2f} MiB), over the "
        f"{SIZE_LIMIT_BYTES / 1024 / 1024:.0f} MiB limit"
    )


def test_every_frame_declared_by_the_manifest_is_present_on_disk():
    """Catches a partial commit, which validation alone would not.

    A ``.gitignore`` rule that excludes ``frames/`` leaves a manifest promising
    20 frames next to a directory holding none. The failure has to name the
    missing file, because the fix is a ``git add``, not a code change.
    """
    manifest = load_manifest(SAMPLE)
    missing = [
        str(frame_path(SAMPLE, entry["storage"]["pathPattern"], frame).relative_to(SAMPLE))
        for entry in manifest["fields"]
        for frame in range(manifest["timeline"]["frameCount"])
        if not frame_path(SAMPLE, entry["storage"]["pathPattern"], frame).is_file()
    ]
    assert not missing, f"declared but not committed: {missing}"


def test_the_committed_manifest_carries_the_exact_disclaimer():
    """The wording is normative. Paraphrasing it is a licensing-adjacent bug:
    this data must never be mistaken for validated CFD by anyone reading the
    case, and the manifest is the only place that travels with the bytes.
    """
    manifest = load_manifest(SAMPLE)
    assert manifest["case"]["description"] == MOCK_DISCLAIMER
    assert manifest["case"]["quality"] == "visualization-demo"
    assert MOCK_DISCLAIMER in manifest["provenance"]["notes"]


def test_the_committed_json_contains_no_bare_nan_token():
    """``json.dumps`` writes a bare ``NaN``, which strict parsers reject.

    Unreal's ``FJsonSerializer`` is one of them, so a manifest with that token
    does not fail gracefully there — the case simply does not load. The masked
    cells in this very sample are the ones that would produce it.
    """
    def reject(token: str) -> object:
        raise AssertionError(
            f"bare {token!r} literal in committed JSON; strict parsers, "
            "including Unreal's, reject it"
        )

    for name in ("manifest.json", "known_values.json"):
        json.loads((SAMPLE / name).read_text(encoding="utf-8"), parse_constant=reject)


def test_the_committed_sample_still_masks_its_obstacle():
    """Masked cells must be present *and* carry NaN in the physical fields.

    Both halves matter. A sample whose mask is all-ones has no obstacle to
    render; one that masks cells but stores a finite value there feeds a
    plausible-looking number into the volume renderer, which is worse.
    """
    from cfdviz.cvf import read_cvf

    manifest = load_manifest(SAMPLE)
    mask_entry = field_by_id(manifest, "validMask")
    mask = read_cvf(
        frame_path(SAMPLE, mask_entry["storage"]["pathPattern"], 0)
    ).values[..., 0]
    assert np.any(mask == 0), "no cell is masked; the obstacle is missing"

    velocity_entry = field_by_id(manifest, "U")
    velocity = read_cvf(
        frame_path(SAMPLE, velocity_entry["storage"]["pathPattern"], 0)
    ).values
    inside = mask == 0
    assert np.all(np.isnan(velocity[inside])), (
        "the obstacle interior stores finite velocity; the renderer would draw "
        "flow through solid geometry"
    )
    assert np.all(np.isfinite(velocity[~inside])), (
        "a cell outside the obstacle is not finite"
    )


def test_the_committed_sample_ships_its_geometry():
    """Named boundary patches are what the UI labels; a mesh-less case has no
    obstacle to draw and no inlet to point at.
    """
    manifest = load_manifest(SAMPLE)
    patches = {
        patch["name"]
        for mesh in manifest["meshes"]
        for patch in mesh.get("patches", [])
    }
    assert patches == {"cylinderWall", "inlet", "outlet", "sideWalls"}
    for mesh in manifest["meshes"]:
        assert (SAMPLE / mesh["path"]).is_file(), f"{mesh['path']} is not committed"
