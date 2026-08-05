"""The synthetic cylinder-wake generator — plan section 7.

Three ideas govern every assertion here, and all three come from bugs this
project has actually shipped:

1. **Gradients are checked on an ANISOTROPIC grid against a closed form.**
   Computing ``d/dx`` in index space instead of physical space is the classic
   finite-difference bug, and it is *invisible* on a cubic grid with unit
   spacing — every such test passes whether or not the code is right. So the
   test grid has three different, non-unit spacings, and each curl component is
   made to depend on exactly one of them.

2. **Every check is paired with the input that should break it.** For the
   load-bearing assertions (physical spacing, obstacle masking, determinism)
   there is a companion test that feeds in the *wrong* thing and asserts the
   check fails. A metric that cannot fail is not a check.

3. **Statistics are compared against the decoded bytes**, not against the
   arrays the generator happened to hold in memory. That is the only way to
   catch a writer that computes its manifest statistics over masked cells.
"""

from __future__ import annotations

import json
import math
from pathlib import Path

import numpy as np
import pytest

from cfdviz.case import validate_case, verify_known_values
from cfdviz.cvf import read_cvf
from cfdviz.cvm import read_cvm
from cfdviz.manifest import load_manifest
from cfdviz.mock import (
    MOCK_DISCLAIMER,
    MockCaseParameters,
    curl,
    generate_mock_case,
    low_resolution_parameters,
    obstacle_mask,
    q_criterion,
    velocity_gradient,
)

# ---------------------------------------------------------------------------
# A deliberately anisotropic test grid
# ---------------------------------------------------------------------------

#: Three different spacings, none of them 1.0. Every one of those properties is
#: load-bearing: equal spacings hide an axis mix-up, and unit spacing hides the
#: index-space bug entirely because dividing by 1 is a no-op.
ANISOTROPIC_SPACING = (0.5, 0.25, 0.125)


def _anisotropic_grid(shape: tuple[int, int, int] = (7, 5, 4)):
    """Cell-centre coordinate arrays on the anisotropic test grid."""
    axes = [
        (np.arange(n) + 0.5) * h for n, h in zip(shape, ANISOTROPIC_SPACING)
    ]
    return np.meshgrid(*axes, indexing="ij")


# ---------------------------------------------------------------------------
# Gradient-derived fields in PHYSICAL coordinates
# ---------------------------------------------------------------------------

def _quadratic_field():
    """``U = (a y^2, b z^2, c x^2)`` and its exact curl.

    Quadratic rather than linear so the interior *and* the second-order
    one-sided edge formulas are both exercised exactly. Each curl component
    involves exactly one axis, and a, b, c are distinct, so a transposed axis
    or a wrong spacing cannot cancel out.
    """
    x, y, z = _anisotropic_grid()
    a, b, c = 1.7, -0.9, 2.3
    velocity = np.stack([a * y**2, b * z**2, c * x**2], axis=-1)
    expected = np.stack([-2.0 * b * z, -2.0 * c * x, -2.0 * a * y], axis=-1)
    return velocity, expected


def test_curl_matches_a_closed_form_on_an_anisotropic_grid():
    velocity, expected = _quadratic_field()
    got = curl(velocity, ANISOTROPIC_SPACING)
    assert got.shape == expected.shape
    assert np.allclose(got, expected, rtol=1e-9, atol=1e-9), (
        "curl disagrees with the analytic result; the largest error is "
        f"{np.abs(got - expected).max()}"
    )


def test_curl_computed_in_index_space_does_not_match_the_closed_form():
    """The falsifiability twin of the test above.

    If this passes while the previous test also passes, the previous test is
    not actually measuring the spacing — which is exactly what happens on a
    cubic grid of unit spacing.
    """
    velocity, expected = _quadratic_field()
    index_space = curl(velocity, (1.0, 1.0, 1.0))
    assert not np.allclose(index_space, expected, rtol=1e-3, atol=1e-3), (
        "curl in index space matched the physical-coordinate answer, so the "
        "spacing argument is being ignored and the test above cannot fail"
    )


