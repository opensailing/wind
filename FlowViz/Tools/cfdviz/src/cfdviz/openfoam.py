"""Convert explicit OpenFOAM sampled-set tables into CFDViz data."""

from __future__ import annotations

import csv
import gzip
import io
import math
from dataclasses import dataclass
from numbers import Integral, Real
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Any, Final, Iterable, Sequence

import numpy as np

from .convert import ConversionError, require_safe_identifier

__all__ = [
    "OpenFOAMError",
    "OpenFOAMFieldSpec",
    "OpenFOAMFrameSource",
    "OpenFOAMLattice",
    "OpenFOAMSampledSet",
    "OpenFOAMScatteredFrame",
    "discover_sampled_set_frames",
    "openfoam_field",
    "read_sampled_set",
    "scatter_sampled_set",
]


class OpenFOAMError(ConversionError):
    """OpenFOAM sampled output is ambiguous, malformed, or unsupported."""


@dataclass(frozen=True)
class OpenFOAMFieldSpec:
    """One explicitly typed sampled-set field in source-column order."""

    source_name: str
    field_id: str
    component_count: int
    components: tuple[str, ...]
    unit: str
    semantic: str
    vector_kind: str | None = None
    phase: dict[str, Any] | None = None

    def __post_init__(self) -> None:
        if not isinstance(self.source_name, str) or not self.source_name:
            raise OpenFOAMError("OpenFOAM source field name must be non-empty")
        object.__setattr__(
            self,
            "field_id",
            require_safe_identifier(self.field_id, label="field id"),
        )
        if self.component_count not in (1, 3):
            raise OpenFOAMError("OpenFOAM fields must be scalar or three-component vector")
        if len(self.components) != self.component_count:
            raise OpenFOAMError(
                f"field {self.source_name!r} declares {self.component_count} components "
                f"but names {len(self.components)}"
            )
        if not isinstance(self.unit, str) or not self.unit:
            raise OpenFOAMError(f"field {self.source_name!r} unit must be non-empty")
        if not isinstance(self.semantic, str) or not self.semantic:
            raise OpenFOAMError(f"field {self.source_name!r} semantic must be non-empty")
        expected_kind = None if self.component_count == 1 else self.vector_kind
        if expected_kind not in (None, "true", "pseudo"):
            raise OpenFOAMError(
                f"field {self.source_name!r} vector_kind must be 'true' or 'pseudo'"
            )

    @property
    def source_columns(self) -> tuple[str, ...]:
        if self.component_count == 1:
            return (self.source_name,)
        return tuple(f"{self.source_name}_{suffix}" for suffix in ("x", "y", "z"))


@dataclass(frozen=True)
class OpenFOAMFrameSource:
    time: Decimal
    path: Path


@dataclass(frozen=True)
class OpenFOAMSampledSet:
    path: Path
    coordinates: np.ndarray
    values: dict[str, np.ndarray]
    row_numbers: tuple[int, ...]


@dataclass(frozen=True)
class OpenFOAMLattice:
    origin: tuple[float, float, float]
    spacing: tuple[float, float, float]
    points: tuple[int, int, int]
    coordinate_tolerance: float

    def __post_init__(self) -> None:
        raw_origin = tuple(self.origin)
        if len(raw_origin) != 3 or any(
            not isinstance(value, Real)
            or isinstance(value, (bool, np.bool_))
            or not math.isfinite(float(value))
            for value in raw_origin
        ):
            raise OpenFOAMError(
                f"lattice origin must contain three finite numeric values; got "
                f"{raw_origin}"
            )
        origin = tuple(float(value) for value in raw_origin)

        raw_spacing = tuple(self.spacing)
        if len(raw_spacing) != 3 or any(
            not isinstance(value, Real)
            or isinstance(value, (bool, np.bool_))
            or not math.isfinite(float(value))
            or float(value) <= 0
            for value in raw_spacing
        ):
            raise OpenFOAMError(
                f"lattice spacing must contain three finite positive numeric values; "
                f"got {raw_spacing}"
            )
        spacing = tuple(float(value) for value in raw_spacing)

        raw_points = tuple(self.points)
        if len(raw_points) != 3 or any(
            not isinstance(value, Integral)
            or isinstance(value, (bool, np.bool_))
            or int(value) < 2
            for value in raw_points
        ):
            raise OpenFOAMError(
                f"lattice point counts must contain three integers >= 2; got "
                f"{raw_points}"
            )
        points = tuple(int(value) for value in raw_points)

        raw_tolerance = self.coordinate_tolerance
        if (
            not isinstance(raw_tolerance, Real)
            or isinstance(raw_tolerance, (bool, np.bool_))
        ):
            raise OpenFOAMError(
                "coordinate_tolerance must be a finite non-negative number"
            )
        tolerance = float(raw_tolerance)
        if (
            not math.isfinite(tolerance)
            or tolerance < 0
            or any(tolerance >= 0.5 * delta for delta in spacing)
        ):
            raise OpenFOAMError(
                "coordinate_tolerance must be finite, non-negative, and less than "
                "half every lattice spacing"
            )
        object.__setattr__(self, "origin", origin)
        object.__setattr__(self, "spacing", spacing)
        object.__setattr__(self, "points", points)
        object.__setattr__(self, "coordinate_tolerance", tolerance)

    @property
    def dimensions(self) -> tuple[int, int, int]:
        return tuple(value - 1 for value in self.points)


