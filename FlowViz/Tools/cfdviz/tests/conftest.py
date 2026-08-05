"""Shared fixtures and byte-level helpers for the cfdviz test suite.

Two ideas run through every test module here, and both come from
``Docs/VISUAL_QA.md`` section 4 ("a metric that cannot fail is not a check"):

1. **Layout is asserted against hand-built bytes**, never only against a
   round-trip. A round-trip passes when the writer and the reader are wrong in
   the same way; a hand-built buffer built straight from the spec's offset table
   does not.
2. **Corruption tests name the reason.** Every ``pytest.raises`` in this suite
   carries a ``match=`` pattern, so a test cannot pass because the file was
   missing when it was supposed to fail on a CRC.

The ``tamper_*`` helpers exist for the second point: they overwrite one field
and then *repair* the header CRC, so the reader is forced to fail on the field
under test rather than on the checksum that happens to cover it.
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

import numpy as np
import pytest

# pyproject.toml already sets pythonpath = ["src"], but tests are also run from
# the repository root by agents and CI wrappers, where that ini is not picked up.
_SRC = Path(__file__).resolve().parents[1] / "src"
if str(_SRC) not in sys.path:
    sys.path.insert(0, str(_SRC))

from cfdviz.crc32c import crc32c  # noqa: E402


# ---------------------------------------------------------------------------
# Bit-exact comparison
# ---------------------------------------------------------------------------

def bit_pattern(array: np.ndarray) -> np.ndarray:
    """Reinterpret ``array`` as unsigned integers of the same width.

    Comparing floating-point payloads with ``==`` is useless for this format:
    ``NaN != NaN``, and ``-0.0 == 0.0``. The spec requires NaN to survive
    *bit-exactly* (rule 1.7), so every payload comparison in this suite goes
    through here.
    """
    array = np.asarray(array)
    width = array.dtype.itemsize
    return array.view({1: "u1", 2: "<u2", 4: "<u4", 8: "<u8"}[width])


def assert_bits_equal(actual: np.ndarray, expected: np.ndarray, message: str = "") -> None:
    """Assert two arrays are identical bit pattern for bit pattern."""
    actual = np.asarray(actual)
    expected = np.asarray(expected)
    assert actual.dtype == expected.dtype, f"dtype {actual.dtype} != {expected.dtype} {message}"
    assert actual.shape == expected.shape, f"shape {actual.shape} != {expected.shape} {message}"
    assert np.array_equal(bit_pattern(actual), bit_pattern(expected)), (
        f"payload differs bit-for-bit {message}"
    )


# ---------------------------------------------------------------------------
# Header CRC repair, so corruption tests fail for the reason under test
# ---------------------------------------------------------------------------

def _repair_header_crc(blob: bytearray, header_bytes: int, crc_offset: int) -> None:
    """Recompute the header CRC in place, per the spec's zero-the-field rule."""
    blank = bytearray(blob[:header_bytes])
    blank[crc_offset : crc_offset + 4] = b"\x00\x00\x00\x00"
    struct.pack_into("<I", blob, crc_offset, crc32c(bytes(blank)))


def tamper(blob: bytes, offset: int, fmt: str, *values,
           header_bytes: int, crc_offset: int) -> bytes:
    """Overwrite a header field and repair the header CRC.

    Args:
        blob: The original file bytes.
        offset: Byte offset of the field to overwrite.
        fmt: ``struct`` format for the field, e.g. ``"<Q"``.
        values: Replacement value(s).
        header_bytes: Size of the fixed header (128 for CVF, 96 for CVM/CVA).
        crc_offset: Offset of ``headerCrc32c`` (104 for CVF, 80 for CVM/CVA).

    Returns:
        The modified bytes, with a header CRC that is once again correct.
    """
    out = bytearray(blob)
    struct.pack_into(fmt, out, offset, *values)
    _repair_header_crc(out, header_bytes, crc_offset)
    return bytes(out)


def tamper_cvf(blob: bytes, offset: int, fmt: str, *values) -> bytes:
    """:func:`tamper` for a CVF file (128-byte header, CRC at 104)."""
    return tamper(blob, offset, fmt, *values, header_bytes=128, crc_offset=104)


def tamper_cvm(blob: bytes, offset: int, fmt: str, *values) -> bytes:
    """:func:`tamper` for a CVM file (96-byte header, CRC at 80)."""
    return tamper(blob, offset, fmt, *values, header_bytes=96, crc_offset=80)


def tamper_cva(blob: bytes, offset: int, fmt: str, *values) -> bytes:
    """:func:`tamper` for a CVA file (96-byte header, CRC at 80)."""
    return tamper(blob, offset, fmt, *values, header_bytes=96, crc_offset=80)


# ---------------------------------------------------------------------------
# Sample payloads
# ---------------------------------------------------------------------------

#: Values that a naive implementation gets wrong: NaN (must survive, rule 1.7),
#: both infinities, negative zero (compares equal to +0.0 but has other bits),
#: and a subnormal.
AWKWARD_FLOAT32 = np.array(
    [np.nan, -np.nan, np.inf, -np.inf, 0.0, -0.0, 5e-324 if False else 1.4e-45, 1.0],
    dtype="<f4",
)

AWKWARD_FLOAT16 = np.array(
    [np.nan, np.inf, -np.inf, 0.0, -0.0, 6e-08, 1.0, -1.0],
    dtype="<f2",
)


