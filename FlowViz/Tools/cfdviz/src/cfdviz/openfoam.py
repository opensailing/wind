"""Convert explicit OpenFOAM sampled-set tables into CFDViz data."""

from __future__ import annotations

import csv
import gzip
import hashlib
import io
import math
import re
import uuid
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from numbers import Integral, Real
from pathlib import Path
from typing import Any, Final, Iterable, Sequence

import numpy as np

from .case import validate_case, write_known_values
from .codecs import codec_id_from_name
from .convert import (
    ConversionError,
    QualityAccumulator,
    StatisticsAccumulator,
    dump_json,
    require_safe_identifier,
    staged_output,
)
from .cvf import CVFError, write_cvf
from .manifest import FORMAT_VERSION

__all__ = [
    "OpenFOAMError",
    "OpenFOAMFieldSpec",
    "OpenFOAMFrameSource",
    "OpenFOAMImportParameters",
    "OpenFOAMLattice",
    "OpenFOAMSampledSet",
    "OpenFOAMScatteredFrame",
    "discover_sampled_set_frames",
    "import_openfoam_case",
    "openfoam_field",
    "read_sampled_set",
    "scatter_sampled_set",
]

_CASE_NAMESPACE: Final = uuid.UUID("6b37c23a-3d94-5fcc-85db-79c0f9414942")
_GRID_ID: Final = "main"
_MASK_ID: Final = "validMask"
_AXIS_PATTERN: Final = re.compile(r"^[+-][XYZ]$")


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
        if self.component_count == 1 and self.vector_kind is not None:
            raise OpenFOAMError(
                f"scalar field {self.source_name!r} must not declare vector_kind"
            )
        if self.component_count == 3 and self.vector_kind not in ("true", "pseudo"):
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
class OpenFOAMImportParameters:
    """Complete explicit interpretation of one sampled-set function output."""

    output: Path
    name: str
    input_directory: Path
    set_name: str
    format: str
    fields: tuple[OpenFOAMFieldSpec, ...]
    lattice: OpenFOAMLattice
    source_axes: tuple[str, str, str]
    source_precision: str
    write_precision: int
    sampling_method: str
    solver_method: str
    export_command: str
    separator: str = ","
    length_unit: str = "m"
    time_unit: str = "s"
    mass_unit: str = "kg"
    temperature_unit: str = "K"
    codec: str = "zlib"
    level: int | None = None
    float_type: str = "float32"
    brick_size: tuple[int, int, int] = (32, 32, 32)
    solver_version: str | None = None
    solver_commit: str | None = None
    solver_configuration: str | None = None
    source_case: str | None = None
    max_input_bytes: int | None = None
    force: bool = False

    def __post_init__(self) -> None:
        object.__setattr__(self, "output", Path(self.output))
        object.__setattr__(self, "input_directory", Path(self.input_directory))
        object.__setattr__(self, "fields", tuple(self.fields))
        object.__setattr__(self, "source_axes", tuple(self.source_axes))
        raw_bricks = tuple(self.brick_size)
        if len(raw_bricks) != 3 or any(
            not isinstance(value, Integral)
            or isinstance(value, (bool, np.bool_))
            or not 1 <= int(value) <= 65535
            for value in raw_bricks
        ):
            raise OpenFOAMError(
                "brick_size must contain three integers in [1, 65535]; got "
                f"{raw_bricks}"
            )
        object.__setattr__(
            self,
            "brick_size",
            tuple(int(value) for value in raw_bricks),
        )
        if not isinstance(self.lattice, OpenFOAMLattice):
            raise OpenFOAMError("lattice must be an OpenFOAMLattice declaration")
        if not isinstance(self.force, bool):
            raise OpenFOAMError("force must be a boolean")
        if not isinstance(self.name, str) or not self.name:
            raise OpenFOAMError("case name must be a non-empty string")
        if not isinstance(self.set_name, str) or not self.set_name or any(
            character in self.set_name for character in "/\\"
        ):
            raise OpenFOAMError("set_name must be a safe non-empty file stem")
        if self.format not in ("csv", "raw"):
            raise OpenFOAMError("format must be 'csv' or 'raw'")
        if (
            not isinstance(self.separator, str)
            or len(self.separator) != 1
            or self.separator in "\r\n\0"
        ):
            raise OpenFOAMError(
                "separator must be exactly one non-newline, non-NUL character"
            )
        if not self.fields:
            raise OpenFOAMError("at least the OpenFOAM U field is required")
        if any(not isinstance(field, OpenFOAMFieldSpec) for field in self.fields):
            raise OpenFOAMError("fields must contain only OpenFOAMFieldSpec values")
        field_ids = [field.field_id for field in self.fields]
        source_names = [field.source_name for field in self.fields]
        if field_ids.count("U") != 1 or next(
            (
                field.component_count
                for field in self.fields
                if field.field_id == "U"
            ),
            None,
        ) != 3:
            raise OpenFOAMError(
                "exactly one three-component OpenFOAM U field is required"
            )
        if _MASK_ID in field_ids:
            raise OpenFOAMError(f"field id {_MASK_ID!r} is reserved for the point mask")
        if len(field_ids) != len(set(field_ids)):
            raise OpenFOAMError(f"field ids must be unique; got {field_ids}")
        if len(source_names) != len(set(source_names)):
            raise OpenFOAMError(
                f"OpenFOAM source field names must be unique; got {source_names}"
            )
        if (
            len(self.source_axes) != 3
            or any(
                not isinstance(axis, str) or not _AXIS_PATTERN.fullmatch(axis)
                for axis in self.source_axes
            )
            or {axis[1] for axis in self.source_axes if isinstance(axis, str)}
            != {"X", "Y", "Z"}
        ):
            raise OpenFOAMError(
                "source_axes must map source X/Y/Z to a signed permutation of "
                "canonical axes, for example ('+X', '+Y', '+Z')"
            )
        if self.source_precision not in ("float32", "float64"):
            raise OpenFOAMError("source_precision must be 'float32' or 'float64'")
        if (
            not isinstance(self.write_precision, int)
            or isinstance(self.write_precision, bool)
            or self.write_precision < 1
        ):
            raise OpenFOAMError("write_precision must be a positive integer")
        for label, value in (
            ("sampling_method", self.sampling_method),
            ("solver_method", self.solver_method),
            ("export_command", self.export_command),
            ("length_unit", self.length_unit),
            ("time_unit", self.time_unit),
            ("mass_unit", self.mass_unit),
            ("temperature_unit", self.temperature_unit),
        ):
            if not isinstance(value, str) or not value:
                raise OpenFOAMError(f"{label} must be a non-empty string")
        for label, value in (
            ("solver_version", self.solver_version),
            ("solver_commit", self.solver_commit),
            ("solver_configuration", self.solver_configuration),
            ("source_case", self.source_case),
        ):
            if value is not None and (not isinstance(value, str) or not value):
                raise OpenFOAMError(f"{label} must be non-empty when present")
        if self.float_type not in ("float16", "float32"):
            raise OpenFOAMError(
                f"float_type must be 'float16' or 'float32'; got {self.float_type!r}"
            )
        codec_id_from_name(self.codec)
        if self.level is not None and self.codec != "zlib":
            raise OpenFOAMError(
                "compression level is supported only for zlib; the selected "
                f"codec {self.codec!r} ignores it"
            )
        if self.level is not None and (
            not isinstance(self.level, int)
            or isinstance(self.level, bool)
            or not 0 <= self.level <= 9
        ):
            raise OpenFOAMError("zlib compression level must be an integer in [0, 9]")
        if self.max_input_bytes is not None and (
            not isinstance(self.max_input_bytes, int)
            or isinstance(self.max_input_bytes, bool)
            or self.max_input_bytes < 0
        ):
            raise OpenFOAMError("max_input_bytes must be a non-negative integer")
        output = self.output.resolve(strict=False)
        source = self.input_directory.resolve(strict=False)
        if source == output or output in source.parents:
            raise OpenFOAMError(
                f"input {self.input_directory} is inside output {self.output}; "
                "replacing the case would delete its own source data"
            )


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