@dataclass(frozen=True)
class OpenFOAMScatteredFrame:
    values: dict[str, np.ndarray]
    valid_mask: np.ndarray


_STANDARD_FIELDS: Final = {
    "U": {
        "field_id": "U",
        "unit": "m/s",
        "semantic": "velocity",
        "vector_kind": "true",
    },
    "Q": {
        "field_id": "qCriterion",
        "unit": "1/s2",
        "semantic": "q-criterion",
    },
    "vorticity": {
        "field_id": "vorticity",
        "unit": "1/s",
        "semantic": "vorticity",
        "vector_kind": "pseudo",
    },
    "T": {
        "field_id": "temperature",
        "unit": "K",
        "semantic": "temperature",
    },
    "rho": {
        "field_id": "density",
        "unit": "kg/m3",
        "semantic": "density",
    },
    "k": {
        "field_id": "k",
        "unit": "m2/s2",
        "semantic": "turbulent-kinetic-energy",
    },
    "epsilon": {
        "field_id": "epsilon",
        "unit": "m2/s3",
        "semantic": "dissipation-rate",
    },
    "omega": {
        "field_id": "omega",
        "unit": "1/s",
        "semantic": "specific-dissipation-rate",
    },
    "nut": {
        "field_id": "nut",
        "unit": "m2/s",
        "semantic": "kinematic-viscosity",
    },
}


def openfoam_field(
    source_name: str,
    kind: str,
    *,
    field_id: str | None = None,
    unit: str | None = None,
    semantic: str | None = None,
    vector_kind: str | None = None,
    pressure_kind: str | None = None,
    secondary_phase: str | None = None,
) -> OpenFOAMFieldSpec:
    """Declare one sampled-set field without inferring type from header suffixes."""
    if kind not in ("scalar", "vector"):
        raise OpenFOAMError(f"field {source_name!r} kind must be 'scalar' or 'vector'")
    defaults = dict(_STANDARD_FIELDS.get(source_name, {}))
    phase = None
    if source_name == "p":
        if pressure_kind not in ("kinematic", "dynamic"):
            raise OpenFOAMError(
                "OpenFOAM p requires pressure_kind='kinematic' or 'dynamic'"
            )
        defaults = {
            "field_id": "pressure",
            "unit": "m2/s2" if pressure_kind == "kinematic" else "Pa",
            "semantic": "pressure",
        }
    elif source_name == "p_rgh":
        defaults = {
            "field_id": "p_rgh",
            "unit": "m2/s2",
            "semantic": "reduced-pressure",
        }
    elif source_name.startswith("alpha."):
        primary = source_name.split(".", 1)[1]
        if not primary or not secondary_phase:
            raise OpenFOAMError(
                f"field {source_name!r} requires an explicit secondary_phase"
            )
        defaults = {
            "field_id": source_name,
            "unit": "1",
            "semantic": "volume-fraction",
        }
        phase = {
            "representation": "volume-fraction",
            "primaryPhase": primary,
            "secondaryPhase": secondary_phase,
            "interfaceValue": 0.5,
            "inside": "greater-than-interface",
        }
    if not defaults and (field_id is None or unit is None or semantic is None):
        raise OpenFOAMError(
            f"unknown OpenFOAM field {source_name!r} requires field_id, unit, and semantic"
        )
    component_count = 1 if kind == "scalar" else 3
    resolved_kind = vector_kind or defaults.get("vector_kind")
    if component_count == 3 and resolved_kind is None:
        raise OpenFOAMError(
            f"vector field {source_name!r} requires vector_kind='true' or 'pseudo'"
        )
    return OpenFOAMFieldSpec(
        source_name=source_name,
        field_id=field_id or defaults["field_id"],
        component_count=component_count,
        components=("value",) if component_count == 1 else ("x", "y", "z"),
        unit=unit or defaults["unit"],
        semantic=semantic or defaults["semantic"],
        vector_kind=resolved_kind,
        phase=phase,
    )


