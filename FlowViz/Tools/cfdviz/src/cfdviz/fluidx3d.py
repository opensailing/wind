"""Convert FluidX3D legacy VTK volumes into complete CFDViz 1.1 cases.

FluidX3D's VTK writer labels every array ``data`` and writes lattice positions as
``POINT_DATA`` even though those positions denote lattice-cell centres.  This
converter therefore takes field identity and units from explicit parameters,
shifts the grid origin by half a cell, and never infers SI conversion from the
source file.
"""

from __future__ import annotations

import hashlib
import json
import math
import re
import uuid
from dataclasses import dataclass
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
from .vtk_legacy import VTKError, VTKLegacyHeader, read_structured_points

__all__ = [
    "FluidX3DError",
    "FluidX3DFieldSource",
    "FluidX3DImportParameters",
    "fluidx3d_field",
    "import_fluidx3d_case",
]

_CASE_NAMESPACE: Final = uuid.UUID("24f4408d-440a-53e1-a919-4eb120a599e8")
_STEP_PATTERN: Final = re.compile(r"(?:^|[-_])(\d+)\.vtk$", re.IGNORECASE)
_GRID_ID: Final = "main"
_MASK_ID: Final = "validMask"
_SOLID_FLAG: Final = 0x01
_BOUNDARY_MASK: Final = 0x03
_GAS_FLAG: Final = 0x20
_SURFACE_MASK: Final = 0x38
_FILENAME_STEP_CYCLE: Final = 1_000_000_000
_MAX_SAFE_STEP: Final = (1 << 53) - 1
_AXIS_PATTERN: Final = re.compile(r"^[+-][XYZ]$")


class FluidX3DError(ConversionError):
    """FluidX3D inputs cannot be converted without guessing or losing data."""


@dataclass(frozen=True)
class _FieldPreset:
    name: str
    components: tuple[str, ...]
    semantic: str
    colormap: str
    description: str
    unit_kind: str
    invalid_on_solid: bool = False
    invalid_on_gas: bool = False
    phase: dict[str, Any] | None = None


_PRESETS: Final[dict[str, _FieldPreset]] = {
    "U": _FieldPreset(
        "Velocity",
        ("x", "y", "z"),
        "velocity",
        "viridis",
        "FluidX3D velocity exported by write_device_to_vtk.",
        "velocity",
        invalid_on_solid=True,
        invalid_on_gas=True,
    ),
    "density": _FieldPreset(
        "Density",
        ("rho",),
        "density",
        "viridis",
        "FluidX3D density exported by write_device_to_vtk.",
        "density",
        invalid_on_solid=True,
        invalid_on_gas=True,
    ),
    "phi": _FieldPreset(
        "Phase volume fraction",
        ("phi",),
        "phase-fraction",
        "coolwarm",
        "FluidX3D free-surface phase volume fraction.",
        "dimensionless",
        invalid_on_solid=True,
        phase={
            "representation": "volume-fraction",
            "primaryPhase": "liquid",
            "secondaryPhase": "gas",
            "interfaceValue": 0.5,
            "inside": "greater-than-interface",
        },
    ),
    "temperature": _FieldPreset(
        "Temperature",
        ("T",),
        "temperature",
        "magma",
        "FluidX3D temperature exported by write_device_to_vtk.",
        "temperature",
        invalid_on_solid=True,
        invalid_on_gas=True,
    ),
    "force": _FieldPreset(
        "Force",
        ("x", "y", "z"),
        "force",
        "coolwarm",
        "FluidX3D force field exported by write_device_to_vtk.",
        "force",
    ),
}


@dataclass(frozen=True)
class FluidX3DFieldSource:
    """One field sequence with semantics fixed by a reviewed preset."""

    field_id: str
    files: tuple[Path, ...]
    value_scale: float = 1.0
    unit: str | None = None

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "field_id",
            require_safe_identifier(self.field_id, label="field id"),
        )
        if self.field_id not in _PRESETS:
            supported = ", ".join(_PRESETS)
            raise FluidX3DError(
                f"unsupported FluidX3D field {self.field_id!r}; supported: "
                f"{supported}"
            )
        object.__setattr__(self, "files", tuple(Path(path) for path in self.files))
        if not self.files:
            raise FluidX3DError(f"field {self.field_id!r} has no VTK files")
        scale = float(self.value_scale)
        if not math.isfinite(scale) or scale <= 0:
            raise FluidX3DError(
                f"field {self.field_id!r} value_scale must be finite and positive"
            )
        object.__setattr__(self, "value_scale", scale)
        if self.unit is not None and (
            not isinstance(self.unit, str) or not self.unit
        ):
            raise FluidX3DError(
                f"field {self.field_id!r} unit must be a non-empty string when "
                "present"
            )


def fluidx3d_field(
    field_id: str,
    files: Iterable[Path | str],
    *,
    value_scale: float = 1.0,
    unit: str | None = None,
) -> FluidX3DFieldSource:
    """Build a source from a known FluidX3D field preset.

    The VTK array name is deliberately not consulted: FluidX3D writes ``data``
    for every physical field, so the caller must state which field each sequence
    represents.
    """
    try:
        preset = _PRESETS[field_id]
    except KeyError as exc:
        supported = ", ".join(_PRESETS)
        raise FluidX3DError(
            f"unsupported FluidX3D field {field_id!r}; supported: {supported}"
        ) from exc
    return FluidX3DFieldSource(
        field_id=field_id,
        files=tuple(Path(path) for path in files),
        value_scale=value_scale,
        unit=unit,
    )


