"""Shared evidence and output helpers for external-solver converters."""

from __future__ import annotations

import shutil
from pathlib import Path

import numpy as np
import pytest

from cfdviz.convert import (
    ConversionError,
    QualityAccumulator,
    StatisticsAccumulator,
    require_safe_identifier,
    staged_output,
)


def test_statistics_describe_stored_values_not_precise_source_values():
    source = np.array([[[[1.0003], [4.0]]]], dtype=np.float64)
    stored = source.astype("<f2")
    accumulator = StatisticsAccumulator(1)

    accumulator.add(stored, mask=None)

    statistics = accumulator.to_manifest()
    assert statistics is not None
    assert statistics["globalComponentMin"] == [float(stored.min())]
    assert statistics["globalComponentMin"] != [float(source.min())]


def test_statistics_ignore_masked_and_nan_values():
    values = np.array([[[[1.0]], [[np.nan]], [[1.0e20]]]], dtype="<f4")
    mask = np.array([[[True], [True], [False]]])
    accumulator = StatisticsAccumulator(1)

    accumulator.add(values, mask)

    statistics = accumulator.to_manifest()
    assert statistics is not None
    assert statistics["globalComponentMin"] == [1.0]
    assert statistics["globalComponentMax"] == [1.0]


def test_statistics_widen_values_in_bounded_cell_chunks(monkeypatch):
    values = np.arange(5, dtype="<f4").reshape(5, 1)
    accumulator = StatisticsAccumulator(1)
    chunk_sizes: list[int] = []
    original = accumulator._add_chunk

    def record_chunk(data: np.ndarray, mask: np.ndarray | None) -> None:
        chunk_sizes.append(data.shape[0])
        original(data, mask)

    monkeypatch.setattr("cfdviz.convert._STATISTICS_CHUNK_CELLS", 2)
    monkeypatch.setattr(accumulator, "_add_chunk", record_chunk)
    accumulator.add(values, mask=None)

    assert chunk_sizes == [2, 2, 1]
    assert accumulator.to_manifest()["globalComponentMax"] == [4.0]


def test_quality_metrics_use_physical_spanwise_spacing_and_all_frames():
    x, y, z = np.indices((3, 2, 4), dtype=np.float64)
    first = np.stack((x, 2.0 * y, 3.0 * z), axis=-1)
    second = 2.0 * first
    mask = np.ones((3, 2, 4), dtype=bool)
    accumulator = QualityAccumulator((3, 2, 4), (0.5, 2.0, 3.0))

    accumulator.add(first, mask)
    accumulator.add(second, mask)

    metrics = accumulator.to_manifest(
        frame_count=2,
        grid_id="main",
        velocity_field="U",
    )
    expected_rms = np.sqrt(np.mean(np.concatenate((first, second), axis=0) ** 2,
                                   axis=(0, 1, 2)))
    assert metrics["velocityComponentRms"] == pytest.approx(expected_rms)
    # du_z/dz is 1 in frame one and 2 in frame two; RMS over both is sqrt(2.5).
    assert metrics["spanwiseGradientRms"] == pytest.approx(np.sqrt(2.5))
    assert metrics["activeDimensions"] == [3, 2, 4]
    assert metrics["effectiveSpatialDimensions"] == 3
    assert metrics["activeCellCount"] == 24
    assert metrics["temporalFrameCount"] == 2


def test_quality_spanwise_uses_one_sided_edges_and_centered_interior():
    z = np.arange(4, dtype=np.float64)
    velocity = np.zeros((1, 1, 4, 3), dtype=np.float64)
    velocity[0, 0, :, 0] = z * z
    accumulator = QualityAccumulator((1, 1, 4), (1.0, 1.0, 1.0))

    accumulator.add(velocity, np.ones((1, 1, 4), dtype=bool))
    metrics = accumulator.to_manifest(
        frame_count=1,
        grid_id="main",
        velocity_field="U",
    )

    # d(z^2)/dz is sampled as [1, 2, 4, 5] by edge one-sided and interior
    # centered stencils, so RMS = sqrt((1 + 4 + 16 + 25) / 4).
    assert metrics["spanwiseGradientRms"] == pytest.approx(np.sqrt(11.5))


