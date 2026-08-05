"""``manifest.json`` — loading, schema validation, and the section 3.1 invariants.

See ``Docs/CFDVIZ_FORMAT.md`` section 3. The JSON Schema in
:data:`SCHEMA_PATH` is itself normative and names this module as the
authoritative implementation of two things JSON Schema cannot express on its
own:

* :func:`is_safe_relative_path` — the path-traversal rule of spec 1.3. The
  schema's ``relativePath`` pattern mirrors this function so that schema
  validation alone already rejects traversal, but this function is the one that
  wins if the two ever disagree.
* :func:`validate_manifest` — the cross-reference, uniqueness, and ordering
  invariants of spec 3.1.

Validation **returns problems rather than raising** them. Spec section 10
requires a validator to report every issue in a case, so stopping at the first
one would make the tool useless on exactly the files that need it most.
:func:`load_manifest` does raise, because a manifest that will not parse leaves
nothing to report against.

``jsonschema`` is an optional dependency. When it is absent, schema validation
is skipped and the invariant checks — which are the ones that catch real
authoring mistakes — still run. That is a deliberate degradation, not a
silent one: :func:`schema_validation_available` lets a caller say so.
"""

from __future__ import annotations

import json
import math
import re
from pathlib import Path
from typing import Any, Final, Iterable, Sequence

__all__ = [
    "SCHEMA_PATH",
    "FORMAT_MARKER",
    "FORMAT_VERSION",
    "SUPPORTED_MAJOR_VERSION",
    "VALID_DATA_TYPES",
    "VALID_ASSOCIATIONS",
    "RESERVED_ASSOCIATIONS",
    "ManifestError",
    "is_safe_relative_path",
    "load_schema",
    "schema_validation_available",
    "load_manifest",
    "validate_manifest",
    "field_by_id",
    "grid_by_id",
    "frame_path",
]

#: The normative schema, shipped inside the package (see ``package-data``).
SCHEMA_PATH: Final[Path] = (
    Path(__file__).resolve().parent / "schema" / "cfdviz-1.0.schema.json"
)

#: ``format`` must be exactly this string; anything else is not a CFDViz case.
FORMAT_MARKER: Final = "CFDViz"

#: The format version this implementation writes.
FORMAT_VERSION: Final = "1.0.0"

#: The major version this implementation reads. A newer *minor* is accepted
#: (spec 1.4); a different major is not.
SUPPORTED_MAJOR_VERSION: Final = 1

#: Spec 3.3 — float64 field storage is not supported in 1.0.
VALID_DATA_TYPES: Final[frozenset[str]] = frozenset({"float16", "float32", "uint8"})

#: Spec 3.2 — 1.0 supports cell and point only.
VALID_ASSOCIATIONS: Final[frozenset[str]] = frozenset({"cell", "point"})

#: Spec 3.2 — reserved for a future version and MUST be rejected in 1.0. Named
#: so the error can say "reserved" rather than the less useful "unknown".
RESERVED_ASSOCIATIONS: Final[frozenset[str]] = frozenset(
    {"mesh-vertex", "mesh-element", "integration-point", "face", "particle"}
)

_VERSION_RE: Final = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:[-+][0-9A-Za-z.+-]+)?$")

#: Control characters are refused outright: they cannot appear in a legitimate
#: path and they are a classic way to smuggle something past a log or a UI.
_CONTROL_CHARACTERS: Final = re.compile(r"[\x00-\x1f]")

#: A Windows drive prefix, e.g. ``C:``. Absolute on Windows even without a
#: leading slash, so a naive "does not start with /" check would let it through.
_DRIVE_PREFIX: Final = re.compile(r"^[A-Za-z]:")


class ManifestError(Exception):
    """A manifest could not be read or parsed.

    Raised only for problems that leave nothing to validate. Everything that can
    be reported is reported, by :func:`validate_manifest`, as a list of strings.
    """


# ---------------------------------------------------------------------------
# Path safety — spec 1.3
# ---------------------------------------------------------------------------

def is_safe_relative_path(path: str) -> bool:
    """Return whether ``path`` is a case-relative path that cannot escape the root.

    Spec 1.3: a reader MUST reject any path containing a ``..`` segment, an
    absolute path, a drive letter, or a leading ``/``. **The check is lexical
    and happens before any filesystem call**, which rules out an implementation
    built on ``Path.resolve()``: resolving ``frames/000000/../../frames/x`` lands
    back inside the root, so a resolve-based check would accept a path the spec
    forbids — and would also touch the filesystem while deciding whether it is
    safe to do so.

    Backslashes are rejected as well. They separate on Windows, so ``..\\x``
    would escape there while looking like a single innocuous filename here.

    Args:
        path: The candidate path, exactly as it appears in the manifest.

    Returns:
        ``True`` if the path is safe to join onto a case root.
    """
    if not isinstance(path, str) or not path:
        return False
    if _CONTROL_CHARACTERS.search(path):
        return False
    if "\\" in path:
        return False
    if path.startswith("/"):
        return False
    if _DRIVE_PREFIX.match(path):
        return False
    # Split on '/' only: a '..' *segment* is the danger, a '..' inside a name
    # (``..hidden``, ``x..y``) is an ordinary filename.
    return all(segment != ".." for segment in path.split("/"))