@dataclass(frozen=True)
class FluidX3DImportParameters:
    """Complete, explicit interpretation of one FluidX3D export sequence."""

    output: Path
    name: str
    fields: tuple[FluidX3DFieldSource, ...]
    source_time_step: float
    solver_step_offset: int
    source_units: str
    source_axes: tuple[str, str, str]
    solver_method: str
    flag_files: tuple[Path, ...] = ()
    assume_all_cells_valid: bool = False
    force_interpretation: str | None = None
    length_scale: float = 1.0
    velocity_scale: float = 1.0
    length_unit: str | None = None
    time_unit: str | None = None
    mass_unit: str | None = None
    temperature_unit: str | None = None
    codec: str = "zlib"
    level: int | None = None
    float_type: str = "float32"
    brick_size: tuple[int, int, int] = (32, 32, 32)
    excluded_flag_bits: int = 0
    solver_version: str | None = None
    solver_commit: str | None = None
    solver_configuration: str | None = None
    source_case: str | None = None
    max_payload_bytes: int | None = None
    force: bool = False

    def __post_init__(self) -> None:
        object.__setattr__(self, "output", Path(self.output))
        object.__setattr__(self, "fields", tuple(self.fields))
        object.__setattr__(
            self,
            "flag_files",
            tuple(Path(path) for path in self.flag_files),
        )
        object.__setattr__(
            self,
            "brick_size",
            tuple(int(value) for value in self.brick_size),
        )
        object.__setattr__(self, "source_axes", tuple(self.source_axes))
        if (
            not isinstance(self.solver_step_offset, int)
            or isinstance(self.solver_step_offset, bool)
            or self.solver_step_offset < 0
            or self.solver_step_offset % _FILENAME_STEP_CYCLE != 0
        ):
            raise FluidX3DError(
                "solver_step_offset must be a non-negative multiple of "
                f"{_FILENAME_STEP_CYCLE}"
            )
        if not isinstance(self.force, bool):
            raise FluidX3DError("force must be a boolean")
        if not isinstance(self.assume_all_cells_valid, bool):
            raise FluidX3DError("assume_all_cells_valid must be a boolean")
        if self.source_units not in ("si", "lattice"):
            raise FluidX3DError(
                f"source_units must be 'si' or 'lattice'; got {self.source_units!r}"
            )
        unit_defaults = (
            ("m", "s", "kg", "K")
            if self.source_units == "si"
            else (
                "lattice-unit",
                "lattice-step",
                "lattice-mass",
                "lattice-temperature",
            )
        )
        for label, default in zip(
            ("length_unit", "time_unit", "mass_unit", "temperature_unit"),
            unit_defaults,
        ):
            if getattr(self, label) is None:
                object.__setattr__(self, label, default)
        if (
            len(self.source_axes) != 3
            or any(not _AXIS_PATTERN.fullmatch(axis) for axis in self.source_axes)
            or {axis[1] for axis in self.source_axes} != {"X", "Y", "Z"}
        ):
            raise FluidX3DError(
                "source_axes must map source X/Y/Z to a signed permutation of "
                "canonical axes, for example ('+X', '+Y', '+Z')"
            )
        if not self.name:
            raise FluidX3DError("case name must not be empty")
        if not self.fields:
            raise FluidX3DError("at least the FluidX3D U field is required")
        ids = [source.field_id for source in self.fields]
        if ids.count("U") != 1:
            raise FluidX3DError("exactly one FluidX3D U field is required")
        if len(ids) != len(set(ids)):
            raise FluidX3DError(f"field ids must be unique; got {ids}")
        if self.flag_files and self.assume_all_cells_valid:
            raise FluidX3DError(
                "assume_all_cells_valid cannot be combined with a flags sequence"
            )
        if not self.flag_files and not self.assume_all_cells_valid:
            raise FluidX3DError(
                "a FluidX3D flags sequence is required unless "
                "assume_all_cells_valid=True is declared explicitly"
            )
        if self.excluded_flag_bits and not self.flag_files:
            raise FluidX3DError(
                "excluded_flag_bits requires a FluidX3D flags sequence"
            )
        if "force" in ids:
            if self.force_interpretation not in ("boundary", "volume"):
                raise FluidX3DError(
                    "force_interpretation must be 'boundary' or 'volume' when "
                    "the force field is imported"
                )
            if not self.flag_files:
                raise FluidX3DError(
                    "the force field requires a FluidX3D flags sequence"
                )
        elif self.force_interpretation is not None:
            raise FluidX3DError(
                "force_interpretation is only valid when importing force"
            )
        phi_source = next(
            (source for source in self.fields if source.field_id == "phi"),
            None,
        )
        if phi_source is not None and phi_source.value_scale != 1.0:
            raise FluidX3DError(
                "field 'phi' value_scale must be 1.0 so interfaceValue=0.5 "
                "retains its physical meaning"
            )
        if len(self.brick_size) != 3 or any(
            value < 1 or value > 65535 for value in self.brick_size
        ):
            raise FluidX3DError(
                f"brick_size must contain three integers in [1, 65535]; got "
                f"{self.brick_size}"
            )
        for label, value in (
            ("source_time_step", self.source_time_step),
            ("length_scale", self.length_scale),
            ("velocity_scale", self.velocity_scale),
        ):
            number = float(value)
            if not math.isfinite(number) or number <= 0:
                raise FluidX3DError(f"{label} must be finite and positive; got {value}")
            object.__setattr__(self, label, number)
        for label, value in (
            ("length_unit", self.length_unit),
            ("time_unit", self.time_unit),
            ("mass_unit", self.mass_unit),
            ("temperature_unit", self.temperature_unit),
            ("solver_method", self.solver_method),
        ):
            if not isinstance(value, str) or not value:
                raise FluidX3DError(f"{label} must be a non-empty string")
        velocity_source = next(
            source for source in self.fields if source.field_id == "U"
        )
        expected_velocity_unit = f"{self.length_unit}/{self.time_unit}"
        if (
            velocity_source.unit is not None
            and velocity_source.unit != expected_velocity_unit
        ):
            raise FluidX3DError(
                f"field 'U' unit must be {expected_velocity_unit!r}; set base "
                "units and velocity_scale instead so timeline sampling remains "
                "dimensionally valid"
            )
        for label, value in (
            ("solver_version", self.solver_version),
            ("solver_commit", self.solver_commit),
            ("solver_configuration", self.solver_configuration),
            ("source_case", self.source_case),
        ):
            if value is not None and (not isinstance(value, str) or not value):
                raise FluidX3DError(f"{label} must be non-empty when present")
        if self.float_type not in ("float16", "float32"):
            raise FluidX3DError(
                f"float_type must be 'float16' or 'float32'; got {self.float_type!r}"
            )
        codec_id_from_name(self.codec)
        if self.level is not None and self.codec != "zlib":
            raise FluidX3DError(
                "compression level is supported only for zlib; the selected "
                f"codec {self.codec!r} ignores it"
            )
        if self.level is not None and (
            not isinstance(self.level, int)
            or isinstance(self.level, bool)
            or not 0 <= self.level <= 9
        ):
            raise FluidX3DError("zlib compression level must be an integer in [0, 9]")
        bits = int(self.excluded_flag_bits)
        if bits < 0 or bits > 0xFF:
            raise FluidX3DError(
                f"excluded_flag_bits must be in [0, 255]; got {bits}"
            )
        object.__setattr__(self, "excluded_flag_bits", bits)
        if self.max_payload_bytes is not None and self.max_payload_bytes < 0:
            raise FluidX3DError("max_payload_bytes must be non-negative")
        self._reject_output_containing_sources()

    def _reject_output_containing_sources(self) -> None:
        output = self.output.resolve(strict=False)
        sources = [path for field in self.fields for path in field.files]
        sources.extend(self.flag_files)
        for source in sources:
            resolved = source.resolve(strict=False)
            if resolved == output or output in resolved.parents:
                raise FluidX3DError(
                    f"input {source} is inside output {self.output}; replacing the "
                    "case would delete its own source data"
                )