def _read_text(path: Path, max_input_bytes: int | None) -> str:
    if max_input_bytes is not None and (
        not isinstance(max_input_bytes, int)
        or isinstance(max_input_bytes, bool)
        or max_input_bytes < 0
    ):
        raise OpenFOAMError("max_input_bytes must be a non-negative integer")
    try:
        raw = path.read_bytes()
        if max_input_bytes is not None and len(raw) > max_input_bytes:
            raise OpenFOAMError(
                f"{path}: compressed/on-disk input is {len(raw)} bytes, above limit "
                f"{max_input_bytes}"
            )
        if path.suffix.lower() == ".gz":
            raw = gzip.decompress(raw)
        if max_input_bytes is not None and len(raw) > max_input_bytes:
            raise OpenFOAMError(
                f"{path}: decompressed input is {len(raw)} bytes, above limit "
                f"{max_input_bytes}"
            )
        return raw.decode("utf-8")
    except OpenFOAMError:
        raise
    except (OSError, UnicodeError, gzip.BadGzipFile) as exc:
        raise OpenFOAMError(f"{path}: cannot read sampled-set text: {exc}") from exc


def _expected_columns(fields: Sequence[OpenFOAMFieldSpec]) -> list[str]:
    columns = ["x", "y", "z"]
    for field in fields:
        columns.extend(field.source_columns)
    return columns


def _raw_header_matches(actual: str, expected: str) -> bool:
    return actual == expected or (actual.endswith("...") and expected.startswith(actual[:-3]))


def read_sampled_set(
    path: Path | str,
    *,
    fields: Sequence[OpenFOAMFieldSpec],
    format: str,
    separator: str = ",",
    max_input_bytes: int | None = None,
) -> OpenFOAMSampledSet:
    """Read one OpenFOAM sampledSets raw or CSV table with explicit columns."""
    source = Path(path)
    declared = tuple(fields)
    if not declared:
        raise OpenFOAMError("at least one OpenFOAM field schema is required")
    if len({field.source_name for field in declared}) != len(declared):
        raise OpenFOAMError("OpenFOAM source field names must be unique")
    if len({field.field_id for field in declared}) != len(declared):
        raise OpenFOAMError("CFDViz target field ids must be unique")
    expected = _expected_columns(declared)
    text = _read_text(source, max_input_bytes)

    rows: list[tuple[int, list[str]]] = []
    if format == "csv":
        if not isinstance(separator, str) or len(separator) != 1:
            raise OpenFOAMError("CSV separator must be exactly one character")
        parsed = list(csv.reader(io.StringIO(text), delimiter=separator))
        nonempty = [(index, row) for index, row in enumerate(parsed, start=1) if row]
        if not nonempty:
            raise OpenFOAMError(f"{source}: sampled-set CSV is empty")
        header_line, header = nonempty[0]
        if header[:3] != ["x", "y", "z"]:
            raise OpenFOAMError(
                f"{source}:{header_line}: coordinate columns are {header[:4]!r}; "
                "export OpenFOAM sampledSets with axis xyz"
            )
        if header != expected:
            raise OpenFOAMError(
                f"{source}:{header_line}: header {header!r} does not match explicit "
                f"field columns {expected!r}; OpenFOAM may have omitted a field"
            )
        rows = nonempty[1:]
    elif format == "raw":
        lines = text.splitlines()
        header_index = next((i for i, line in enumerate(lines) if line.strip()), None)
        if header_index is None or not lines[header_index].lstrip().startswith("#"):
            raise OpenFOAMError(f"{source}: raw sampled set must begin with a '# ' header")
        header = lines[header_index].lstrip()[1:].split()
        if header[:3] != ["x", "y", "z"]:
            raise OpenFOAMError(
                f"{source}:{header_index + 1}: coordinate columns are {header[:4]!r}; "
                "export OpenFOAM sampledSets with axis xyz"
            )
        if len(header) != len(expected) or any(
            not _raw_header_matches(actual, wanted)
            for actual, wanted in zip(header, expected)
        ):
            raise OpenFOAMError(
                f"{source}:{header_index + 1}: header {header!r} does not match "
                f"explicit field columns {expected!r}"
            )
        rows = [
            (index, line.split())
            for index, line in enumerate(lines[header_index + 1 :], start=header_index + 2)
            if line.strip()
        ]
    else:
        raise OpenFOAMError(f"sampled-set format must be 'csv' or 'raw'; got {format!r}")

    coordinates: list[list[float]] = []
    field_rows = {field.field_id: [] for field in declared}
    row_numbers: list[int] = []
    for row_number, row in rows:
        if len(row) != len(expected):
            raise OpenFOAMError(
                f"{source}:{row_number}: row has {len(row)} columns, expected "
                f"{len(expected)}"
            )
        try:
            numbers = [float(token) for token in row]
        except ValueError as exc:
            raise OpenFOAMError(
                f"{source}:{row_number}: non-numeric sampled value: {exc}"
            ) from exc
        if any(not math.isfinite(value) for value in numbers[:3]):
            raise OpenFOAMError(
                f"{source}:{row_number}: coordinates must be finite; got {numbers[:3]}"
            )
        if any(math.isinf(value) for value in numbers[3:]):
            raise OpenFOAMError(
                f"{source}:{row_number}: sampled field values contain infinity"
            )
        coordinates.append(numbers[:3])
        cursor = 3
        for field in declared:
            stop = cursor + field.component_count
            field_rows[field.field_id].append(numbers[cursor:stop])
            cursor = stop
        row_numbers.append(row_number)

    return OpenFOAMSampledSet(
        path=source,
        coordinates=np.asarray(coordinates, dtype=np.float64).reshape(-1, 3),
        values={
            field.field_id: np.asarray(
                field_rows[field.field_id],
                dtype=np.float64,
            ).reshape(-1, field.component_count)
            for field in declared
        },
        row_numbers=tuple(row_numbers),
    )