def test_quality_spanwise_stencil_requires_current_cell_and_neighbors():
    velocity = np.zeros((1, 1, 3, 3), dtype=np.float64)
    velocity[0, 0, :, 0] = [0.0, np.nan, 4.0]
    accumulator = QualityAccumulator((1, 1, 3), (1.0, 1.0, 1.0))

    accumulator.add(velocity, np.ones((1, 1, 3), dtype=bool))

    with pytest.raises(ConversionError, match="no valid spanwise stencil"):
        accumulator.to_manifest(
            frame_count=1,
            grid_id="main",
            velocity_field="U",
        )


def test_quality_metrics_expose_a_spanwise_degenerate_extrusion():
    x, y, z = np.indices((4, 3, 2), dtype=np.float64)
    velocity = np.stack((x + y, y, np.zeros_like(z)), axis=-1)
    mask = np.ones((4, 3, 2), dtype=bool)
    accumulator = QualityAccumulator((4, 3, 2), (1.0, 1.0, 1.0))

    accumulator.add(velocity, mask)
    metrics = accumulator.to_manifest(
        frame_count=1,
        grid_id="main",
        velocity_field="U",
    )

    assert metrics["effectiveSpatialDimensions"] == 3
    assert metrics["velocityComponentRms"][2] == 0.0
    assert metrics["spanwiseGradientRms"] == 0.0


def test_quality_metrics_exclude_masked_outlier_cells():
    velocity = np.ones((3, 2, 2, 3), dtype=np.float64)
    velocity[2, 1, 1] = 1.0e20
    mask = np.ones((3, 2, 2), dtype=bool)
    mask[2, 1, 1] = False
    accumulator = QualityAccumulator((3, 2, 2), (1.0, 1.0, 1.0))

    accumulator.add(velocity, mask)
    metrics = accumulator.to_manifest(
        frame_count=1,
        grid_id="main",
        velocity_field="U",
    )

    assert metrics["velocityComponentRms"] == pytest.approx([1.0, 1.0, 1.0])
    assert metrics["activeCellCount"] == 11


def test_quality_bounds_do_not_enumerate_every_active_coordinate(monkeypatch):
    velocity = np.ones((4, 3, 2, 3), dtype=np.float64)
    mask = np.zeros((4, 3, 2), dtype=bool)
    mask[1:4, 1:3, :] = True
    accumulator = QualityAccumulator((4, 3, 2), (1.0, 1.0, 1.0))
    accumulator.add(velocity, mask)

    def reject_argwhere(*_args, **_kwargs):
        raise AssertionError("active bounds must not enumerate an N-by-3 coordinate array")

    monkeypatch.setattr(np, "argwhere", reject_argwhere)
    metrics = accumulator.to_manifest(
        frame_count=1,
        grid_id="main",
        velocity_field="U",
    )

    assert metrics["activeDimensions"] == [3, 2, 2]
    assert metrics["activeCellCount"] == 12


def test_quality_metrics_require_at_least_one_valid_velocity_cell():
    accumulator = QualityAccumulator((2, 2, 2), (1.0, 1.0, 1.0))
    velocity = np.full((2, 2, 2, 3), np.nan)

    accumulator.add(velocity, np.zeros((2, 2, 2), dtype=bool))

    with pytest.raises(ConversionError, match="no valid velocity cells"):
        accumulator.to_manifest(
            frame_count=1,
            grid_id="main",
            velocity_field="U",
        )


def test_quality_point_association_averages_eight_corners_to_cell_centres():
    x, y, z = np.indices((3, 2, 3), dtype=np.float64)
    x *= 2.0
    y *= 3.0
    z *= 5.0
    velocity = np.stack(
        (
            x + 2.0 * y + 3.0 * z,
            -4.0 * x + 5.0 * y - 6.0 * z,
            7.0 * x - 8.0 * y + 9.0 * z,
        ),
        axis=-1,
    )
    accumulator = QualityAccumulator(
        (2, 1, 2),
        (2.0, 3.0, 5.0),
        association="point",
    )

    accumulator.add(velocity, np.ones((3, 2, 3), dtype=bool))
    metrics = accumulator.to_manifest(
        frame_count=1,
        grid_id="main",
        velocity_field="U",
    )

    assert metrics["velocityComponentRms"] == pytest.approx(
        [21.383404780343096, 34.2235299172952, 52.576135270672]
    )
    assert metrics["spanwiseGradientRms"] == pytest.approx(np.sqrt(126.0))
    assert metrics["activeCellCount"] == 4
    assert metrics["activeDimensions"] == [2, 1, 2]
    assert metrics["effectiveSpatialDimensions"] == 2