@dataclass(frozen=True)
class _Sequence:
    files: dict[int, Path]


@dataclass(frozen=True)
class _Grid:
    dimensions: tuple[int, int, int]
    vtk_origin: tuple[float, float, float]
    vtk_spacing: tuple[float, float, float]


def _filename_step(path: Path) -> int:
    match = _STEP_PATTERN.search(path.name)
    if match is None:
        raise FluidX3DError(
            f"{path}: filename must end in '-<solverStep>.vtk' or "
            "'_<solverStep>.vtk' so source cadence is not guessed"
        )
    return int(match.group(1), 10)


def _step(path: Path, *, offset: int) -> int:
    step = _filename_step(path) + offset
    if step > _MAX_SAFE_STEP:
        raise FluidX3DError(
            f"{path}: solver step {step} cannot be represented exactly by the "
            "CFDViz JSON/Unreal timeline"
        )
    return step


def _sequence(
    files: Sequence[Path],
    *,
    label: str,
    step_offset: int,
) -> _Sequence:
    filename_steps = [_filename_step(path) for path in files]
    if any(
        right < left
        for left, right in zip(filename_steps, filename_steps[1:])
    ) or (
        filename_steps
        and max(filename_steps) - min(filename_steps)
        > _FILENAME_STEP_CYCLE // 2
    ):
        raise FluidX3DError(
            f"{label} may span FluidX3D's nine-digit filename rollover; a single "
            "solver_step_offset cannot reconstruct that chronology. Convert each "
            "billion-step epoch separately with its matching offset."
        )

    indexed: dict[int, Path] = {}
    for path in files:
        step = _step(path, offset=step_offset)
        if step in indexed:
            raise FluidX3DError(
                f"{label} has two files for solver step {step}: "
                f"{indexed[step]} and {path}"
            )
        indexed[step] = path
    if not indexed:
        raise FluidX3DError(f"{label} has no VTK files")
    return _Sequence(files=indexed)


