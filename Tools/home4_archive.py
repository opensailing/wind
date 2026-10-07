#!/usr/bin/env python3
"""Inspect safe HOME4 NPZ archives; convert explicitly mapped snapshots to recording v3 or VTI.

No pickle, solver execution, spatial reconstruction or replacement CFD data.
NumPy is the only non-standard dependency. Native recording v3 requires physical
metres/seconds; VTI may retain explicitly identified lattice units. All axis/unit
conventions must come from CLI arguments or an explicit run_spec.archive block.
"""
from __future__ import annotations

import argparse
import contextlib
import ctypes
from dataclasses import dataclass
import hashlib
import io
import json
import math
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import sys
import tempfile
from typing import Any
import xml.etree.ElementTree as ET
import zipfile
import zlib

import numpy as np


class ArchiveError(ValueError):
    """A bounded archive or mapping contract is invalid; no output was published."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ArchiveError(message)


@dataclass(frozen=True)
class Limits:
    max_members: int = 128
    max_uncompressed_bytes: int = 512 * 1024 * 1024
    max_archive_bytes: int = 512 * 1024 * 1024
    max_header_bytes: int = 8192
    max_values: int = 16_000_000
    max_compression_ratio: int = 4096
    max_metadata_chars: int = 16384


@dataclass(frozen=True)
class Member:
    name: str
    dtype: np.dtype
    shape: tuple[int, ...]
    fortran_order: bool
    header_bytes: int
    data_bytes: int


class SafeNPZ:
    """Validated zip/NPY container. Arrays are read without pickle and CRC checked."""

    def __init__(self, path: Path | str, limits: Limits = Limits()):
        self.path, self.limits = Path(path), limits
        self.members: dict[str, Member] = {}
        self._file = None
        self._zip = None
        try:
            require(self.path.is_file(), "Archive must be a regular file")
            self._file = self.path.open("rb")
            before = os.fstat(self._file.fileno())
            require(stat.S_ISREG(before.st_mode), "Archive must be a regular file")
            require(0 < before.st_size <= limits.max_archive_bytes, "Archive compressed size exceeds limit")
            self._stat = (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
            self.sha256 = hashlib.file_digest(self._file, "sha256").hexdigest()
            self._file.seek(0)
            self._zip = zipfile.ZipFile(self._file)
            infos = self._zip.infolist()
            require(0 < len(infos) <= limits.max_members, "Archive member count exceeds limit")
            require(sum(i.file_size for i in infos) <= limits.max_uncompressed_bytes, "Archive expands beyond byte limit")
            seen: set[str] = set()
            for info in infos:
                name = info.filename
                require(re.fullmatch(r"[A-Za-z0-9_.-]{1,120}\.npy", name) is not None,
                        "Archive member names must be flat bounded .npy identifiers")
                key = name[:-4]
                require(key not in (".", "..") and key.lower() not in seen, "Duplicate archive member names")
                seen.add(key.lower())
                require(not info.flag_bits & 1, "Encrypted archives are unsupported")
                require(info.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED), "Unsupported zip compression")
                require(not stat.S_ISLNK(info.external_attr >> 16), "Archive symlinks are unsupported")
                require(info.file_size >= 10 and info.compress_size > 0, "Empty or truncated NPY member")
                require(info.file_size <= max(1, info.compress_size) * limits.max_compression_ratio,
                        "Archive compression ratio exceeds safety limit")
                with self._zip.open(info) as stream:
                    version = np.lib.format.read_magic(stream)
                    require(version in ((1, 0), (2, 0)), "Unsupported NPY header version")
                    length_bytes = 2 if version == (1, 0) else 4
                    length_raw = stream.read(length_bytes)
                    require(len(length_raw) == length_bytes, "Truncated NPY header length")
                    header_length = int.from_bytes(length_raw, "little")
                    require(0 < header_length <= limits.max_header_bytes, "NPY header exceeds byte limit")
                    header = stream.read(header_length)
                    require(len(header) == header_length, "Truncated NPY header")
                    reader = np.lib.format.read_array_header_1_0 if version == (1, 0) else np.lib.format.read_array_header_2_0
                    shape, order, dtype = reader(io.BytesIO(length_raw + header), max_header_size=limits.max_header_bytes)
                    require(dtype.kind in "biufSU" and not dtype.hasobject and dtype.fields is None,
                            "Object, structured or unsupported NPY dtype is forbidden")
                    require(len(shape) <= 8 and all(type(n) is int and 0 <= n <= limits.max_values for n in shape),
                            "Invalid or unbounded NPY shape")
                    count = math.prod(shape)
                    require(count <= limits.max_values, "NPY element count exceeds limit")
                    require(0 < dtype.itemsize <= limits.max_metadata_chars * 4, "NPY dtype item size exceeds limit")
                    data_bytes = count * dtype.itemsize
                    require(stream.tell() + data_bytes == info.file_size, "NPY payload size does not match its header")
                    self.members[key] = Member(key, dtype, tuple(shape), bool(order), stream.tell(), data_bytes)
            self._check_identity()
        except (ArchiveError, OSError, ValueError, EOFError, zipfile.BadZipFile) as exc:
            self.close()
            if isinstance(exc, ArchiveError):
                raise
            raise ArchiveError(f"Invalid NPZ archive: {exc}") from exc

    def _check_identity(self) -> None:
        require(self._file is not None, "Archive is closed")
        current = os.fstat(self._file.fileno())
        require((current.st_dev, current.st_ino, current.st_size, current.st_mtime_ns) == self._stat,
                "Archive changed during import")

    def read(self, name: str) -> np.ndarray:
        require(name in self.members, f"Required archive array is missing: {name}")
        self._check_identity()
        try:
            with self._zip.open(name + ".npy") as stream:
                value = np.load(stream, allow_pickle=False, max_header_size=self.limits.max_header_bytes)
                require(stream.read(1) == b"", "NPY contains unexpected trailing data")
            self._check_identity()
        except (OSError, ValueError, EOFError, zipfile.BadZipFile) as exc:
            raise ArchiveError(f"Cannot read array {name}: {exc}") from exc
        if value.dtype.kind in "biuf":
            require(np.isfinite(value).all(), f"Nonfinite values in {name}")
        return value

    def scalar(self, name: str) -> Any:
        member = self.members.get(name)
        require(member is not None and member.shape == (), f"{name} must be an explicit scalar")
        require(member.data_bytes <= self.limits.max_metadata_chars * 4, f"{name} scalar is too large")
        value = self.read(name).item()
        if isinstance(value, bytes):
            try:
                value = value.decode("utf-8", errors="strict")
            except UnicodeError as exc:
                raise ArchiveError(f"{name} is not UTF-8") from exc
        if isinstance(value, str):
            require(len(value) <= self.limits.max_metadata_chars and not any(ord(c) < 32 and c not in "\n\r\t" for c in value),
                    f"{name} metadata is too long or contains control characters")
        return value

    def close(self) -> None:
        if self._zip is not None:
            self._zip.close()
            self._zip = None
        if self._file is not None:
            self._file.close()
            self._file = None

    def __enter__(self) -> "SafeNPZ":
        return self

    def __exit__(self, *_args: Any) -> None:
        self.close()


def _json_object(pairs: list[tuple[str, Any]]) -> dict:
    result = {}
    for key, value in pairs:
        require(key not in result, "Duplicate JSON metadata key")
        result[key] = value
    return result


def bounded_json(text: str) -> Any:
    require(len(text) <= 1024 * 1024, "run_spec JSON exceeds byte limit")
    try:
        result = json.loads(text, object_pairs_hook=_json_object,
                            parse_constant=lambda value: (_ for _ in ()).throw(ArchiveError("Nonfinite JSON constant")))
    except (ValueError, RecursionError) as exc:
        raise ArchiveError(f"Invalid run_spec JSON: {exc}") from exc
    def walk(value: Any, depth: int = 0) -> None:
        require(depth <= 16, "JSON metadata nesting exceeds limit")
        if isinstance(value, dict):
            require(len(value) <= 256, "JSON metadata object is too large")
            for key, item in value.items():
                require(isinstance(key, str) and len(key) <= 120, "JSON metadata key is too long")
                walk(item, depth + 1)
        elif isinstance(value, list):
            require(len(value) <= 256, "JSON metadata array is too large")
            for item in value:
                walk(item, depth + 1)
        elif isinstance(value, float):
            require(math.isfinite(value), "Nonfinite JSON metadata")
        elif isinstance(value, str):
            require(len(value) <= 16384, "JSON metadata string is too long")
    walk(result)
    return result


def inspect_archive(path: Path | str, limits: Limits = Limits()) -> dict:
    with SafeNPZ(path, limits) as archive:
        report = {"kind": "home4_npz_inspection", "version": 1, "source": str(Path(path).resolve()),
                  "sourceSHA256": archive.sha256, "members": {},
                  "limitations": ["Inspection does not establish solver integration or numerical validation.",
                                   "Array axis and unit conventions are not inferred from dimensions or names."]}
        for name, member in archive.members.items():
            item = {"dtype": str(member.dtype), "shape": list(member.shape), "fortranOrder": member.fortran_order,
                    "dataBytes": member.data_bytes}
            if member.shape == ():
                item["value"] = archive.scalar(name)
            elif member.dtype.kind in "biuf":
                values = archive.read(name)
                item["finite"] = True
                if values.size:
                    item["range"] = [float(values.min()), float(values.max())]
            else:
                item["value"] = None
                item["note"] = "Non-scalar string arrays are not interpreted as provenance metadata."
            report["members"][name] = item
        if "run_spec" in archive.members:
            raw = archive.scalar("run_spec")
            require(isinstance(raw, str), "run_spec must be a scalar JSON string")
            report["runSpec"] = bounded_json(raw)
        return report


@dataclass(frozen=True)
class Mapping:
    axis_order: str
    coordinate_units: str
    velocity_units: str
    dx_m: float | None = None
    dt_s: float | None = None
    rho_ref_kg_m3: float | None = None
    phi_liquid_min: float = 0.5
    time_origin_s: float = 0.0
    metadata_order: str | None = None

    def validate(self, physical: bool = False) -> None:
        require(self.axis_order in ("xyz", "xzy", "yxz", "yzx", "zxy", "zyx"), "Explicit array axis order is required")
        require(self.metadata_order in ("xyz", "array"), "Explicit origin/spacing metadata order (xyz or array) is required")
        require(self.coordinate_units in ("lattice", "physical"), "Explicit origin/spacing units are required")
        require(self.velocity_units in ("lattice", "physical"), "Explicit velocity units are required")
        for value, name in ((self.dx_m, "dx_m"), (self.dt_s, "dt_s"), (self.rho_ref_kg_m3, "rho_ref_kg_m3")):
            require(value is None or (type(value) in (int, float) and math.isfinite(value) and value > 0), f"{name} must be finite positive")
        require(type(self.phi_liquid_min) in (int, float) and math.isfinite(self.phi_liquid_min) and 0 <= self.phi_liquid_min <= 1,
                "Air mask threshold must lie between 0 and 1")
        require(math.isfinite(self.time_origin_s) and self.time_origin_s >= 0, "Physical time origin must be finite nonnegative")
        if physical:
            require(self.dt_s is not None, "Native conversion requires explicit seconds per solver step")
            require(self.coordinate_units == "physical" or self.dx_m is not None, "Native lattice coordinates require explicit metres per cell")
            require(self.velocity_units == "physical" or self.dx_m is not None, "Native lattice velocities require explicit metres per cell")
            require(self.dx_m is None or self.dt_s is None or math.isfinite(self.dx_m / self.dt_s), "Velocity unit scale overflows")

    def as_dict(self) -> dict:
        return {"axisOrder": self.axis_order, "coordinateUnits": self.coordinate_units, "velocityUnits": self.velocity_units,
                "dxMeters": self.dx_m, "dtSeconds": self.dt_s, "densityReferenceKgM3": self.rho_ref_kg_m3,
                "phiLiquidMin": self.phi_liquid_min, "timeOriginSeconds": self.time_origin_s, "metadataOrder": self.metadata_order}


@dataclass
class Snapshot:
    source: Path
    source_sha256: str
    step: int
    origin: np.ndarray
    spacing: np.ndarray
    fields: dict[str, np.ndarray]  # Canonical XYZ axis order; flatten(order='F') is x fastest.
    mapping: Mapping
    run_spec: dict | None
    selected_indices: tuple[np.ndarray, np.ndarray, np.ndarray]
    original_shape: tuple[int, int, int]
    limits: Limits

    @property
    def shape(self) -> tuple[int, int, int]:
        return self.fields["phi"].shape

    @property
    def physical_time(self) -> float | None:
        if self.mapping.dt_s is None:
            return None
        value = self.mapping.time_origin_s + self.step * self.mapping.dt_s
        require(math.isfinite(value), "Physical timestamp overflows")
        return value


def _triple(array: np.ndarray, name: str, positive: bool = False) -> np.ndarray:
    require(array.dtype.kind in "iuf" and array.shape in ((), (3,)), f"{name} must be a numeric scalar or XYZ triple")
    result = np.repeat(array, 3) if array.shape == () else array
    result = _exact_float64(np.asarray(result), name)
    require(np.isfinite(result).all() and (not positive or (result > 0).all()), f"{name} must be finite" + (" positive" if positive else ""))
    return result


def _exact_float64(array: np.ndarray, name: str) -> np.ndarray:
    require(array.dtype.kind in "biuf", f"{name} must have a supported real numeric dtype")
    if array.dtype.kind == "f":
        require(array.dtype.itemsize <= 8, f"{name} source precision exceeds lossless float64 recording support")
    if array.dtype.kind in "iu" and array.size:
        require(int(array.min()) >= -(2**53) and int(array.max()) <= 2**53,
                f"{name} integer source precision exceeds exact float64 support")
    return array.astype(np.float64, copy=True)


def _canonical(array: np.ndarray, order: str) -> np.ndarray:
    return np.transpose(array, tuple(order.index(axis) for axis in "xyz"))


def parse_crop(text: str | None) -> tuple[tuple[int, int], ...] | None:
    if text is None:
        return None
    require(re.fullmatch(r"\d+:\d+,\d+:\d+,\d+:\d+", text) is not None,
            "Crop must be XSTART:XSTOP,YSTART:YSTOP,ZSTART:ZSTOP in original XYZ indices")
    return tuple(tuple(int(n) for n in pair.split(":")) for pair in text.split(","))


def read_snapshot(path: Path | str, mapping: Mapping, *, crop: tuple[tuple[int, int], ...] | None = None,
                  stride: int = 1, limits: Limits = Limits()) -> Snapshot:
    mapping.validate()
    require(type(stride) is int and 1 <= stride <= 1024, "Preview stride must be a positive bounded integer")
    with SafeNPZ(path, limits) as archive:
        arrays = {key: archive.read(key) for key in ("ux", "uy", "uz", "phi", "solid")}
        shape = arrays["phi"].shape
        require(len(shape) == 3 and all(n >= 1 for n in shape), "Snapshot must have a nonempty 3-D lattice")
        for key, value in arrays.items():
            require(value.shape == shape and value.dtype.kind in "biuf", f"Snapshot shape/dtype mismatch: {key}")
        require(((arrays["solid"] == 0) | (arrays["solid"] == 1)).all(), "Solid mask must be explicitly binary")
        raw_step = archive.scalar("iteration")
        require(type(raw_step) is int and 0 <= raw_step <= 2147483647, "iteration must be an original nonnegative int32 solver step")
        origin = _triple(archive.read("origin"), "origin")
        spacing = _triple(archive.read("spacing"), "spacing", positive=True)
        if mapping.metadata_order == "array":
            axes = [mapping.axis_order.index(axis) for axis in "xyz"]
            origin, spacing = origin[axes], spacing[axes]
        for key in tuple(arrays):
            arrays[key] = _canonical(arrays[key], mapping.axis_order)
        canonical_shape = arrays["phi"].shape
        require(crop is None or (len(crop) == 3 and all(type(a) is int and type(b) is int and 0 <= a < b <= canonical_shape[i]
                for i, (a, b) in enumerate(crop))), "Crop falls outside original XYZ lattice extents")
        bounds = crop or tuple((0, n) for n in canonical_shape)
        selected = tuple(np.arange(a, b, stride, dtype=np.int64) for a, b in bounds)
        slices = tuple(slice(a, b, stride) for a, b in bounds)
        for key in tuple(arrays):
            arrays[key] = _exact_float64(arrays[key][slices], key)
        for key in ("p_star", "Pi_h", "Pi_h0", "rho", "nu", "Sxx", "Syy", "Szz", "Sxy", "Sxz", "Syz"):
            if key in archive.members:
                value = archive.read(key)
                require(value.shape == shape and value.dtype.kind in "iuf", f"Optional field shape/dtype mismatch: {key}")
                arrays[key] = _exact_float64(_canonical(value, mapping.axis_order)[slices], key)
        run_spec = None
        if "run_spec" in archive.members:
            raw = archive.scalar("run_spec")
            require(isinstance(raw, str), "run_spec must be a scalar JSON string")
            run_spec = bounded_json(raw)
            require(isinstance(run_spec, dict), "run_spec must be a JSON object")
        archive_spec = (run_spec or {}).get("archive", {})
        require(isinstance(archive_spec, dict), "Original archive conventions must be an object")
        source_units = archive_spec.get("fieldUnits", {})
        require(isinstance(source_units, dict), "Original archive.fieldUnits must be an object")
        for key in sorted(archive.members):
            if key in arrays or not re.fullmatch(r"(?:a[34]_[A-Za-z0-9_]+|[JP][xyz]{1,2}_phi|grad_phi_[xyz]|F_[A-Za-z0-9_]+|tau_fld|sdf)", key):
                continue
            unit = source_units.get(key)
            require(isinstance(unit, str) and 0 < len(unit) <= 128 and unit.strip() and all(ord(c) >= 32 and ord(c) != 127 for c in unit),
                    f"Original {key} requires explicit run_spec.archive.fieldUnits; its solver normalization is unknown")
            value = archive.read(key)
            require(value.shape == shape and value.dtype.kind in "iuf", f"Original advanced field shape/dtype mismatch: {key}")
            arrays[key] = _exact_float64(_canonical(value, mapping.axis_order)[slices], key)
        origin = origin + np.array([a for a, _ in bounds]) * spacing
        spacing = spacing * stride
        require(np.isfinite(origin).all() and np.isfinite(spacing).all(), "Crop/stride coordinates overflow")
        return Snapshot(Path(path).resolve(), archive.sha256, raw_step, origin, spacing, arrays, mapping,
                        run_spec, selected, canonical_shape, limits)


def derivative_fields(snapshot: Snapshot) -> dict[str, np.ndarray]:
    """Central differences of original samples, excluding the entire one-node air/solid halo."""
    if snapshot.shape != snapshot.original_shape:
        original = read_snapshot(snapshot.source, snapshot.mapping, limits=snapshot.limits)
        require(original.source_sha256 == snapshot.source_sha256, "Archive changed before derivative evaluation")
        selection = np.ix_(*snapshot.selected_indices)
        return {key: values[selection].copy() for key, values in derivative_fields(original).items()}
    f, spacing = snapshot.fields, snapshot.spacing
    shape = snapshot.shape
    require(math.prod(shape) <= 2_000_000, "Original derivative grid exceeds the bounded two-million-node workload")
    liquid = (f["phi"] >= snapshot.mapping.phi_liquid_min) & (f["solid"] == 0)
    valid = np.zeros(shape, dtype=bool)
    if min(shape) >= 3:
        inner = (slice(1, -1),) * 3
        good = liquid[inner].copy()
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    neighbor = tuple(slice(0, -2) if d < 0 else slice(2, None) if d > 0 else slice(1, -1)
                                     for d in (dx, dy, dz))
                    good &= liquid[neighbor]
        valid[inner] = good
    gradient = np.zeros((3, 3) + shape, dtype=np.float64)
    for component, key in enumerate(("ux", "uy", "uz")):
        for axis in range(3):
            if shape[axis] < 3:
                continue
            middle = [slice(None)] * 3
            left, right = middle.copy(), middle.copy()
            middle[axis], left[axis], right[axis] = slice(1, -1), slice(None, -2), slice(2, None)
            gradient[(component, axis, *middle)] = (f[key][tuple(right)] - f[key][tuple(left)]) / (2 * spacing[axis])
    omega = np.array([gradient[2, 1] - gradient[1, 2], gradient[0, 2] - gradient[2, 0], gradient[1, 0] - gradient[0, 1]])
    strain = (gradient + gradient.swapaxes(0, 1)) / 2
    rotation = (gradient - gradient.swapaxes(0, 1)) / 2
    outputs = {"derivative_valid": valid.astype(np.float64),
               "vorticity_x": omega[0], "vorticity_y": omega[1], "vorticity_z": omega[2],
               "vorticity_magnitude": np.sqrt(np.sum(omega**2, axis=0)),
               "q": .5 * (np.sum(rotation**2, axis=(0, 1)) - np.sum(strain**2, axis=(0, 1))),
               "divergence": np.trace(gradient, axis1=0, axis2=1),
               "helicity": f["ux"] * omega[0] + f["uy"] * omega[1] + f["uz"] * omega[2]}
    for key, value in outputs.items():
        if key != "derivative_valid":
            value[~valid] = 0  # Explicitly masked values; valid mask must accompany them.
        require(np.isfinite(value).all(), f"Derivative overflow: {key}")
    return outputs


def derive_fields(snapshot: Snapshot, *, derivatives: bool = False, pressure_convention: str | None = None) -> dict[str, np.ndarray]:
    f = snapshot.fields
    result = {"speed": np.sqrt(f["ux"]**2 + f["uy"]**2 + f["uz"]**2)}
    if derivatives:
        result.update(derivative_fields(snapshot))
    if pressure_convention is not None:
        require(pressure_convention == "wb_lattice", "Only explicitly identified WB lattice pressure reconstruction is supported")
        required = ("rho", "p_star", "Pi_h", "Pi_h0")
        require(all(key in f for key in required), "WB pressure needs rho, p_star, Pi_h and Pi_h0 arrays")
        require(snapshot.mapping.velocity_units == "lattice", "WB lattice pressure requires lattice velocity/state conventions")
        result["pressure"] = f["rho"] * (1 / 3) * f["p_star"] + f["Pi_h"] - f["Pi_h0"]
    strain_keys = ("Sxx", "Syy", "Szz", "Sxy", "Sxz", "Syz")
    if all(key in f for key in strain_keys) and "nu" in f:
        require((f["nu"] >= 0).all(), "Stored viscosity must be nonnegative")
        result["dissipation"] = 2 * f["nu"] * (f["Sxx"]**2 + f["Syy"]**2 + f["Szz"]**2 +
                              2 * (f["Sxy"]**2 + f["Sxz"]**2 + f["Syz"]**2))
    for key, value in result.items():
        require(np.isfinite(value).all(), f"Derived field overflow: {key}")
    return result


def native_support_fields(snapshot: Snapshot, stride: int) -> dict[str, np.ndarray]:
    """Conservative original-node support: preview interpolation never skips an unseen solid/air node."""
    require(math.prod(snapshot.original_shape) <= 2_000_000, "Native original mask support exceeds the bounded two-million-node workload")
    if stride == 1:
        return {"solid_support": (snapshot.fields["solid"] == 0).astype(float),
                "liquid_support": ((snapshot.fields["solid"] == 0) & (snapshot.fields["phi"] >= snapshot.mapping.phi_liquid_min)).astype(float)}
    with SafeNPZ(snapshot.source, snapshot.limits) as archive:
        require(archive.sha256 == snapshot.source_sha256, "Archive changed before original mask support evaluation")
        solid = _canonical(archive.read("solid"), snapshot.mapping.axis_order)
        phi = _canonical(archive.read("phi"), snapshot.mapping.axis_order)
        require(solid.shape == snapshot.original_shape and phi.shape == solid.shape and ((solid == 0) | (solid == 1)).all(),
                "Original support masks changed shape or type")
    coords = np.meshgrid(*snapshot.selected_indices, indexing="ij")
    low = [np.maximum(0, c - stride) for c in coords]
    high = [np.minimum(snapshot.original_shape[a], c + stride + 1) for a, c in enumerate(coords)]
    result = {}
    for key, invalid in (("solid_support", solid != 0), ("liquid_support", (solid != 0) | (phi < snapshot.mapping.phi_liquid_min))):
        prefix = np.pad(invalid.astype(np.int64), ((1, 0),) * 3)
        for axis in range(3):
            np.cumsum(prefix, axis=axis, out=prefix)
        counts = np.zeros(snapshot.shape, dtype=np.int64)
        for corner in range(8):
            at = tuple(high[a] if corner & (1 << a) else low[a] for a in range(3))
            counts += (1 if corner.bit_count() % 2 == 1 else -1) * prefix[at]
        result[key] = (counts == 0).astype(float)
    return result


def range_report(snapshot: Snapshot, derived: dict[str, np.ndarray], physical: bool = False) -> dict:
    liquid = (snapshot.fields["phi"] >= snapshot.mapping.phi_liquid_min) & (snapshot.fields["solid"] == 0)
    ranges = {}
    for key, values in {**snapshot.fields, **derived}.items():
        masked = key.startswith(("vorticity", "a3_", "a4_", "S", "F_")) or key in ("q", "helicity", "pressure", "p_star", "Pi_h", "Pi_h0", "divergence", "dissipation")
        mask = liquid.copy() if masked else (snapshot.fields["solid"] == 0)
        if key in ("q", "helicity", "divergence") or key.startswith("vorticity"):
            mask &= derived["derivative_valid"] > 0
        unit, scale = _field_unit(key, snapshot, physical)
        available = values[mask] * scale
        require(np.isfinite(available).all(), "Display range scale overflows")
        if not available.size:
            ranges[key] = {"available": False, "airMaskDefault": masked, "unit": unit}
            continue
        percentile = float(np.percentile(available, 99))
        minimum, maximum = float(available.min()), float(available.max())
        ranges[key] = {"available": True, "minimum": minimum, "maximum": maximum, "firstFramePercentile99": percentile,
                       "clippedAbovePercentile99": int(np.count_nonzero(available > percentile)),
                       "airMaskDefault": masked, "sampleCount": int(available.size), "unit": unit}
    return {"policy": "99th percentile of first selected original frame, held for the sequence",
            "phiLiquidMin": snapshot.mapping.phi_liquid_min, "derivativeExclusion": "one-node stencil touching air, solid or outside the supplied grid",
            "phiOutsideUnitInterval": int(np.count_nonzero((snapshot.fields["phi"] < 0) | (snapshot.fields["phi"] > 1))),
            "ranges": ranges}


def file_digest(path: Path | str) -> str:
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path: Path, value: Any) -> None:
    data = json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n"
    require(len(data.encode("utf-8")) <= 8 * 1024 * 1024, "Output metadata exceeds native size limit")
    path.write_text(data, encoding="utf-8")


@contextlib.contextmanager
def staged_directory(output: Path | str):
    output = Path(output)
    require(not output.exists(), "Choose a new output directory; previous exports are never overwritten")
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix="." + output.name + "-", dir=output.parent))
    try:
        yield stage
        require(not output.exists(), "Output directory appeared during conversion")
        _publish_new_directory(stage, output)
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def _publish_new_directory(stage: Path, output: Path) -> None:
    """Atomic publication with an exclusive destination, including a competing writer race."""
    libc = ctypes.CDLL(None, use_errno=True)
    if sys.platform == "darwin":
        rename = libc.renamex_np
        rename.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint)
        rename.restype = ctypes.c_int
        result = rename(os.fsencode(stage), os.fsencode(output), 4)  # RENAME_EXCL
    elif sys.platform.startswith("linux") and hasattr(libc, "renameat2"):
        rename = libc.renameat2
        rename.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint)
        rename.restype = ctypes.c_int
        result = rename(-100, os.fsencode(stage), -100, os.fsencode(output), 1)  # RENAME_NOREPLACE
    else:
        raise ArchiveError("Atomic exclusive directory publication is unsupported on this platform")
    if result != 0:
        error = ctypes.get_errno()
        raise ArchiveError(f"Atomic output publication failed: {os.strerror(error)}")


def _field_unit(key: str, snapshot: Snapshot, physical: bool) -> tuple[str, float]:
    m = snapshot.mapping
    vel = 1.0
    coord = 1.0
    if physical:
        if m.coordinate_units == "lattice":
            require(m.dx_m is not None, "Missing physical spacing scale")
            coord = m.dx_m
        if m.velocity_units == "lattice":
            require(m.dx_m is not None and m.dt_s is not None, "Missing physical velocity scale")
            vel = m.dx_m / m.dt_s
    coordinate_unit = "m" if physical or m.coordinate_units == "physical" else "lu_length"
    velocity_unit = "m/s" if physical or m.velocity_units == "physical" else "lu_velocity"
    if key in ("ux", "uy", "uz", "speed"):
        return velocity_unit, vel
    if key in ("phi", "solid", "derivative_valid", "solid_support", "liquid_support"):
        return "1", 1.0
    if key.startswith("vorticity") or key == "divergence":
        return ("1/s" if coordinate_unit == "m" and velocity_unit == "m/s" else f"({velocity_unit})/({coordinate_unit})"), vel / coord
    if key == "q":
        return ("1/s2" if coordinate_unit == "m" and velocity_unit == "m/s" else f"(({velocity_unit})/({coordinate_unit}))^2"), (vel / coord)**2
    if key == "helicity":
        return ("m/s2" if coordinate_unit == "m" and velocity_unit == "m/s" else f"({velocity_unit})^2/({coordinate_unit})"), vel**2 / coord
    # Optional solver states remain clearly identified lattice quantities. No physical convention is inferred.
    if key == "pressure":
        if physical:
            require(m.dx_m is not None and m.dt_s is not None and m.rho_ref_kg_m3 is not None,
                    "Physical WB pressure requires dx, dt and reference density")
            return "Pa", m.rho_ref_kg_m3 * (m.dx_m / m.dt_s)**2
        return "lu_pressure", 1.0
    if key == "rho":
        if physical and m.rho_ref_kg_m3 is not None:
            return "kg/m3", m.rho_ref_kg_m3
        return "lu_density", 1.0
    if key == "p_star":
        return "1", 1.0
    if key in ("Pi_h", "Pi_h0"):
        if physical and m.dx_m is not None and m.dt_s is not None and m.rho_ref_kg_m3 is not None:
            return "Pa", m.rho_ref_kg_m3 * (m.dx_m / m.dt_s)**2
        return "lu_pressure", 1.0
    if key == "nu":
        if physical and m.dx_m is not None and m.dt_s is not None:
            return "m2/s", m.dx_m**2 / m.dt_s
        return "cells2/step", 1.0
    if key.startswith("S"):
        if physical and m.dt_s is not None:
            return "1/s", 1 / m.dt_s
        return "1/step", 1.0
    if key == "dissipation":
        if physical and m.dx_m is not None and m.dt_s is not None:
            return "m2/s3", m.dx_m**2 / m.dt_s**3
        return "cells2/step3", 1.0
    source_units = (snapshot.run_spec or {}).get("archive", {}).get("fieldUnits", {})
    if key in source_units:
        # Advanced solver normalization is an explicit original-unit label, with original values retained exactly.
        return source_units[key], 1.0
    raise ArchiveError(f"Unsupported field unit convention: {key}")


def _array_descriptor(path: Path, shape: tuple[int, ...], frames: bool = False) -> dict:
    expected = math.prod(shape) * 8
    require(path.stat().st_size == expected, "Written native array size mismatch")
    result = {"path": path.name, "dtype": "float64", "byteOrder": "little", "shape": list(shape),
              "byteLength": expected, "sha256": file_digest(path)}
    if frames:
        crc = []
        with path.open("rb") as stream:
            for _ in range(shape[0]):
                crc.append(zlib.crc32(stream.read(shape[1] * 8)))
        result["frameCRC32"] = crc
    return result


def _source_ids(snapshot: Snapshot) -> np.ndarray:
    x, y, z = np.meshgrid(*snapshot.selected_indices, indexing="ij")
    nx, ny, _ = snapshot.original_shape
    return (x + nx * (y + ny * z)).ravel(order="F").astype("<i8")


def _coordinates(snapshot: Snapshot, physical: bool) -> np.ndarray:
    coord = snapshot.mapping.dx_m if physical and snapshot.mapping.coordinate_units == "lattice" else 1.0
    axes = [snapshot.origin[i] + np.arange(snapshot.shape[i], dtype=np.float64) * snapshot.spacing[i] for i in range(3)]
    values = np.meshgrid(*axes, indexing="ij")
    result = np.column_stack([a.ravel(order="F") * coord for a in values]).astype("<f8")
    require(np.isfinite(result).all(), "Physical coordinates overflow")
    return result


def _snapshot_sidecar(snapshot: Snapshot, derived: dict[str, np.ndarray], crop: Any, stride: int) -> dict:
    return {"source": str(snapshot.source), "sourceSHA256": snapshot.source_sha256, "iteration": snapshot.step,
            "timeLatticeSteps": snapshot.step, "timePhysicalSeconds": snapshot.physical_time,
            "mapping": snapshot.mapping.as_dict(), "originalDimensionsXYZ": list(snapshot.original_shape),
            "selectedDimensionsXYZ": list(snapshot.shape), "origin": snapshot.origin.tolist(), "spacing": snapshot.spacing.tolist(),
            "cropXYZ": crop, "previewStride": stride, "previewOnly": stride != 1,
            "derivedFields": list(derived), "runSpec": snapshot.run_spec}


def convert_snapshots(sources: list[Path | str], output: Path | str, mapping: Mapping, *,
                      title: str = "HOME4 imported snapshots", source_url: str, attribution: str,
                      crop: tuple[tuple[int, int], ...] | None = None, stride: int = 1, derivatives: bool = False,
                      pressure_convention: str | None = None, limits: Limits = Limits()) -> dict:
    mapping.validate(physical=True)
    require(1 <= len(sources) <= 100000, "Native recording needs 1–100000 original snapshots")
    require(isinstance(title, str) and 0 < len(title) <= 256 and not any(ord(c) < 32 or ord(c) == 127 for c in title), "Invalid native recording title")
    require(isinstance(source_url, str) and 0 < len(source_url) <= 2048 and not any(ord(c) < 32 or ord(c) == 127 for c in source_url), "Explicit source identity URL/URI is required")
    require(isinstance(attribution, str) and 0 < len(attribution) <= 16384 and "\x00" not in attribution, "Source attribution is required")
    first = read_snapshot(sources[0], mapping, crop=crop, stride=stride, limits=limits)
    require(all(n <= 512 for n in first.shape), "Native display grid dimensions exceed 512 nodes; choose an explicit crop/preview stride")
    points = math.prod(first.shape)
    require(points <= 1000000, "Native point reader supports at most one million selected nodes; use an explicitly recorded preview stride")
    require(min(first.shape) >= 2, "Native 3-D renderer needs non-degenerate extents on every axis")
    first_derived = derive_fields(first, derivatives=derivatives, pressure_convention=pressure_convention)
    first_derived.update(native_support_fields(first, stride))
    field_ids = list(first.fields) + list(first_derived)
    require(len(field_ids) <= 32 and len(set(field_ids)) == len(field_ids), "Native field count exceeds limit or duplicates a supplied source field")
    field_mins = {key: math.inf for key in field_ids}
    field_maxs = {key: -math.inf for key in field_ids}
    first_report = range_report(first, first_derived, physical=True)
    snapshots = []
    ids = _source_ids(first)
    xyz = _coordinates(first, physical=True)
    require(np.max(xyz.max(axis=0) - xyz.min(axis=0)) <= 1.e8, "Native physical viewing extent exceeds renderer limits")
    required_bytes = (len(field_ids) * len(sources) * points * 8) + points * 32 + 8 * 1024 * 1024
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    require(shutil.disk_usage(output.parent).free >= required_bytes, "Insufficient disk space for native conversion")
    with staged_directory(output) as stage:
        xyz.tofile(stage / "coordinates.f64")
        ids.tofile(stage / "point-ids.i64")
        with contextlib.ExitStack() as handles:
            streams = {key: handles.enter_context((stage / (key + ".f64")).open("wb")) for key in field_ids}
            previous_step = -1
            previous_time = -math.inf
            for ordinal, source in enumerate(sources):
                snapshot = first if ordinal == 0 else read_snapshot(source, mapping, crop=crop, stride=stride, limits=limits)
                require(snapshot.step > previous_step, "Snapshots must be supplied in strictly increasing original solver-step order")
                require(snapshot.physical_time > previous_time, "Physical times must remain strictly increasing after explicit unit mapping")
                require(snapshot.shape == first.shape and snapshot.original_shape == first.original_shape and
                        np.array_equal(snapshot.origin, first.origin) and np.array_equal(snapshot.spacing, first.spacing),
                        "Native recording requires unchanged original grid geometry")
                original_identity_keys = ("runId", "recipeId", "lineageId", "reference", "fluids")
                require(all((snapshot.run_spec or {}).get(k) == (first.run_spec or {}).get(k) for k in original_identity_keys),
                        "Original run identity/reference anchors changed across the timeline")
                derived = first_derived if ordinal == 0 else derive_fields(snapshot, derivatives=derivatives, pressure_convention=pressure_convention)
                if ordinal != 0:
                    derived.update(native_support_fields(snapshot, stride))
                require(list(snapshot.fields) + list(derived) == field_ids, "Snapshot source/derived fields differ across the timeline")
                for key, values in {**snapshot.fields, **derived}.items():
                    _unit, scale = _field_unit(key, snapshot, physical=True)
                    require(_unit == _field_unit(key, first, physical=True)[0], "Original field units changed across the timeline")
                    data = (values.ravel(order="F") * scale).astype("<f8")
                    require(np.isfinite(data).all(), f"Physical field conversion overflows: {key}")
                    field_mins[key] = min(field_mins[key], float(data.min()))
                    field_maxs[key] = max(field_maxs[key], float(data.max()))
                    data.tofile(streams[key])
                snapshots.append(_snapshot_sidecar(snapshot, derived, crop, stride))
                previous_step = snapshot.step
                previous_time = snapshot.physical_time
        fields = []
        labels = {"ux": "Velocity X", "uy": "Velocity Y", "uz": "Velocity Z", "phi": "Phase field", "solid": "Source solid mask",
                  "speed": "Derived speed", "p_star": "Normalized pressure p*", "Pi_h": "Hydrostatic head", "Pi_h0": "Reference hydrostatic head"}
        expressions = {"speed": "sqrt(ux^2+uy^2+uz^2)", "q": "0.5*(Omega:Omega-S:S) from central differences",
                       "divergence": "du_x/dx+du_y/dy+du_z/dz from central differences",
                       "helicity": "u dot curl(u) from central differences", "pressure": "rho*(1/3)*p_star+(Pi_h-Pi_h0), explicit wb_lattice convention",
                       "dissipation": "2*nu*(Sxx^2+Syy^2+Szz^2+2*(Sxy^2+Sxz^2+Syz^2)) from stored state",
                       "derivative_valid": "1 only where the complete original one-node halo is liquid and nonsolid; 0 means unavailable"}
        expressions.update(solid_support="1 only if the complete original support around a retained preview node is nonsolid; summed-volume original mask check",
                           liquid_support="1 only if the complete original support around a retained preview node is liquid and nonsolid; summed-volume original mask check")
        for key in field_ids:
            unit, _scale = _field_unit(key, first, physical=True)
            field = {"id": key, "label": labels.get(key, key.replace("_", " ").title()), "unit": unit,
                     "association": "point", "origin": "derived" if key in first_derived else "source", "static": False,
                     "range": [field_mins[key], field_maxs[key]],
                     "array": _array_descriptor(stage / (key + ".f64"), (len(sources), points), frames=True)}
            range_info = first_report["ranges"][key]
            field["airMaskDefault"] = range_info["airMaskDefault"]
            if range_info["available"]:
                field["displayRange"] = {"minimum": range_info["minimum"], "maximum": range_info["firstFramePercentile99"],
                                         "policy": "first_frame_percentile_99", "clippedAbove": range_info["clippedAbovePercentile99"]}
            if key in ("q", "divergence", "helicity") or key.startswith("vorticity"):
                field["validityMask"] = "derivative_valid"
            if key in first_derived:
                field["expression"] = expressions.get(key, "central-difference curl(u) with conservative one-node air/solid exclusion and explicit derivative_valid mask")
            if key in ("ux", "uy", "uz"):
                field.update(vector="velocity", component={"ux": "x", "uy": "y", "uz": "z"}[key])
            fields.append(field)
        write_json(stage / "source-manifest.json", {"version": 1, "kind": "home4_source_snapshot_manifest", "snapshots": snapshots})
        provenance = {"version": 1, "kind": "home4_archive_conversion", "converter": "Tools/home4_archive.py",
                      "converterSHA256": file_digest(__file__), "mapping": mapping.as_dict(),
                      "sourcesManifest": {"path": "source-manifest.json", "sha256": file_digest(stage / "source-manifest.json"),
                                          "frameCount": len(snapshots)},
                      "sourceURL": source_url, "topology": "Original structured-grid sample nodes retained as native points with an affine structuredGrid attachment; no cylinder reconstruction.",
                      "pointIdDefinition": "original x + Nx*(y + Ny*z), before crop or preview stride",
                      "changes": "Explicit array-axis permutation, crop/preview selection, physical unit mapping, float64 storage, separately identified derived fields."}
        write_json(stage / "provenance.json", provenance)
        require((stage / "provenance.json").stat().st_size <= 256 * 1024, "Native provenance exceeds reader size limit")
        (stage / "ATTRIBUTION.txt").write_text(attribution + "\nSource: " + source_url + "\nChanges: " + provenance["changes"] + "\n", encoding="utf-8")
        write_json(stage / "home4-grid.json", {"version": 1, "kind": "home4_original_structured_grid", "layout": "x_fastest_node_grid",
                  "coordinateUnit": "m", "dimensionsXYZ": list(first.shape), "minimum": xyz.min(axis=0).tolist(),
                  "maximum": xyz.max(axis=0).tolist(), "mapping": mapping.as_dict(),
                  "limitations": ["Metadata describes source structured samples; it is not the cylinder_z derived volume-reconstruction contract.",
                                   "Changing source solid masks are retained per frame; native point topology has no cell connectivity."]})
        write_json(stage / "ranges.json", first_report)
        identity = hashlib.sha256(("".join(s["sourceSHA256"] for s in snapshots) + json.dumps(mapping.as_dict(), sort_keys=True) +
                                  json.dumps({"crop": crop, "stride": stride, "fields": field_ids,
                                              "pressureConvention": pressure_convention}, sort_keys=True)).encode()).hexdigest()
        descriptor = {"version": 3, "kind": "field_recording", "id": "HOME4_" + identity[:32], "title": title,
                      "sourceURL": source_url, "spatialDimensions": 3, "coordinateUnit": "m", "timeUnit": "s",
                      "pointCount": points, "frameCount": len(sources),
                      "topology": {"kind": "points", "origin": "source", "connectivity": None},
                      "coordinates": _array_descriptor(stage / "coordinates.f64", (points, 3)),
                      "pointIds": {"path": "point-ids.i64", "dtype": "int64", "byteOrder": "little", "count": points,
                                   "sha256": file_digest(stage / "point-ids.i64")},
                      "sourceBounds": {"min": xyz.min(axis=0).tolist(), "max": xyz.max(axis=0).tolist()},
                      "frames": [{"index": s["iteration"], "label": "step_" + str(s["iteration"]), "time": s["timePhysicalSeconds"]} for s in snapshots],
                      "timeOrigin": "Original solver iteration*explicit dtSeconds + explicit timeOriginSeconds; steps and seconds remain distinct.",
                      "fields": fields, "defaultScalar": "speed", "provenanceSHA256": file_digest(stage / "provenance.json"),
                      "attributionSHA256": file_digest(stage / "ATTRIBUTION.txt"),
                      "limitations": ["HOME4 solver is not connected; imported arrays are supplied source snapshots.",
                                      "Native version-3 topology retains source nodes as points, without spatial cell interpolation or a structured-volume attachment.",
                                      "Derivative invalid nodes carry zero display values and an explicit derivative_valid mask; they are unavailable measurements.",
                                      "Pressure is unavailable unless all WB components and its explicit lattice convention are supplied.",
                                      "Crop and preview stride are recorded in provenance; preview selection is not the full numerical grid."]}
        reference = (first.run_spec or {}).get("reference", {})
        fluids = (first.run_spec or {}).get("fluids", {})
        require(isinstance(reference, dict) and isinstance(fluids, dict), "Original reference/fluid metadata must be objects")
        reference_anchors = {key: reference.get(key) for key in ("lengthCells", "speedCellsPerStep", "timeSteps")}
        reference_anchors["densityLattice"] = fluids.get("rhoHeavy")
        for key, value in reference_anchors.items():
            require(value is None or (type(value) in (int, float) and math.isfinite(value) and value > 0),
                    f"Original reference anchor must be finite positive: {key}")
        # These values come only from the imported run_spec and explicit import map, never the next-run case.
        original_spacing = first.spacing / stride
        original_origin = first.origin - np.array([a for a, _ in (crop or ((0, n) for n in first.original_shape))]) * original_spacing
        coord_scale = mapping.dx_m if mapping.coordinate_units == "lattice" else 1.
        descriptor["structuredGrid"] = {"version": 1, "kind": "home4_structured_source", "layout": "x_fastest_node_grid",
            "dimensionsXYZ": list(first.shape), "originalDimensionsXYZ": list(first.original_shape),
            "originMeters": (first.origin * coord_scale).tolist(), "spacingMeters": (first.spacing * coord_scale).tolist(),
            "originalOriginXYZ": original_origin.tolist(), "originalSpacingXYZ": original_spacing.tolist(),
            "cropMinimumXYZ": [a for a, _ in (crop or ((0, n) for n in first.original_shape))],
            "cropMaximumXYZ": [b for _, b in (crop or ((0, n) for n in first.original_shape))],
            "previewStride": stride, "axisOrder": mapping.axis_order, "metadataOrder": mapping.metadata_order,
            "coordinateUnits": mapping.coordinate_units, "velocityUnits": mapping.velocity_units,
            "timeOriginSeconds": mapping.time_origin_s,
            "units": {"dxMeters": mapping.dx_m, "dtSeconds": mapping.dt_s, "densityReferenceKgM3": mapping.rho_ref_kg_m3},
            "reference": reference_anchors, "phaseField": "phi", "solidField": "solid", "phiLiquidMin": mapping.phi_liquid_min,
            "derivativeValidityField": "derivative_valid" if "derivative_valid" in field_ids else "",
            "solidSupportField": "solid_support", "liquidSupportField": "liquid_support",
            "sourceManifest": provenance["sourcesManifest"]}
        for imported_key, grid_key in (("runId", "sourceRunId"), ("recipeId", "recipeId"), ("lineageId", "lineageId")):
            value = (first.run_spec or {}).get(imported_key)
            if value is not None:
                require(isinstance(value, str) and 0 < len(value) <= 256 and value.strip() and all(ord(c) >= 32 and ord(c) != 127 for c in value),
                        f"Invalid original source identity: {imported_key}")
                descriptor["structuredGrid"][grid_key] = value
        descriptor["limitations"][1] = "Original structured samples retained with explicit affine-grid metadata; native trilinear display interpolation excludes source masks."
        write_json(stage / "recording.json", descriptor)
    return descriptor


def export_vti(sources: list[Path | str], output: Path | str, mapping: Mapping, *,
               crop: tuple[tuple[int, int], ...] | None = None, stride: int = 1,
               derivatives: bool = False, pressure_convention: str | None = None,
               physical: bool = False, limits: Limits = Limits()) -> dict:
    mapping.validate(physical=physical)
    require(1 <= len(sources) <= 100000, "VTI export needs a bounded original snapshot list")
    report = {"version": 1, "kind": "home4_vti_export", "mapping": mapping.as_dict(), "snapshots": [],
              "coordinateUnit": "m" if physical or mapping.coordinate_units == "physical" else "lu_length",
              "limitations": ["Source structured grid and masks are retained; no replacement CFD values are generated.",
                               "Derived invalid stencils are explicitly masked, not measured zero."]}
    with staged_directory(output) as stage:
        previous_step = -1
        for ordinal, source in enumerate(sources):
            snapshot = read_snapshot(source, mapping, crop=crop, stride=stride, limits=limits)
            require(snapshot.step > previous_step, "VTI sequence requires increasing original solver steps")
            derived = derive_fields(snapshot, derivatives=derivatives, pressure_convention=pressure_convention)
            if ordinal == 0:
                report["ranges"] = range_report(snapshot, derived, physical=physical)
            nx, ny, nz = snapshot.shape
            extent = f"0 {nx-1} 0 {ny-1} 0 {nz-1}"
            coord_scale = mapping.dx_m if physical and mapping.coordinate_units == "lattice" else 1.0
            origin, spacing = snapshot.origin * coord_scale, snapshot.spacing * coord_scale
            require(np.isfinite(origin).all() and np.isfinite(spacing).all(), "VTI coordinates overflow")
            root = ET.Element("VTKFile", type="ImageData", version="1.0", byte_order="LittleEndian", header_type="UInt64")
            image = ET.SubElement(root, "ImageData", WholeExtent=extent,
                                  Origin=" ".join(format(float(x), ".17g") for x in origin),
                                  Spacing=" ".join(format(float(x), ".17g") for x in spacing))
            piece = ET.SubElement(image, "Piece", Extent=extent)
            data = ET.SubElement(piece, "PointData", Scalars="phi", Vectors="velocity")
            arrays = {**snapshot.fields, **derived}
            payloads: list[bytes] = []
            offset = 0
            fields = {}
            for name, values in arrays.items():
                unit, scale = _field_unit(name, snapshot, physical=physical)
                converted = (values.ravel(order="F") * scale).astype("<f8")
                require(np.isfinite(converted).all(), f"VTI field scaling overflows: {name}")
                raw = converted.tobytes()
                ET.SubElement(data, "DataArray", type="Float64", Name=name, NumberOfComponents="1", format="appended", offset=str(offset))
                payloads.append(struct.pack("<Q", len(raw)) + raw)
                offset += 8 + len(raw)
                fields[name] = {"unit": unit, "origin": "derived" if name in derived else "source"}
            velocity_scale = _field_unit("ux", snapshot, physical)[1]
            velocity = np.column_stack([snapshot.fields[k].ravel(order="F") * velocity_scale for k in ("ux", "uy", "uz")]).astype("<f8")
            require(np.isfinite(velocity).all(), "VTI velocity scaling overflows")
            raw = velocity.tobytes()
            ET.SubElement(data, "DataArray", type="Float64", Name="velocity", NumberOfComponents="3", format="appended", offset=str(offset))
            payloads.append(struct.pack("<Q", len(raw)) + raw)
            ET.SubElement(piece, "CellData")
            xml = ET.tostring(root, encoding="utf-8", xml_declaration=True)
            require(xml.endswith(b"</VTKFile>"), "Internal VTI XML serialization error")
            name = f"snapshot-{snapshot.step:010d}.vti"
            with (stage / name).open("wb") as stream:
                stream.write(xml[:-len(b"</VTKFile>")])
                stream.write(b'<AppendedData encoding="raw">_')
                for payload in payloads:
                    stream.write(payload)
                stream.write(b"</AppendedData></VTKFile>\n")
            sidecar = _snapshot_sidecar(snapshot, derived, crop, stride)
            sidecar.update(file=name, sha256=file_digest(stage / name), coordinateUnit=report["coordinateUnit"], fields=fields)
            write_json(stage / (name + ".json"), sidecar)
            report["snapshots"].append(sidecar)
            previous_step = snapshot.step
        pvd = ET.Element("VTKFile", type="Collection", version="0.1", byte_order="LittleEndian")
        collection = ET.SubElement(pvd, "Collection")
        for snapshot in report["snapshots"]:
            time = snapshot["timePhysicalSeconds"] if snapshot["timePhysicalSeconds"] is not None else snapshot["iteration"]
            ET.SubElement(collection, "DataSet", timestep=format(time, ".17g"), group="", part="0", file=snapshot["file"])
        ET.ElementTree(pvd).write(stage / "sequence.pvd", encoding="utf-8", xml_declaration=True)
        report["timelineUnit"] = "s" if mapping.dt_s is not None else "steps"
        write_json(stage / "provenance.json", report)
    return report


def mapping_from_run_spec(path: Path | str | None, first_source: Path | str) -> dict:
    if path is not None:
        source = Path(path)
        require(source.stat().st_size <= 1024 * 1024, "run_spec sidecar exceeds size limit")
        spec = bounded_json(source.read_text(encoding="utf-8"))
    else:
        with SafeNPZ(first_source) as archive:
            if "run_spec" not in archive.members:
                return {}
            raw = archive.scalar("run_spec")
            require(isinstance(raw, str), "run_spec must be a scalar JSON string")
            spec = bounded_json(raw)
    require(isinstance(spec, dict), "run_spec must be an object")
    archive = spec.get("archive", {})
    units = spec.get("units", {})
    require(isinstance(archive, dict) and isinstance(units, dict), "run_spec archive/units contracts must be objects")
    return {"axis_order": archive.get("axisOrder"), "coordinate_units": archive.get("coordinateUnits"),
            "velocity_units": archive.get("velocityUnits"), "dx_m": units.get("dxMeters"), "dt_s": units.get("dtSeconds"),
            "rho_ref_kg_m3": units.get("densityReferenceKgM3"), "phi_liquid_min": archive.get("phiLiquidMin", .5),
            "time_origin_s": archive.get("timeOriginSeconds", 0.), "metadata_order": archive.get("metadataOrder")}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    inspect = commands.add_parser("inspect", help="Bounded metadata/shape/range inspection without pickle")
    inspect.add_argument("source", type=Path)
    inspect.add_argument("--output", type=Path, help="Optional new JSON output path (stdout by default)")
    for name in ("convert", "vti"):
        command = commands.add_parser(name, help="Native point recording" if name == "convert" else "Structured-grid VTI sequence")
        command.add_argument("sources", nargs="+", type=Path)
        command.add_argument("--output", type=Path, required=True)
        command.add_argument("--axis-order", choices=("xyz", "xzy", "yxz", "yzx", "zxy", "zyx"),
                             help="Array axes; origin/spacing component order is a separate required convention")
        command.add_argument("--coordinate-units", choices=("lattice", "physical"), help="Explicit convention for source origin/spacing")
        command.add_argument("--metadata-order", choices=("xyz", "array"), help="Explicit component order of origin/spacing triples")
        command.add_argument("--velocity-units", choices=("lattice", "physical"), help="Explicit convention for ux/uy/uz")
        command.add_argument("--dx-m", type=float)
        command.add_argument("--dt-s", type=float)
        command.add_argument("--rho-ref-kg-m3", type=float)
        command.add_argument("--time-origin-s", type=float)
        command.add_argument("--phi-liquid-min", type=float)
        command.add_argument("--run-spec", type=Path, help="JSON with units and explicit archive.axisOrder/coordinateUnits/velocityUnits")
        command.add_argument("--crop", help="XSTART:XSTOP,YSTART:YSTOP,ZSTART:ZSTOP original XYZ indices")
        command.add_argument("--preview-stride", type=int, default=1)
        command.add_argument("--derivatives", action="store_true", help="Derived curl/Q/helicity/divergence with explicit invalid-stencil mask")
        command.add_argument("--pressure-convention", choices=("wb_lattice",))
        if name == "convert":
            command.add_argument("--title", default="HOME4 imported snapshots")
            command.add_argument("--source-url", required=True, help="Explicit source identity, e.g. urn:sha256:...")
            command.add_argument("--attribution-file", type=Path, required=True)
        else:
            command.add_argument("--physical", action="store_true", help="Map grid/velocity and defined quantities to physical units")
    args = parser.parse_args(argv)
    try:
        if args.command == "inspect":
            report = inspect_archive(args.source)
            if args.output is None:
                print(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False))
            else:
                require(not args.output.exists(), "Inspection output already exists")
                args.output.parent.mkdir(parents=True, exist_ok=True)
                fd, temporary = tempfile.mkstemp(prefix="." + args.output.name, dir=args.output.parent)
                os.close(fd)
                try:
                    write_json(Path(temporary), report)
                    # Hard link publishes only if destination is still absent.
                    os.link(temporary, args.output)
                finally:
                    Path(temporary).unlink(missing_ok=True)
            return 0
        contract = mapping_from_run_spec(args.run_spec, args.sources[0])
        for key in ("axis_order", "coordinate_units", "velocity_units", "dx_m", "dt_s", "rho_ref_kg_m3", "phi_liquid_min", "time_origin_s", "metadata_order"):
            value = getattr(args, key)
            if value is not None:
                contract[key] = value
        require(all(contract.get(key) is not None for key in ("axis_order", "coordinate_units", "velocity_units", "metadata_order")),
                "Supply explicit --axis-order, --metadata-order, --coordinate-units and --velocity-units or their run_spec.archive equivalents")
        mapping = Mapping(**contract)
        common = {"crop": parse_crop(args.crop), "stride": args.preview_stride, "derivatives": args.derivatives,
                  "pressure_convention": args.pressure_convention}
        if args.command == "convert":
            require(args.attribution_file.stat().st_size <= 65536, "Attribution file exceeds size limit")
            result = convert_snapshots(args.sources, args.output, mapping, title=args.title, source_url=args.source_url,
                                       attribution=args.attribution_file.read_text(encoding="utf-8"), **common)
            print(json.dumps({"output": str(args.output), "recording": "recording.json", "frameCount": result["frameCount"],
                              "pointCount": result["pointCount"], "nativeTopology": "points", "structuredVolumeNativeIntegration": "unavailable"}))
        else:
            result = export_vti(args.sources, args.output, mapping, physical=args.physical, **common)
            print(json.dumps({"output": str(args.output), "sequence": "sequence.pvd", "snapshots": len(result["snapshots"]),
                              "coordinateUnit": result["coordinateUnit"], "timelineUnit": result["timelineUnit"]}))
        return 0
    except (ArchiveError, OSError, ValueError, zipfile.BadZipFile) as exc:
        parser.exit(2, f"home4_archive: {exc}\n")


if __name__ == "__main__":
    raise SystemExit(main())