def test_velocity_gradient_scales_each_axis_by_its_own_spacing():
    """``J[i, j] = du_i/dx_j`` — one wrong axis must not be hidden by another."""
    x, y, z = _anisotropic_grid()
    velocity = np.stack([2.0 * y, 3.0 * z, 5.0 * x], axis=-1)
    jacobian = velocity_gradient(velocity, ANISOTROPIC_SPACING)

    expected = np.zeros((3, 3))
    expected[0, 1] = 2.0  # du/dy
    expected[1, 2] = 3.0  # dv/dz
    expected[2, 0] = 5.0  # dw/dx
    for i in range(3):
        for j in range(3):
            assert np.allclose(jacobian[..., i, j], expected[i, j], atol=1e-9), (
                f"J[{i},{j}] is {jacobian[..., i, j].mean()} not {expected[i, j]}"
            )


def test_q_criterion_of_rotation_plus_strain_matches_the_closed_form():
    """``Q = omega^2 - s^2`` for solid-body rotation plus planar strain.

    Both terms are needed: pure shear gives ``Q = 0`` for *any* spacing, so a
    shear-only test cannot detect an index-space gradient.
    """
    x, y, _ = _anisotropic_grid()
    omega, strain = 3.0, 1.25
    velocity = np.stack(
        [-omega * y + strain * x, omega * x - strain * y, np.zeros_like(x)],
        axis=-1,
    )
    q = q_criterion(velocity_gradient(velocity, ANISOTROPIC_SPACING))
    assert np.allclose(q, omega**2 - strain**2, rtol=1e-9, atol=1e-9)


def test_q_criterion_computed_in_index_space_does_not_match_the_closed_form():
    x, y, _ = _anisotropic_grid()
    omega, strain = 3.0, 1.25
    velocity = np.stack(
        [-omega * y + strain * x, omega * x - strain * y, np.zeros_like(x)],
        axis=-1,
    )
    q = q_criterion(velocity_gradient(velocity, (1.0, 1.0, 1.0)))
    assert not np.allclose(q, omega**2 - strain**2, rtol=1e-3, atol=1e-3)


def test_q_criterion_of_pure_shear_is_zero():
    """Rotation and strain cancel exactly in a parallel shear flow."""
    _, y, _ = _anisotropic_grid()
    velocity = np.stack([4.0 * y, np.zeros_like(y), np.zeros_like(y)], axis=-1)
    q = q_criterion(velocity_gradient(velocity, ANISOTROPIC_SPACING))
    assert np.allclose(q, 0.0, atol=1e-9)


# ---------------------------------------------------------------------------
# A small generated case, shared by the case-level tests
# ---------------------------------------------------------------------------

def tiny_parameters(**overrides) -> MockCaseParameters:
    """Default physics, small enough to generate inside a test run.

    The resolution is chosen so the cylinder still covers several cells: a grid
    too coarse to contain a single masked cell would make the masking tests
    vacuous.
    """
    settings: dict = {
        "dimensions": (48, 24, 6),
        "frame_count": 4,
        "brick_size": (16, 16, 8),
    }
    settings.update(overrides)
    return MockCaseParameters(**settings)


@pytest.fixture(scope="module")
def tiny_case(tmp_path_factory) -> Path:
    """One generated case, reused by every read-only test in this module."""
    root = tmp_path_factory.mktemp("mock") / "Tiny.cfdviz"
    generate_mock_case(root, tiny_parameters())
    return root


def _field_values(root: Path, field_id: str, frame: int) -> np.ndarray:
    manifest = load_manifest(root)
    entry = next(f for f in manifest["fields"] if f["id"] == field_id)
    pattern = entry["storage"]["pathPattern"]
    return read_cvf(root / pattern.format(frame=frame)).values


# ---------------------------------------------------------------------------
# The obstacle is masked
# ---------------------------------------------------------------------------