def _ordered_sources(
    parameters: FluidX3DImportParameters,
) -> tuple[list[FluidX3DFieldSource], dict[str, _Sequence], _Sequence | None, list[int]]:
    sources = sorted(
        parameters.fields,
        key=lambda source: (source.field_id != "U", source.field_id),
    )
    sequences = {
        source.field_id: _sequence(
            source.files,
            label=f"field {source.field_id}",
            step_offset=parameters.solver_step_offset,
        )
        for source in sources
    }
    velocity_steps = set(sequences["U"].files)
    for source in sources:
        steps = set(sequences[source.field_id].files)
        if steps != velocity_steps:
            raise FluidX3DError(
                f"field {source.field_id} steps {sorted(steps)} do not match "
                f"U steps {sorted(velocity_steps)}"
            )
    flags = None
    if parameters.flag_files:
        flags = _sequence(
            parameters.flag_files,
            label="flag field",
            step_offset=parameters.solver_step_offset,
        )
        if set(flags.files) != velocity_steps:
            raise FluidX3DError(
                f"flag field steps {sorted(flags.files)} do not match "
                f"U steps {sorted(velocity_steps)}"
            )
    return sources, sequences, flags, sorted(velocity_steps)


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
    return (
        tuple(source_for_canonical),
        tuple(signs),
    )


def _grid_from(header: VTKLegacyHeader, source_axes: Sequence[str]) -> _Grid:
    source_for_canonical, signs = _axis_mapping(source_axes)
    dimensions: list[int] = []
    origin: list[float] = []
    spacing: list[float] = []
    for canonical_index, source_index in enumerate(source_for_canonical):
        count = header.dimensions[source_index]
        delta = header.spacing[source_index]
        source_origin = header.origin[source_index]
        sign = signs[canonical_index]
        dimensions.append(count)
        spacing.append(delta)
        origin.append(
            source_origin
            if sign > 0
            else -(source_origin + (count - 1) * delta)
        )
    return _Grid(tuple(dimensions), tuple(origin), tuple(spacing))


def _transform_values(
    values: np.ndarray,
    *,
    source_axes: Sequence[str],
    vector: bool,
) -> np.ndarray:
    source_for_canonical, signs = _axis_mapping(source_axes)
    mapped = np.asarray(values).transpose((*source_for_canonical, 3))
    for canonical_index, sign in enumerate(signs):
        if sign < 0:
            mapped = np.flip(mapped, axis=canonical_index)
    if not vector:
        return mapped.copy(order="C")

    transformed = np.empty_like(mapped)
    for canonical_index, source_index in enumerate(source_for_canonical):
        if signs[canonical_index] > 0:
            transformed[..., canonical_index] = mapped[..., source_index]
        else:
            transformed[..., canonical_index] = -mapped[..., source_index]
    return transformed


def _same_grid(left: _Grid, right: _Grid) -> bool:
    return (
        left.dimensions == right.dimensions
        and np.allclose(left.vtk_origin, right.vtk_origin, rtol=0.0, atol=1e-12)
        and np.allclose(left.vtk_spacing, right.vtk_spacing, rtol=0.0, atol=1e-12)
    )


def _read(
    path: Path,
    *,
    parameters: FluidX3DImportParameters,
) -> tuple[VTKLegacyHeader, np.ndarray]:
    try:
        header, values = read_structured_points(
            path,
            max_payload_bytes=parameters.max_payload_bytes,
        )
    except VTKError as exc:
        raise FluidX3DError(str(exc)) from exc
    expected_title = f"FluidX3D {path.name}"
    if header.title != expected_title:
        raise FluidX3DError(
            f"{path}: VTK title {header.title!r} does not match the FluidX3D "
            f"writer signature {expected_title!r}"
        )
    return header, values


def _preset(source: FluidX3DFieldSource) -> _FieldPreset:
    return _PRESETS[source.field_id]


def _check_field_header(
    path: Path,
    header: VTKLegacyHeader,
    source: FluidX3DFieldSource,
) -> None:
    expected = len(_preset(source).components)
    if header.component_count != expected:
        raise FluidX3DError(
            f"{path}: field {source.field_id!r} needs {expected} component(s), "
            f"but VTK SCALARS declares {header.component_count}"
        )
    if header.scalar_type not in ("float", "double"):
        raise FluidX3DError(
            f"{path}: field {source.field_id!r} must be floating-point, got "
            f"VTK type {header.scalar_type!r}"
        )


def _source_precision(scalar_type: str) -> str:
    return "float64" if scalar_type == "double" else "float32"


def _unit(source: FluidX3DFieldSource, parameters: FluidX3DImportParameters) -> str:
    if source.unit is not None:
        return source.unit
    unit_kind = _preset(source).unit_kind
    if unit_kind == "velocity":
        return f"{parameters.length_unit}/{parameters.time_unit}"
    if unit_kind == "density":
        return f"{parameters.mass_unit}/{parameters.length_unit}3"
    if unit_kind == "temperature":
        return parameters.temperature_unit
    if unit_kind == "force":
        return (
            f"{parameters.mass_unit}*{parameters.length_unit}/"
            f"{parameters.time_unit}2"
        )
    return "1"