def discover_sampled_set_frames(
    input_directory: Path | str,
    *,
    set_name: str,
    format: str,
) -> tuple[OpenFOAMFrameSource, ...]:
    """Discover one sampled-set file per numeric OpenFOAM time directory."""
    root = Path(input_directory)
    if not root.is_dir():
        raise OpenFOAMError(f"{root}: OpenFOAM sampled-set directory does not exist")
    if not set_name or any(character in set_name for character in "/\\"):
        raise OpenFOAMError(f"set_name {set_name!r} is not a safe file stem")
    if format not in ("csv", "raw"):
        raise OpenFOAMError("sampled-set format must be 'csv' or 'raw'")
    suffix = ".csv" if format == "csv" else ".xy"

    by_time: dict[Decimal, tuple[str, Path]] = {}
    for directory in root.iterdir():
        if not directory.is_dir():
            continue
        try:
            time = Decimal(directory.name)
        except InvalidOperation:
            continue
        if not time.is_finite() or time < 0:
            raise OpenFOAMError(
                f"{directory}: time directory must be finite and non-negative"
            )
        if time in by_time:
            other_name, _ = by_time[time]
            raise OpenFOAMError(
                f"{root}: time directories {other_name!r} and {directory.name!r} "
                f"represent the same numeric time {time}"
            )
        plain = directory / f"{set_name}{suffix}"
        compressed = directory / f"{set_name}{suffix}.gz"
        matches = [path for path in (plain, compressed) if path.is_file()]
        if len(matches) != 1:
            raise OpenFOAMError(
                f"{directory}: expected exactly one {set_name}{suffix}[.gz] file; "
                f"found {len(matches)}"
            )
        by_time[time] = (directory.name, matches[0])

    if not by_time:
        raise OpenFOAMError(f"{root}: no numeric OpenFOAM time directories found")
    ordered = sorted(by_time.items(), key=lambda item: item[0])
    float_times = [float(time) for time, _ in ordered]
    if any(not math.isfinite(value) for value in float_times) or any(
        left == right for left, right in zip(float_times, float_times[1:])
    ):
        raise OpenFOAMError(
            f"{root}: distinct decimal times cannot be represented distinctly as "
            "CFDViz timeline floats"
        )
    return tuple(
        OpenFOAMFrameSource(time=time, path=metadata[1])
        for time, metadata in ordered
    )


