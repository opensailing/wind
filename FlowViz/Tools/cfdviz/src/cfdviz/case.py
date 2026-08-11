"""Case-level validation and the cross-language verification bridge.

See ``Docs/CFDVIZ_FORMAT.md`` sections 9 and 10.

Section 10 fixes what this module owes a user: a report covering manifest schema
conformance, every section 3.1 invariant, per-frame presence of every declared
field file, header and per-brick CRC results, ``uncompressedBytes`` consistency,
declared versus recomputed statistics *with the discrepancy shown*, timeline
monotonicity, mask coverage, and NaN/masked counts. And, crucially: a malformed
case "MUST produce a specific, actionable error message identifying the
offending file and byte offset. It MUST NOT crash, and MUST NOT report success."

That last sentence is why :func:`validate_case` catches broadly and converts
every failure into a report entry. A traceback is not a validation report, and
an exception that escapes to the CLI would take the *rest* of the case's
diagnostics with it — exactly when the user most needs them.

Section 9's ``known_values.json`` is the other half: a machine-checkable bridge
that the Python generator writes and the Unreal automation test reads. Neither
side may regenerate the other's expectations, and comparison is on the exact
IEEE 754 ``bits`` string, never on the human-readable ``value``.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Final, Sequence

import numpy as np

from .codecs import CodecError, UnsupportedCodecError, codec_id_from_name
from .convert import ConversionError, QualityAccumulator
from .crc32c import CHECK_VALUE, crc32c
from .cva import CVAError, read_cva
from .cvf import CVFError, CVFFormatError, CVFReader, read_cvf
from .cvm import CVMError, read_cvm
from .manifest import (
    ManifestError,
    field_by_id,
    frame_path,
    grid_by_id,
    is_safe_relative_path,
    load_manifest,
    schema_validation_available,
    validate_manifest,
)

__all__ = [
    "KNOWN_VALUES_NAME",
    "FieldStatistics",
    "ValidationReport",
    "validate_case",
    "build_known_values",
    "write_known_values",
    "verify_known_values",
]

KNOWN_VALUES_NAME: Final = "known_values.json"

#: Relative tolerance when comparing declared statistics against recomputed
#: ones. Statistics travel through JSON as decimal text, so an exact comparison
#: would fail on round-tripping alone; this is loose enough to survive that and
#: far too tight to hide a real disagreement.
_STATISTICS_TOLERANCE: Final = 1e-6

#: How many non-finite entities a single array may contribute to the bridge.
#: Every NaN is worth pinning, but an array that is mostly NaN would otherwise
#: expand into a bridge file larger than the data it describes.
_NONFINITE_SAMPLE_LIMIT: Final = 4


@dataclass(slots=True)
class FieldStatistics:
    """Recomputed per-field statistics across every stored frame.

    ``NaN`` values and cells rejected by the grid's mask field are excluded
    (spec 1.7 and 4.4.7), and they are *counted*, because spec 10 requires the
    report to state how many of each there were.

    Attributes:
        minimum: ``(C,)`` per-component minimum. ``+inf`` when nothing is valid.
        maximum: ``(C,)`` per-component maximum. ``-inf`` when nothing is valid.
        valid_count: Samples that were neither NaN nor masked.
        nan_count: Samples excluded because they were NaN.
        masked_count: Samples excluded by the mask field.
        magnitude_min: Minimum vector magnitude over valid samples.
        magnitude_max: Maximum vector magnitude over valid samples.
    """

    minimum: np.ndarray
    maximum: np.ndarray
    valid_count: int = 0
    nan_count: int = 0
    masked_count: int = 0
    magnitude_min: float = math.inf
    magnitude_max: float = -math.inf

    @property
    def component_count(self) -> int:
        """Components these statistics describe."""
        return int(self.minimum.shape[0])


@dataclass(slots=True)
class ValidationReport:
    """The outcome of validating one case.

    Attributes:
        root: The case root that was validated.
        errors: Problems that make the case invalid. Non-empty means failure.
        warnings: Observations that do not invalidate the case, such as schema
            validation having been skipped because ``jsonschema`` is absent.
        notes: Informational lines included in the rendered report.
        statistics: Recomputed statistics per field id.
        checked_files: Field files opened and decoded.
        checked_bricks: Brick directory entries verified.
    """

    root: Path
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    statistics: dict[str, FieldStatistics] = field(default_factory=dict)
    checked_files: int = 0
    checked_bricks: int = 0

    @property
    def ok(self) -> bool:
        """Whether the case is valid. Any error at all means it is not."""
        return not self.errors

    def __bool__(self) -> bool:
        return self.ok

    def error(self, message: str) -> None:
        """Record a problem that invalidates the case."""
        self.errors.append(message)

    def warn(self, message: str) -> None:
        """Record an observation that does not invalidate the case."""
        self.warnings.append(message)

    def note(self, message: str) -> None:
        """Record an informational line."""
        self.notes.append(message)

    def to_dict(self) -> dict[str, Any]:
        """A JSON-serializable summary, for ``--json`` output."""
        return {
            "root": str(self.root),
            "ok": self.ok,
            "errors": list(self.errors),
            "warnings": list(self.warnings),
            "notes": list(self.notes),
            "checkedFiles": self.checked_files,
            "checkedBricks": self.checked_bricks,
            "fields": {
                name: {
                    "componentMin": [
                        None if math.isinf(v) else float(v) for v in stats.minimum
                    ],
                    "componentMax": [
                        None if math.isinf(v) else float(v) for v in stats.maximum
                    ],
                    "validCount": stats.valid_count,
                    "nanCount": stats.nan_count,
                    "maskedCount": stats.masked_count,
                }
                for name, stats in self.statistics.items()
            },
        }

    def render(self) -> str:
        """A human-readable report.

        The verdict word is ``OK`` or ``FAIL`` and appears exactly once, so a
        caller — or a script grepping the output — cannot mistake a listed error
        for a passing run.
        """
        lines = [f"case: {self.root}"]
        lines.extend(f"  {note}" for note in self.notes)
        for name, stats in sorted(self.statistics.items()):
            extent = ", ".join(
                "n/a" if math.isinf(lo) else f"[{lo:g}, {hi:g}]"
                for lo, hi in zip(stats.minimum, stats.maximum)
            )
            lines.append(
                f"  field {name}: range {extent}; valid {stats.valid_count}, "
                f"NaN {stats.nan_count}, masked {stats.masked_count}"
            )
        lines.append(
            f"  checked {self.checked_files} field files, "
            f"{self.checked_bricks} bricks"
        )
        for warning in self.warnings:
            lines.append(f"  warning: {warning}")
        for error in self.errors:
            lines.append(f"  error: {error}")
        lines.append("FAIL" if self.errors else "OK")
        return "\n".join(lines)


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

def _finite_float(value: Any) -> float | None:
    """Coerce to a finite float, or ``None`` if that is not possible."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    number = float(value)
    return None if math.isnan(number) or math.isinf(number) else number


