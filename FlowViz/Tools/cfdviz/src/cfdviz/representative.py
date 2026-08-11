"""Falsifiable qualification gates for representative external-solver cases.

CFDViz validation proves that a case is internally consistent.  It does not prove
that the data came from a solver, occupies a genuinely three-dimensional domain,
or changes over time.  This module checks those product-level claims directly
against provenance and decoded stored values.
"""

from __future__ import annotations

import hashlib
import json
import math
from dataclasses import dataclass, field
from numbers import Integral
from pathlib import Path
from typing import Any, Final, Sequence

import numpy as np

from .case import validate_case, verify_known_values
from .cvf import read_cvf
from .manifest import field_by_id, frame_path, grid_by_id, load_manifest

__all__ = [
    "RepresentativeCaseRequirements",
    "RepresentativeCaseReport",
    "qualify_representative_case",
]

_TEMPORAL_CHUNK_VALUES: Final = 1 << 20


def _positive_triple(
    values: Sequence[int],
    *,
    label: str,
) -> tuple[int, int, int]:
    raw = tuple(values)
    if len(raw) != 3 or any(
        not isinstance(value, Integral)
        or isinstance(value, (bool, np.bool_))
        or int(value) < 1
        for value in raw
    ):
        raise ValueError(f"{label} must contain three positive integers; got {raw}")
    return (int(raw[0]), int(raw[1]), int(raw[2]))


def _finite_nonnegative(value: float, *, label: str) -> float:
    number = float(value)
    if not math.isfinite(number) or number < 0:
        raise ValueError(f"{label} must be finite and non-negative; got {value!r}")
    return number


@dataclass(frozen=True)
class RepresentativeCaseRequirements:
    """Thresholds that a performance/demo case must satisfy.

    Ratios are dimensionless so the same gate can qualify lattice-unit and SI
    cases.  The spanwise-gradient ratio is ``RMS(dU/dz) * dz / RMS(U)``.  The
    temporal ratio is adjacent-frame ``RMS(delta U) / RMS(U)`` over values finite
    in both frames.
    """

    solver_name: str
    solver_revision: str
    minimum_active_dimensions: tuple[int, int, int] = (128, 64, 64)
    minimum_frames: int = 20
    minimum_spanwise_velocity_ratio: float = 0.005
    minimum_spanwise_gradient_ratio: float = 0.005
    minimum_temporal_change_ratio: float = 1e-5
    maximum_feature_displacement_cells: float = 2.0
    velocity_field: str = "U"

    def __post_init__(self) -> None:
        for label in ("solver_name", "solver_revision", "velocity_field"):
            value = getattr(self, label)
            if not isinstance(value, str) or not value:
                raise ValueError(f"{label} must be a non-empty string")
        object.__setattr__(
            self,
            "minimum_active_dimensions",
            _positive_triple(
                self.minimum_active_dimensions,
                label="minimum_active_dimensions",
            ),
        )
        if (
            not isinstance(self.minimum_frames, Integral)
            or isinstance(self.minimum_frames, (bool, np.bool_))
            or int(self.minimum_frames) < 2
        ):
            raise ValueError("minimum_frames must be an integer of at least 2")
        object.__setattr__(self, "minimum_frames", int(self.minimum_frames))
        for label in (
            "minimum_spanwise_velocity_ratio",
            "minimum_spanwise_gradient_ratio",
            "minimum_temporal_change_ratio",
            "maximum_feature_displacement_cells",
        ):
            object.__setattr__(
                self,
                label,
                _finite_nonnegative(getattr(self, label), label=label),
            )
        if self.maximum_feature_displacement_cells == 0:
            raise ValueError("maximum_feature_displacement_cells must be positive")


@dataclass(slots=True)
class RepresentativeCaseReport:
    """Qualification result with the measured evidence used by every gate."""

    root: Path
    requirements: RepresentativeCaseRequirements
    errors: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    metrics: dict[str, Any] = field(default_factory=dict)

    @property
    def ok(self) -> bool:
        return not self.errors

    def error(self, message: str) -> None:
        self.errors.append(message)

    def note(self, message: str) -> None:
        self.notes.append(message)

    def to_dict(self) -> dict[str, Any]:
        return {
            "root": str(self.root),
            "ok": self.ok,
            "errors": list(self.errors),
            "notes": list(self.notes),
            "requirements": {
                "solverName": self.requirements.solver_name,
                "solverRevision": self.requirements.solver_revision,
                "minimumActiveDimensions": list(
                    self.requirements.minimum_active_dimensions
                ),
                "minimumFrames": self.requirements.minimum_frames,
                "minimumSpanwiseVelocityRatio": (
                    self.requirements.minimum_spanwise_velocity_ratio
                ),
                "minimumSpanwiseGradientRatio": (
                    self.requirements.minimum_spanwise_gradient_ratio
                ),
                "minimumTemporalChangeRatio": (
                    self.requirements.minimum_temporal_change_ratio
                ),
                "maximumFeatureDisplacementCells": (
                    self.requirements.maximum_feature_displacement_cells
                ),
            },
            "metrics": dict(self.metrics),
        }

    def render(self) -> str:
        lines = [f"representative case: {self.root}"]
        for key, value in sorted(self.metrics.items()):
            lines.append(f"  {key}: {value}")
        lines.extend(f"  {note}" for note in self.notes)
        lines.extend(f"  error: {error}" for error in self.errors)
        lines.append("FAIL" if self.errors else "OK")
        return "\n".join(lines)