def scatter_sampled_set(
    table: OpenFOAMSampledSet,
    *,
    lattice: OpenFOAMLattice,
    fields: Sequence[OpenFOAMFieldSpec],
    source_precision: str,
    write_precision: int,
) -> OpenFOAMScatteredFrame:
    """Scatter unordered sampled points onto a declared Cartesian point lattice."""
    declared = tuple(fields)
    if source_precision not in ("float32", "float64"):
        raise OpenFOAMError("source_precision must be 'float32' or 'float64'")
    if (
        not isinstance(write_precision, int)
        or isinstance(write_precision, bool)
        or write_precision < 1
    ):
        raise OpenFOAMError("write_precision must be a positive integer")
    if table.coordinates.shape[0] != len(table.row_numbers):
        raise OpenFOAMError(f"{table.path}: coordinate and row counts disagree")
    for field in declared:
        values = table.values.get(field.field_id)
        if values is None or values.shape != (
            table.coordinates.shape[0],
            field.component_count,
        ):
            raise OpenFOAMError(
                f"{table.path}: values for field {field.field_id!r} do not match rows"
            )
    velocity = next((field for field in declared if field.field_id == "U"), None)
    if velocity is None or velocity.component_count != 3:
        raise OpenFOAMError("OpenFOAM lattice validity requires a three-component U field")

    stored = {
        field.field_id: np.full(
            lattice.points + (field.component_count,),
            np.nan,
            dtype=np.float64,
        )
        for field in declared
    }
    valid = np.zeros(lattice.points, dtype=np.uint8)
    seen: dict[tuple[int, int, int], tuple[int, np.ndarray]] = {}
    # OpenFOAM formats pTraits<Type>::max with limited decimal precision, so
    # the text can round slightly below the exact binary maximum.
    sentinel_threshold = np.finfo(source_precision).max / 100.0

    for row_index, (coordinate, row_number) in enumerate(
        zip(table.coordinates, table.row_numbers)
    ):
        indices: list[int] = []
        for axis in range(3):
            fractional = (
                coordinate[axis] - lattice.origin[axis]
            ) / lattice.spacing[axis]
            nearest = int(round(float(fractional)))
            expected = lattice.origin[axis] + nearest * lattice.spacing[axis]
            residual = abs(float(coordinate[axis]) - expected)
            if residual > lattice.coordinate_tolerance:
                raise OpenFOAMError(
                    f"{table.path}:{row_number}: coordinate {coordinate.tolist()} is "
                    f"off lattice on axis {'XYZ'[axis]}; nearest {expected!r} at index "
                    f"{nearest}, residual {residual!r} exceeds tolerance "
                    f"{lattice.coordinate_tolerance!r}"
                )
            if not 0 <= nearest < lattice.points[axis]:
                raise OpenFOAMError(
                    f"{table.path}:{row_number}: coordinate {coordinate.tolist()} maps "
                    f"outside lattice point counts {lattice.points}"
                )
            indices.append(nearest)
        index = (indices[0], indices[1], indices[2])
        if index in seen:
            previous_row, previous_coordinate = seen[index]
            raise OpenFOAMError(
                f"{table.path}:{row_number}: duplicate lattice index {index} for "
                f"coordinates {previous_coordinate.tolist()} (row {previous_row}) and "
                f"{coordinate.tolist()}"
            )
        seen[index] = (row_number, coordinate)

        flattened = np.concatenate(
            [table.values[field.field_id][row_index] for field in declared]
        )
        sentinel = np.abs(flattened) >= sentinel_threshold
        if sentinel.any():
            if not sentinel.all():
                raise OpenFOAMError(
                    f"{table.path}:{row_number}: OpenFOAM invalid-location sentinel is "
                    "mixed with ordinary field values"
                )
            continue

        for field in declared:
            row_values = table.values[field.field_id][row_index]
            if field.component_count == 3:
                missing = np.isnan(row_values)
                if missing.any() and not missing.all():
                    raise OpenFOAMError(
                        f"{table.path}:{row_number}: vector field "
                        f"{field.source_name!r} has only some NaN components"
                    )
        velocity_values = table.values[velocity.field_id][row_index]
        if not np.isfinite(velocity_values).all():
            continue
        for field in declared:
            stored[field.field_id][index] = table.values[field.field_id][row_index]
        valid[index] = 1

    return OpenFOAMScatteredFrame(values=stored, valid_mask=valid)