def _close_enough(declared: float, recomputed: float) -> bool:
    """Whether a declared statistic matches a recomputed one after JSON text."""
    if math.isinf(recomputed):
        return False
    scale = max(1.0, abs(declared), abs(recomputed))
    return abs(declared - recomputed) <= _STATISTICS_TOLERANCE * scale


def _accumulate(stats: FieldStatistics, values: np.ndarray,
                mask: np.ndarray | None) -> None:
    """Fold one frame's values into ``stats``, excluding NaN and masked cells."""
    components = values.shape[3]
    flat = values.reshape(-1, components)
    with np.errstate(invalid="ignore"):
        finite = ~np.isnan(flat.astype(np.float64))

    if mask is None:
        keep = np.ones(flat.shape[0], dtype=bool)
    else:
        keep = mask.reshape(-1)
        stats.masked_count += int(np.count_nonzero(~keep))

    valid = finite & keep[:, None]
    # NaN inside a masked cell is counted as masked, not as NaN: it was excluded
    # for the mask's reason, and double-counting it would make the two totals
    # add up to more than the cells that exist.
    stats.nan_count += int(np.count_nonzero(~finite & keep[:, None]))
    stats.valid_count += int(np.count_nonzero(valid[:, 0])) if components else 0

    if not np.any(valid):
        return
    with np.errstate(invalid="ignore"):
        data = flat.astype(np.float64)
    minimum = np.where(valid, data, np.inf).min(axis=0)
    maximum = np.where(valid, data, -np.inf).max(axis=0)
    stats.minimum = np.minimum(stats.minimum, minimum)
    stats.maximum = np.maximum(stats.maximum, maximum)

    rows = np.all(valid, axis=1)
    if bool(np.any(rows)):
        magnitude = np.sqrt((data[rows] ** 2).sum(axis=1))
        stats.magnitude_min = min(stats.magnitude_min, float(magnitude.min()))
        stats.magnitude_max = max(stats.magnitude_max, float(magnitude.max()))


def _describe(exc: Exception, relative: str) -> str:
    """Render an exception against the case-relative path the manifest uses.

    A :class:`CVFFormatError` already carries an absolute path in its message.
    Prefixing that with the relative path would print the file twice, so its
    ``detail`` and ``offset`` are re-rendered instead — the offset is kept,
    since spec 10 requires the report to name the file *and* the byte offset.
    """
    if isinstance(exc, CVFFormatError):
        where = f" byte offset {exc.offset}:" if exc.offset is not None else ""
        return f"{relative}:{where} {exc.detail}"
    return f"{relative}: {exc}"


def _validate_field_file(report: ValidationReport, path: Path, relative: str,
                         entry: dict[str, Any], grid: dict[str, Any] | None,
                         frame: int, time: float | None) -> np.ndarray | None:
    """Validate one ``.cvf`` and return its values, or ``None`` on failure.

    Every check here names ``relative`` rather than the absolute path, so the
    report reads the same on every machine and matches what the manifest says.
    """
    try:
        with CVFReader(path) as reader:
            header = reader.header
            report.checked_files += 1
            report.checked_bricks += len(reader.directory)

            if grid is not None:
                declared = grid.get("dimensions")
                if isinstance(declared, list) and len(declared) == 3:
                    actual = list(header.dimensions)
                    if actual != [int(v) for v in declared]:
                        report.error(
                            f"{relative}: dimensions {tuple(actual)} disagree with the "
                            f"manifest grid {grid.get('id')!r} dimensions "
                            f"{tuple(declared)}"
                        )
            numeric_id = entry.get("numericId")
            if isinstance(numeric_id, int) and header.field_numeric_id != numeric_id:
                report.error(
                    f"{relative}: header fieldNumericId {header.field_numeric_id} "
                    f"disagrees with the manifest numericId {numeric_id}"
                )
            if header.frame_index != frame:
                report.error(
                    f"{relative}: header frameIndex {header.frame_index} disagrees "
                    f"with the frame directory index {frame}"
                )
            if time is not None and not _close_enough(time, header.simulation_time):
                report.error(
                    f"{relative}: header simulationTime {header.simulation_time!r} "
                    f"disagrees with timeline.times[{frame}] = {time!r}"
                )
            declared_type = entry.get("dataType")
            if declared_type is not None and header.data_type_name != declared_type:
                report.error(
                    f"{relative}: stored dataType {header.data_type_name!r} disagrees "
                    f"with the manifest dataType {declared_type!r}"
                )
            declared_association = entry.get("association")
            if (
                declared_association is not None
                and header.association_name != declared_association
            ):
                report.error(
                    f"{relative}: stored association {header.association_name!r} "
                    f"disagrees with the manifest association "
                    f"{declared_association!r}"
                )
            declared_components = entry.get("componentCount")
            if (
                isinstance(declared_components, int)
                and header.component_count != declared_components
            ):
                report.error(
                    f"{relative}: stored componentCount {header.component_count} "
                    f"disagrees with the manifest componentCount "
                    f"{declared_components}"
                )
            declared_codec = (entry.get("storage") or {}).get("codec")
            if isinstance(declared_codec, str):
                try:
                    expected_codec = codec_id_from_name(declared_codec)
                except UnsupportedCodecError:
                    expected_codec = None
                if expected_codec is not None and header.codec != expected_codec:
                    # Not an error: the writer may legitimately fall back to
                    # 'none' when compression would grow a brick, and the file
                    # remains decodable either way.
                    report.warn(
                        f"{relative}: stored codec {header.codec_name!r} differs from "
                        f"the manifest codec {declared_codec!r}"
                    )
            # read_all decodes every brick, which verifies every payload CRC and
            # every uncompressedBytes product along the way.
            return reader.read_all()
    except (CVFError, CVFFormatError, CodecError, UnsupportedCodecError, OSError) as exc:
        report.error(_describe(exc, relative))
        return None