def test_the_obstacle_is_masked_and_nothing_else_is(tiny_case: Path):
    """Masked cells are exactly the cells whose CENTRE is inside the cylinder.

    The expectation is rebuilt here from ``origin + spacing * (i + 0.5)``, which
    is spec 3.2's cell-centre rule. A generator that used node coordinates
    (``origin + spacing * i``) produces a mask shifted half a cell in x and y,
    and this comparison is what notices.
    """
    parameters = tiny_parameters()
    nx, ny, nz = parameters.dimensions
    sx, sy, _ = parameters.spacing
    ox, oy, _ = parameters.origin
    cx, cy = parameters.cylinder_center

    i = np.arange(nx)[:, None]
    j = np.arange(ny)[None, :]
    distance_squared = (ox + sx * (i + 0.5) - cx) ** 2 + (
        oy + sy * (j + 0.5) - cy
    ) ** 2
    inside = distance_squared < parameters.cylinder_radius**2
    assert inside.any(), "the test grid is too coarse to contain any masked cell"

    stored = _field_values(tiny_case, "validMask", 0)[..., 0]
    assert stored.dtype == np.uint8
    for k in range(nz):
        assert np.array_equal(stored[:, :, k] == 0, inside), (
            f"masked cells at k={k} are not the cylinder interior"
        )


def test_the_mask_does_not_match_a_node_coordinate_convention(tiny_case: Path):
    """Falsifiability twin: the cell-centre and node conventions must differ.

    Without this, a half-cell error would be untestable — the assertion above
    would be comparing two identical arrays and could never fail.
    """
    parameters = tiny_parameters()
    nx, ny, _ = parameters.dimensions
    sx, sy, _ = parameters.spacing
    ox, oy, _ = parameters.origin
    cx, cy = parameters.cylinder_center

    i = np.arange(nx)[:, None]
    j = np.arange(ny)[None, :]
    node = (ox + sx * i - cx) ** 2 + (oy + sy * j - cy) ** 2 < (
        parameters.cylinder_radius**2
    )
    stored = _field_values(tiny_case, "validMask", 0)[..., 0]
    assert not np.array_equal(stored[:, :, 0] == 0, node), (
        "cell-centre and node masking agree on this grid, so the masking test "
        "above cannot detect a half-cell error"
    )


def test_the_obstacle_interior_carries_the_nan_sentinel(tiny_case: Path):
    """Masked cells hold NaN, not a plausible-looking zero.

    Spec 1.7 makes NaN legal and requires it to survive bit-exactly, and a zero
    inside a solid renders as "the flow stopped" rather than "there is no data
    here".
    """
    mask = _field_values(tiny_case, "validMask", 0)[..., 0] != 0
    for field_id in ("U", "pressure", "speed", "vorticity", "qCriterion"):
        values = _field_values(tiny_case, field_id, 0)
        interior = values[~mask]
        assert interior.size, "no masked cells to check"
        assert np.all(np.isnan(interior)), (
            f"{field_id} stores non-NaN values inside the obstacle"
        )


def test_every_field_is_finite_outside_the_obstacle(tiny_case: Path):
    """Plan section 7: velocity, pressure and derived fields are finite outside."""
    manifest = load_manifest(tiny_case)
    frames = manifest["timeline"]["frameCount"]
    for frame in range(frames):
        mask = _field_values(tiny_case, "validMask", frame)[..., 0] != 0
        for entry in manifest["fields"]:
            if entry["id"] == "validMask":
                continue
            values = _field_values(tiny_case, entry["id"], frame)
            outside = values[mask]
            assert np.all(np.isfinite(outside.astype(np.float64))), (
                f"{entry['id']} frame {frame} has "
                f"{int(np.count_nonzero(~np.isfinite(outside.astype(np.float64))))} "
                "non-finite values outside the obstacle"
            )


# ---------------------------------------------------------------------------
# Frame consistency and the timeline
# ---------------------------------------------------------------------------

def test_all_frames_have_consistent_dimensions(tiny_case: Path):
    manifest = load_manifest(tiny_case)
    grid = manifest["grids"][0]
    declared = tuple(grid["dimensions"])
    for entry in manifest["fields"]:
        for frame in range(manifest["timeline"]["frameCount"]):
            values = _field_values(tiny_case, entry["id"], frame)
            assert values.shape[:3] == declared, (
                f"{entry['id']} frame {frame} is {values.shape[:3]}, "
                f"expected {declared}"
            )
            assert values.shape[3] == entry["componentCount"]


