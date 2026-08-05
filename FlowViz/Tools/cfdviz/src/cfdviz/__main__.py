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
import sys
from pathlib import Path
from typing import Sequence

from . import FORMAT_VERSION, __version__
from .case import (
    KNOWN_VALUES_NAME,
    build_known_values,
    validate_case,
    verify_known_values,
    write_known_values,
)
from .codecs import CodecError
from .cvf import CVFError, CVFReader
from .manifest import ManifestError, load_manifest

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
        description="Inspect and validate CFDViz 1.0 cases.",
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

    info = subparsers.add_parser(
        "info", help="summarise a case directory or a single .cvf file"
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
    return parser


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
    print(f"  frames {len(times)}", end="")
    if times:
        print(f"  t = {times[0]} .. {times[-1]}")
    else:
        print()

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
        print(
            f"  field {entry.get('id')} (#{entry.get('numericId')}): "
            f"{entry.get('componentCount')}x{entry.get('dataType')} "
            f"{entry.get('association')} codec={storage.get('codec')}"
        )
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


_COMMANDS = {
    "validate": _validate,
    "info": _info,
    "known-values": _known_values,
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
    except (CVFError, CodecError, ManifestError, OSError) as exc:
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