def validate_case(root: Path | str) -> ValidationReport:
    """Validate a whole case directory, per spec section 10.

    Never raises for a malformed case: every problem, including one that would
    otherwise be an exception, is recorded in the returned report. That is what
    lets the CLI report *all* of a broken case's problems instead of the first.

    Args:
        root: The case root directory.

    Returns:
        A :class:`ValidationReport`. ``report.ok`` is ``True`` only when nothing
        at all went wrong.
    """
    root = Path(root)
    report = ValidationReport(root=root)

    if not root.is_dir():
        report.error(f"{root}: case root is not a directory")
        return report
    if not schema_validation_available():
        report.warn(
            "jsonschema is not installed; manifest schema conformance was not "
            "checked (pip install cfdviz[schema]). Section 3.1 invariants were still "
            "checked."
        )

    try:
        manifest = load_manifest(root)
    except ManifestError as exc:
        report.error(str(exc))
        return report

    for problem in validate_manifest(manifest):
        report.error(problem)

    timeline = manifest.get("timeline") if isinstance(manifest.get("timeline"), dict) else {}
    times = timeline.get("times") if isinstance(timeline.get("times"), list) else []
    fields = manifest.get("fields") if isinstance(manifest.get("fields"), list) else []
    report.note(f"{len(times)} frames, {len(fields)} fields")

    entries = [entry for entry in fields if isinstance(entry, dict)]
    mask_field_ids = {
        grid.get("maskField")
        for grid in (manifest.get("grids") or [])
        if isinstance(grid, dict) and grid.get("maskField")
    }
    statistics: list[tuple[dict[str, Any], FieldStatistics]] = []
    for entry in entries:
        components = entry.get("componentCount")
        components = components if isinstance(components, int) and components > 0 else 1
        statistics.append(
            (
                entry,
                FieldStatistics(
                    minimum=np.full(components, np.inf),
                    maximum=np.full(components, -np.inf),
                ),
            )
        )

    declared_quality = manifest.get("qualityMetrics")
    quality: QualityAccumulator | None = None
    quality_grid_id: str | None = None
    quality_velocity_field: str | None = None
    quality_failed = False
    if isinstance(declared_quality, dict):
        grid_id = declared_quality.get("grid")
        velocity_field = declared_quality.get("velocityField")
        quality_grid = grid_by_id(manifest, grid_id) if isinstance(grid_id, str) else None
        velocity_entry = (
            field_by_id(manifest, velocity_field)
            if isinstance(velocity_field, str)
            else None
        )
        if (
            isinstance(quality_grid, dict)
            and isinstance(velocity_field, str)
            and isinstance(velocity_entry, dict)
        ):
            dimensions = quality_grid.get("dimensions")
            spacing = quality_grid.get("spacing")
            association = velocity_entry.get("association")
            if (
                isinstance(dimensions, list)
                and isinstance(spacing, list)
                and isinstance(association, str)
            ):
                try:
                    quality = QualityAccumulator(
                        dimensions,
                        spacing,
                        association=association,
                    )
                    quality_grid_id = grid_id
                    quality_velocity_field = velocity_field
                except (ConversionError, TypeError, ValueError) as exc:
                    report.error(f"qualityMetrics could not be recomputed: {exc}")
                    quality_failed = True

    # Process one complete frame at a time. Each grid mask is decoded once for the
    # frame, used by every associated field, and released before the next frame.
    for frame in range(len(times)):
        frame_masks: dict[str, np.ndarray] = {}
        for entry in entries:
            field_id = entry.get("id")
            if field_id not in mask_field_ids:
                continue
            values = _read_field_frame(report, root, manifest, entry, frame, times)
            if values is not None:
                frame_masks[str(field_id)] = values

        for entry, stats in statistics:
            field_id = entry.get("id")
            if field_id in mask_field_ids:
                values = frame_masks.get(str(field_id))
            else:
                values = _read_field_frame(report, root, manifest, entry, frame, times)
            if values is None:
                continue

            grid = grid_by_id(manifest, entry.get("grid")) if entry.get("grid") else None
            mask_id = grid.get("maskField") if isinstance(grid, dict) else None
            mask = None
            mask_resolved = True
            if mask_id and mask_id != field_id:
                stored = frame_masks.get(str(mask_id))
                if stored is None:
                    report.error(
                        f"field {field_id!r} frame {frame}: mask field {mask_id!r} "
                        "could not be read, so mask coverage is unknown"
                    )
                    mask_resolved = False
                elif stored.shape[:3] != values.shape[:3]:
                    report.error(
                        f"field {field_id!r} frame {frame}: mask field {mask_id!r} has "
                        f"extent {stored.shape[:3]} but the field is "
                        f"{values.shape[:3]}"
                    )
                    mask_resolved = False
                else:
                    mask = stored[..., 0] != 0
            _accumulate(stats, values, mask)

            if (
                quality is not None
                and not quality_failed
                and field_id == quality_velocity_field
                and entry.get("grid") == quality_grid_id
                and mask_resolved
            ):
                quality_mask = (
                    mask
                    if mask is not None
                    else np.ones(values.shape[:3], dtype=bool)
                )
                try:
                    quality.add(values, quality_mask)
                except (ConversionError, TypeError, ValueError) as exc:
                    report.error(f"qualityMetrics could not be recomputed: {exc}")
                    quality_failed = True

    for entry, stats in statistics:
        field_id = entry.get("id")
        if field_id:
            report.statistics[str(field_id)] = stats
            _check_declared_statistics(report, entry, stats)

    if (
        quality is not None
        and not quality_failed
        and isinstance(declared_quality, dict)
        and quality_grid_id is not None
        and quality_velocity_field is not None
    ):
        try:
            recomputed_quality = quality.to_manifest(
                frame_count=len(times),
                grid_id=quality_grid_id,
                velocity_field=quality_velocity_field,
            )
        except (ConversionError, TypeError, ValueError) as exc:
            report.error(f"qualityMetrics could not be recomputed: {exc}")
        else:
            _check_declared_quality_metrics(
                report,
                declared_quality,
                recomputed_quality,
            )

    # Only when the stored data is otherwise sound. On a case whose files are
    # already broken, every bridge sample would fail for the *same* reason and
    # bury the real diagnosis under a wall of repeats — and a bridge mismatch
    # against a file that will not even decode tells the user nothing new.
    if report.ok:
        _check_known_values(report, root)
    elif (root / KNOWN_VALUES_NAME).is_file():
        report.note(
            f"{KNOWN_VALUES_NAME} was not checked: the stored data must be valid "
            "first"
        )
    return report