@dataclass(frozen=True)
class _CanonicalGrid:
    dimensions: tuple[int, int, int]
    points: tuple[int, int, int]
    origin: tuple[float, float, float]
    spacing: tuple[float, float, float]


def _axis_mapping(
    source_axes: Sequence[str],
) -> tuple[tuple[int, int, int], tuple[int, int, int]]:
    source_for_canonical = [0, 0, 0]
    signs = [1, 1, 1]
    canonical_indices = {"X": 0, "Y": 1, "Z": 2}
    for source_index, token in enumerate(source_axes):
        canonical_index = canonical_indices[token[1]]
        source_for_canonical[canonical_index] = source_index
        signs[canonical_index] = 1 if token[0] == "+" else -1
    return tuple(source_for_canonical), tuple(signs)


def _orientation_determinant(source_axes: Sequence[str]) -> int:
    source_for_canonical, signs = _axis_mapping(source_axes)
    matrix = np.zeros((3, 3), dtype=np.int8)
    for canonical_index, source_index in enumerate(source_for_canonical):
        matrix[canonical_index, source_index] = signs[canonical_index]
    return int(round(float(np.linalg.det(matrix))))


def _canonical_grid(
    lattice: OpenFOAMLattice,
    source_axes: Sequence[str],
) -> _CanonicalGrid:
    source_for_canonical, signs = _axis_mapping(source_axes)
    points: list[int] = []
    origin: list[float] = []
    spacing: list[float] = []
    for canonical_index, source_index in enumerate(source_for_canonical):
        count = lattice.points[source_index]
        delta = lattice.spacing[source_index]
        source_origin = lattice.origin[source_index]
        sign = signs[canonical_index]
        points.append(count)
        spacing.append(delta)
        origin.append(
            source_origin
            if sign > 0
            else -(source_origin + (count - 1) * delta)
        )
    point_counts = tuple(points)
    return _CanonicalGrid(
        dimensions=tuple(value - 1 for value in point_counts),
        points=point_counts,
        origin=tuple(origin),
        spacing=tuple(spacing),
    )