@pytest.mark.parametrize("invalid_kind", ["mask", "nan"])
def test_quality_point_association_requires_all_eight_corners_valid(
    invalid_kind: str,
):
    velocity = np.ones((3, 3, 3, 3), dtype=np.float64)
    mask = np.ones((3, 3, 3), dtype=bool)
    if invalid_kind == "mask":
        mask[0, 0, 0] = False
    else:
        velocity[0, 0, 0, 1] = np.nan
    accumulator = QualityAccumulator(
        (2, 2, 2),
        (1.0, 1.0, 1.0),
        association="point",
    )

    accumulator.add(velocity, mask)
    metrics = accumulator.to_manifest(
        frame_count=1,
        grid_id="main",
        velocity_field="U",
    )

    assert metrics["activeCellCount"] == 7
    assert metrics["velocityComponentRms"] == pytest.approx([1.0, 1.0, 1.0])


def test_quality_point_association_uses_point_extent_for_mask():
    accumulator = QualityAccumulator(
        (2, 2, 2),
        (1.0, 1.0, 1.0),
        association="point",
    )

    with pytest.raises(ValueError, match=r"mask.*\(3, 3, 3\).*\(2, 2, 2\)"):
        accumulator.add(
            np.ones((3, 3, 3, 3), dtype=np.float64),
            np.ones((2, 2, 2), dtype=bool),
        )


def test_quality_point_association_rejects_cell_sized_velocity():
    accumulator = QualityAccumulator(
        (2, 2, 2),
        (1.0, 1.0, 1.0),
        association="point",
    )

    with pytest.raises(
        ValueError,
        match=r"velocity.*\(3, 3, 3, 3\).*\(2, 2, 2, 3\)",
    ):
        accumulator.add(
            np.ones((2, 2, 2, 3), dtype=np.float64),
            np.ones((3, 3, 3), dtype=bool),
        )


def test_quality_point_association_rejects_infinity_before_averaging():
    velocity = np.ones((2, 2, 2, 3), dtype=np.float64)
    velocity[0, 0, 0, 0] = np.inf
    accumulator = QualityAccumulator(
        (1, 1, 1),
        (1.0, 1.0, 1.0),
        association="point",
    )

    with pytest.raises(ConversionError, match="infinity"):
        accumulator.add(velocity, np.ones((2, 2, 2), dtype=bool))


def test_quality_point_average_widens_before_summing():
    velocity = np.full((2, 2, 2, 3), np.float32(1.0e38), dtype=np.float32)
    accumulator = QualityAccumulator(
        (1, 1, 1),
        (1.0, 1.0, 1.0),
        association="point",
    )

    accumulator.add(velocity, np.ones((2, 2, 2), dtype=bool))
    metrics = accumulator.to_manifest(
        frame_count=1,
        grid_id="main",
        velocity_field="U",
    )

    assert np.isfinite(metrics["velocityComponentRms"]).all()
    assert metrics["velocityComponentRms"] == pytest.approx([1.0e38] * 3)


def test_quality_rejects_unknown_association():
    with pytest.raises(ValueError, match="association"):
        QualityAccumulator(
            (2, 2, 2),
            (1.0, 1.0, 1.0),
            association="vertex",
        )


@pytest.mark.parametrize(
    "dimensions",
    [
        (True, 2, 2),
        (2.5, 2, 2),
        (0, 2, 2),
        (-1, 2, 2),
    ],
)
def test_quality_rejects_non_integer_or_non_positive_dimensions(dimensions):
    with pytest.raises(ValueError, match="dimensions"):
        QualityAccumulator(dimensions, (1.0, 1.0, 1.0))


def test_quality_accepts_numpy_integer_dimensions():
    accumulator = QualityAccumulator(
        (np.int64(2), np.int32(3), np.uint16(4)),
        (1.0, 1.0, 1.0),
        association="cell",
    )

    assert accumulator.dimensions == (2, 3, 4)


def test_existing_nonempty_output_is_refused_without_force(tmp_path: Path):
    output = tmp_path / "case.cfdviz"
    output.mkdir()
    (output / "stale.txt").write_text("old", encoding="utf-8")

    with pytest.raises(ConversionError, match=r"case\.cfdviz.*not empty.*force"):
        with staged_output(output, force=False):
            pass

    assert (output / "stale.txt").read_text(encoding="utf-8") == "old"