# ---------------------------------------------------------------------------
# Schema
# ---------------------------------------------------------------------------

_SCHEMA_CACHE: dict[str, Any] | None = None


def load_schema() -> dict[str, Any]:
    """Load the normative JSON Schema shipped with the package."""
    global _SCHEMA_CACHE
    if _SCHEMA_CACHE is None:
        _SCHEMA_CACHE = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    return _SCHEMA_CACHE


def schema_validation_available() -> bool:
    """Whether ``jsonschema`` is installed and schema checks will actually run."""
    try:
        import jsonschema  # noqa: F401
    except ImportError:
        return False
    return True


def _schema_problems(manifest: dict[str, Any]) -> list[str]:
    """Every schema violation, sorted, or an empty list if the check is skipped."""
    try:
        import jsonschema
    except ImportError:
        return []
    validator = jsonschema.Draft202012Validator(load_schema())
    problems = []
    for error in sorted(validator.iter_errors(manifest), key=lambda e: list(e.path)):
        location = "/".join(str(part) for part in error.path) or "<root>"
        problems.append(f"manifest schema: {location}: {error.message}")
    return problems


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------

def load_manifest(path: Path | str) -> dict[str, Any]:
    """Read and parse ``manifest.json``.

    Args:
        path: The manifest file, or the case root directory containing it.

    Returns:
        The parsed manifest object.

    Raises:
        ManifestError: If the file cannot be read, carries a byte-order mark,
            is not valid JSON, or is not a JSON object. The message always names
            the file, per spec 10.
    """
    path = Path(path)
    if path.is_dir():
        path = path / "manifest.json"
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise ManifestError(f"{path}: cannot read manifest: {exc}") from exc

    # Spec 1.2: UTF-8 without a BOM. Detected explicitly rather than stripped,
    # because a BOM means the writer is not producing what the format requires
    # and strict parsers elsewhere in the toolchain will choke on it.
    if raw.startswith(b"\xef\xbb\xbf"):
        raise ManifestError(
            f"{path}: manifest starts with a UTF-8 byte-order mark; CFDViz JSON is "
            "UTF-8 without a BOM (spec 1.2)"
        )
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ManifestError(f"{path}: manifest is not valid UTF-8: {exc}") from exc
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError as exc:
        raise ManifestError(
            f"{path}: line {exc.lineno} column {exc.colno}: invalid JSON: {exc.msg}"
        ) from exc
    if not isinstance(parsed, dict):
        raise ManifestError(
            f"{path}: manifest must be a JSON object, got {type(parsed).__name__}"
        )
    return parsed


# ---------------------------------------------------------------------------
# Validation — spec 3.1
# ---------------------------------------------------------------------------

def _as_list(value: Any) -> list[Any]:
    """Coerce to a list, so a malformed manifest degrades instead of crashing."""
    return value if isinstance(value, list) else []


def _duplicates(values: Iterable[Any]) -> list[Any]:
    """Values appearing more than once, in first-seen order."""
    seen: set[Any] = set()
    repeated: list[Any] = []
    for value in values:
        try:
            if value in seen:
                if value not in repeated:
                    repeated.append(value)
            else:
                seen.add(value)
        except TypeError:  # unhashable: schema validation already flagged it
            continue
    return repeated