def _effective_scale(
    source: FluidX3DFieldSource,
    parameters: FluidX3DImportParameters,
) -> float:
    scale = source.value_scale
    if source.field_id == "U":
        scale *= parameters.velocity_scale
    return scale


def _field_mask(
    source: FluidX3DFieldSource,
    *,
    flags: np.ndarray | None,
    grid_mask: np.ndarray,
    force_interpretation: str | None,
) -> np.ndarray:
    mask = grid_mask.copy()
    if flags is None:
        return mask
    solid = (flags & _BOUNDARY_MASK) == _SOLID_FLAG
    gas = (flags & _SURFACE_MASK) == _GAS_FLAG
    if source.field_id == "force":
        assert force_interpretation is not None
        return mask & (solid if force_interpretation == "boundary" else ~solid & ~gas)
    preset = _preset(source)
    if preset.invalid_on_solid:
        mask &= ~solid
    if preset.invalid_on_gas:
        mask &= ~gas
    return mask


def _stored_values(
    source_values: np.ndarray,
    *,
    source: FluidX3DFieldSource,
    mask: np.ndarray,
    parameters: FluidX3DImportParameters,
    path: Path,
    step: int,
) -> np.ndarray:
    source_array = np.asarray(source_values)
    target_dtype = np.dtype("<f2" if parameters.float_type == "float16" else "<f4")
    scale = _effective_scale(source, parameters)
    same_storage = source_array.dtype == target_dtype
    stored = np.empty(source_array.shape, dtype=target_dtype, order="C")

    for z_index in range(source_array.shape[2]):
        source_slice = source_array[:, :, z_index, :]
        active = mask[:, :, z_index]
        if same_storage and scale == 1.0:
            scaled = source_slice
            stored_slice = source_slice.copy(order="C")
        else:
            with np.errstate(over="ignore", invalid="ignore"):
                scaled = source_slice.astype(np.float64) * scale
                stored_slice = scaled.astype(target_dtype)
            if same_storage:
                source_nan = np.isnan(source_slice)
                stored_slice[source_nan] = source_slice[source_nan]

        if np.isinf(scaled[active]).any():
            raise FluidX3DError(
                f"{path.name}: solver step {step} field {source.field_id!r} "
                "contains infinity after scaling"
            )
        if source.field_id == "phi":
            phase = scaled[..., 0][active]
            finite = phase[np.isfinite(phase)]
            if finite.size and (
                float(finite.min()) < 0.0 or float(finite.max()) > 1.0
            ):
                raise FluidX3DError(
                    f"{path.name}: solver step {step} field 'phi' is declared as "
                    "a volume fraction but active values fall outside [0, 1]"
                )
        overflow = np.isfinite(scaled) & ~np.isfinite(stored_slice)
        overflow &= active[..., None]
        if overflow.any():
            flat = int(np.argmax(overflow.reshape(-1)))
            raise FluidX3DError(
                f"{path.name}: solver step {step} field {source.field_id!r} "
                f"value {scaled.reshape(-1)[flat]!r} overflows "
                f"{parameters.float_type} storage"
            )
        stored_slice[~active] = np.nan
        stored[:, :, z_index, :] = stored_slice
    return stored


def _update_digest(
    digest: Any,
    *,
    label: str,
    step: int,
    header: VTKLegacyHeader,
    values: np.ndarray,
) -> None:
    metadata = {
        "label": label,
        "step": step,
        "dimensions": header.dimensions,
        "origin": header.origin,
        "spacing": header.spacing,
        "scalarType": header.scalar_type,
        "componentCount": header.component_count,
    }
    digest.update(dump_json(metadata, indent=None).encode("utf-8"))
    contiguous = np.ascontiguousarray(values)
    digest.update(memoryview(contiguous).cast("B"))


def _case_id(digest: Any) -> str:
    return str(uuid.uuid5(_CASE_NAMESPACE, digest.hexdigest()))


def _tool_version() -> str:
    # Local import avoids a package-initialisation cycle when this module is
    # re-exported from cfdviz.__init__.
    from . import __version__

    return __version__


def _field_entry(
    *,
    source: FluidX3DFieldSource,
    numeric_id: int,
    statistics: StatisticsAccumulator,
    solver_precision: str,
    parameters: FluidX3DImportParameters,
) -> dict[str, Any]:
    preset = _preset(source)
    components = list(preset.components)
    entry: dict[str, Any] = {
        "numericId": numeric_id,
        "id": source.field_id,
        "name": preset.name,
        "description": preset.description,
        "semantic": preset.semantic,
        "kind": "vector" if len(components) > 1 else "scalar",
        "components": components,
        "componentCount": len(components),
        "dataType": parameters.float_type,
        "association": "cell",
        "grid": _GRID_ID,
        "unit": _unit(source, parameters),
        "solverPrecision": solver_precision,
        "temporalInterpolation": "linear",
        "storage": {
            "type": "bricked-volume",
            "codec": parameters.codec,
            "brickSize": list(parameters.brick_size),
            "pathPattern": f"frames/{{frame:06d}}/{source.field_id}.cvf",
        },
        "display": {
            "defaultComponent": (
                "magnitude" if len(components) > 1 else components[0]
            ),
            "defaultColorMap": preset.colormap,
            "defaultRangeMode": "global",
        },
    }
    if parameters.level is not None and parameters.codec != "none":
        entry["storage"]["level"] = parameters.level
    if preset.phase is not None:
        entry["phase"] = dict(preset.phase)
    declared_statistics = statistics.to_manifest()
    if declared_statistics is not None:
        entry["statistics"] = declared_statistics
    return entry