def test_force_replaces_the_output_instead_of_leaving_stale_frames(tmp_path: Path):
    output = tmp_path / "case.cfdviz"
    stale = output / "frames" / "000099" / "U.cvf"
    stale.parent.mkdir(parents=True)
    stale.write_bytes(b"old")

    with staged_output(output, force=True) as staging:
        (staging / "manifest.json").write_text("new", encoding="utf-8")

    assert not stale.exists()
    assert (output / "manifest.json").read_text(encoding="utf-8") == "new"


def test_staged_output_publishes_a_complete_tree_atomically(tmp_path: Path):
    output = tmp_path / "case.cfdviz"

    with staged_output(output, force=False) as staging:
        assert staging != output
        (staging / "manifest.json").write_text("complete", encoding="utf-8")
        assert not output.exists()

    assert (output / "manifest.json").read_text(encoding="utf-8") == "complete"


def test_failed_staged_output_preserves_the_previous_tree(tmp_path: Path):
    output = tmp_path / "case.cfdviz"
    output.mkdir()
    old = output / "manifest.json"
    old.write_text("old", encoding="utf-8")

    with pytest.raises(RuntimeError, match="conversion failed"):
        with staged_output(output, force=True) as staging:
            (staging / "manifest.json").write_text("partial", encoding="utf-8")
            raise RuntimeError("conversion failed")

    assert old.read_text(encoding="utf-8") == "old"
    assert not list(tmp_path.glob(".case.cfdviz.*.partial"))


def test_backup_cleanup_failure_does_not_report_a_failed_publish(
    monkeypatch,
    tmp_path: Path,
):
    output = tmp_path / "case.cfdviz"
    output.mkdir()
    (output / "manifest.json").write_text("old", encoding="utf-8")
    original_rmtree = shutil.rmtree

    def fail_backup_cleanup(path: Path):
        if Path(path).name.endswith(".backup"):
            raise OSError("backup cleanup failed")
        return original_rmtree(path)

    monkeypatch.setattr("cfdviz.convert.shutil.rmtree", fail_backup_cleanup)
    with staged_output(output, force=True) as staging:
        (staging / "manifest.json").write_text("new", encoding="utf-8")

    assert (output / "manifest.json").read_text(encoding="utf-8") == "new"
    assert list(tmp_path.glob(".case.cfdviz.*.backup"))


def test_publish_rename_failure_restores_the_previous_tree(
    monkeypatch,
    tmp_path: Path,
):
    output = tmp_path / "case.cfdviz"
    output.mkdir()
    old = output / "manifest.json"
    old.write_text("old", encoding="utf-8")
    original_replace = Path.replace

    def fail_staging_publish(path: Path, target: Path):
        if path.name.endswith(".partial") and Path(target) == output:
            raise OSError("publish rename failed")
        return original_replace(path, target)

    monkeypatch.setattr(Path, "replace", fail_staging_publish)
    with pytest.raises(OSError, match="publish rename failed"):
        with staged_output(output, force=True) as staging:
            (staging / "manifest.json").write_text("new", encoding="utf-8")

    assert old.read_text(encoding="utf-8") == "old"
    assert not list(tmp_path.glob(".case.cfdviz.*.backup"))


def test_symlinked_output_parent_is_never_followed(tmp_path: Path):
    real_parent = tmp_path / "elsewhere"
    output = real_parent / "case.cfdviz"
    output.mkdir(parents=True)
    old = output / "manifest.json"
    old.write_text("old", encoding="utf-8")
    linked_parent = tmp_path / "linked-parent"
    linked_parent.symlink_to(real_parent, target_is_directory=True)

    with pytest.raises(ConversionError, match="symlink"):
        with staged_output(linked_parent / "case.cfdviz", force=True):
            pass

    assert old.read_text(encoding="utf-8") == "old"


@pytest.mark.parametrize("identifier", ["../U", "frames/U", r"C:\\U", "", "."])
def test_solver_names_cannot_become_unsafe_field_paths(identifier: str):
    with pytest.raises(ConversionError, match="safe identifier"):
        require_safe_identifier(identifier, label="field id")


def test_safe_field_identifier_is_returned_unchanged():
    assert require_safe_identifier("alphaWater_1", label="field id") == "alphaWater_1"