def test_times_are_strictly_monotonic_and_match_the_frame_headers(tiny_case: Path):
    manifest = load_manifest(tiny_case)
    times = manifest["timeline"]["times"]
    assert len(times) == manifest["timeline"]["frameCount"]
    assert all(b > a for a, b in zip(times, times[1:])), times

    entry = next(f for f in manifest["fields"] if f["id"] == "U")
    for frame, time in enumerate(times):
        path = tiny_case / entry["storage"]["pathPattern"].format(frame=frame)
        header = read_cvf(path).header
        assert header.frame_index == frame
        assert header.simulation_time == pytest.approx(time, abs=1e-12)


# ---------------------------------------------------------------------------
# Global statistics vs. the decoded data
# ---------------------------------------------------------------------------

def _recompute(root: Path, field_id: str, *, apply_mask: bool):
    """Min/max per component over the decoded bytes of every frame."""
    manifest = load_manifest(root)
    entry = next(f for f in manifest["fields"] if f["id"] == field_id)
    components = entry["componentCount"]
    minimum = np.full(components, np.inf)
    maximum = np.full(components, -np.inf)
    for frame in range(manifest["timeline"]["frameCount"]):
        values = _field_values(root, field_id, frame).astype(np.float64)
        flat = values.reshape(-1, components)
        keep = np.ones(flat.shape[0], dtype=bool)
        if apply_mask:
            keep = (
                _field_values(root, "validMask", frame)[..., 0] != 0
            ).reshape(-1)
        valid = ~np.isnan(flat) & keep[:, None]
        minimum = np.minimum(minimum, np.where(valid, flat, np.inf).min(axis=0))
        maximum = np.maximum(maximum, np.where(valid, flat, -np.inf).max(axis=0))
    return minimum, maximum


def test_manifest_statistics_match_the_decoded_data(tiny_case: Path):
    """Spec 10 and plan section 7: declared statistics equal recomputed ones."""
    manifest = load_manifest(tiny_case)
    for entry in manifest["fields"]:
        if entry["id"] == "validMask":
            continue
        declared = entry.get("statistics")
        assert declared, f"{entry['id']} declares no statistics"
        minimum, maximum = _recompute(tiny_case, entry["id"], apply_mask=True)
        assert declared["globalComponentMin"] == pytest.approx(
            list(minimum), rel=0, abs=0
        ), f"{entry['id']} min"
        assert declared["globalComponentMax"] == pytest.approx(
            list(maximum), rel=0, abs=0
        ), f"{entry['id']} max"


def test_the_mask_is_redundant_for_statistics_on_this_case(tiny_case: Path):
    """Documents *why* the test above cannot, on its own, prove mask handling.

    Masked cells store NaN, and statistics already ignore NaN (spec 4.4.7), so
    on this case the two exclusion rules select the same cells and a writer
    that ignored the mask entirely would produce identical numbers. Asserting
    that here keeps the fact visible: the mask handling itself is proven by
    ``test_the_statistics_accumulator_honours_the_mask`` below, on data where
    the obstacle holds finite values and the two rules genuinely differ.
    """
    with_mask = _recompute(tiny_case, "pressure", apply_mask=True)
    without_mask = _recompute(tiny_case, "pressure", apply_mask=False)
    assert np.array_equal(with_mask[0], without_mask[0])
    assert np.array_equal(with_mask[1], without_mask[1])


