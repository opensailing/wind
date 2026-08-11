"""Shared, solver-independent support for external CFD converters."""

from __future__ import annotations

import json
import math
import re
import shutil
import tempfile
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Final, Iterator, Sequence

import numpy as np

__all__ = [
    "ConversionError",
    "QualityAccumulator",
    "StatisticsAccumulator",
    "dump_json",
    "require_safe_identifier",
    "staged_output",
]

_SAFE_IDENTIFIER: Final = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")
_STATISTICS_CHUNK_CELLS: Final = 1 << 20


class ConversionError(Exception):
    """External data cannot be represented honestly as a CFDViz case."""


def dump_json(payload: Any, *, indent: int = 2) -> str:
    """Serialize strict RFC 8259 JSON, refusing NaN and infinity tokens."""
    return json.dumps(
        payload,
        indent=indent,
        sort_keys=True,
        ensure_ascii=False,
        allow_nan=False,
    )


def require_safe_identifier(identifier: str, *, label: str) -> str:
    """Return a solver-provided identifier only when it is path-safe."""
    if not isinstance(identifier, str) or not _SAFE_IDENTIFIER.fullmatch(identifier):
        raise ConversionError(
            f"{label} {identifier!r} is not a safe identifier; use letters, "
            "digits, '.', '_', or '-', starting with a letter or digit"
        )
    return identifier


def _reject_symlink_components(path: Path) -> None:
    absolute = path.expanduser()
    if not absolute.is_absolute():
        absolute = Path.cwd() / absolute
    for component in (absolute, *absolute.parents):
        if component.is_symlink():
            raise ConversionError(
                f"output {path} traverses symlink {component} and will not be replaced"
            )


@contextmanager
def staged_output(output: Path | str, *, force: bool) -> Iterator[Path]:
    """Transactionally publish a complete case tree after conversion succeeds.

    Staging and backup directories are siblings of the target so every rename is
    on one filesystem. A failed publish restores the previous tree, and no output
    path component may be a symlink.
    """
    root = Path(output)
    _reject_symlink_components(root)
    if root.exists() and not root.is_dir():
        raise ConversionError(f"output {root} exists and is not a directory")
    if root.exists() and any(root.iterdir()) and not force:
        raise ConversionError(
            f"output {root} is not empty; pass force=True or --force to replace it"
        )

    root.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(
        tempfile.mkdtemp(
            prefix=f".{root.name}.",
            suffix=".partial",
            dir=root.parent,
        )
    )
    backup = Path(f"{staging}.backup")
    committed = False
    try:
        yield staging

        # Recheck at commit time: another process may have changed the path while
        # conversion was running. Without force, its data still wins.
        _reject_symlink_components(root)
        if root.exists():
            if not root.is_dir():
                raise ConversionError(f"output {root} exists and is not a directory")
            if any(root.iterdir()) and not force:
                raise ConversionError(
                    f"output {root} became non-empty during conversion; pass "
                    "force=True or --force to replace it"
                )
            root.replace(backup)
        try:
            staging.replace(root)
        except BaseException:
            if backup.exists() and not root.exists():
                backup.replace(root)
            raise
        committed = True
        if backup.exists():
            try:
                shutil.rmtree(backup)
            except OSError:
                # The new tree is already live. Reporting conversion failure now
                # would lie about the committed output and invite a destructive retry.
                pass
    finally:
        if not committed and staging.exists():
            shutil.rmtree(staging)
        # Never delete an unrestored backup: it may be the only surviving copy of
        # the user's previous valid case after an exceptional filesystem failure.