def _read_field_frame(report: ValidationReport, root: Path, manifest: dict[str, Any],
                      entry: dict[str, Any], frame: int,
                      times: Sequence[Any]) -> np.ndarray | None:
    """Locate, open, and validate one field file for one frame."""
    storage = entry.get("storage")
    if not isinstance(storage, dict):
        return None
    pattern = storage.get("pathPattern")
    if not isinstance(pattern, str):
        return None
    try:
        path = frame_path(root, pattern, frame)
    except ManifestError as exc:
        report.error(str(exc))
        return None
    relative = pattern.format(frame=frame)

    if not path.is_file():
        report.error(
            f"{relative}: declared field {entry.get('id')!r} is missing for frame "
            f"{frame}"
        )
        return None

    time = _finite_float(times[frame]) if frame < len(times) else None
    grid = grid_by_id(manifest, entry.get("grid")) if entry.get("grid") else None
    return _validate_field_file(report, path, relative, entry, grid, frame, time)


def _check_declared_quality_metrics(
    report: ValidationReport,
    declared: dict[str, Any],
    recomputed: dict[str, Any],
) -> None:
    """Compare representative-data claims against stored velocity evidence."""
    for key in (
        "activeCellCount",
        "activeDimensions",
        "effectiveSpatialDimensions",
        "temporalFrameCount",
    ):
        value = declared.get(key)
        if key == "activeDimensions":
            well_formed = (
                isinstance(value, list)
                and len(value) == 3
                and all(isinstance(item, int) and not isinstance(item, bool) for item in value)
            )
        else:
            well_formed = isinstance(value, int) and not isinstance(value, bool)
        if well_formed and value != recomputed[key]:
            report.error(
                f"declared qualityMetrics.{key} is {value!r} but the stored data "
                f"gives {recomputed[key]!r}"
            )

    declared_rms = declared.get("velocityComponentRms")
    if isinstance(declared_rms, list) and len(declared_rms) == 3:
        parsed_rms = [_finite_float(value) for value in declared_rms]
        if all(value is not None for value in parsed_rms) and any(
            not _close_enough(float(value), float(stored))
            for value, stored in zip(parsed_rms, recomputed["velocityComponentRms"])
        ):
            report.error(
                "declared qualityMetrics.velocityComponentRms is "
                f"{declared_rms!r} but the stored data gives "
                f"{recomputed['velocityComponentRms']!r}"
            )

    declared_gradient = _finite_float(declared.get("spanwiseGradientRms"))
    if declared_gradient is not None and not _close_enough(
        declared_gradient,
        float(recomputed["spanwiseGradientRms"]),
    ):
        report.error(
            "declared qualityMetrics.spanwiseGradientRms is "
            f"{declared_gradient!r} but the stored data gives "
            f"{recomputed['spanwiseGradientRms']!r}"
        )


def _check_declared_statistics(report: ValidationReport, entry: dict[str, Any],
                               stats: FieldStatistics) -> None:
    """Compare manifest-declared statistics against the recomputed ones.

    Spec 10 requires the *discrepancy* to be shown, not merely flagged, so both
    numbers appear in the message.
    """
    declared = entry.get("statistics")
    if not isinstance(declared, dict):
        return
    field_id = entry.get("id")

    for key, recomputed in (
        ("globalComponentMin", stats.minimum),
        ("globalComponentMax", stats.maximum),
    ):
        values = declared.get(key)
        if not isinstance(values, list):
            continue
        if len(values) != stats.component_count:
            report.error(
                f"field {field_id!r}: statistics.{key} has {len(values)} entries but "
                f"the field has {stats.component_count} components"
            )
            continue
        for index, value in enumerate(values):
            number = _finite_float(value)
            if number is None:
                report.error(
                    f"field {field_id!r}: statistics.{key}[{index}] is {value!r}, "
                    "which is not a finite number"
                )
                continue
            if not _close_enough(number, float(recomputed[index])):
                report.error(
                    f"field {field_id!r}: declared statistics.{key}[{index}] is "
                    f"{number!r} but the stored data gives "
                    f"{float(recomputed[index])!r}"
                )

    for key, recomputed in (
        ("globalMagnitudeMin", stats.magnitude_min),
        ("globalMagnitudeMax", stats.magnitude_max),
    ):
        number = _finite_float(declared.get(key))
        if number is None:
            continue
        if not _close_enough(number, recomputed):
            report.error(
                f"field {field_id!r}: declared statistics.{key} is {number!r} but the "
                f"stored data gives {recomputed!r}"
            )


def _check_known_values(report: ValidationReport, root: Path) -> None:
    """Verify ``known_values.json`` if the case ships one (spec 9)."""
    path = root / KNOWN_VALUES_NAME
    if not path.is_file():
        return
    try:
        bridge = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        report.error(f"{KNOWN_VALUES_NAME}: {exc}")
        return
    for problem in verify_known_values(root, bridge):
        report.error(f"{KNOWN_VALUES_NAME}: {problem}")


# ---------------------------------------------------------------------------
# known_values.json — spec 9
# ---------------------------------------------------------------------------

def _bits_of(value: np.generic) -> str:
    """The exact IEEE 754 bit pattern, uppercase hex, sized to the dtype.

    Spec 9.1: 8 digits for float32, 4 for float16, 2 for uint8. Comparison
    happens on this string and never on the decimal ``value``, which makes the
    Python/Unreal check exact and immune to text-formatting differences.
    """
    raw = np.asarray(value).tobytes()
    return f"0x{int.from_bytes(raw, 'little'):0{2 * len(raw)}X}"