def _transform_values(
    values: np.ndarray,
    *,
    source_axes: Sequence[str],
    vector_kind: str | None,
) -> np.ndarray:
    source_for_canonical, signs = _axis_mapping(source_axes)
    mapped = np.asarray(values).transpose((*source_for_canonical, 3))
    for canonical_index, sign in enumerate(signs):
        if sign < 0:
            mapped = np.flip(mapped, axis=canonical_index)
    if vector_kind is None:
        return mapped.copy(order="C")

    transformed = np.empty_like(mapped)
    orientation = (
        _orientation_determinant(source_axes)
        if vector_kind == "pseudo"
        else 1
    )
    for canonical_index, source_index in enumerate(source_for_canonical):
        transformed[..., canonical_index] = (
            orientation
            * signs[canonical_index]
            * mapped[..., source_index]
        )
    return transformed


def _stored_values(
    values: np.ndarray,
    *,
    field: OpenFOAMFieldSpec,
    mask: np.ndarray,
    parameters: OpenFOAMImportParameters,
    path: Path,
    time: Decimal,
) -> np.ndarray:
    source = np.asarray(values)
    target_dtype = np.dtype("<f2" if parameters.float_type == "float16" else "<f4")
    stored = np.empty(source.shape, dtype=target_dtype, order="C")
    for z_index in range(source.shape[2]):
        source_slice = source[:, :, z_index, :]
        active = mask[:, :, z_index]
        if np.isinf(source_slice[active]).any():
            raise OpenFOAMError(
                f"{path}: time {time} field {field.source_name!r} contains infinity"
            )
        if field.phase is not None:
            phase = source_slice[..., 0][active]
            finite = phase[np.isfinite(phase)]
            if finite.size and (
                float(finite.min()) < 0.0 or float(finite.max()) > 1.0
            ):
                raise OpenFOAMError(
                    f"{path}: time {time} field {field.source_name!r} is declared "
                    "as a volume fraction but active values fall outside [0, 1]"
                )
        with np.errstate(over="ignore", invalid="ignore"):
            stored_slice = source_slice.astype(target_dtype)
        overflow = np.isfinite(source_slice) & ~np.isfinite(stored_slice)
        overflow &= active[..., None]
        if overflow.any():
            flat = int(np.argmax(overflow.reshape(-1)))
            raise OpenFOAMError(
                f"{path}: time {time} field {field.source_name!r} value "
                f"{source_slice.reshape(-1)[flat]!r} overflows "
                f"{parameters.float_type} storage"
            )
        stored_slice[~active] = np.nan
        stored[:, :, z_index, :] = stored_slice
    return stored