def _mask_entry(
    *,
    statistics: StatisticsAccumulator,
    parameters: FluidX3DImportParameters,
) -> dict[str, Any]:
    entry: dict[str, Any] = {
        "numericId": 1,
        "id": _MASK_ID,
        "name": "Valid cell mask",
        "description": (
            "1 for imported lattice cells and 0 only for cells excluded by the "
            "configured global FluidX3D flag bitmask; field-specific undefined "
            "values are stored as NaN."
        ),
        "semantic": "mask",
        "kind": "scalar",
        "components": ["valid"],
        "componentCount": 1,
        "dataType": "uint8",
        "association": "cell",
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
    declared_statistics = statistics.to_manifest()
    if declared_statistics is not None:
        entry["statistics"] = declared_statistics
    return entry


def _manifest(
    *,
    parameters: FluidX3DImportParameters,
    sources: Sequence[FluidX3DFieldSource],
    steps: Sequence[int],
    times: Sequence[float],
    grid: _Grid,
    case_id: str,
    field_statistics: dict[str, StatisticsAccumulator],
    mask_statistics: StatisticsAccumulator,
    solver_precision: dict[str, str],
    quality: dict[str, Any],
    max_speed: float,
) -> dict[str, Any]:
    spacing = [value * parameters.length_scale for value in grid.vtk_spacing]
    origin = [
        (grid.vtk_origin[index] - 0.5 * grid.vtk_spacing[index])
        * parameters.length_scale
        for index in range(3)
    ]
    solver: dict[str, str] = {
        "name": "FluidX3D",
        "method": parameters.solver_method,
    }
    for key, value in (
        ("version", parameters.solver_version),
        ("commit", parameters.solver_commit),
        ("configuration", parameters.solver_configuration),
    ):
        if value is not None:
            solver[key] = value

    timeline: dict[str, Any] = {
        "frameCount": len(times),
        "times": list(times),
        "steps": list(steps),
        "defaultInterpolation": "linear",
    }
    if len(steps) > 1:
        strides = [right - left for left, right in zip(steps, steps[1:])]
        if len(set(strides)) == 1:
            stride = strides[0]
            displacement = (
                max_speed
                * parameters.source_time_step
                * stride
                / min(spacing)
            )
            timeline["sampling"] = {
                "sourceTimeStep": parameters.source_time_step,
                "storedStepStride": stride,
                "maxFeatureDisplacementCells": displacement,
            }

    fields: list[dict[str, Any]] = [
        _mask_entry(statistics=mask_statistics, parameters=parameters)
    ]
    for numeric_id, source in enumerate(sources, start=2):
        fields.append(
            _field_entry(
                source=source,
                numeric_id=numeric_id,
                statistics=field_statistics[source.field_id],
                solver_precision=solver_precision[source.field_id],
                parameters=parameters,
            )
        )

    notes = [
        "FluidX3D legacy VTK 3.0 STRUCTURED_POINTS payloads were decoded from "
        "big-endian component-interleaved storage.",
        "FluidX3D VTK sample positions were interpreted as lattice cell centres; "
        "the CFDViz grid corner is VTK origin minus half a spacing.",
        "The VTK SCALARS array name was not used as physical field identity; "
        "field identity came from the explicit converter mapping.",
        f"VTK coordinates and spacing were multiplied by lengthScale="
        f"{parameters.length_scale!r} into units.length={parameters.length_unit!r}.",
        f"Velocity values were multiplied by velocityScale="
        f"{parameters.velocity_scale!r} into unit "
        f"{parameters.length_unit}/{parameters.time_unit}.",
        f"One solver step was declared as {parameters.source_time_step!r} "
        f"{parameters.time_unit}; solverStepOffset="
        f"{parameters.solver_step_offset} was added to filename suffixes to "
        "account explicitly for FluidX3D's nine-digit default filenames.",
        f"FluidX3D sourceUnits={parameters.source_units!r} was declared explicitly; "
        "the VTK header does not record whether SI conversion was enabled.",
        f"Source axes {parameters.source_axes!r} were transformed into canonical "
        "right-handed +X-forward, +Z-up coordinates.",
    ]
    if parameters.flag_files:
        notes.append(
            f"validMask excludes only cells where FluidX3D flags & "
            f"0x{parameters.excluded_flag_bits:02X} is nonzero. Each physical "
            "field additionally stores NaN where its FluidX3D preset is undefined "
            "(for example velocity in exact solid and exact gas cells)."
        )
    else:
        notes.append(
            "No FluidX3D flags sequence was supplied; the caller explicitly "
            "declared assumeAllCellsValid and validMask marks every lattice cell "
            "active."
        )
    for source in sources:
        if source.value_scale != 1.0:
            notes.append(
                f"Field {source.field_id!r} was multiplied by explicit "
                f"valueScale={source.value_scale!r}."
            )
    if parameters.force_interpretation == "boundary":
        notes.append(
            "FluidX3D force was explicitly interpreted as boundary force; force "
            "is retained only on exact solid cells and stored as NaN elsewhere."
        )
    elif parameters.force_interpretation == "volume":
        notes.append(
            "FluidX3D force was explicitly interpreted as volume force; force is "
            "retained only on active fluid cells and stored as NaN in exact solid "
            "and exact gas cells."
        )

    provenance: dict[str, Any] = {
        "sourceType": "external-solver",
        "generatorCommand": "python -m cfdviz import-fluidx3d",
        "generatorVersion": _tool_version(),
        "exportCommand": "write_device_to_vtk",
        "notes": notes,
    }
    if parameters.source_case is not None:
        provenance["sourceCase"] = parameters.source_case
    if parameters.solver_commit is not None:
        provenance["sourceRevision"] = parameters.solver_commit

    tags = ["external-solver", "FluidX3D"]
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
                "External FluidX3D solver output converted for visualization; "
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
            "origin": origin,
        },
        "timeline": timeline,
        "grids": [
            {
                "id": _GRID_ID,
                "name": "FluidX3D lattice",
                "type": "uniform-cartesian",
                "dimensions": list(grid.dimensions),
                "origin": origin,
                "spacing": spacing,
                "maskField": _MASK_ID,
            }
        ],
        "fields": fields,
        "derivedFields": [
            {
                "id": "velocityMagnitude",
                "name": "Velocity magnitude",
                "expression": "mag(U)",
                "unit": _unit(next(source for source in sources if source.field_id == "U"), parameters),
                "components": ["magnitude"],
                "componentCount": 1,
            }
        ],
        "qualityMetrics": quality,
        "provenance": provenance,
    }