def test_the_statistics_accumulator_honours_the_mask():
    """A masked outlier must not reach the declared statistics.

    This is the check the plan asks for — "global statistics match the decoded
    data" catches a writer that folds obstacle cells in — and it is written on
    data where it can actually fail: the obstacle holds a *finite* extreme
    value, so including it moves the minimum by 1000.
    """
    from cfdviz.mock import _StatisticsAccumulator

    values = np.ones((4, 3, 2, 1), dtype=np.float64)
    mask = np.ones((4, 3, 2), dtype=bool)
    mask[1, 1, 0] = False
    values[1, 1, 0, 0] = -1000.0

    masked = _StatisticsAccumulator(1)
    masked.add(values, mask)
    assert masked.to_manifest()["globalComponentMin"] == [1.0]

    unmasked = _StatisticsAccumulator(1)
    unmasked.add(values, None)
    assert unmasked.to_manifest()["globalComponentMin"] == [-1000.0], (
        "the mask makes no difference even to a finite outlier, so this check "
        "cannot fail"
    )


def test_declared_statistics_are_exactly_representable_in_the_storage_dtype(
    tmp_path: Path,
):
    """Statistics must describe the STORED bytes, not the pre-cast float64.

    With ``float16`` storage the two differ in almost every digit, so a
    generator that accumulated statistics before casting declares a minimum
    that no value in the case attains. On float32 the gap is small enough to
    slip under a relative tolerance; on float16 it cannot.
    """
    parameters = tiny_parameters(float_type="float16", frame_count=2)
    root = generate_mock_case(tmp_path / "Half.cfdviz", parameters)
    manifest = load_manifest(root)
    for entry in manifest["fields"]:
        if entry["dataType"] != "float16":
            continue
        for key in ("globalComponentMin", "globalComponentMax"):
            for value in entry["statistics"][key]:
                assert float(np.float16(value)) == value, (
                    f"{entry['id']}.{key} value {value!r} is not a float16, so it "
                    "was computed before the storage cast"
                )


def test_the_generated_case_validates(tiny_case: Path):
    report = validate_case(tiny_case)
    assert report.ok, report.render()


def test_the_known_values_bridge_is_written_and_self_consistent(tiny_case: Path):
    bridge = json.loads((tiny_case / "known_values.json").read_text("utf-8"))
    assert bridge["crc32cCheck"] == "0xE3069283"
    assert bridge["samples"], "the bridge carries no field samples"
    assert any(s.get("note") == "masked" for s in bridge["samples"]), (
        "spec 9.3 requires a sample inside the masked obstacle"
    )
    assert any(s.get("note") == "nan" for s in bridge["samples"]), (
        "spec 9.3 requires a NaN sample when the case contains one"
    )
    assert verify_known_values(tiny_case, bridge) == []


# ---------------------------------------------------------------------------
# The manifest: labelling and JSON that strict parsers accept
# ---------------------------------------------------------------------------

def test_the_manifest_carries_the_exact_disclaimer(tiny_case: Path):
    manifest = load_manifest(tiny_case)
    assert manifest["case"]["description"] == (
        "Synthetic visualization demonstration; not validation-grade CFD."
    )
    assert MOCK_DISCLAIMER == manifest["case"]["description"]
    assert manifest["case"]["quality"] == "visualization-demo"


def test_the_manifest_contains_no_bare_nan_or_infinity_token(tiny_case: Path):
    """Unreal's JSON parser rejects ``NaN``; ``json.dumps`` emits it by default.

    Parsing with ``parse_constant`` is the check that matters — a plain
    ``json.loads`` accepts ``NaN`` happily and would let the bug through.
    """
    text = (tiny_case / "manifest.json").read_text("utf-8")

    def reject(token: str):
        raise AssertionError(f"manifest contains the bare JSON token {token!r}")

    json.loads(text, parse_constant=reject)


def test_generation_refuses_to_emit_a_non_finite_statistic(tmp_path: Path):
    """The guard behind the test above, exercised directly."""
    from cfdviz.mock import dump_json

    with pytest.raises(ValueError):
        dump_json({"statistics": {"globalComponentMin": [float("nan")]}})


# ---------------------------------------------------------------------------
# Determinism
# ---------------------------------------------------------------------------