def _update_digest(
    digest: Any,
    *,
    frame: OpenFOAMFrameSource,
    table: OpenFOAMSampledSet,
    fields: Sequence[OpenFOAMFieldSpec],
) -> None:
    digest.update(
        dump_json(
            {
                "time": str(frame.time),
                "rowNumbers": table.row_numbers,
                "fieldIds": [field.field_id for field in fields],
            },
            indent=None,
        ).encode("utf-8")
    )
    digest.update(memoryview(np.ascontiguousarray(table.coordinates)).cast("B"))
    for field in fields:
        digest.update(
            memoryview(np.ascontiguousarray(table.values[field.field_id])).cast("B")
        )


def _case_id(digest: Any) -> str:
    return str(uuid.uuid5(_CASE_NAMESPACE, digest.hexdigest()))


def _tool_version() -> str:
    from . import __version__

    return __version__


def _field_entry(
    *,
    field: OpenFOAMFieldSpec,
    numeric_id: int,
    statistics: StatisticsAccumulator,
    parameters: OpenFOAMImportParameters,
) -> dict[str, Any]:
    entry: dict[str, Any] = {
        "numericId": numeric_id,
        "id": field.field_id,
        "name": field.source_name,
        "description": (
            f"OpenFOAM sampled-set field {field.source_name!r}, imported without "
            "spatial interpolation."
        ),
        "semantic": field.semantic,
        "kind": "vector" if field.component_count == 3 else "scalar",
        "components": list(field.components),
        "componentCount": field.component_count,
        "dataType": parameters.float_type,
        "association": "point",
        "grid": _GRID_ID,
        "unit": field.unit,
        "solverPrecision": parameters.source_precision,
        "temporalInterpolation": "linear",
        "storage": {
            "type": "bricked-volume",
            "codec": parameters.codec,
            "brickSize": list(parameters.brick_size),
            "pathPattern": f"frames/{{frame:06d}}/{field.field_id}.cvf",
        },
        "display": {
            "defaultComponent": (
                "magnitude" if field.component_count == 3 else field.components[0]
            ),
            "defaultColorMap": (
                "coolwarm"
                if field.semantic in ("vorticity", "q-criterion", "pressure")
                else "viridis"
            ),
            "defaultRangeMode": "global",
        },
    }
    if parameters.level is not None and parameters.codec != "none":
        entry["storage"]["level"] = parameters.level
    if field.phase is not None:
        entry["phase"] = dict(field.phase)
    declared = statistics.to_manifest()
    if declared is not None:
        entry["statistics"] = declared
    return entry


def _mask_entry(
    *,
    statistics: StatisticsAccumulator,
    parameters: OpenFOAMImportParameters,
) -> dict[str, Any]:
    entry: dict[str, Any] = {
        "numericId": 1,
        "id": _MASK_ID,
        "name": "Valid sample-point mask",
        "description": (
            "1 where OpenFOAM supplied a finite three-component velocity sample; "
            "0 for missing rows, invalid-location sentinels, or missing velocity."
        ),
        "semantic": "mask",
        "kind": "scalar",
        "components": ["valid"],
        "componentCount": 1,
        "dataType": "uint8",
        "association": "point",
        "grid": _GRID_ID,
        "unit": "1",
        "temporalInterpolation": "nearest",
        "storage": {
            "type": "bricked-volume",
            "codec": "none",
            "brickSize": list(parameters.brick_size),
            "pathPattern": "frames/{frame:06d}/validMask.cvf",
        },
        "display": {
            "defaultComponent": "valid",
            "defaultColorMap": "grayscale",
            "defaultRangeMode": "manual",
            "recommendedRange": [0.0, 1.0],
        },
    }
    declared = statistics.to_manifest()
    if declared is not None:
        entry["statistics"] = declared
    return entry


