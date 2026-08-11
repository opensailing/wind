"""The ``cfdviz`` command-line tool.

``pyproject.toml`` declares ``cfdviz = "cfdviz.__main__:main"``, and the module
is also runnable as ``python -m cfdviz``. Spec section 10 requires the validator
to be usable exactly this way::

    python -m cfdviz validate Sample.cfdviz

Subcommands:

``validate``
    Check a case against the schema, every section 3.1 invariant, and every
    stored byte. Exits non-zero if anything is wrong. ``--json`` emits a
    machine-readable report for CI.
``info``
    Summarise a case or a single ``.cvf`` file.
``known-values``
    Write ``known_values.json`` (spec 9), or with ``--check``, verify the one
    already there without touching it.
``generate-mock``
    Write the synthetic cylinder-wake case of plan section 7. ``--low-res``
    selects the small preset that is committed to source control.
``import-fluidx3d``
    Convert explicitly identified FluidX3D legacy VTK volume sequences.
``extract``
    Pull one field at one frame out of a case, as a summary, a single voxel, or
    a ``.npy`` file.
``benchmark-read``
    Time a full decode of the case and report throughput.

Two rules govern the exit status, and both come from spec 10's "MUST NOT report
success":

1. A failed validation exits non-zero. Wording is for humans; the status is what
   a CI job reads.
2. No error path prints a traceback. An unreadable file is a fact about the
   input, not a bug in the tool, and a stack trace buries the one line the user
   needs. Unexpected exceptions are caught at the top level and reported as
   ``internal error`` — still non-zero, still one line.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
from pathlib import Path
from typing import Any, Sequence

import numpy as np

from . import FORMAT_VERSION, __version__
from .case import (
    KNOWN_VALUES_NAME,
    build_known_values,
    validate_case,
    verify_known_values,
    write_known_values,
)
from .codecs import CODEC_NAMES, CODEC_ZSTD, CodecError
from .convert import ConversionError
from .cvf import CVFError, CVFReader
from .cvm import CVMError
from .fluidx3d import (
    FluidX3DImportParameters,
    fluidx3d_field,
    import_fluidx3d_case,
)
from .manifest import ManifestError, field_by_id, frame_path, load_manifest
from .mock import (
    MockCaseParameters,
    default_parameters,
    generate_mock_case,
    high_resolution_parameters,
    low_resolution_parameters,
)

__all__ = ["main", "build_parser"]

#: Exit statuses. Distinguished so a caller can tell "the case is invalid" from
#: "the tool could not run", which are different problems with different fixes.
EXIT_OK = 0
EXIT_INVALID = 1
EXIT_USAGE = 2


def build_parser() -> argparse.ArgumentParser:
    """Construct the argument parser for the ``cfdviz`` command."""
    parser = argparse.ArgumentParser(
        prog="cfdviz",
        description="Inspect and validate CFDViz 1.1 cases.",
    )
    parser.add_argument(
        "--version",
        action="store_true",
        help="print the format version this tool implements and exit",
    )
    subparsers = parser.add_subparsers(dest="command")

    validate = subparsers.add_parser(
        "validate", help="validate a case directory (spec section 10)"
    )
    validate.add_argument("case", type=Path, help="the .cfdviz case root")
    validate.add_argument(
        "--json", action="store_true", dest="as_json",
        help="emit the report as JSON instead of text",
    )

    # ``inspect`` is the name plan section 7 lists; ``info`` is what this
    # package shipped first. Both reach the same handler rather than one being
    # renamed, because renaming breaks whatever already types the other.
    info = subparsers.add_parser(
        "info",
        aliases=["inspect"],
        help="summarise a case directory or a single .cvf file",
    )
    info.add_argument("target", type=Path, help="a case root or a .cvf file")

    known = subparsers.add_parser(
        "known-values",
        help=f"write or check {KNOWN_VALUES_NAME} (spec section 9)",
    )
    known.add_argument("case", type=Path, help="the .cfdviz case root")
    known.add_argument(
        "--check", action="store_true",
        help=(
            "verify the existing bridge file against the case instead of "
            "writing one; never modifies the file"
        ),
    )

    _add_generate_mock(subparsers)
    _add_import_fluidx3d(subparsers)
    _add_extract(subparsers)
    _add_benchmark_read(subparsers)
    return parser


#: Codecs ``generate-mock`` will *write*. zstd is deliberately absent: spec 7
#: reserves it and the Unreal reader must reject it, so a case written with it
#: would be one this project cannot load. Leaving it out of the choices makes
#: that an argparse usage error (rc 2) with the legal values listed, rather
#: than a case that only fails later, in Unreal, on someone else's machine.
_WRITABLE_CODECS = tuple(
    name for codec, name in sorted(CODEC_NAMES.items()) if codec != CODEC_ZSTD
)


def _add_generate_mock(subparsers: argparse._SubParsersAction) -> None:
    """Register ``generate-mock`` and every parameter plan section 7 requires.

    The defaults are :class:`~cfdviz.mock.MockCaseParameters`' own, read off the
    dataclass rather than repeated here, so the CLI help and the library cannot
    drift apart.
    """
    defaults = default_parameters()
    generate = subparsers.add_parser(
        "generate-mock",
        help="write the synthetic cylinder-wake case (plan section 7)",
        description=(
            "Generate a synthetic unsteady cylinder wake. The data is a "
            "closed-form construction for visualization demonstration and is "
            "not validation-grade CFD; the manifest says so."
        ),
    )
    generate.add_argument(
        "--output", type=Path, required=True, help="destination case directory"
    )
    generate.add_argument(
        "--high-res", action="store_true",
        help="use the film-tier demo preset: 168x84x36, 40 frames (~400 MB)")
    generate.add_argument(
        "--low-res", action="store_true",
        help=(
            "use the small preset that fits in source control and that the "
            "packaged application auto-loads"
        ),
    )
    generate.add_argument(
        "--dimensions", type=int, nargs=3, metavar=("NX", "NY", "NZ"),
        help=f"grid cell counts (default {list(defaults.dimensions)})",
    )
    generate.add_argument(
        "--frames", type=int, dest="frame_count",
        help=f"number of stored frames (default {defaults.frame_count})",
    )
    generate.add_argument(
        "--frame-interval", type=float,
        help=f"seconds between stored frames (default {defaults.frame_interval})",
    )
    generate.add_argument(
        "--domain", type=float, nargs=3, metavar=("LX", "LY", "LZ"),
        help=f"physical extent in metres (default {list(defaults.domain)})",
    )
    generate.add_argument(
        "--origin", type=float, nargs=3, metavar=("X", "Y", "Z"),
        help=f"grid corner position (default {list(defaults.origin)})",
    )
    generate.add_argument(
        "--inlet-velocity", type=float,
        help=f"uniform inflow speed, m/s (default {defaults.inlet_velocity})",
    )
    generate.add_argument(
        "--cylinder-radius", type=float,
        help=f"obstacle radius, m (default {defaults.cylinder_radius})",
    )
    generate.add_argument(
        "--shedding-frequency", type=float,
        help=f"shedding cycles per second (default {defaults.shedding_frequency})",
    )
    generate.add_argument(
        "--circulation", type=float,
        help=f"peak vortex circulation, m2/s (default {defaults.circulation})",
    )
    generate.add_argument(
        "--core-radius", type=float,
        help=f"Lamb-Oseen core radius, m (default {defaults.core_radius})",
    )
    generate.add_argument(
        "--spanwise-perturbation", type=float,
        help=(
            "relative spanwise modulation; 0 makes the volume a perfect "
            f"extrusion (default {defaults.spanwise_perturbation})"
        ),
    )
    generate.add_argument(
        "--codec", choices=_WRITABLE_CODECS,
        help=f"payload codec (default {defaults.codec!r})",
    )
    generate.add_argument(
        "--level", type=int, help="codec level; omitted uses the codec default"
    )
    precision = generate.add_mutually_exclusive_group()
    precision.add_argument(
        "--float16", action="store_const", const="float16", dest="float_type",
        help="store float fields as float16, halving every payload",
    )
    precision.add_argument(
        "--float32", action="store_const", const="float32", dest="float_type",
        help=f"store float fields as float32 (default {defaults.float_type!r})",
    )
    generate.add_argument(
        "--brick-size", type=int, nargs=3, metavar=("BX", "BY", "BZ"),
        help=f"CVF brick extent in voxels (default {list(defaults.brick_size)})",
    )
    generate.add_argument(
        "--seed", type=int,
        help=f"jitter seed; the same seed gives byte-identical output "
             f"(default {defaults.seed})",
    )
    generate.add_argument("--name", help="case name recorded in the manifest")


def _add_import_fluidx3d(subparsers: argparse._SubParsersAction) -> None:
    importer = subparsers.add_parser(
        "import-fluidx3d",
        help="convert FluidX3D legacy VTK volume sequences",
        description=(
            "Convert binary VTK STRUCTURED_POINTS files written by FluidX3D. "
            "Each --field group starts with its physical field id followed by "
            "one or more files; shell-expanded globs are accepted. Units and "
            "scales are explicit because FluidX3D VTK files do not disclose "
            "whether SI conversion was enabled."
        ),
    )
    importer.add_argument("--output", type=Path, required=True)
    importer.add_argument("--name", required=True, help="case name in the manifest")
    importer.add_argument(
        "--field",
        action="append",
        nargs="+",
        required=True,
        metavar="ID_OR_VTK",
        help=(
            "field id followed by its VTK files; repeat for another field. "
            "Supported ids: U, density, phi, temperature, force"
        ),
    )
    importer.add_argument(
        "--flags",
        type=Path,
        nargs="+",
        default=(),
        metavar="VTK",
        help="FluidX3D flags sequence used for field-specific validity",
    )
    importer.add_argument(
        "--assume-all-cells-valid",
        action="store_true",
        help="explicitly declare every lattice cell valid when --flags is absent",
    )
    importer.add_argument(
        "--force-interpretation",
        choices=("boundary", "volume"),
        help="required meaning of an imported force field",
    )
    importer.add_argument(
        "--source-time-step",
        type=float,
        required=True,
        help="duration of one solver step in --time-unit",
    )
    importer.add_argument(
        "--solver-step-offset",
        type=int,
        required=True,
        help=(
            "multiple of 1000000000 added to FluidX3D's nine-digit filename "
            "step suffix"
        ),
    )
    importer.add_argument(
        "--source-units",
        choices=("si", "lattice"),
        required=True,
        help="unit mode passed to FluidX3D write_vtk; never inferred",
    )
    importer.add_argument(
        "--source-axes",
        nargs=3,
        required=True,
        metavar=("SOURCE_X", "SOURCE_Y", "SOURCE_Z"),
        help=(
            "signed canonical direction of source X/Y/Z, for example "
            "--source-axes +X +Y +Z"
        ),
    )
    importer.add_argument("--length-scale", type=float, default=1.0)
    importer.add_argument("--velocity-scale", type=float, default=1.0)
    importer.add_argument("--length-unit")
    importer.add_argument("--time-unit")
    importer.add_argument("--mass-unit")
    importer.add_argument("--temperature-unit")
    importer.add_argument(
        "--field-scale",
        action="append",
        default=(),
        metavar="ID=SCALE",
        help="additional explicit multiplier for one field; repeatable",
    )
    importer.add_argument(
        "--field-unit",
        action="append",
        default=(),
        metavar="ID=UNIT",
        help="override one field's displayed unit; repeatable",
    )
    importer.add_argument("--codec", choices=_WRITABLE_CODECS, default="zlib")
    importer.add_argument("--level", type=int)
    precision = importer.add_mutually_exclusive_group()
    precision.add_argument(
        "--float16", action="store_const", const="float16", dest="float_type"
    )
    precision.add_argument(
        "--float32", action="store_const", const="float32", dest="float_type"
    )
    importer.set_defaults(float_type="float32")
    importer.add_argument(
        "--brick-size",
        type=int,
        nargs=3,
        default=(32, 32, 32),
        metavar=("BX", "BY", "BZ"),
    )
    importer.add_argument(
        "--excluded-flag-bits",
        type=lambda value: int(value, 0),
        default=0,
        help="global flag bits excluded from validMask, decimal or 0x-prefixed",
    )
    importer.add_argument("--solver-version")
    importer.add_argument("--solver-commit")
    importer.add_argument("--solver-configuration")
    importer.add_argument(
        "--solver-method",
        required=True,
        help="explicit lattice/method, for example 'lattice-Boltzmann D3Q19'",
    )
    importer.add_argument("--source-case")
    importer.add_argument("--max-payload-bytes", type=int)
    importer.add_argument(
        "--force",
        action="store_true",
        help="replace a non-empty output only after conversion succeeds",
    )


def _add_extract(subparsers: argparse._SubParsersAction) -> None:
    extract = subparsers.add_parser(
        "extract",
        help="read one field at one frame out of a case",
        description=(
            "Decode one field at one frame. With --output the raw array is "
            "written as .npy in its stored dtype; with --voxel a single value "
            "is printed; otherwise a summary."
        ),
    )
    extract.add_argument("case", type=Path, help="the .cfdviz case root")
    extract.add_argument(
        "--frame", type=int, required=True, help="stored frame index"
    )
    extract.add_argument(
        "--field", required=True, help="field id, e.g. U or pressure"
    )
    extract.add_argument(
        "--voxel", type=int, nargs=3, metavar=("I", "J", "K"),
        help="print one voxel instead of a summary",
    )
    extract.add_argument(
        "--output", type=Path,
        help="write the decoded array as .npy, in its stored dtype",
    )
    extract.add_argument(
        "--json", action="store_true", dest="as_json",
        help="emit JSON, with exact IEEE 754 bit patterns alongside the values",
    )


def _add_benchmark_read(subparsers: argparse._SubParsersAction) -> None:
    benchmark = subparsers.add_parser(
        "benchmark-read",
        help="time a full decode of a case and report throughput",
        description=(
            "Decode every declared field of every frame and report elapsed "
            "time and throughput. Decoded (uncompressed) bytes are reported, "
            "not file size, so the number is comparable across codecs."
        ),
    )
    benchmark.add_argument("case", type=Path, help="the .cfdviz case root")
    benchmark.add_argument(
        "--field", action="append", dest="fields", metavar="ID",
        help="restrict to this field; repeatable. Default: every field",
    )
    benchmark.add_argument(
        "--repeat", type=int, default=1,
        help="decode the case this many times and report the fastest pass",
    )
    benchmark.add_argument(
        "--json", action="store_true", dest="as_json",
        help="emit the result as JSON instead of text",
    )


def _validate(arguments: argparse.Namespace) -> int:
    """Run ``validate``."""
    report = validate_case(arguments.case)
    if arguments.as_json:
        print(json.dumps(report.to_dict(), indent=2))
    else:
        print(report.render())
    return EXIT_OK if report.ok else EXIT_INVALID


def _info_case(root: Path) -> int:
    """Summarise a case directory."""
    manifest = load_manifest(root)
    case = manifest.get("case") or {}
    timeline = manifest.get("timeline") or {}
    times = timeline.get("times") or []

    print(f"case {case.get('id', '<no id>')}  {case.get('name', '')}".rstrip())
    print(f"  format {manifest.get('format')} {manifest.get('version')}")
    if case.get("quality"):
        print(f"  quality {case.get('quality')}")
    solver = case.get("solver") or {}
    if isinstance(solver, dict) and solver:
        solver_identity = " ".join(
            str(part) for part in (solver.get("name"), solver.get("version")) if part
        )
        solver_line = "  solver"
        if solver_identity:
            solver_line += f" {solver_identity}"
        if solver.get("method"):
            solver_line += f" method={solver['method']}"
        print(solver_line)
        if solver.get("commit") or solver.get("configuration"):
            details = []
            if solver.get("commit"):
                details.append(f"commit={solver['commit']}")
            if solver.get("configuration"):
                details.append(f"configuration={solver['configuration']}")
            print(f"    {' '.join(details)}")

    provenance = manifest.get("provenance") or {}
    if isinstance(provenance, dict) and provenance:
        source_line = "  source"
        if provenance.get("sourceType"):
            source_line += f" {provenance['sourceType']}"
        if provenance.get("sourceRevision"):
            source_line += f" revision={provenance['sourceRevision']}"
        print(source_line)
        if provenance.get("exportCommand"):
            print(f"    export command={provenance['exportCommand']}")

    print(f"  frames {len(times)}", end="")
    if times:
        print(f"  t = {times[0]} .. {times[-1]}")
    else:
        print()

    sampling = timeline.get("sampling") or {}
    if isinstance(sampling, dict) and sampling:
        print(
            "  temporal sampling "
            f"dt={sampling.get('sourceTimeStep')} stride="
            f"{sampling.get('storedStepStride')} max displacement="
            f"{sampling.get('maxFeatureDisplacementCells')} cells/snapshot"
        )

    quality_metrics = manifest.get("qualityMetrics") or {}
    if isinstance(quality_metrics, dict) and quality_metrics:
        active_dimensions = "x".join(
            str(value) for value in quality_metrics.get("activeDimensions", [])
        )
        print(
            f"  representative metrics effective "
            f"{quality_metrics.get('effectiveSpatialDimensions')}D, "
            f"active={quality_metrics.get('activeCellCount')} cells, "
            f"active dimensions={active_dimensions}, "
            f"velocity={quality_metrics.get('velocityField')}"
        )
        print(
            f"    velocity RMS={quality_metrics.get('velocityComponentRms')} "
            f"spanwise gradient RMS={quality_metrics.get('spanwiseGradientRms')} "
            f"temporal frames={quality_metrics.get('temporalFrameCount')}"
        )

    for grid in manifest.get("grids") or []:
        if not isinstance(grid, dict):
            continue
        dimensions = grid.get("dimensions") or []
        extent = "x".join(str(n) for n in dimensions)
        print(
            f"  grid {grid.get('id')}: {extent} {grid.get('type', '')} "
            f"spacing {grid.get('spacing')}".rstrip()
        )

    for entry in manifest.get("fields") or []:
        if not isinstance(entry, dict):
            continue
        storage = entry.get("storage") or {}
        summary = (
            f"  field {entry.get('id')} (#{entry.get('numericId')}): "
            f"{entry.get('componentCount')}x{entry.get('dataType')} "
            f"{entry.get('association')} codec={storage.get('codec')}"
        )
        phase = entry.get("phase")
        if isinstance(phase, dict):
            summary += (
                f" phase={phase.get('representation')}@"
                f"{phase.get('interfaceValue')} {phase.get('inside')}"
            )
        print(summary)
    for mesh in manifest.get("meshes") or []:
        if isinstance(mesh, dict):
            print(f"  mesh {mesh.get('id')}: {mesh.get('path')}")
    return EXIT_OK


def _info_file(path: Path) -> int:
    """Summarise a single ``.cvf`` file."""
    with CVFReader(path, verify_crc=False) as reader:
        header = reader.header
        extent = "x".join(str(n) for n in header.dimensions)
        stored = "x".join(str(n) for n in header.value_extent)
        bricks = "x".join(str(n) for n in header.brick_size)
        print(f"{path.name}: CFDViz volume field {header.major_version}."
              f"{header.minor_version}")
        print(f"  dimensions   {extent} cells ({stored} stored values)")
        print(f"  dataType     {header.data_type_name} x{header.component_count}")
        print(f"  association  {header.association_name}")
        print(f"  codec        {header.codec_name}")
        print(f"  bricks       {len(reader.directory)} of {bricks}"
              f"{' (sparse)' if header.is_sparse else ''}")
        print(f"  frame        {header.frame_index} at t = "
              f"{header.simulation_time}")
        print(f"  fieldId      {header.field_numeric_id}")
    return EXIT_OK


def _info(arguments: argparse.Namespace) -> int:
    """Run ``info`` against either a case root or a single file."""
    target: Path = arguments.target
    if target.is_dir():
        return _info_case(target)
    if not target.exists():
        print(f"{target}: no such file or directory", file=sys.stderr)
        return EXIT_INVALID
    return _info_file(target)


def _known_values(arguments: argparse.Namespace) -> int:
    """Run ``known-values``, in write or check mode."""
    root: Path = arguments.case
    if not arguments.check:
        path = write_known_values(root)
        bridge = json.loads(path.read_text(encoding="utf-8"))
        print(f"wrote {path} with {_sample_summary(bridge)}")
        return EXIT_OK

    path = root / KNOWN_VALUES_NAME
    if not path.is_file():
        print(f"{path}: no bridge file to check", file=sys.stderr)
        return EXIT_INVALID
    try:
        bridge = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        print(f"{path}: line {exc.lineno} column {exc.colno}: {exc.msg}",
              file=sys.stderr)
        return EXIT_INVALID

    # Deliberately does not regenerate: rewriting the file on mismatch would
    # turn every disagreement into a silent pass, which is the exact failure
    # mode this bridge exists to catch (spec 9.4).
    problems = verify_known_values(root, bridge)
    if problems:
        print(f"{path}: {len(problems)} mismatch(es)", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return EXIT_INVALID
    print(f"{path}: {_sample_summary(bridge)} match")
    return EXIT_OK


def _sample_summary(bridge: dict) -> str:
    """``"81 samples (17 field, 34 mesh, 30 array)"``.

    The total alone would be honest but useless, and the field count alone —
    which is what this printed first — reads as full coverage while saying
    nothing about whether the geometry was checked. Naming each group means a
    zero is visible as a zero.
    """
    groups = (
        ("field", len(bridge.get("samples") or [])),
        ("mesh", len(bridge.get("meshSamples") or [])),
        ("array", len(bridge.get("arraySamples") or [])),
    )
    total = sum(count for _, count in groups)
    breakdown = ", ".join(f"{count} {name}" for name, count in groups)
    return f"{total} samples ({breakdown})"


# ---------------------------------------------------------------------------
# generate-mock
# ---------------------------------------------------------------------------

#: CLI destination -> :class:`MockCaseParameters` field. Only names present here
#: are forwarded, so a flag that is left unset keeps the dataclass default
#: rather than overwriting it with ``None``.
_MOCK_OVERRIDES = {
    "dimensions": "dimensions",
    "frame_count": "frame_count",
    "frame_interval": "frame_interval",
    "domain": "domain",
    "origin": "origin",
    "inlet_velocity": "inlet_velocity",
    "cylinder_radius": "cylinder_radius",
    "shedding_frequency": "shedding_frequency",
    "circulation": "circulation",
    "core_radius": "core_radius",
    "spanwise_perturbation": "spanwise_perturbation",
    "codec": "codec",
    "level": "level",
    "float_type": "float_type",
    "brick_size": "brick_size",
    "seed": "seed",
    "name": "name",
}


def _generate_mock(arguments: argparse.Namespace) -> int:
    """Run ``generate-mock``."""
    overrides = {
        target: getattr(arguments, source)
        for source, target in _MOCK_OVERRIDES.items()
        if getattr(arguments, source, None) is not None
    }
    for key in ("dimensions", "domain", "origin", "brick_size"):
        if key in overrides:
            overrides[key] = tuple(overrides[key])

    build = default_parameters
    if arguments.low_res:
        build = low_resolution_parameters
    if arguments.high_res:
        build = high_resolution_parameters
    try:
        parameters = build(**overrides)
    except (ValueError, CodecError) as exc:
        # An impossible case is a fact about the request, not a crash: one line,
        # no traceback, non-zero status (spec 10's rule, applied to generation).
        print(f"cfdviz: {exc}", file=sys.stderr)
        return EXIT_INVALID

    root = generate_mock_case(arguments.output, parameters)
    total = sum(path.stat().st_size for path in root.rglob("*") if path.is_file())
    extent = "x".join(str(n) for n in parameters.dimensions)
    print(
        f"wrote {root}: {extent} cells, {parameters.frame_count} frames, "
        f"{len(list(root.rglob('*.cvf')))} field files, "
        f"{total / 1024 / 1024:.2f} MiB"
    )
    print(f"  {parameters.name}: {parameters.case_id}")
    print("  Synthetic visualization demonstration; not validation-grade CFD.")
    return EXIT_OK


# ---------------------------------------------------------------------------
# import-fluidx3d
# ---------------------------------------------------------------------------


def _assignment_map(
    values: Sequence[str],
    *,
    label: str,
    convert: Any,
) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for value in values:
        key, separator, raw = value.partition("=")
        if not separator or not key or not raw:
            raise ConversionError(f"{label} must use ID=VALUE syntax; got {value!r}")
        if key in result:
            raise ConversionError(f"{label} repeats field {key!r}")
        try:
            result[key] = convert(raw)
        except (TypeError, ValueError) as exc:
            raise ConversionError(f"invalid {label} {value!r}: {exc}") from exc
    return result


def _import_fluidx3d(arguments: argparse.Namespace) -> int:
    grouped: dict[str, list[Path]] = {}
    for group in arguments.field:
        field_id, *raw_paths = group
        if not raw_paths:
            raise ConversionError(
                f"--field {field_id!r} needs at least one VTK file"
            )
        grouped.setdefault(field_id, []).extend(Path(path) for path in raw_paths)

    scales = _assignment_map(
        arguments.field_scale,
        label="--field-scale",
        convert=float,
    )
    units = _assignment_map(
        arguments.field_unit,
        label="--field-unit",
        convert=str,
    )
    unknown_overrides = (set(scales) | set(units)) - set(grouped)
    if unknown_overrides:
        raise ConversionError(
            f"field overrides name fields not supplied by --field: "
            f"{sorted(unknown_overrides)}"
        )

    sources = tuple(
        fluidx3d_field(
            field_id,
            paths,
            value_scale=scales.get(field_id, 1.0),
            unit=units.get(field_id),
        )
        for field_id, paths in grouped.items()
    )
    parameters = FluidX3DImportParameters(
        output=arguments.output,
        name=arguments.name,
        fields=sources,
        source_time_step=arguments.source_time_step,
        solver_step_offset=arguments.solver_step_offset,
        source_units=arguments.source_units,
        source_axes=tuple(arguments.source_axes),
        solver_method=arguments.solver_method,
        flag_files=tuple(arguments.flags),
        assume_all_cells_valid=arguments.assume_all_cells_valid,
        force_interpretation=arguments.force_interpretation,
        length_scale=arguments.length_scale,
        velocity_scale=arguments.velocity_scale,
        length_unit=arguments.length_unit,
        time_unit=arguments.time_unit,
        mass_unit=arguments.mass_unit,
        temperature_unit=arguments.temperature_unit,
        codec=arguments.codec,
        level=arguments.level,
        float_type=arguments.float_type,
        brick_size=tuple(arguments.brick_size),
        excluded_flag_bits=arguments.excluded_flag_bits,
        solver_version=arguments.solver_version,
        solver_commit=arguments.solver_commit,
        solver_configuration=arguments.solver_configuration,
        source_case=arguments.source_case,
        max_payload_bytes=arguments.max_payload_bytes,
        force=arguments.force,
    )
    root = import_fluidx3d_case(parameters)
    manifest = load_manifest(root)
    timeline = manifest["timeline"]
    dimensions = manifest["grids"][0]["dimensions"]
    total = sum(path.stat().st_size for path in root.rglob("*") if path.is_file())
    print(
        f"wrote {root} from FluidX3D: "
        f"{'x'.join(str(value) for value in dimensions)} cells, "
        f"{timeline['frameCount']} frames, {len(manifest['fields'])} fields, "
        f"{total / 1024 / 1024:.2f} MiB"
    )
    print(f"  external solver data: {manifest['case']['id']}")
    return EXIT_OK


# ---------------------------------------------------------------------------
# extract
# ---------------------------------------------------------------------------

def _resolve_field(manifest: dict[str, Any], field_id: str) -> dict[str, Any]:
    """Look up a field, or raise with the ids that *do* exist.

    "unknown field 'temperature'" leaves the user guessing; listing the
    alternatives turns a second run of the tool into a fix.
    """
    entry = field_by_id(manifest, field_id)
    if entry is None:
        available = sorted(
            str(f.get("id")) for f in (manifest.get("fields") or [])
            if isinstance(f, dict) and f.get("id")
        )
        raise ManifestError(
            f"unknown field {field_id!r}; this case declares {available}"
        )
    return entry


def _bits_of(value: np.generic) -> str:
    """Exact IEEE 754 bit pattern, uppercase hex, sized to the dtype (spec 9.1)."""
    raw = np.asarray(value).tobytes()
    return f"0x{int.from_bytes(raw, 'little'):0{2 * len(raw)}X}"


def _jsonable(value: np.generic) -> float | int | None:
    """A JSON-safe companion to the bits. ``None`` for NaN and infinities.

    JSON has no NaN or Infinity literal, and ``json.dumps`` emits bare tokens
    for them that strict parsers — Unreal's among them — reject outright. The
    bit pattern carries the value exactly regardless, so this simply goes null.
    """
    number = float(value)
    if math.isnan(number) or math.isinf(number):
        return None
    if np.issubdtype(np.asarray(value).dtype, np.integer):
        return int(value)
    return number


def _extract(arguments: argparse.Namespace) -> int:
    """Run ``extract``."""
    root: Path = arguments.case
    if not root.is_dir():
        print(f"{root}: case root is not a directory", file=sys.stderr)
        return EXIT_INVALID

    manifest = load_manifest(root)
    entry = _resolve_field(manifest, arguments.field)
    times = (manifest.get("timeline") or {}).get("times") or []
    frame = int(arguments.frame)
    if not 0 <= frame < len(times):
        print(
            f"frame {frame} is outside the case's {len(times)} stored frames "
            f"(0..{len(times) - 1})",
            file=sys.stderr,
        )
        return EXIT_INVALID

    pattern = (entry.get("storage") or {}).get("pathPattern")
    if not isinstance(pattern, str):
        print(
            f"field {arguments.field!r} declares no storage.pathPattern",
            file=sys.stderr,
        )
        return EXIT_INVALID
    path = frame_path(root, pattern, frame)
    if not path.is_file():
        print(f"{pattern.format(frame=frame)}: no such file", file=sys.stderr)
        return EXIT_INVALID

    with CVFReader(path) as reader:
        values = reader.read_all()
        header = reader.header

    if arguments.voxel is not None:
        return _extract_voxel(arguments, manifest, entry, values, frame)

    if arguments.output is not None:
        # Saved in the STORED dtype, not promoted to float64: spec 1.6 forbids
        # silently rescaling values, and a float16 case that comes back as
        # float64 has quietly gained precision it never had.
        destination = Path(arguments.output)
        destination.parent.mkdir(parents=True, exist_ok=True)
        np.save(destination, values, allow_pickle=False)

    summary = _summarize(arguments.field, frame, times[frame], values, header)
    if arguments.as_json:
        if arguments.output is not None:
            summary["output"] = str(arguments.output)
        print(json.dumps(summary, indent=2, allow_nan=False))
    else:
        _print_summary(summary, arguments.output)
    return EXIT_OK


def _summarize(field_id: str, frame: int, time: Any, values: np.ndarray,
               header: Any) -> dict[str, Any]:
    """Per-component range over the decoded values, ignoring NaN."""
    components = values.shape[3]
    data = values.astype(np.float64).reshape(-1, components)
    valid = ~np.isnan(data)
    minimum: list[float | None] = []
    maximum: list[float | None] = []
    for index in range(components):
        column = data[valid[:, index], index]
        minimum.append(float(column.min()) if column.size else None)
        maximum.append(float(column.max()) if column.size else None)
    return {
        "field": field_id,
        "frame": frame,
        "time": float(time),
        "dimensions": [int(n) for n in values.shape[:3]],
        "componentCount": components,
        "dataType": header.data_type_name,
        "codec": header.codec_name,
        "min": minimum,
        "max": maximum,
        "nanCount": int(np.count_nonzero(~valid)),
        "valueCount": int(data.size),
    }


def _print_summary(summary: dict[str, Any], output: Path | None) -> None:
    extent = "x".join(str(n) for n in summary["dimensions"])
    print(
        f"{summary['field']} frame {summary['frame']} at t = {summary['time']}"
    )
    print(
        f"  {extent} cells x{summary['componentCount']} "
        f"{summary['dataType']} codec={summary['codec']}"
    )
    for index, (low, high) in enumerate(zip(summary["min"], summary["max"])):
        label = "no valid data" if low is None else f"min {low:.6g}  max {high:.6g}"
        print(f"  component {index}: {label}")
    print(f"  NaN {summary['nanCount']} of {summary['valueCount']} values")
    if output is not None:
        print(f"  wrote {output}")


def _extract_voxel(arguments: argparse.Namespace, manifest: dict[str, Any],
                   entry: dict[str, Any], values: np.ndarray,
                   frame: int) -> int:
    """Print one voxel, saying "masked" where the mask rejects the cell."""
    voxel = tuple(int(v) for v in arguments.voxel)
    extent = values.shape[:3]
    if any(not 0 <= voxel[axis] < extent[axis] for axis in range(3)):
        print(
            f"voxel {list(voxel)} is outside the {extent[0]}x{extent[1]}x"
            f"{extent[2]} grid",
            file=sys.stderr,
        )
        return EXIT_INVALID

    sample = values[voxel]
    masked = _voxel_is_masked(arguments.case, manifest, entry, voxel, frame)

    if arguments.as_json:
        print(json.dumps(
            {
                "field": arguments.field,
                "frame": frame,
                "voxel": list(voxel),
                "value": [_jsonable(v) for v in sample],
                "bits": [_bits_of(v) for v in sample],
                "masked": masked,
            },
            indent=2, allow_nan=False,
        ))
        return EXIT_OK

    names = entry.get("components") or [str(i) for i in range(len(sample))]
    rendered = "  ".join(
        f"{name}={float(value):.6g}" for name, value in zip(names, sample)
    )
    # "masked" is a different fact from "nan": one says there is no data here
    # by construction, the other says the number is not a number. Printing only
    # the value would collapse the two.
    suffix = "  [masked: this cell is inside the obstacle]" if masked else ""
    print(f"{arguments.field}{list(voxel)} frame {frame}: {rendered}{suffix}")
    print("  bits " + " ".join(_bits_of(value) for value in sample))
    return EXIT_OK


def _voxel_is_masked(root: Path, manifest: dict[str, Any],
                     entry: dict[str, Any], voxel: tuple[int, int, int],
                     frame: int) -> bool:
    """Whether the grid's mask field rejects this cell. ``False`` if unknown."""
    grid = None
    for candidate in manifest.get("grids") or []:
        if isinstance(candidate, dict) and candidate.get("id") == entry.get("grid"):
            grid = candidate
            break
    mask_id = grid.get("maskField") if isinstance(grid, dict) else None
    if not mask_id or mask_id == entry.get("id"):
        return False
    mask_entry = field_by_id(manifest, str(mask_id))
    if mask_entry is None:
        return False
    pattern = (mask_entry.get("storage") or {}).get("pathPattern")
    if not isinstance(pattern, str):
        return False
    path = frame_path(root, pattern, frame)
    if not path.is_file():
        return False
    with CVFReader(path) as reader:
        cell = reader.read_region(voxel, tuple(v + 1 for v in voxel))
    return bool(cell.reshape(-1)[0] == 0)