def _case_digest(root: Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    total = 0
    for path in sorted(candidate for candidate in root.rglob("*") if candidate.is_file()):
        relative = path.relative_to(root).as_posix().encode("utf-8")
        size = path.stat().st_size
        total += size
        digest.update(len(relative).to_bytes(4, "little"))
        digest.update(relative)
        digest.update(size.to_bytes(8, "little"))
        with path.open("rb") as stream:
            while chunk := stream.read(1 << 20):
                digest.update(chunk)
    return total, digest.hexdigest()


def _adjacent_change_ratios(
    root: Path,
    *,
    manifest: dict[str, Any],
    field_id: str,
    frame_count: int,
    velocity_norm: float,
) -> list[float]:
    entry = field_by_id(manifest, field_id)
    pattern = entry["storage"]["pathPattern"]
    previous = read_cvf(frame_path(root, pattern, 0)).values.reshape(-1)
    ratios: list[float] = []
    for frame in range(1, frame_count):
        current = read_cvf(frame_path(root, pattern, frame)).values.reshape(-1)
        if current.shape != previous.shape:
            raise ValueError(
                f"velocity frame {frame} has {current.size} values; previous frame "
                f"has {previous.size}"
            )
        square_sum = 0.0
        count = 0
        for start in range(0, current.size, _TEMPORAL_CHUNK_VALUES):
            stop = min(start + _TEMPORAL_CHUNK_VALUES, current.size)
            left = previous[start:stop]
            right = current[start:stop]
            finite = np.isfinite(left) & np.isfinite(right)
            if not finite.any():
                continue
            difference = right[finite].astype(np.float64) - left[finite].astype(
                np.float64
            )
            square_sum += float(np.dot(difference, difference))
            count += int(difference.size)
        if count == 0:
            raise ValueError(
                f"adjacent velocity frames {frame - 1} and {frame} share no finite values"
            )
        ratios.append(math.sqrt(square_sum / count) / velocity_norm)
        previous = current
    return ratios


def qualify_representative_case(
    root: Path | str,
    requirements: RepresentativeCaseRequirements,
) -> RepresentativeCaseReport:
    """Measure and qualify one external-solver demo/performance case.

    Every failure is accumulated into the report.  The function never treats a
    valid container as sufficient evidence of representative CFD by itself.
    """

    root = Path(root)
    report = RepresentativeCaseReport(root=root, requirements=requirements)

    validation = validate_case(root)
    if not validation.ok:
        for error in validation.errors:
            report.error(f"case validation: {error}")

    try:
        manifest = load_manifest(root)
    except Exception as exc:
        report.error(f"manifest could not be loaded: {type(exc).__name__}: {exc}")
        return report

    case = manifest.get("case") or {}
    solver = case.get("solver") or {}
    provenance = manifest.get("provenance") or {}
    timeline = manifest.get("timeline") or {}
    quality = manifest.get("qualityMetrics") or {}

    if case.get("quality") != "external-solver-sample":
        report.error(
            "case.quality must be 'external-solver-sample' for a representative case"
        )
    if provenance.get("sourceType") != "external-solver":
        report.error("provenance.sourceType must be 'external-solver'")
    if solver.get("name") != requirements.solver_name:
        report.error(
            f"solver name is {solver.get('name')!r}; expected "
            f"{requirements.solver_name!r}"
        )
    revision = provenance.get("sourceRevision")
    if revision != requirements.solver_revision:
        report.error(
            f"source revision is {revision!r}; expected "
            f"{requirements.solver_revision!r}"
        )
    if not solver.get("configuration"):
        report.error("solver configuration is absent")
    if not provenance.get("sourceCase"):
        report.error("provenance.sourceCase is absent")
    source_artifacts = provenance.get("sourceArtifacts") or {}
    if source_artifacts.get("flagsSequence") is not True:
        report.error(
            "provenance does not prove that a FluidX3D flags sequence was imported"
        )

    frame_count = timeline.get("frameCount")
    report.metrics["frameCount"] = frame_count
    if not isinstance(frame_count, int) or isinstance(frame_count, bool):
        report.error("timeline.frameCount is not an integer")
        frame_count = 0
    elif frame_count < requirements.minimum_frames:
        report.error(
            f"timeline has {frame_count} frames; at least "
            f"{requirements.minimum_frames} are required"
        )

    sampling = timeline.get("sampling") or {}
    displacement = sampling.get("maxFeatureDisplacementCells")
    report.metrics["maxFeatureDisplacementCells"] = displacement
    if not isinstance(displacement, (int, float)) or isinstance(displacement, bool):
        report.error("timeline sampling does not declare max feature displacement")
    elif not math.isfinite(float(displacement)) or float(displacement) <= 0:
        report.error("timeline max feature displacement must be finite and positive")
    elif float(displacement) > requirements.maximum_feature_displacement_cells:
        report.error(
            f"max feature displacement is {displacement} cells/snapshot; limit is "
            f"{requirements.maximum_feature_displacement_cells}"
        )
    if not isinstance(sampling.get("sourceTimeStep"), (int, float)) or not isinstance(
        sampling.get("storedStepStride"), int
    ):
        report.error("timeline sampling does not declare source cadence and stride")

    active_dimensions = quality.get("activeDimensions")
    report.metrics["activeDimensions"] = active_dimensions
    if not (
        isinstance(active_dimensions, list)
        and len(active_dimensions) == 3
        and all(isinstance(value, int) and not isinstance(value, bool) for value in active_dimensions)
    ):
        report.error("qualityMetrics.activeDimensions is absent or malformed")
    else:
        for axis, actual, minimum in zip(
            "XYZ", active_dimensions, requirements.minimum_active_dimensions
        ):
            if actual < minimum:
                report.error(
                    f"active {axis} dimension is {actual}; at least {minimum} is required"
                )
    effective_dimensions = quality.get("effectiveSpatialDimensions")
    report.metrics["effectiveSpatialDimensions"] = effective_dimensions
    if effective_dimensions != 3:
        report.error(
            f"effective spatial dimensions is {effective_dimensions!r}; expected 3"
        )

    velocity_rms = quality.get("velocityComponentRms")
    spanwise_gradient_rms = quality.get("spanwiseGradientRms")
    velocity_norm = math.nan
    if (
        isinstance(velocity_rms, list)
        and len(velocity_rms) == 3
        and all(
            isinstance(value, (int, float))
            and not isinstance(value, bool)
            and math.isfinite(float(value))
            for value in velocity_rms
        )
    ):
        velocity_norm = math.sqrt(sum(float(value) ** 2 for value in velocity_rms))
    if not math.isfinite(velocity_norm) or velocity_norm <= 0:
        report.error("qualityMetrics velocity RMS has no finite nonzero magnitude")
    else:
        spanwise_ratio = abs(float(velocity_rms[2])) / velocity_norm
        report.metrics["spanwiseVelocityRatio"] = spanwise_ratio
        if spanwise_ratio < requirements.minimum_spanwise_velocity_ratio:
            report.error(
                f"spanwise velocity ratio is {spanwise_ratio:.9g}; minimum is "
                f"{requirements.minimum_spanwise_velocity_ratio:.9g}"
            )

        try:
            grid = grid_by_id(manifest, quality.get("grid"))
            spacing_z = float(grid["spacing"][2])
            gradient_ratio = float(spanwise_gradient_rms) * spacing_z / velocity_norm
        except (KeyError, TypeError, ValueError, OverflowError) as exc:
            report.error(f"spanwise gradient ratio could not be computed: {exc}")
        else:
            report.metrics["spanwiseGradientRatio"] = gradient_ratio
            if not math.isfinite(gradient_ratio):
                report.error("spanwise gradient ratio is not finite")
            elif gradient_ratio < requirements.minimum_spanwise_gradient_ratio:
                report.error(
                    f"spanwise gradient ratio is {gradient_ratio:.9g}; minimum is "
                    f"{requirements.minimum_spanwise_gradient_ratio:.9g}"
                )

    known_values_path = root / "known_values.json"
    try:
        bridge = json.loads(known_values_path.read_text(encoding="utf-8"))
        samples = bridge.get("samples") or []
        report.metrics["knownValueSamples"] = len(samples)
        if not samples:
            report.error("known_values.json declares no samples")
        problems = verify_known_values(root, bridge)
        for problem in problems:
            report.error(f"known values: {problem}")
    except Exception as exc:
        report.error(
            f"known_values.json could not be verified: {type(exc).__name__}: {exc}"
        )

    if frame_count >= 2 and math.isfinite(velocity_norm) and velocity_norm > 0:
        try:
            ratios = _adjacent_change_ratios(
                root,
                manifest=manifest,
                field_id=requirements.velocity_field,
                frame_count=frame_count,
                velocity_norm=velocity_norm,
            )
        except Exception as exc:
            report.error(
                f"adjacent velocity change could not be measured: "
                f"{type(exc).__name__}: {exc}"
            )
        else:
            minimum_change = min(ratios)
            report.metrics["minimumAdjacentVelocityChangeRatio"] = minimum_change
            report.metrics["maximumAdjacentVelocityChangeRatio"] = max(ratios)
            if minimum_change < requirements.minimum_temporal_change_ratio:
                report.error(
                    f"adjacent velocity change ratio falls to "
                    f"{minimum_change:.9g}; minimum is "
                    f"{requirements.minimum_temporal_change_ratio:.9g}"
                )

    try:
        case_bytes, sha256 = _case_digest(root)
    except OSError as exc:
        report.error(f"case size/hash could not be computed: {exc}")
    else:
        report.metrics["caseBytes"] = case_bytes
        report.metrics["sha256"] = sha256

    return report