def _manifest(
    *,
    parameters: OpenFOAMImportParameters,
    grid: _CanonicalGrid,
    frames: Sequence[OpenFOAMFrameSource],
    case_id: str,
    field_statistics: dict[str, StatisticsAccumulator],
    mask_statistics: StatisticsAccumulator,
    quality: dict[str, Any],
) -> dict[str, Any]:
    solver: dict[str, str] = {
        "name": "OpenFOAM",
        "method": parameters.solver_method,
    }
    for key, value in (
        ("version", parameters.solver_version),
        ("commit", parameters.solver_commit),
        ("configuration", parameters.solver_configuration),
    ):
        if value is not None:
            solver[key] = value

    fields: list[dict[str, Any]] = [
        _mask_entry(statistics=mask_statistics, parameters=parameters)
    ]
    for numeric_id, field in enumerate(parameters.fields, start=2):
        fields.append(
            _field_entry(
                field=field,
                numeric_id=numeric_id,
                statistics=field_statistics[field.field_id],
                parameters=parameters,
            )
        )

    notes = [
        "OpenFOAM sampledSets output was read from one explicit scalar/vector "
        "column schema; unavailable fields were not inferred from shifted columns.",
        "The sampled-set axis was required to be xyz and every row was scattered "
        "only onto the explicitly declared Cartesian point lattice; no spatial "
        "interpolation or resampling was performed.",
        f"Sampling method {parameters.sampling_method!r}, source scalar precision "
        f"{parameters.source_precision!r}, and OpenFOAM write precision "
        f"{parameters.write_precision} were declared explicitly.",
        f"Coordinates were accepted only within tolerance "
        f"{parameters.lattice.coordinate_tolerance!r} of the declared lattice.",
        "Missing sample rows and OpenFOAM pTraits<Type>::max invalid-location "
        "sentinels were stored as NaN with validMask=0.",
        f"Source axes {parameters.source_axes!r} were transformed into canonical "
        "right-handed +X-forward, +Z-up coordinates; true vectors and "
        "pseudovectors used their distinct orientation transforms.",
        "Native requested OpenFOAM Q and vorticity fields were retained when "
        "present; the converter did not replace them with derived approximations.",
    ]
    provenance: dict[str, Any] = {
        "sourceType": "external-solver",
        "generatorCommand": "python -m cfdviz import-openfoam",
        "generatorVersion": _tool_version(),
        "exportCommand": parameters.export_command,
        "notes": notes,
    }
    if parameters.source_case is not None:
        provenance["sourceCase"] = parameters.source_case
    if parameters.solver_commit is not None:
        provenance["sourceRevision"] = parameters.solver_commit

    velocity = next(field for field in parameters.fields if field.field_id == "U")
    tags = ["external-solver", "OpenFOAM"]
    tags.append(
        "three-dimensional"
        if all(dimension > 1 for dimension in grid.dimensions)
        else "lower-dimensional"
    )
    return {
        "format": "CFDViz",
        "version": FORMAT_VERSION,
        "case": {
            "id": case_id,
            "name": parameters.name,
            "description": (
                "External OpenFOAM solver output converted for visualization; "
                "FlowViz did not perform the simulation."
            ),
            "quality": "external-solver-sample",
            "solver": solver,
            "tags": tags,
        },
        "units": {
            "length": parameters.length_unit,
            "time": parameters.time_unit,
            "mass": parameters.mass_unit,
            "temperature": parameters.temperature_unit,
        },
        "coordinates": {
            "handedness": "right",
            "upAxis": "Z",
            "forwardAxis": "X",
            "origin": list(grid.origin),
        },
        "timeline": {
            "frameCount": len(frames),
            "times": [float(frame.time) for frame in frames],
            "defaultInterpolation": "linear",
        },
        "grids": [
            {
                "id": _GRID_ID,
                "name": "OpenFOAM sampled point lattice",
                "type": "uniform-cartesian",
                "dimensions": list(grid.dimensions),
                "origin": list(grid.origin),
                "spacing": list(grid.spacing),
                "maskField": _MASK_ID,
            }
        ],
        "fields": fields,
        "derivedFields": [
            {
                "id": "velocityMagnitude",
                "name": "Velocity magnitude",
                "expression": "mag(U)",
                "unit": velocity.unit,
                "components": ["magnitude"],
                "componentCount": 1,
            }
        ],
        "qualityMetrics": quality,
        "provenance": provenance,
    }