def validate_manifest(manifest: dict[str, Any]) -> list[str]:
    """Check a manifest against the schema and every spec 3.1 invariant.

    Returns problems rather than raising, so a validator can report all of them
    at once (spec 10). An empty list means the manifest is valid.

    Args:
        manifest: A parsed manifest object.

    Returns:
        Human-readable problem descriptions, in a stable order.
    """
    problems: list[str] = []
    if not isinstance(manifest, dict):
        return [f"manifest must be a JSON object, got {type(manifest).__name__}"]

    problems.extend(_schema_problems(manifest))

    # -- format marker and version ------------------------------------------
    marker = manifest.get("format")
    if marker != FORMAT_MARKER:
        problems.append(
            f"format is {marker!r}, expected {FORMAT_MARKER!r}; this is not a CFDViz "
            "case"
        )

    version = manifest.get("version")
    match = _VERSION_RE.match(version) if isinstance(version, str) else None
    if match is None:
        problems.append(
            f"version {version!r} is not a MAJOR.MINOR.PATCH semantic version"
        )
    elif int(match.group(1)) != SUPPORTED_MAJOR_VERSION:
        # Spec 1.4: unsupported major rejects; a newer minor is fine.
        problems.append(
            f"unsupported major version {match.group(1)} in version {version!r}; this "
            f"reader implements CFDViz {SUPPORTED_MAJOR_VERSION}.x"
        )

    # -- timeline ------------------------------------------------------------
    timeline = manifest.get("timeline")
    if isinstance(timeline, dict):
        times = _as_list(timeline.get("times"))
        frame_count = timeline.get("frameCount")
        if isinstance(frame_count, int) and not isinstance(frame_count, bool):
            if frame_count != len(times):
                problems.append(
                    f"timeline.frameCount is {frame_count} but timeline.times has "
                    f"{len(times)} entries"
                )
            steps = timeline.get("steps")
            if isinstance(steps, list) and len(steps) != frame_count:
                problems.append(
                    f"timeline.frameCount is {frame_count} but timeline.steps has "
                    f"{len(steps)} entries"
                )
        numeric = [t for t in times if isinstance(t, (int, float))
                   and not isinstance(t, bool)]
        if len(numeric) != len(times):
            problems.append("timeline.times contains a non-numeric entry")
        elif any(math.isnan(t) for t in numeric):
            # A NaN time is never "greater than" its predecessor, so it would be
            # caught below too; saying so plainly is more actionable.
            problems.append(
                "timeline.times contains NaN, which is not a legal time and cannot be "
                "monotonically increasing"
            )
        elif any(b <= a for a, b in zip(numeric, numeric[1:])):
            problems.append(
                "timeline.times is not strictly monotonically increasing: "
                f"{numeric!r}"
            )

    # -- grids ---------------------------------------------------------------
    grids = _as_list(manifest.get("grids"))
    grid_ids = [g.get("id") for g in grids if isinstance(g, dict)]
    for repeated in _duplicates(grid_ids):
        problems.append(f"duplicate grid id {repeated!r}")

    fields = _as_list(manifest.get("fields"))
    field_ids = [f.get("id") for f in fields if isinstance(f, dict)]
    known_field_ids = set(field_ids)

    for index, grid in enumerate(grids):
        if not isinstance(grid, dict):
            problems.append(f"grids[{index}] is not an object")
            continue
        label = f"grid {grid.get('id', index)!r}"
        dimensions = grid.get("dimensions")
        if isinstance(dimensions, list) and len(dimensions) == 3:
            for axis, value in zip("XYZ", dimensions):
                if not isinstance(value, int) or isinstance(value, bool) or value < 1:
                    problems.append(
                        f"{label}: dimensions {axis} is {value!r}; every dimension "
                        "must be an integer of at least 1"
                    )
        else:
            problems.append(f"{label}: dimensions must be 3 integers")

        spacing = grid.get("spacing")
        if isinstance(spacing, list) and len(spacing) == 3:
            for axis, value in zip("XYZ", spacing):
                if (
                    not isinstance(value, (int, float))
                    or isinstance(value, bool)
                    or not (value > 0)
                ):
                    problems.append(
                        f"{label}: spacing {axis} is {value!r}; every spacing "
                        "component must be greater than 0"
                    )
        else:
            problems.append(f"{label}: spacing must be 3 numbers")

        mask_field = grid.get("maskField")
        if mask_field is not None and mask_field not in known_field_ids:
            problems.append(
                f"{label}: maskField {mask_field!r} does not name a declared field; "
                f"declared fields are {sorted(str(f) for f in known_field_ids)}"
            )

    # -- fields --------------------------------------------------------------
    for repeated in _duplicates(field_ids):
        problems.append(f"duplicate field id {repeated!r}")
    for repeated in _duplicates(
        f.get("numericId") for f in fields if isinstance(f, dict)
    ):
        problems.append(f"duplicate field numericId {repeated!r}")

    known_grid_ids = set(grid_ids)
    for index, entry in enumerate(fields):
        if not isinstance(entry, dict):
            problems.append(f"fields[{index}] is not an object")
            continue
        label = f"field {entry.get('id', index)!r}"

        grid_id = entry.get("grid")
        if grid_id not in known_grid_ids:
            problems.append(
                f"{label}: grid {grid_id!r} does not name a declared grid; declared "
                f"grids are {sorted(str(g) for g in known_grid_ids)}"
            )

        components = entry.get("components")
        component_count = entry.get("componentCount")
        if isinstance(components, list) and isinstance(component_count, int) and not (
            isinstance(component_count, bool)
        ):
            if len(components) != component_count:
                problems.append(
                    f"{label}: componentCount is {component_count} but components has "
                    f"{len(components)} names: {components!r}"
                )

        data_type = entry.get("dataType")
        if data_type not in VALID_DATA_TYPES:
            extra = (
                " float64 field storage is not supported in 1.0 (spec 3.3); record it "
                "in solverPrecision instead."
                if data_type == "float64"
                else ""
            )
            problems.append(
                f"{label}: dataType {data_type!r} is not one of "
                f"{sorted(VALID_DATA_TYPES)}.{extra}"
            )

        association = entry.get("association")
        if association not in VALID_ASSOCIATIONS:
            extra = (
                " That association is reserved for a future version and MUST be "
                "rejected in 1.0 (spec 3.2)."
                if association in RESERVED_ASSOCIATIONS
                else ""
            )
            problems.append(
                f"{label}: association {association!r} is not one of "
                f"{sorted(VALID_ASSOCIATIONS)}.{extra}"
            )

        storage = entry.get("storage")
        if isinstance(storage, dict):
            pattern = storage.get("pathPattern")
            if not isinstance(pattern, str) or not is_safe_relative_path(pattern):
                problems.append(
                    f"{label}: storage.pathPattern {pattern!r} is not a safe "
                    "case-relative path; it must not be absolute, contain a drive "
                    "letter, a backslash, or a '..' segment (spec 1.3)"
                )
        else:
            problems.append(f"{label}: storage must be an object")

    # -- meshes and structures ----------------------------------------------
    meshes = _as_list(manifest.get("meshes"))
    for repeated in _duplicates(m.get("id") for m in meshes if isinstance(m, dict)):
        problems.append(f"duplicate mesh id {repeated!r}")
    for index, mesh in enumerate(meshes):
        if not isinstance(mesh, dict):
            problems.append(f"meshes[{index}] is not an object")
            continue
        mesh_path = mesh.get("path")
        if not isinstance(mesh_path, str) or not is_safe_relative_path(mesh_path):
            problems.append(
                f"mesh {mesh.get('id', index)!r}: path {mesh_path!r} is not a safe "
                "case-relative path (spec 1.3)"
            )

    known_mesh_ids = {m.get("id") for m in meshes if isinstance(m, dict)}
    for index, structure in enumerate(_as_list(manifest.get("structures"))):
        if not isinstance(structure, dict):
            problems.append(f"structures[{index}] is not an object")
            continue
        mesh_id = structure.get("mesh")
        if mesh_id not in known_mesh_ids:
            problems.append(
                f"structure {structure.get('id', index)!r}: mesh {mesh_id!r} does not "
                "name a declared mesh"
            )

    return problems