class StatisticsAccumulator:
    """Accumulate manifest statistics over exact stored values."""

    def __init__(self, components: int) -> None:
        if not isinstance(components, int) or isinstance(components, bool) or components < 1:
            raise ValueError("components must be a positive integer")
        self.minimum = np.full(components, np.inf, dtype=np.float64)
        self.maximum = np.full(components, -np.inf, dtype=np.float64)
        self.magnitude_min = math.inf
        self.magnitude_max = -math.inf
        self.components = components

    def add(self, values: np.ndarray, mask: np.ndarray | None) -> None:
        raw = np.asarray(values)
        if raw.size % self.components != 0:
            raise ValueError(
                f"values contain {raw.size} scalars, which is not divisible by "
                f"{self.components} components"
            )
        flat = raw.reshape(-1, self.components)
        mask_values = None if mask is None else np.asarray(mask, dtype=bool).reshape(-1)
        if mask_values is not None and mask_values.size != flat.shape[0]:
            raise ValueError(
                f"mask has {mask_values.size} cells; values have {flat.shape[0]}"
            )

        for start in range(0, flat.shape[0], _STATISTICS_CHUNK_CELLS):
            stop = min(start + _STATISTICS_CHUNK_CELLS, flat.shape[0])
            with np.errstate(invalid="ignore"):
                data = flat[start:stop].astype(np.float64)
            active = None if mask_values is None else mask_values[start:stop]
            self._add_chunk(data, active)

    def _add_chunk(self, data: np.ndarray, mask: np.ndarray | None) -> None:
        valid = ~np.isnan(data)
        if mask is not None:
            valid &= mask.reshape(-1, 1)
        if not valid.any():
            return
        self.minimum = np.minimum(
            self.minimum,
            np.where(valid, data, np.inf).min(axis=0),
        )
        self.maximum = np.maximum(
            self.maximum,
            np.where(valid, data, -np.inf).max(axis=0),
        )
        complete_rows = valid.all(axis=1)
        if complete_rows.any():
            magnitudes = np.sqrt((data[complete_rows] ** 2).sum(axis=1))
            self.magnitude_min = min(
                self.magnitude_min,
                float(magnitudes.min()),
            )
            self.magnitude_max = max(
                self.magnitude_max,
                float(magnitudes.max()),
            )

    def to_manifest(self) -> dict[str, Any] | None:
        """Return a finite statistics object, or ``None`` when none exists."""
        if not np.all(np.isfinite(self.minimum)) or not np.all(
            np.isfinite(self.maximum)
        ):
            return None
        statistics: dict[str, Any] = {
            "globalComponentMin": [float(value) for value in self.minimum],
            "globalComponentMax": [float(value) for value in self.maximum],
        }
        if math.isfinite(self.magnitude_min) and math.isfinite(self.magnitude_max):
            statistics["globalMagnitudeMin"] = self.magnitude_min
            statistics["globalMagnitudeMax"] = self.magnitude_max
        return statistics


def _triple(values: Sequence[float], *, label: str) -> tuple[float, float, float]:
    if len(values) != 3:
        raise ValueError(f"{label} must contain exactly three values")
    result = tuple(float(value) for value in values)
    if any(not math.isfinite(value) or value <= 0 for value in result):
        raise ValueError(f"{label} must contain finite positive values; got {result}")
    return (result[0], result[1], result[2])