def import_openfoam_case(parameters: OpenFOAMImportParameters) -> Path:
    """Convert one explicit OpenFOAM sampled-set sequence to CFDViz 1.1."""
    frames = discover_sampled_set_frames(
        parameters.input_directory,
        set_name=parameters.set_name,
        format=parameters.format,
    )
    grid = _canonical_grid(parameters.lattice, parameters.source_axes)
    codec = codec_id_from_name(parameters.codec)
    field_statistics = {
        field.field_id: StatisticsAccumulator(field.component_count)
        for field in parameters.fields
    }
    mask_statistics = StatisticsAccumulator(1)
    quality = QualityAccumulator(
        grid.dimensions,
        grid.spacing,
        association="point",
    )
    digest = hashlib.sha256()
    digest.update(
        dump_json(
            {
                "name": parameters.name,
                "times": [str(frame.time) for frame in frames],
                "setName": parameters.set_name,
                "format": parameters.format,
                "separator": parameters.separator,
                "lattice": {
                    "origin": parameters.lattice.origin,
                    "spacing": parameters.lattice.spacing,
                    "points": parameters.lattice.points,
                    "coordinateTolerance": parameters.lattice.coordinate_tolerance,
                },
                "sourceAxes": parameters.source_axes,
                "sourcePrecision": parameters.source_precision,
                "writePrecision": parameters.write_precision,
                "samplingMethod": parameters.sampling_method,
                "solverMethod": parameters.solver_method,
                "units": [
                    parameters.length_unit,
                    parameters.time_unit,
                    parameters.mass_unit,
                    parameters.temperature_unit,
                ],
                "floatType": parameters.float_type,
                "fields": [
                    {
                        "sourceName": field.source_name,
                        "id": field.field_id,
                        "components": field.components,
                        "unit": field.unit,
                        "semantic": field.semantic,
                        "vectorKind": field.vector_kind,
                        "phase": field.phase,
                    }
                    for field in parameters.fields
                ],
            },
            indent=None,
        ).encode("utf-8")
    )

    try:
        with staged_output(parameters.output, force=parameters.force) as root:
            for frame_index, frame in enumerate(frames):
                table = read_sampled_set(
                    frame.path,
                    fields=parameters.fields,
                    format=parameters.format,
                    separator=parameters.separator,
                    max_input_bytes=parameters.max_input_bytes,
                )
                scattered = scatter_sampled_set(
                    table,
                    lattice=parameters.lattice,
                    fields=parameters.fields,
                    source_precision=parameters.source_precision,
                    write_precision=parameters.write_precision,
                )
                _update_digest(
                    digest,
                    frame=frame,
                    table=table,
                    fields=parameters.fields,
                )
                point_mask = _transform_values(
                    scattered.valid_mask[..., None],
                    source_axes=parameters.source_axes,
                    vector_kind=None,
                )[..., 0].astype(bool, copy=False)

                for numeric_id, field in enumerate(parameters.fields, start=2):
                    transformed = _transform_values(
                        scattered.values[field.field_id],
                        source_axes=parameters.source_axes,
                        vector_kind=field.vector_kind,
                    )
                    stored = _stored_values(
                        transformed,
                        field=field,
                        mask=point_mask,
                        parameters=parameters,
                        path=frame.path,
                        time=frame.time,
                    )
                    field_statistics[field.field_id].add(stored, point_mask)
                    if field.field_id == "U":
                        quality.add(stored, point_mask)
                    write_cvf(
                        root / f"frames/{frame_index:06d}/{field.field_id}.cvf",
                        values=stored,
                        dtype=parameters.float_type,
                        association="point",
                        codec=codec,
                        level=parameters.level,
                        brick_size=parameters.brick_size,
                        frame_index=frame_index,
                        field_numeric_id=numeric_id,
                        simulation_time=float(frame.time),
                        dimensions=grid.dimensions,
                        mask=point_mask,
                    )

                stored_mask = point_mask.astype(np.uint8)[..., None]
                mask_statistics.add(stored_mask, None)
                write_cvf(
                    root / f"frames/{frame_index:06d}/validMask.cvf",
                    values=stored_mask,
                    dtype="uint8",
                    association="point",
                    codec=codec_id_from_name("none"),
                    brick_size=parameters.brick_size,
                    frame_index=frame_index,
                    field_numeric_id=1,
                    simulation_time=float(frame.time),
                    dimensions=grid.dimensions,
                )

            quality_manifest = quality.to_manifest(
                frame_count=len(frames),
                grid_id=_GRID_ID,
                velocity_field="U",
            )
            manifest = _manifest(
                parameters=parameters,
                grid=grid,
                frames=frames,
                case_id=_case_id(digest),
                field_statistics=field_statistics,
                mask_statistics=mask_statistics,
                quality=quality_manifest,
            )
            (root / "manifest.json").write_text(
                dump_json(manifest) + "\n",
                encoding="utf-8",
            )
            write_known_values(root)
            report = validate_case(root)
            if not report.ok:
                raise OpenFOAMError(
                    "generated OpenFOAM case failed validation:\n" + report.render()
                )
    except OpenFOAMError:
        raise
    except (ConversionError, CVFError, OSError, ValueError) as exc:
        raise OpenFOAMError(str(exc)) from exc

    return parameters.output
