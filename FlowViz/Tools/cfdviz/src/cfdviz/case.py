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
from .crc32c import CHECK_VALUE, crc32c
from .cvf import CVFError, CVFFormatError, CVFReader, read_cvf
from .manifest import (
    FORMAT_VERSION,
    ManifestError,
    field_by_id,
    frame_path,
    grid_by_id,
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

    mask_field_ids = {
        grid.get("maskField")
        for grid in (manifest.get("grids") or [])
        if isinstance(grid, dict) and grid.get("maskField")
    }

    # Mask fields are decoded first, and their values kept: every other field's
    # statistics depend on them, and reading a mask twice would both double the
    # checked-file count and report a broken mask once per field that used it.
    mask_values: dict[tuple[str, int], np.ndarray] = {}
    for entry in fields:
        if not isinstance(entry, dict) or entry.get("id") not in mask_field_ids:
            continue
        for frame in range(len(times)):
            values = _read_field_frame(report, root, manifest, entry, frame, times)
            if values is not None:
                mask_values[(str(entry.get("id")), frame)] = values

    for entry in fields:
        if not isinstance(entry, dict):
            continue
        field_id = entry.get("id")
        grid = grid_by_id(manifest, entry.get("grid")) if entry.get("grid") else None
        mask_id = grid.get("maskField") if isinstance(grid, dict) else None

        components = entry.get("componentCount")
        components = components if isinstance(components, int) and components > 0 else 1
        stats = FieldStatistics(
            minimum=np.full(components, np.inf),
            maximum=np.full(components, -np.inf),
        )

        for frame in range(len(times)):
            if field_id in mask_field_ids:
                values = mask_values.get((str(field_id), frame))
            else:
                values = _read_field_frame(report, root, manifest, entry, frame, times)
            if values is None:
                continue

            mask = None
            if mask_id and mask_id != field_id:
                stored = mask_values.get((str(mask_id), frame))
                if stored is None:
                    report.error(
                        f"field {field_id!r} frame {frame}: mask field {mask_id!r} "
                        "could not be read, so mask coverage is unknown"
                    )
                elif stored.shape[:3] != values.shape[:3]:
                    report.error(
                        f"field {field_id!r} frame {frame}: mask field {mask_id!r} has "
                        f"extent {stored.shape[:3]} but the field is "
                        f"{values.shape[:3]}"
                    )
                else:
                    mask = stored[..., 0] != 0
            _accumulate(stats, values, mask)

        if field_id:
            report.statistics[str(field_id)] = stats
            _check_declared_statistics(report, entry, stats)

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
                if components > 1:
                    samples.append(
                        _sample(
                            field_id, frame, middle, components - 1,
                            values[middle][components - 1], "last-component",
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
                    with np.errstate(invalid="ignore"):
                        nan_mask = np.isnan(values.astype(np.float64))
                    if bool(np.any(nan_mask)):
                        index = np.argwhere(nan_mask)[0]
                        voxel = (int(index[0]), int(index[1]), int(index[2]))
                        samples.append(
                            _sample(
                                field_id, frame, voxel, int(index[3]),
                                values[voxel][int(index[3])], "nan",
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
            rejected = np.argwhere(mask_values[..., 0] == 0)
            if len(rejected):
                voxel = tuple(int(v) for v in rejected[0])
                samples.append(
                    _sample(
                        str(mask_id), frames[0], voxel, 0,
                        mask_values[voxel][0], "masked",
                    )
                )

    return {
        "formatVersion": FORMAT_VERSION,
        "caseId": (manifest.get("case") or {}).get("id"),
        "crc32cCheck": f"0x{CHECK_VALUE:08X}",
        "samples": samples,
    }


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

    cache: dict[tuple[str, int], np.ndarray] = {}
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
        if key not in cache:
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
                cache[key] = read_cvf(frame_path(root, pattern, frame)).values
            except (CVFError, CVFFormatError, CodecError, ManifestError, OSError) as exc:
                problems.append(f"samples[{index}]: {exc}")
                continue

        values = cache[key]
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
    return problems