# ---------------------------------------------------------------------------
# benchmark-read
# ---------------------------------------------------------------------------

def _benchmark_read(arguments: argparse.Namespace) -> int:
    """Run ``benchmark-read``."""
    root: Path = arguments.case
    if not root.is_dir():
        print(f"{root}: case root is not a directory", file=sys.stderr)
        return EXIT_INVALID

    manifest = load_manifest(root)
    times = (manifest.get("timeline") or {}).get("times") or []
    wanted = set(arguments.fields) if arguments.fields else None
    entries = [
        entry for entry in (manifest.get("fields") or [])
        if isinstance(entry, dict)
        and (wanted is None or entry.get("id") in wanted)
    ]
    if wanted:
        missing = wanted - {str(entry.get("id")) for entry in entries}
        if missing:
            raise ManifestError(
                f"unknown field(s) {sorted(missing)}; this case declares "
                f"{sorted(str(f.get('id')) for f in manifest.get('fields') or [])}"
            )
    if not entries or not times:
        print(f"{root}: nothing to read", file=sys.stderr)
        return EXIT_INVALID

    best: dict[str, float] | None = None
    best_total = math.inf
    decoded = 0
    repeats = max(1, int(arguments.repeat))

    for pass_index in range(repeats):
        per_field: dict[str, float] = {}
        bytes_this_pass = 0
        for entry in entries:
            pattern = (entry.get("storage") or {}).get("pathPattern")
            if not isinstance(pattern, str):
                continue
            field_id = str(entry.get("id"))
            elapsed = 0.0
            for frame in range(len(times)):
                path = frame_path(root, pattern, frame)
                # perf_counter, not process_time: a reader's cost is dominated
                # by I/O the process is *waiting* on, and CPU time would hide
                # exactly the stall this benchmark exists to expose.
                start = time.perf_counter()
                values = read_all_values(path)
                elapsed += time.perf_counter() - start
                bytes_this_pass += values.nbytes
            per_field[field_id] = elapsed
        total = sum(per_field.values())
        if total < best_total:
            best_total, best = total, per_field
        decoded = bytes_this_pass

    assert best is not None
    seconds = best_total
    # A zero-second elapsed time would make throughput infinite, which JSON
    # cannot express and which is never a true measurement anyway.
    throughput = decoded / seconds / (1024 * 1024) if seconds > 0 else 0.0
    report = {
        "case": str(root),
        "frames": len(times),
        "fields": {name: round(value, 6) for name, value in sorted(best.items())},
        "decodedBytes": decoded,
        "seconds": seconds,
        "megabytesPerSecond": throughput,
        "repeats": repeats,
    }
    if arguments.as_json:
        print(json.dumps(report, indent=2, allow_nan=False))
        return EXIT_OK

    print(f"{root}")
    print(
        f"  {len(times)} frames x {len(best)} fields, "
        f"{decoded / 1024 / 1024:.2f} MiB decoded"
    )
    print(f"  {seconds:.4f} s  ->  {throughput:.1f} MB/s")
    if repeats > 1:
        print(f"  fastest of {repeats} passes")
    for name, value in sorted(best.items(), key=lambda item: -item[1]):
        share = 100.0 * value / seconds if seconds > 0 else 0.0
        print(f"    {name:<20} {value:.4f} s  ({share:.0f}%)")
    return EXIT_OK


