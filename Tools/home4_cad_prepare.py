#!/usr/bin/env python3
"""Verified STEP/IGES tessellation using the real FreeCAD/OpenCASCADE kernel.

Run with FreeCAD's Python or a Python exposing FreeCAD and Part. The native UI
discovers an installed FreeCAD, or uses explicitly configured interpreter/lib.
Output is a bounded ASCII STL plus a provenance JSON manifest, never CFD data.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import tempfile

MAX_SOURCE = 64 * 1024 * 1024
MAX_TRIANGLES = 500_000
MAX_VERTICES = 1_500_000


def pinned_bytes(path: Path) -> bytes:
    with path.open("rb") as stream:
        data = stream.read(MAX_SOURCE + 1)
    if not data or len(data) > MAX_SOURCE:
        raise ValueError("CAD source must be nonempty and at most 64 MiB")
    return data


def prepare(source: Path, destination: Path, tolerance: float, library: str = "",
            expected_sha256: str = "") -> dict:
    if source.suffix.lower() not in {".step", ".stp", ".iges", ".igs"}:
        raise ValueError("The CAD kernel adapter accepts STEP/STP and IGES/IGS")
    if not math.isfinite(tolerance) or tolerance <= 0:
        raise ValueError("Tessellation tolerance must be finite and positive in source units")
    data = pinned_bytes(source)
    digest = hashlib.sha256(data).hexdigest()
    if expected_sha256 and digest != expected_sha256.lower():
        raise ValueError("CAD source differs from the requested original SHA256")
    if library:
        sys.path.insert(0, library)
    import FreeCAD  # type: ignore[import-not-found]
    import Part  # type: ignore[import-not-found]

    # The kernel parses a private byte-pinned copy, not a concurrently writable
    # original path. No new document or GUI is constructed.
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="home4-cad-", dir=destination.parent) as work:
        pinned = Path(work) / ("original" + source.suffix.lower())
        pinned.write_bytes(data)
        shape = Part.Shape()
        shape.read(str(pinned))
        if shape.isNull() or not shape.isValid():
            raise ValueError("CAD kernel reports an empty or invalid shape")
        vertices, faces = shape.tessellate(tolerance)
        if not faces or len(faces) > MAX_TRIANGLES or len(vertices) > MAX_VERTICES:
            raise ValueError("Tessellation exceeds the bounded native mesh contract; increase tolerance")
        for vertex in vertices:
            if not all(math.isfinite(float(x)) and abs(float(x)) <= 1e12 for x in vertex):
                raise ValueError("CAD tessellation has nonfinite or unsupported coordinates")
        if hashlib.sha256(pinned_bytes(source)).hexdigest() != digest:
            raise ValueError("CAD source changed while preparing the preview")
        destination.parent.mkdir(parents=True, exist_ok=True)
        manifest_path = destination.with_suffix(destination.suffix + ".json")
        if destination.exists() or manifest_path.exists():
            raise ValueError("Preparation output already exists; choose a new cache identity")
        pending = destination.with_name(destination.name + ".pending")
        try:
            with pending.open("x", encoding="ascii", newline="\n") as stream:
                stream.write("solid verified_home4_cad\n")
                for face in faces:
                    if len(face) != 3 or any(i < 0 or i >= len(vertices) for i in face):
                        raise ValueError("Invalid tessellation triangle")
                    a, b, c = (vertices[i] for i in face)
                    normal = (b - a).cross(c - a)
                    length = normal.Length
                    if length <= 1e-18:
                        continue
                    normal /= length
                    stream.write("facet normal %.17g %.17g %.17g\nouter loop\n" % tuple(normal))
                    for vertex in (a, b, c):
                        stream.write("vertex %.17g %.17g %.17g\n" % tuple(vertex))
                    stream.write("endloop\nendfacet\n")
                stream.write("endsolid verified_home4_cad\n")
                stream.flush()
                os.fsync(stream.fileno())
            if pending.stat().st_size > MAX_SOURCE:
                raise ValueError("Tessellated STL exceeds the native 64 MiB limit; increase tolerance")
            manifest = {
                "version": 1, "kind": "geometry-preparation", "source_path": str(source.resolve()),
                "source_sha256": digest, "output_sha256": hashlib.sha256(pending.read_bytes()).hexdigest(),
                "kernel": "FreeCAD/OpenCASCADE", "kernel_version": ".".join(FreeCAD.Version()[:3]),
                "tolerance_source_units": tolerance, "vertices": len(vertices), "triangles": len(faces),
                "closed": bool(shape.isClosed()), "solids": len(shape.Solids),
                "volume_source_units3": float(shape.Volume),
                "source_coordinates_preserved": True,
            }
            # Native cache directories are uniquely owned. Hard-link publication
            # still guarantees an unrelated output is never replaced.
            os.link(pending, destination)
            try:
                with manifest_path.open("x", encoding="utf-8") as stream:
                    json.dump(manifest, stream, allow_nan=False, indent=2)
                    stream.write("\n")
                    stream.flush()
                    os.fsync(stream.fileno())
            except BaseException:
                destination.unlink(missing_ok=True)
                raise
            return manifest
        finally:
            pending.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--library", default="")
    parser.add_argument("--tolerance", required=True, type=float)
    parser.add_argument("--expected-sha256", default="")
    args = parser.parse_args()
    try:
        result = prepare(args.input, args.output, args.tolerance, args.library, args.expected_sha256)
        print(json.dumps(result, allow_nan=False))
        return 0
    except Exception as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