def _readable_value(value: np.generic) -> float | int | None:
    """The human-readable companion to ``bits``. ``None`` for non-finite values.

    JSON has no NaN or Infinity token, and emitting one would produce a file
    that strict parsers on the Unreal side reject. ``bits`` carries the truth
    regardless, so the readable field simply goes null.
    """
    number = float(value)
    if math.isnan(number) or math.isinf(number):
        return None
    if np.issubdtype(np.asarray(value).dtype, np.integer):
        return int(value)
    return number


def _sample(field_id: str, frame: int, voxel: tuple[int, int, int], component: int,
            value: np.generic, note: str | None = None) -> dict[str, Any]:
    """One entry of the ``samples`` array."""
    entry: dict[str, Any] = {
        "frame": frame,
        "field": field_id,
        "voxel": [int(voxel[0]), int(voxel[1]), int(voxel[2])],
        "component": int(component),
        "value": _readable_value(value),
        "bits": _bits_of(value),
    }
    if note:
        entry["note"] = note
    return entry


def _first_nan_index(values: np.ndarray) -> tuple[int, int, int, int] | None:
    """Find one NaN with at most one Z plane of temporary mask storage."""
    for z_index in range(values.shape[2]):
        plane = np.isnan(values[:, :, z_index, :])
        matches = np.flatnonzero(plane)
        if matches.size:
            x_index, y_index, component = np.unravel_index(
                int(matches[0]),
                plane.shape,
            )
            return int(x_index), int(y_index), z_index, int(component)
    return None


def _first_zero_index(values: np.ndarray) -> tuple[int, int, int] | None:
    """Find one zero-valued mask cell without an N-by-3 coordinate array."""
    for z_index in range(values.shape[2]):
        plane = values[:, :, z_index, 0] == 0
        matches = np.flatnonzero(plane)
        if matches.size:
            x_index, y_index = np.unravel_index(int(matches[0]), plane.shape)
            return int(x_index), int(y_index), z_index
    return None