def import_fluidx3d_case(parameters: FluidX3DImportParameters) -> Path:
    """Convert explicitly identified FluidX3D VTK sequences to CFDViz 1.1."""
    sources, sequences, flag_sequence, steps = _ordered_sources(parameters)
    times: list[float] = []
    for step in steps:
        path = sequences["U"].files[step]
        try:
            time = float(step) * parameters.source_time_step
        except OverflowError as exc:
            raise FluidX3DError(
                f"{path}: solver step {step} and source_time_step cannot produce "
                "a representable timeline time"
            ) from exc
        if not math.isfinite(time):
            raise FluidX3DError(
                f"{path}: solver step {step} and source_time_step produce a "
                "non-finite timeline time"
            )
        times.append(time)
    if any(right <= left for left, right in zip(times, times[1:])):
        raise FluidX3DError("converted timeline times are not strictly increasing")

    codec = codec_id_from_name(parameters.codec)
    field_statistics = {
        source.field_id: StatisticsAccumulator(len(_preset(source).components))
        for source in sources
    }
    mask_statistics = StatisticsAccumulator(1)
    solver_precision: dict[str, str] = {}
    quality: QualityAccumulator | None = None
    reference_grid: _Grid | None = None
    max_speed = 0.0
    digest = hashlib.sha256()
    digest.update(
        dump_json(
            {
                "name": parameters.name,
                "steps": steps,
                "sourceTimeStep": parameters.source_time_step,
                "solverStepOffset": parameters.solver_step_offset,
                "sourceUnits": parameters.source_units,
                "sourceAxes": parameters.source_axes,
                "solverMethod": parameters.solver_method,
                "lengthScale": parameters.length_scale,
                "velocityScale": parameters.velocity_scale,
                "units": [
                    parameters.length_unit,
                    parameters.time_unit,
                    parameters.mass_unit,
                    parameters.temperature_unit,
                ],
                "floatType": parameters.float_type,
                "excludedFlagBits": parameters.excluded_flag_bits,
                "forceInterpretation": parameters.force_interpretation,
                "fields": [
                    {
                        "id": source.field_id,
                        "valueScale": source.value_scale,
                        "unit": _unit(source, parameters),
                    }
                    for source in sources
                ],
            },
            indent=None,
        ).encode("utf-8")
    )

    try:
        with staged_output(parameters.output, force=parameters.force) as root:
            for frame, (step, time) in enumerate(zip(steps, times)):
                flags: np.ndarray | None = None
                grid_mask: np.ndarray | None = None
                flag_grid: _Grid | None = None
                flag_path: Path | None = None
                if flag_sequence is not None:
                    flag_path = flag_sequence.files[step]
                    flag_header, raw_flag_values = _read(
                        flag_path,
                        parameters=parameters,
                    )
                    if (
                        flag_header.component_count != 1
                        or flag_header.scalar_type != "unsigned_char"
                    ):
                        raise FluidX3DError(
                            f"{flag_path}: FluidX3D flags must be one "
                            "unsigned_char component"
                        )
                    flag_grid = _grid_from(flag_header, parameters.source_axes)
                    if reference_grid is not None and not _same_grid(
                        flag_grid, reference_grid
                    ):
                        raise FluidX3DError(
                            f"{flag_path.name} grid does not match the U grid"
                        )
                    flag_values = _transform_values(
                        raw_flag_values,
                        source_axes=parameters.source_axes,
                        vector=False,
                    )
                    flags = flag_values[..., 0].astype(np.uint8, copy=False)
                    grid_mask = (
                        flags & parameters.excluded_flag_bits
                    ) == 0
                    _update_digest(
                        digest,
                        label="flags",
                        step=step,
                        header=flag_header,
                        values=raw_flag_values,
                    )

                for numeric_id, source in enumerate(sources, start=2):
                    path = sequences[source.field_id].files[step]
                    header, raw_source_values = _read(path, parameters=parameters)
                    _check_field_header(path, header, source)
                    current_grid = _grid_from(header, parameters.source_axes)
                    if reference_grid is None:
                        reference_grid = current_grid
                        quality = QualityAccumulator(
                            reference_grid.dimensions,
                            tuple(
                                value * parameters.length_scale
                                for value in reference_grid.vtk_spacing
                            ),
                        )
                        if (
                            grid_mask is not None
                            and grid_mask.shape != reference_grid.dimensions
                        ):
                            raise FluidX3DError(
                                f"{path.name} grid does not match the flags grid"
                            )
                        if flag_grid is not None and not _same_grid(
                            flag_grid, reference_grid
                        ):
                            assert flag_path is not None
                            raise FluidX3DError(
                                f"{flag_path.name} grid does not match the U grid"
                            )
                    elif not _same_grid(current_grid, reference_grid):
                        raise FluidX3DError(
                            f"{path.name} grid does not match the U grid"
                        )
                    if grid_mask is None:
                        grid_mask = np.ones(reference_grid.dimensions, dtype=bool)
                    elif grid_mask.shape != reference_grid.dimensions:
                        raise FluidX3DError(
                            f"{path.name} grid does not match the flags grid"
                        )

                    precision = _source_precision(header.scalar_type)
                    previous_precision = solver_precision.get(source.field_id)
                    if previous_precision is not None and precision != previous_precision:
                        raise FluidX3DError(
                            f"field {source.field_id!r} changes solver precision "
                            f"from {previous_precision} to {precision} at {path}"
                        )
                    solver_precision[source.field_id] = precision
                    source_values = _transform_values(
                        raw_source_values,
                        source_axes=parameters.source_axes,
                        vector=len(_preset(source).components) == 3,
                    )
                    field_mask = _field_mask(
                        source,
                        flags=flags,
                        grid_mask=grid_mask,
                        force_interpretation=parameters.force_interpretation,
                    )
                    stored = _stored_values(
                        source_values,
                        source=source,
                        mask=field_mask,
                        parameters=parameters,
                        path=path,
                        step=step,
                    )
                    field_statistics[source.field_id].add(stored, field_mask)
                    _update_digest(
                        digest,
                        label=source.field_id,
                        step=step,
                        header=header,
                        values=raw_source_values,
                    )

                    if source.field_id == "U":
                        assert quality is not None
                        quality.add(stored, field_mask)
                        for z_index in range(stored.shape[2]):
                            with np.errstate(invalid="ignore"):
                                selected = stored[:, :, z_index][
                                    field_mask[:, :, z_index]
                                ].astype(np.float64)
                            selected = selected[
                                np.isfinite(selected).all(axis=1)
                            ]
                            if selected.size:
                                speed = np.sqrt((selected * selected).sum(axis=1))
                                max_speed = max(max_speed, float(speed.max()))

                    write_cvf(
                        root / f"frames/{frame:06d}/{source.field_id}.cvf",
                        values=stored,
                        dtype=parameters.float_type,
                        association="cell",
                        codec=codec,
                        level=parameters.level,
                        brick_size=parameters.brick_size,
                        frame_index=frame,
                        field_numeric_id=numeric_id,
                        simulation_time=time,
                        dimensions=reference_grid.dimensions,
                        mask=field_mask,
                    )

                assert reference_grid is not None
                assert quality is not None
                assert grid_mask is not None
                stored_mask = grid_mask.astype(np.uint8)[..., None]
                mask_statistics.add(stored_mask, None)
                write_cvf(
                    root / f"frames/{frame:06d}/validMask.cvf",
                    values=stored_mask,
                    dtype="uint8",
                    association="cell",
                    codec=codec_id_from_name("none"),
                    brick_size=parameters.brick_size,
                    frame_index=frame,
                    field_numeric_id=1,
                    simulation_time=time,
                    dimensions=reference_grid.dimensions,
                )

            assert reference_grid is not None
            assert quality is not None
            quality_manifest = quality.to_manifest(
                frame_count=len(steps),
                grid_id=_GRID_ID,
                velocity_field="U",
            )
            manifest = _manifest(
                parameters=parameters,
                sources=sources,
                steps=steps,
                times=times,
                grid=reference_grid,
                case_id=_case_id(digest),
                field_statistics=field_statistics,
                mask_statistics=mask_statistics,
                solver_precision=solver_precision,
                quality=quality_manifest,
                max_speed=max_speed,
            )
            (root / "manifest.json").write_text(
                dump_json(manifest) + "\n",
                encoding="utf-8",
            )
            write_known_values(root)
            report = validate_case(root)
            if not report.ok:
                raise FluidX3DError(
                    "generated FluidX3D case failed validation:\n" + report.render()
                )
    except FluidX3DError:
        raise
    except (ConversionError, CVFError, VTKError, OSError, ValueError) as exc:
        raise FluidX3DError(str(exc)) from exc

    return parameters.output