def read_all_values(path: Path):
    """Decode one ``.cvf`` completely, verifying every CRC on the way."""
    with CVFReader(path) as reader:
        return reader.read_all()


_COMMANDS = {
    "validate": _validate,
    "info": _info,
    "inspect": _info,
    "known-values": _known_values,
    "generate-mock": _generate_mock,
    "import-fluidx3d": _import_fluidx3d,
    "extract": _extract,
    "benchmark-read": _benchmark_read,
}


def main(argv: Sequence[str] | None = None) -> int:
    """Entry point for the ``cfdviz`` console script.

    Args:
        argv: Arguments *excluding* the program name. Defaults to ``sys.argv``.

    Returns:
        A process exit status: 0 on success, non-zero otherwise. Nothing here
        raises for bad input — a broken case is reported, not thrown.
    """
    parser = build_parser()
    arguments = parser.parse_args(list(sys.argv[1:] if argv is None else argv))

    if arguments.version:
        print(f"cfdviz {__version__} (CFDViz format {FORMAT_VERSION})")
        return EXIT_OK
    if not arguments.command:
        parser.print_usage()
        print("cfdviz: a subcommand is required", file=sys.stderr)
        return EXIT_USAGE

    handler = _COMMANDS[arguments.command]
    try:
        return handler(arguments)
    except (CVFError, CodecError, ConversionError, ManifestError, OSError) as exc:
        # Expected failure: the input is bad. One line, no traceback.
        print(f"cfdviz: {exc}", file=sys.stderr)
        return EXIT_INVALID
    except Exception as exc:  # noqa: BLE001 - last line of defence
        # Unexpected failure: still no traceback on stdout, still non-zero, but
        # labelled so a bug report can be told apart from a bad file.
        print(f"cfdviz: internal error: {type(exc).__name__}: {exc}",
              file=sys.stderr)
        return EXIT_INVALID


if __name__ == "__main__":  # pragma: no cover - exercised via subprocess
    sys.exit(main())