def build_known_values(root: Path | str) -> dict[str, Any]:
    """Build the cross-language verification bridge for a case (spec 9).

    The sample set covers, at minimum, what spec 9.3 requires: a scalar field
    and a vector field, the first and last stored frame, a voxel inside the
    masked obstacle, a voxel on a partial edge brick, and a NaN voxel if the
    case contains one. Each of those carries a ``note`` naming which requirement
    it satisfies, so a reviewer can see the coverage without re-deriving it.

    Args:
        root: The case root directory.

    Returns:
        The bridge object, ready to serialize.

    Raises:
        ManifestError: If the manifest cannot be read.
    """
    root = Path(root)
    manifest = load_manifest(root)
    times = (manifest.get("timeline") or {}).get("times") or []
    frames = sorted({0, max(len(times) - 1, 0)}) if times else [0]

    samples: list[dict[str, Any]] = []
    mask_field_ids = {
        grid.get("maskField")
        for grid in (manifest.get("grids") or [])
        if isinstance(grid, dict) and grid.get("maskField")
    }

    for entry in manifest.get("fields") or []:
        if not isinstance(entry, dict):
            continue
        field_id = str(entry.get("id"))
        pattern = (entry.get("storage") or {}).get("pathPattern")
        if not isinstance(pattern, str):
            continue

        for frame in frames:
            path = frame_path(root, pattern, frame)
            if not path.is_file():
                continue
            with CVFReader(path) as reader:
                values = reader.read_all()
                extent = values.shape[:3]
                components = values.shape[3]

                # A voxel roughly in the middle: representative, and away from
                # the origin so an off-by-one in indexing shows up.
                middle = tuple(max(0, n // 2) for n in extent)
                samples.append(
                    _sample(field_id, frame, middle, 0, values[middle][0], "interior")
                )
                # EVERY remaining component, not just the last one.
                #
                # This sampled only component `components - 1`, which on a
                # 3-component field left component 1 pinned by nothing. That is
                # the exact set a swizzle typo gets wrong: `float4(v.x, v.z,
                # v.z, 1)` is correct at components 0 and 2 and wrong in the
                # middle, so it passed the whole bridge. Replacing component 1
                # of every voxel with component 2 produced no complaint at all.
                #
                # The ends still earn their own notes -- off-by-one bounds
                # errors live there and a reviewer reads for them -- but the
                # middle is a distinct failure mode, not a weaker version of
                # the same one.
                for component in range(1, components):
                    note = "last-component" if component == components - 1 else "component"
                    samples.append(
                        _sample(
                            field_id, frame, middle, component,
                            values[middle][component], note,
                        )
                    )

                # The far corner is on a partial edge brick whenever the volume
                # does not divide evenly by the brick size, which is the case
                # spec 9.3 asks for.
                corner = tuple(n - 1 for n in extent)
                brick = reader.header.brick_size
                partial = any(
                    extent[axis] % brick[axis] != 0 for axis in range(3)
                )
                samples.append(
                    _sample(
                        field_id, frame, corner, 0, values[corner][0],
                        "partial-edge-brick" if partial else "corner",
                    )
                )

                if field_id not in mask_field_ids:
                    nan_index = _first_nan_index(values)
                    if nan_index is not None:
                        x_index, y_index, z_index, component = nan_index
                        voxel = (x_index, y_index, z_index)
                        samples.append(
                            _sample(
                                field_id,
                                frame,
                                voxel,
                                component,
                                values[voxel][component],
                                "nan",
                            )
                        )

        # A voxel inside the masked obstacle, taken from the mask field itself
        # so the sample is meaningful even for a case whose fields are uniform.
        for mask_id in mask_field_ids:
            mask_entry = field_by_id(manifest, mask_id) if mask_id else None
            if mask_entry is None or mask_entry is not entry:
                continue
            mask_pattern = (mask_entry.get("storage") or {}).get("pathPattern")
            if not isinstance(mask_pattern, str):
                continue
            mask_path = frame_path(root, mask_pattern, frames[0])
            if not mask_path.is_file():
                continue
            mask_values = read_cvf(mask_path).values
            voxel = _first_zero_index(mask_values)
            if voxel is not None:
                samples.append(
                    _sample(
                        str(mask_id), frames[0], voxel, 0,
                        mask_values[voxel][0], "masked",
                    )
                )

    meshes, mesh_samples = _mesh_samples(root, manifest)
    array_samples = _array_samples(root, manifest, frames)

    return {
        "formatVersion": manifest.get("version"),
        "caseId": (manifest.get("case") or {}).get("id"),
        "crc32cCheck": f"0x{CHECK_VALUE:08X}",
        "samples": samples,
        "meshes": meshes,
        "meshSamples": mesh_samples,
        "arraySamples": array_samples,
    }


def _spread(count: int, wanted: int = 3) -> list[int]:
    """Up to ``wanted`` indices spread across ``[0, count)``, ends included.

    The ends matter more than the middle: an off-by-one in a reader's loop
    bounds shows up at index 0 or ``count - 1`` and nowhere else.
    """
    if count <= 0:
        return []
    if count <= wanted:
        return list(range(count))
    step = (count - 1) / (wanted - 1)
    return sorted({int(round(index * step)) for index in range(wanted)})


def _mesh_samples(root: Path, manifest: dict[str, Any]) -> tuple[list[dict[str, Any]],
                                                                list[dict[str, Any]]]:
    """Sample every declared mesh: positions, triangles, patchIds, nodeIds.

    ``vertexCount`` and ``triangleCount`` are recorded per mesh because they are
    the fastest way for a reader to discover it has swapped the two
    associations — ``patchIds`` is per-triangle and ``nodeIds`` per-vertex, and
    a reader that confuses them produces plausible arrays of the wrong length.
    """
    meshes: list[dict[str, Any]] = []
    samples: list[dict[str, Any]] = []

    for entry in manifest.get("meshes") or []:
        if not isinstance(entry, dict):
            continue
        relative = entry.get("path")
        if not isinstance(relative, str) or not is_safe_relative_path(relative):
            continue
        path = root / relative
        if not path.is_file():
            continue

        mesh = read_cvm(path)
        mesh_id = str(entry.get("id"))
        meshes.append({
            "id": mesh_id,
            "path": relative,
            "vertexCount": mesh.vertex_count,
            "triangleCount": mesh.triangle_count,
            "hasNormals": mesh.normals is not None,
            "hasPatchIds": mesh.patch_ids is not None,
            "hasNodeIds": mesh.node_ids is not None,
        })

        for vertex in _spread(mesh.vertex_count):
            for component in range(3):
                samples.append({
                    "kind": "position",
                    "mesh": mesh_id,
                    "vertex": vertex,
                    "component": component,
                    "componentName": "XYZ"[component],
                    "value": _readable_value(mesh.positions[vertex][component]),
                    "bits": _bits_of(mesh.positions[vertex][component]),
                })

        for triangle in _spread(mesh.triangle_count):
            # The whole triple in stored order. Corner order is data: winding is
            # CCW seen from outside (spec 5), so a reader that rotates or
            # reverses a triangle flips its facing while keeping every vertex.
            samples.append({
                "kind": "triangle",
                "mesh": mesh_id,
                "triangle": triangle,
                "indices": [int(v) for v in mesh.indices[triangle]],
            })

        if mesh.patch_ids is not None:
            for triangle in _spread(mesh.triangle_count):
                samples.append({
                    "kind": "patchId",
                    "mesh": mesh_id,
                    "triangle": triangle,
                    "value": int(mesh.patch_ids[triangle]),
                })

        if mesh.node_ids is not None:
            for vertex in _spread(mesh.vertex_count):
                samples.append({
                    "kind": "nodeId",
                    "mesh": mesh_id,
                    "vertex": vertex,
                    "value": int(mesh.node_ids[vertex]),
                })

    return meshes, samples


def _array_samples(root: Path, manifest: dict[str, Any],
                   frames: Sequence[int]) -> list[dict[str, Any]]:
    """Sample every ``.cva`` sitting beside a declared mesh.

    CFDViz 1.0's manifest has no slot for mesh-associated arrays — ``meshes[]``
    carries only geometry, and ``fields[]`` is grid storage whose
    ``mesh-vertex`` / ``mesh-element`` associations spec 3.2 reserves for a
    future version. So the files are discovered on disk rather than read out of
    the manifest, and each sample records the path it came from.

    Both the payload and the statistics are sampled. They fail differently:
    payload bits catch a transposed or mis-strided read, while ``validCount``
    catches NaN handling, which is invisible in the payload of a reader that
    coerces NaN to zero *after* counting.
    """
    samples: list[dict[str, Any]] = []
    directory = root / "meshes"
    if not directory.is_dir():
        return samples

    for path in sorted(directory.glob("*.cva")):
        relative = path.relative_to(root).as_posix()
        array = read_cva(path)
        if array.frame_index not in frames:
            continue

        names = array.component_names
        for entity in _nonfinite_rows(array.values, _spread(array.value_count)):
            for component in range(array.component_count):
                value = array.values[entity][component]
                samples.append({
                    "kind": "value",
                    "path": relative,
                    "frame": array.frame_index,
                    "entity": entity,
                    "component": component,
                    "componentName": names[component],
                    "value": _readable_value(value),
                    "bits": _bits_of(value),
                })

        statistics = array.frame_statistics
        if statistics is None:
            continue
        for component in range(array.component_count):
            samples.append({
                "kind": "statistics",
                "path": relative,
                "frame": array.frame_index,
                "component": component,
                "componentName": names[component],
                # Statistics are float64 on disk (spec 6.4) and compared by bits
                # like everything else: a mean that differs in the last place is
                # a real disagreement, not a formatting artefact.
                "minimum": _statistic(statistics.minimum[component]),
                "maximum": _statistic(statistics.maximum[component]),
                "mean": _statistic(statistics.mean[component]),
                "validCount": int(statistics.valid_count[component]),
            })
    return samples


def _nonfinite_rows(values: np.ndarray, chosen: Sequence[int]) -> list[int]:
    """``chosen`` plus every entity holding a non-finite value.

    A spread of indices can miss the NaN entirely, and then the only trace of it
    in the bridge is ``validCount`` — which says one value was excluded but not
    which one, nor which bit pattern arrived. Sampling the row directly pins
    both. Non-finite entities are rare by construction, so this stays bounded in
    practice; the cap keeps a pathological all-NaN array from inflating the
    bridge into something nobody reads.
    """
    picked = set(chosen)
    if values.size:
        rows = np.nonzero(~np.isfinite(values).all(axis=1))[0]
        picked.update(int(row) for row in rows[:_NONFINITE_SAMPLE_LIMIT])
    return sorted(picked)


def _statistic(value: np.generic) -> dict[str, Any]:
    """One float64 statistic, as both a readable number and exact bits.

    An all-NaN component stores the ``+inf`` / ``-inf`` sentinel (spec 6.4), and
    ``mean`` is NaN — none of which JSON can express, so ``value`` goes null and
    ``bits`` carries it.
    """
    number = np.float64(value)
    return {"value": _readable_value(number), "bits": _bits_of(number)}


def write_known_values(root: Path | str) -> Path:
    """Build and write ``known_values.json`` at the case root.

    Returns:
        The path written.
    """
    root = Path(root)
    path = root / KNOWN_VALUES_NAME
    bridge = build_known_values(root)
    path.write_text(json.dumps(bridge, indent=2) + "\n", encoding="utf-8")
    return path


def verify_known_values(root: Path | str, bridge: dict[str, Any]) -> list[str]:
    """Check every ``bits`` entry of a bridge against the case on disk.

    This is the Python-side twin of the Unreal automation test, and it compares
    ``bits`` only. Spec 9.2 is explicit that ``value`` exists for human
    readability and is never the basis of the check, and 9.4 that "a reader that
    cannot reproduce every ``bits`` entry exactly has failed, regardless of how
    close ``value`` looks".

    Args:
        root: The case root directory.
        bridge: A parsed ``known_values.json``.

    Returns:
        A list of problems; empty means every sample matched.
    """
    root = Path(root)
    problems: list[str] = []

    expected_check = f"0x{CHECK_VALUE:08X}"
    if bridge.get("crc32cCheck") != expected_check:
        # If the two sides disagree about CRC-32C itself, nothing downstream can
        # be trusted, so this is reported first and by name.
        problems.append(
            f"crc32cCheck is {bridge.get('crc32cCheck')!r}, expected "
            f"{expected_check!r}; the two implementations do not share a CRC-32C"
        )

    try:
        manifest = load_manifest(root)
    except ManifestError as exc:
        return problems + [str(exc)]

    case_id = (manifest.get("case") or {}).get("id")
    if bridge.get("caseId") != case_id:
        problems.append(
            f"caseId is {bridge.get('caseId')!r} but the manifest declares {case_id!r}"
        )

    samples = bridge.get("samples")
    if not isinstance(samples, list) or not samples:
        problems.append("samples is empty; the bridge proves nothing")
        return problems

    cached_key: tuple[str, int] | None = None
    cached_values: np.ndarray | None = None
    for index, sample in enumerate(samples):
        if not isinstance(sample, dict):
            problems.append(f"samples[{index}] is not an object")
            continue
        field_id = sample.get("field")
        frame = sample.get("frame")
        voxel = sample.get("voxel")
        component = sample.get("component")
        if (
            not isinstance(field_id, str)
            or not isinstance(frame, int)
            or not isinstance(voxel, list)
            or len(voxel) != 3
            or not isinstance(component, int)
        ):
            problems.append(f"samples[{index}] is malformed: {sample!r}")
            continue

        key = (field_id, frame)
        if key != cached_key:
            cached_key = key
            cached_values = None
            entry = field_by_id(manifest, field_id)
            if entry is None:
                problems.append(
                    f"samples[{index}]: field {field_id!r} is not declared in the "
                    "manifest"
                )
                continue
            pattern = (entry.get("storage") or {}).get("pathPattern")
            if not isinstance(pattern, str):
                problems.append(
                    f"samples[{index}]: field {field_id!r} has no storage.pathPattern"
                )
                continue
            try:
                cached_values = read_cvf(
                    frame_path(root, pattern, frame)
                ).values
            except (CVFError, CVFFormatError, CodecError, ManifestError, OSError) as exc:
                problems.append(f"samples[{index}]: {exc}")
                continue

        if cached_values is None:
            continue
        values = cached_values
        x, y, z = (int(v) for v in voxel)
        if not (
            0 <= x < values.shape[0]
            and 0 <= y < values.shape[1]
            and 0 <= z < values.shape[2]
            and 0 <= component < values.shape[3]
        ):
            problems.append(
                f"samples[{index}]: voxel {voxel} component {component} is outside "
                f"the stored extent {values.shape}"
            )
            continue

        actual = _bits_of(values[x, y, z, component])
        if actual != sample.get("bits"):
            problems.append(
                f"samples[{index}] ({field_id} frame {frame} voxel {voxel} component "
                f"{component}): bits are {actual}, the bridge expects "
                f"{sample.get('bits')!r}"
            )

    problems.extend(_verify_mesh_samples(root, manifest, bridge))
    problems.extend(_verify_array_samples(root, bridge))
    return problems


def _verify_mesh_samples(root: Path, manifest: dict[str, Any],
                         bridge: dict[str, Any]) -> list[str]:
    """Check every ``meshSamples`` entry against the ``.cvm`` files on disk."""
    problems: list[str] = []
    samples = bridge.get("meshSamples")
    if not isinstance(samples, list):
        return problems

    declared = {
        str(entry.get("id")): entry
        for entry in (manifest.get("meshes") or [])
        if isinstance(entry, dict)
    }
    cache: dict[str, Any] = {}

    def mesh_for(mesh_id: str, index: int) -> Any:
        if mesh_id in cache:
            return cache[mesh_id]
        entry = declared.get(mesh_id)
        if entry is None:
            problems.append(
                f"meshSamples[{index}]: mesh {mesh_id!r} is not declared in the manifest"
            )
            cache[mesh_id] = None
            return None
        relative = entry.get("path")
        if not isinstance(relative, str) or not is_safe_relative_path(relative):
            problems.append(
                f"meshSamples[{index}]: mesh {mesh_id!r} has no safe path"
            )
            cache[mesh_id] = None
            return None
        try:
            cache[mesh_id] = read_cvm(root / relative)
        except (CVMError, OSError) as exc:
            problems.append(f"meshSamples[{index}]: {relative}: {exc}")
            cache[mesh_id] = None
        return cache[mesh_id]

    # Counts first: a swapped per-vertex/per-triangle association shows up here
    # as a single clear disagreement instead of as a scatter of wrong values.
    for entry in bridge.get("meshes") or []:
        if not isinstance(entry, dict):
            continue
        mesh = mesh_for(str(entry.get("id")), -1)
        if mesh is None:
            continue
        for key, actual in (("vertexCount", mesh.vertex_count),
                            ("triangleCount", mesh.triangle_count)):
            if entry.get(key) != actual:
                problems.append(
                    f"mesh {entry.get('id')!r}: {key} is {actual}, the bridge expects "
                    f"{entry.get(key)!r}"
                )

    for index, sample in enumerate(samples):
        if not isinstance(sample, dict):
            problems.append(f"meshSamples[{index}] is not an object")
            continue
        kind = sample.get("kind")
        mesh = mesh_for(str(sample.get("mesh")), index)
        if mesh is None:
            continue
        label = f"meshSamples[{index}] ({kind} on {sample.get('mesh')!r})"

        if kind == "position":
            vertex, component = sample.get("vertex"), sample.get("component")
            if not _in_range(vertex, mesh.vertex_count) or not _in_range(component, 3):
                problems.append(f"{label}: vertex/component out of range")
                continue
            actual = _bits_of(mesh.positions[vertex][component])
            if actual != sample.get("bits"):
                problems.append(
                    f"{label} vertex {vertex} component {component}: bits are "
                    f"{actual}, the bridge expects {sample.get('bits')!r}"
                )
        elif kind == "triangle":
            triangle = sample.get("triangle")
            if not _in_range(triangle, mesh.triangle_count):
                problems.append(f"{label}: triangle index out of range")
                continue
            actual_indices = [int(v) for v in mesh.indices[triangle]]
            if actual_indices != sample.get("indices"):
                problems.append(
                    f"{label} triangle {triangle}: corners are {actual_indices}, the "
                    f"bridge expects {sample.get('indices')!r} (order is significant; "
                    "winding is CCW from outside)"
                )
        elif kind == "patchId":
            triangle = sample.get("triangle")
            if mesh.patch_ids is None:
                problems.append(f"{label}: the mesh stores no patchIds")
                continue
            if not _in_range(triangle, mesh.triangle_count):
                problems.append(
                    f"{label}: triangle {triangle!r} is out of range for "
                    f"{mesh.triangle_count} triangles — patchIds is per-triangle"
                )
                continue
            actual_value = int(mesh.patch_ids[triangle])
            if actual_value != sample.get("value"):
                problems.append(
                    f"{label} triangle {triangle}: patchId is {actual_value}, the "
                    f"bridge expects {sample.get('value')!r}"
                )
        elif kind == "nodeId":
            vertex = sample.get("vertex")
            if mesh.node_ids is None:
                problems.append(f"{label}: the mesh stores no nodeIds")
                continue
            if not _in_range(vertex, mesh.vertex_count):
                problems.append(
                    f"{label}: vertex {vertex!r} is out of range for "
                    f"{mesh.vertex_count} vertices — nodeIds is per-vertex"
                )
                continue
            actual_value = int(mesh.node_ids[vertex])
            if actual_value != sample.get("value"):
                problems.append(
                    f"{label} vertex {vertex}: nodeId is {actual_value}, the bridge "
                    f"expects {sample.get('value')!r}"
                )
        else:
            problems.append(f"{label}: unknown mesh sample kind {kind!r}")
    return problems


def _verify_array_samples(root: Path, bridge: dict[str, Any]) -> list[str]:
    """Check every ``arraySamples`` entry against the ``.cva`` files on disk."""
    problems: list[str] = []
    samples = bridge.get("arraySamples")
    if not isinstance(samples, list):
        return problems

    cache: dict[str, Any] = {}

    def array_for(relative: Any, index: int) -> Any:
        if not isinstance(relative, str) or not is_safe_relative_path(relative):
            problems.append(f"arraySamples[{index}]: unsafe or missing path")
            return None
        if relative not in cache:
            try:
                cache[relative] = read_cva(root / relative)
            except (CVAError, CodecError, OSError) as exc:
                problems.append(f"arraySamples[{index}]: {relative}: {exc}")
                cache[relative] = None
        return cache[relative]

    for index, sample in enumerate(samples):
        if not isinstance(sample, dict):
            problems.append(f"arraySamples[{index}] is not an object")
            continue
        array = array_for(sample.get("path"), index)
        if array is None:
            continue
        component = sample.get("component")
        if not _in_range(component, array.component_count):
            problems.append(
                f"arraySamples[{index}]: component {component!r} is out of range for "
                f"{array.component_count} components"
            )
            continue

        kind = sample.get("kind")
        label = f"arraySamples[{index}] ({sample.get('path')} component {component}"
        expected_name = sample.get("componentName")
        if expected_name is not None and expected_name != array.component_names[component]:
            problems.append(
                f"{label}): component {component} is named "
                f"{array.component_names[component]!r}, the bridge expects "
                f"{expected_name!r} — the tensor component order disagrees "
                "(spec 6.3 mandates XX YY ZZ XY YZ XZ)"
            )

        if kind == "value":
            entity = sample.get("entity")
            if not _in_range(entity, array.value_count):
                problems.append(f"{label}): entity {entity!r} is out of range")
                continue
            actual = _bits_of(array.values[entity][component])
            if actual != sample.get("bits"):
                problems.append(
                    f"{label} entity {entity}): bits are {actual}, the bridge expects "
                    f"{sample.get('bits')!r}"
                )
        elif kind == "statistics":
            statistics = array.frame_statistics
            if statistics is None:
                problems.append(f"{label}): the array stores no frame statistics")
                continue
            actual_count = int(statistics.valid_count[component])
            if actual_count != sample.get("validCount"):
                problems.append(
                    f"{label}): validCount is {actual_count}, the bridge expects "
                    f"{sample.get('validCount')!r} — a difference here means NaN was "
                    "counted differently by the two readers"
                )
            for key, values in (("minimum", statistics.minimum),
                                ("maximum", statistics.maximum),
                                ("mean", statistics.mean)):
                expected = sample.get(key)
                if not isinstance(expected, dict):
                    continue
                actual = _bits_of(np.float64(values[component]))
                if actual != expected.get("bits"):
                    problems.append(
                        f"{label}): {key} bits are {actual}, the bridge expects "
                        f"{expected.get('bits')!r}"
                    )
        else:
            problems.append(f"{label}): unknown array sample kind {kind!r}")
    return problems


def _in_range(value: Any, limit: int) -> bool:
    """Whether ``value`` is an int usable as an index into ``limit`` entries."""
    return isinstance(value, int) and not isinstance(value, bool) and 0 <= value < limit