# ---------------------------------------------------------------------------
# Convenience lookups
# ---------------------------------------------------------------------------

def field_by_id(manifest: dict[str, Any], field_id: str) -> dict[str, Any] | None:
    """Return the field entry with ``id == field_id``, or ``None``."""
    for entry in _as_list(manifest.get("fields")):
        if isinstance(entry, dict) and entry.get("id") == field_id:
            return entry
    return None


def grid_by_id(manifest: dict[str, Any], grid_id: str) -> dict[str, Any] | None:
    """Return the grid entry with ``id == grid_id``, or ``None``."""
    for entry in _as_list(manifest.get("grids")):
        if isinstance(entry, dict) and entry.get("id") == grid_id:
            return entry
    return None


def frame_path(root: Path | str, pattern: str, frame: int) -> Path:
    """Resolve a ``pathPattern`` for one frame, refusing to escape ``root``.

    The traversal check runs on the **expanded** pattern as well as the pattern
    itself: a format placeholder cannot inject a ``..`` here, but checking the
    result costs nothing and removes the need to reason about that.

    Args:
        root: Case root directory.
        pattern: A ``storage.pathPattern``, e.g. ``frames/{frame:06d}/U.cvf``.
        frame: Frame index to substitute.

    Returns:
        The absolute path of that frame's file.

    Raises:
        ManifestError: If the pattern or its expansion is unsafe, or the
            placeholder cannot be formatted.
    """
    if not is_safe_relative_path(pattern):
        raise ManifestError(
            f"pathPattern {pattern!r} is not a safe case-relative path (spec 1.3)"
        )
    try:
        relative = pattern.format(frame=frame)
    except (KeyError, IndexError, ValueError) as exc:
        raise ManifestError(
            f"pathPattern {pattern!r} could not be formatted for frame {frame}: {exc}"
        ) from exc
    if not is_safe_relative_path(relative):
        raise ManifestError(
            f"pathPattern {pattern!r} expands to the unsafe path {relative!r}"
        )
    return Path(root) / relative