class QualityAccumulator:
    """Stream machine-recomputable representative-data evidence over frames."""

    def __init__(
        self,
        dimensions: Sequence[int],
        spacing: Sequence[float],
    ) -> None:
        if len(dimensions) != 3:
            raise ValueError("dimensions must contain exactly three values")
        parsed_dimensions = tuple(int(value) for value in dimensions)
        if any(value < 1 for value in parsed_dimensions):
            raise ValueError(f"dimensions must be positive; got {parsed_dimensions}")
        self.dimensions = (
            parsed_dimensions[0],
            parsed_dimensions[1],
            parsed_dimensions[2],
        )
        self.spacing = _triple(spacing, label="spacing")
        self.active_union = np.zeros(self.dimensions, dtype=bool)
        self.velocity_square_sum = np.zeros(3, dtype=np.float64)
        self.velocity_count = 0
        self.spanwise_square_sum = 0.0
        self.spanwise_count = 0
        self.frames = 0

    def add(self, velocity: np.ndarray, mask: np.ndarray) -> None:
        values = np.asarray(velocity)
        expected_shape = self.dimensions + (3,)
        if values.shape != expected_shape:
            raise ValueError(
                f"velocity must have shape {expected_shape}; got {values.shape}"
            )
        mask_values = np.asarray(mask, dtype=bool)
        if mask_values.shape != self.dimensions:
            raise ValueError(
                f"mask must have shape {self.dimensions}; got {mask_values.shape}"
            )

        def load_slice(index: int) -> tuple[np.ndarray, np.ndarray]:
            with np.errstate(invalid="ignore"):
                data = values[:, :, index, :].astype(np.float64)
            active = mask_values[:, :, index]
            if np.isinf(data[active]).any():
                raise ConversionError("active velocity cells contain infinity")
            return data, active & np.isfinite(data).all(axis=-1)

        nz = self.dimensions[2]
        previous_data: np.ndarray | None = None
        previous_valid: np.ndarray | None = None
        current_data, current_valid = load_slice(0)
        following = load_slice(1) if nz > 1 else None

        for z_index in range(nz):
            self.active_union[:, :, z_index] |= current_valid
            if current_valid.any():
                selected = current_data[current_valid]
                self.velocity_square_sum += (selected * selected).sum(axis=0)
                self.velocity_count += int(selected.shape[0])

            if nz == 1:
                derivative = np.zeros_like(current_data)
                derivative_valid = current_valid
            elif z_index == 0:
                assert following is not None
                following_data, following_valid = following
                derivative = (following_data - current_data) / self.spacing[2]
                derivative_valid = current_valid & following_valid
            elif z_index == nz - 1:
                assert previous_data is not None
                assert previous_valid is not None
                derivative = (current_data - previous_data) / self.spacing[2]
                derivative_valid = current_valid & previous_valid
            else:
                assert previous_data is not None
                assert previous_valid is not None
                assert following is not None
                following_data, following_valid = following
                derivative = (following_data - previous_data) / (
                    2.0 * self.spacing[2]
                )
                derivative_valid = (
                    current_valid & previous_valid & following_valid
                )

            if derivative_valid.any():
                selected_derivatives = derivative[derivative_valid]
                self.spanwise_square_sum += float(
                    (selected_derivatives * selected_derivatives).sum()
                )
                self.spanwise_count += int(selected_derivatives.shape[0])

            previous_data, previous_valid = current_data, current_valid
            if following is not None:
                current_data, current_valid = following
            following = (
                load_slice(z_index + 2)
                if z_index + 2 < nz
                else None
            )
        self.frames += 1

    def to_manifest(
        self,
        *,
        frame_count: int,
        grid_id: str,
        velocity_field: str,
    ) -> dict[str, Any]:
        if self.frames != frame_count:
            raise ConversionError(
                f"quality evidence saw {self.frames} frames but timeline has "
                f"{frame_count}"
            )
        if self.velocity_count == 0 or not self.active_union.any():
            raise ConversionError("quality evidence has no valid velocity cells")
        if self.spanwise_count == 0:
            raise ConversionError("quality evidence has no valid spanwise stencil")

        active_axes = (
            np.flatnonzero(self.active_union.any(axis=(1, 2))),
            np.flatnonzero(self.active_union.any(axis=(0, 2))),
            np.flatnonzero(self.active_union.any(axis=(0, 1))),
        )
        active_dimensions = np.array(
            [indices[-1] - indices[0] + 1 for indices in active_axes],
            dtype=int,
        )
        velocity_rms = np.sqrt(
            self.velocity_square_sum / float(self.velocity_count)
        )
        spanwise_rms = math.sqrt(
            self.spanwise_square_sum / float(self.spanwise_count)
        )
        return {
            "grid": grid_id,
            "activeCellCount": int(self.active_union.sum()),
            "activeDimensions": [int(value) for value in active_dimensions],
            "effectiveSpatialDimensions": int((active_dimensions > 1).sum()),
            "velocityField": velocity_field,
            "velocityComponentRms": [float(value) for value in velocity_rms],
            "spanwiseGradientRms": float(spanwise_rms),
            "temporalFrameCount": frame_count,
        }