def _tree(root: Path) -> dict[str, bytes]:
    return {
        str(path.relative_to(root)): path.read_bytes()
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def test_the_same_seed_produces_byte_identical_output(tmp_path: Path):
    parameters = tiny_parameters(frame_count=2)
    first = generate_mock_case(tmp_path / "a.cfdviz", parameters)
    second = generate_mock_case(tmp_path / "b.cfdviz", parameters)
    left, right = _tree(first), _tree(second)
    assert sorted(left) == sorted(right)
    for name in left:
        assert left[name] == right[name], f"{name} differs between two runs"


def test_a_different_seed_produces_different_output(tmp_path: Path):
    """Falsifiability twin: the seed must actually reach the data.

    If it does not, the determinism test above passes for a generator that
    ignores its seed entirely, which is not what "deterministic" means.
    """
    first = generate_mock_case(tmp_path / "a.cfdviz", tiny_parameters(frame_count=2))
    second = generate_mock_case(
        tmp_path / "b.cfdviz", tiny_parameters(frame_count=2, seed=99)
    )
    left, right = _tree(first), _tree(second)
    assert any(left[name] != right[name] for name in left), (
        "changing the seed changed nothing, so the determinism test is vacuous"
    )


# ---------------------------------------------------------------------------
# Geometry: named patches, spanwise structure, the vortex street
# ---------------------------------------------------------------------------

def test_the_case_declares_the_four_required_named_patches(tiny_case: Path):
    """Spec 5.3 names them explicitly: inlet, outlet, sideWalls, cylinderWall."""
    manifest = load_manifest(tiny_case)
    declared: dict[str, int] = {}
    for mesh in manifest["meshes"]:
        for patch in mesh.get("patches", []):
            declared[patch["name"]] = patch["id"]
    assert set(declared) >= {"inlet", "outlet", "sideWalls", "cylinderWall"}

    stored: set[int] = set()
    for mesh in manifest["meshes"]:
        data = read_cvm(tiny_case / mesh["path"])
        assert data.patch_ids is not None, f"{mesh['id']} stores no patch ids"
        stored.update(int(v) for v in np.unique(data.patch_ids))
    for name in ("inlet", "outlet", "sideWalls", "cylinderWall"):
        assert declared[name] in stored, (
            f"patch {name!r} (id {declared[name]}) is declared but no triangle "
            "carries it"
        )


def test_the_volume_is_not_a_perfect_spanwise_extrusion(tiny_case: Path):
    """Plan section 7: mild spanwise variation.

    A perfectly extruded volume renders identically from every spanwise slice
    and hides any bug in the third texture axis, so the generator must produce
    genuine 3-D structure.
    """
    velocity = _field_values(tiny_case, "U", 2).astype(np.float64)
    mask = _field_values(tiny_case, "validMask", 2)[..., 0] != 0
    nz = velocity.shape[2]
    assert nz >= 3

    first = velocity[:, :, 0, :][mask[:, :, 0]]
    middle = velocity[:, :, nz // 2, :][mask[:, :, nz // 2]]
    spread = float(np.abs(velocity[mask]).max())
    assert np.abs(first - middle).max() > 0.01 * spread, (
        "two spanwise slices are identical; the volume is an extrusion"
    )

    vorticity = _field_values(tiny_case, "vorticity", 2).astype(np.float64)
    in_plane = np.abs(vorticity[mask][:, :2]).max()
    assert in_plane > 0.0, "vorticity has no x/y components at all"


def test_the_wake_is_an_alternating_vortex_street(tiny_case: Path):
    """Successive shed vortices must spin in opposite directions.

    This is what makes the case *visually* useful: a wake whose vortices all
    share a sign is a jet, not a Karman street.
    """
    from cfdviz.mock import WakeField

    parameters = tiny_parameters()
    field = WakeField(parameters)
    time = 3 * parameters.frame_interval
    signs = [
        math.copysign(1.0, field.vortex_circulation_at(index, time))
        for index in field.active_vortices(time)
    ]
    assert len(signs) >= 2, "no vortices are active in the domain"
    assert all(a * b < 0 for a, b in zip(signs, signs[1:])), (
        f"vortex signs do not alternate: {signs}"
    )


def test_the_inflow_is_uniform_at_the_inlet(tiny_case: Path):
    parameters = tiny_parameters()
    velocity = _field_values(tiny_case, "U", 0).astype(np.float64)
    inlet = velocity[0]
    assert np.allclose(inlet[..., 0], parameters.inlet_velocity, rtol=0.05)
    assert np.abs(inlet[..., 1]).max() < 0.05 * parameters.inlet_velocity
    assert np.abs(inlet[..., 2]).max() < 0.05 * parameters.inlet_velocity


def test_speed_agrees_with_the_stored_velocity(tiny_case: Path):
    """A derived field that disagrees with its source is worse than no field."""
    velocity = _field_values(tiny_case, "U", 1).astype(np.float64)
    speed = _field_values(tiny_case, "speed", 1)[..., 0].astype(np.float64)
    mask = _field_values(tiny_case, "validMask", 1)[..., 0] != 0
    expected = np.sqrt((velocity**2).sum(axis=-1))
    assert np.allclose(speed[mask], expected[mask], rtol=1e-3, atol=1e-3)


def test_obstacle_mask_is_available_without_writing_a_case():
    """The mask helper is part of the public surface, not a private detail."""
    parameters = tiny_parameters()
    mask = obstacle_mask(parameters)
    assert mask.shape == parameters.dimensions
    assert mask.dtype == np.bool_
    assert mask.all(axis=2).sum() < mask[:, :, 0].size


# ---------------------------------------------------------------------------
# The committed low-resolution sample
# ---------------------------------------------------------------------------

def test_the_low_resolution_preset_fits_in_source_control(tmp_path: Path):
    """Plan section 7 wants a sample small enough for ordinary source control.

    Four megabytes is the budget: large enough for a wake worth looking at,
    small enough that cloning the repository is not a download.
    """
    parameters = low_resolution_parameters()
    root = generate_mock_case(tmp_path / "LowRes.cfdviz", parameters)
    total = sum(p.stat().st_size for p in root.rglob("*") if p.is_file())
    assert total < 4 * 1024 * 1024, f"low-resolution case is {total} bytes"
    assert validate_case(root).ok


def test_the_low_resolution_preset_still_resolves_the_cylinder():
    """A sample too coarse to show the obstacle is not a usable demo."""
    parameters = low_resolution_parameters()
    mask = obstacle_mask(parameters)
    masked = int(np.count_nonzero(~mask))
    assert masked > 0, "the low-resolution grid masks nothing at all"
    diameter_cells = 2.0 * parameters.cylinder_radius / parameters.spacing[0]
    assert diameter_cells >= 3.0, (
        f"the cylinder is only {diameter_cells:.1f} cells across"
    )


# ---------------------------------------------------------------------------
# Mesh-associated arrays: the half of the bridge that had no data
# ---------------------------------------------------------------------------
#
# The CVA reader on the Unreal side was checked only against fixtures the same
# test file had written, which proves it is self-consistent and nothing more.
# `known_values.json` is what breaks that circularity for every other format --
# Python writes, C++ reads, the two are compared bit for bit -- and its
# `arraySamples` list was empty, because the generator wrote no `.cva` for the
# extractor to find. An empty list is not a passing check; it is no check.

def test_the_case_ships_a_mesh_associated_array(tiny_case: Path):
    """A ``.cva`` must exist, or the CVA half of the bridge has no input.

    `_array_samples` in case.py globs ``meshes/*.cva``. It is complete and
    correct; it was simply never given a file. The bridge it feeds is the only
    thing standing between the C++ CVA reader and grading its own homework.
    """
    arrays = sorted((tiny_case / "meshes").glob("*.cva"))
    assert arrays, "no .cva was written, so arraySamples will be empty"


def test_the_bridge_carries_array_samples(tiny_case: Path):
    """``arraySamples`` must be non-empty and must agree with the file on disk.

    Both halves matter. Non-empty is what the previous test buys; agreement is
    what makes it evidence. `verify_known_values` re-reads every ``.cva`` and
    compares bits, so a sample generated from different data than the file
    beside it fails here rather than in Unreal against a stale answer key.
    """
    bridge = json.loads((tiny_case / "known_values.json").read_text("utf-8"))
    samples = bridge["arraySamples"]
    assert samples, "the bridge declares no array samples to check"

    kinds = {s["kind"] for s in samples}
    assert "value" in kinds, "no payload sample: a mis-strided read would pass"
    assert "statistics" in kinds, (
        "no statistics sample: validCount is the only thing that catches a "
        "reader which coerces NaN to zero *before* counting, which is "
        "invisible in the payload"
    )
    assert verify_known_values(tiny_case, bridge) == []


def test_the_array_carries_a_nan_so_valid_count_can_disagree(tiny_case: Path):
    """The array must contain a non-finite value.

    ``validCount`` is the statistic that distinguishes a reader which excludes
    NaN from one which folds it to zero. On an all-finite array every reader
    agrees and the field cannot fail -- a pass criterion that cannot fail is
    not a check.
    """
    from cfdviz.cva import read_cva

    path = next(iter(sorted((tiny_case / "meshes").glob("*.cva"))))
    array = read_cva(path)
    assert not np.isfinite(array.values).all(), (
        f"{path.name} is entirely finite, so validCount can never disagree"
    )
    statistics = array.frame_statistics
    assert statistics is not None, "no per-frame statistics were stored"
    assert int(statistics.valid_count[0]) < array.value_count, (
        "validCount equals the value count, so the NaN was counted as valid"
    )


def test_the_wall_pressure_is_the_textbook_cylinder_solution(tiny_case: Path):
    """The array must be right, not merely self-consistent.

    A `.cva` full of arbitrary numbers would satisfy every bridge test above --
    Python would write it, C++ would read it back, and the bits would agree.
    That checks the *plumbing*. This checks the *content*, against a closed-form
    answer neither implementation computed: inviscid flow over a cylinder has

        Cp = 1 - 4 sin^2(theta)

    which is +1 at the upstream stagnation point and -3 at the shoulders. If
    the wall array disagreed with that, the renderer would be colouring the
    obstacle with a plausible-looking lie.
    """
    from cfdviz.cva import read_cva

    parameters = tiny_parameters()
    array = read_cva(tiny_case / "meshes" / "obstacleWallPressure_000000.cva")
    mesh = read_cvm(tiny_case / "meshes" / "obstacle.cvm")

    cx, cy = parameters.cylinder_center
    dx = mesh.positions[:, 0].astype(np.float64) - cx
    dy = mesh.positions[:, 1].astype(np.float64) - cy
    theta = np.arctan2(dy, -dx)

    dynamic = 0.5 * parameters.density * parameters.inlet_velocity**2
    cp = array.values[:, 0].astype(np.float64) / dynamic
    finite = np.isfinite(cp)
    expected = 1.0 - 4.0 * np.sin(theta) ** 2

    # Frame 0 carries the breathing-mode phase factor, so compare shape rather
    # than absolute scale: the phase multiplies every vertex equally.
    scale = np.median(cp[finite] / expected[finite])
    assert np.allclose(cp[finite], scale * expected[finite], rtol=1e-4, atol=1e-4)

    # The named extremes, which are what someone reading the picture checks.
    assert cp[finite].max() == pytest.approx(1.0 * scale, rel=1e-3)
    assert cp[finite].min() == pytest.approx(-3.0 * scale, rel=1e-3)


def test_the_wall_pressure_changes_between_frames(tiny_case: Path):
    """A reader that ignored frameIndex and always returned frame 0 must fail.

    Identical frames make that bug invisible: every comparison passes because
    every frame is the same answer.
    """
    from cfdviz.cva import read_cva

    first = read_cva(tiny_case / "meshes" / "obstacleWallPressure_000000.cva")
    second = read_cva(tiny_case / "meshes" / "obstacleWallPressure_000001.cva")
    assert second.frame_index == 1, "the header does not record its own frame"
    a, b = first.values[:, 0], second.values[:, 0]
    finite = np.isfinite(a) & np.isfinite(b)
    assert not np.array_equal(a[finite], b[finite]), (
        "consecutive frames are identical, so a frame-ignoring reader passes"
    )