def ramp(shape: tuple[int, ...], dtype: str = "<f4") -> np.ndarray:
    """A deterministic, distinct-per-element array of ``shape``.

    Distinct values matter: an array of a repeated constant round-trips
    successfully even through a reader that transposes its axes.
    """
    count = int(np.prod(shape))
    if np.dtype(dtype) == np.dtype("u1"):
        values = (np.arange(count, dtype=np.int64) * 7 + 3) % 251
        return values.astype("u1").reshape(shape)
    values = np.arange(count, dtype=np.float64) * 0.25 - 3.0
    return values.astype(dtype).reshape(shape)


@pytest.fixture
def case_root(tmp_path: Path) -> Path:
    """An empty case directory."""
    root = tmp_path / "Sample.cfdviz"
    root.mkdir()
    return root


# ---------------------------------------------------------------------------
# A small but complete case, for the manifest and CLI tests
# ---------------------------------------------------------------------------

#: Grid the sample case declares. Deliberately not a cube, so a transposing bug
#: cannot survive by symmetry.
SAMPLE_DIMENSIONS = (4, 3, 2)

#: Two frames is the minimum that exercises monotonic-timeline checking and
#: "first and last stored frame" sampling (spec 9.3).
SAMPLE_TIMES = (0.0, 0.5)


def sample_manifest() -> dict:
    """A manifest that satisfies the schema and every section 3.1 invariant.

    Tests mutate a copy of this to make exactly one thing wrong at a time, which
    is what proves each invariant check can actually fail.
    """
    return {
        "format": "CFDViz",
        "version": "1.0.0",
        "case": {
            "id": "d7f46da7-6bd8-4d4b-9410-32d1ea776328",
            "name": "Sample",
            "quality": "visualization-demo",
        },
        "units": {"length": "m", "time": "s"},
        "coordinates": {"handedness": "right", "upAxis": "Z", "forwardAxis": "X"},
        "timeline": {
            "frameCount": len(SAMPLE_TIMES),
            "times": list(SAMPLE_TIMES),
            "steps": [0, 100],
        },
        "grids": [
            {
                "id": "main",
                "type": "uniform-cartesian",
                "dimensions": list(SAMPLE_DIMENSIONS),
                "origin": [0.0, 0.0, 0.0],
                "spacing": [0.1, 0.1, 0.1],
                "maskField": "validMask",
            }
        ],
        "fields": [
            {
                "numericId": 1,
                "id": "pressure",
                "components": ["p"],
                "componentCount": 1,
                "dataType": "float32",
                "association": "cell",
                "grid": "main",
                "unit": "Pa",
                "storage": {
                    "type": "bricked-volume",
                    "codec": "zlib",
                    "brickSize": [4, 4, 4],
                    "pathPattern": "frames/{frame:06d}/pressure.cvf",
                },
            },
            {
                "numericId": 2,
                "id": "U",
                "components": ["x", "y", "z"],
                "componentCount": 3,
                "dataType": "float32",
                "association": "cell",
                "grid": "main",
                "unit": "m/s",
                "storage": {
                    "type": "bricked-volume",
                    "codec": "zlib",
                    "brickSize": [4, 4, 4],
                    "pathPattern": "frames/{frame:06d}/U.cvf",
                },
            },
            {
                "numericId": 3,
                "id": "validMask",
                "components": ["valid"],
                "componentCount": 1,
                "dataType": "uint8",
                "association": "cell",
                "grid": "main",
                "storage": {
                    "type": "bricked-volume",
                    "codec": "none",
                    "brickSize": [4, 4, 4],
                    "pathPattern": "frames/{frame:06d}/validMask.cvf",
                },
            },
        ],
    }


def sample_field_values(field_id: str, frame: int) -> np.ndarray:
    """Deterministic per-field, per-frame payload for the sample case.

    ``pressure`` carries a NaN and a masked cell so the validator's NaN and mask
    accounting has something real to count.
    """
    nx, ny, nz = SAMPLE_DIMENSIONS
    if field_id == "validMask":
        mask = np.ones((nx, ny, nz, 1), dtype="u1")
        mask[0, 0, 0, 0] = 0
        return mask
    if field_id == "U":
        values = ramp((nx, ny, nz, 3), "<f4") + np.float32(frame)
        return values.astype("<f4")
    values = ramp((nx, ny, nz, 1), "<f4") + np.float32(10 * frame)
    values[1, 1, 0, 0] = np.nan
    return values.astype("<f4")


def build_case(root: Path, manifest: dict | None = None) -> Path:
    """Write a complete, valid case directory and return its root.

    Uses the real writers rather than canned bytes: a validator that passes on
    hand-made files but not on the ones this package emits would be worthless.
    """
    from cfdviz.cvf import write_cvf

    manifest = sample_manifest() if manifest is None else manifest
    root.mkdir(parents=True, exist_ok=True)

    times = manifest["timeline"]["times"]
    mask_values = sample_field_values("validMask", 0)[..., 0].astype(bool)
    for frame, time in enumerate(times):
        for entry in manifest["fields"]:
            values = sample_field_values(entry["id"], frame)
            relative = entry["storage"]["pathPattern"].format(frame=frame)
            write_cvf(
                root / relative,
                values=values,
                dtype=entry["dataType"],
                association=entry["association"],
                codec={"none": 0, "zstd": 1, "lz4": 2, "zlib": 3}[
                    entry["storage"]["codec"]
                ],
                brick_size=tuple(entry["storage"].get("brickSize", (32, 32, 32))),
                frame_index=frame,
                field_numeric_id=entry["numericId"],
                simulation_time=time,
                mask=None if entry["id"] == "validMask" else mask_values,
            )

    (root / "manifest.json").write_text(
        json.dumps(manifest, indent=2), encoding="utf-8"
    )
    return root


@pytest.fixture
def valid_case(tmp_path: Path) -> Path:
    """A complete, valid case on disk."""
    return build_case(tmp_path / "Sample.cfdviz")
