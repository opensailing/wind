"""CVF — bricked volume field. Format spec section 4.

This module carries the byte-exactness burden for the format, because CVF is the
container the Unreal reader spends all its time in.

Three kinds of test live here and they are not interchangeable:

* **Layout tests** assert individual header and directory fields at the exact
  offsets in the spec's tables, and decode a file assembled *by hand* from those
  tables. A round-trip cannot do this job: it passes when the writer and the
  reader share the same wrong idea of the layout.
* **Round-trip tests** cover every dtype, every codec, every association, NaN
  and infinity preservation, and partial edge bricks.
* **Rejection tests** feed truncated and corrupted files in and require a
  specific exception. Each one tampers with a single field and then repairs the
  header CRC, so it cannot pass for the wrong reason.
"""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np
import pytest

from conftest import assert_bits_equal, ramp, tamper_cvf
from cfdviz.codecs import CODEC_LZ4, CODEC_NONE, CODEC_ZLIB, CODEC_ZSTD, ZSTD_REJECTION_MESSAGE, UnsupportedCodecError, is_codec_available
from cfdviz.crc32c import crc32c
from cfdviz.cvf import (
    CVF_ASSOC_CELL,
    CVF_ASSOC_POINT,
    CVF_DIRECTORY_ENTRY_BYTES,
    CVF_DTYPE_FLOAT16,
    CVF_DTYPE_FLOAT32,
    CVF_DTYPE_UINT8,
    CVF_ENDIAN_MARKER,
    CVF_HEADER_BYTES,
    CVF_MAGIC,
    CVFError,
    CVFFormatError,
    CVFReader,
    read_cvf,
    write_cvf,
)


# ---------------------------------------------------------------------------
# Constants straight from the spec
# ---------------------------------------------------------------------------

def test_format_constants_match_the_spec():
    assert CVF_MAGIC == b"CFDVOL1\x00"
    assert len(CVF_MAGIC) == 8
    assert CVF_HEADER_BYTES == 128
    assert CVF_DIRECTORY_ENTRY_BYTES == 80
    assert CVF_ENDIAN_MARKER == 0x01020304
    # Section 4.2 — these are bytes on disk, not implementation detail.
    assert (CVF_DTYPE_FLOAT16, CVF_DTYPE_FLOAT32, CVF_DTYPE_UINT8) == (1, 2, 3)
    assert (CVF_ASSOC_CELL, CVF_ASSOC_POINT) == (0, 1)


def test_cvf_dtype_ids_agree_with_cva_where_the_spec_says_they_must():
    """Section 6.2: "1 and 2 match CVF 4.2 so a shared C++ enum needs no translation"."""
    from cfdviz.cva import CVA_DTYPE_FLOAT16, CVA_DTYPE_FLOAT32

    assert CVA_DTYPE_FLOAT16 == CVF_DTYPE_FLOAT16 == 1
    assert CVA_DTYPE_FLOAT32 == CVF_DTYPE_FLOAT32 == 2


# ---------------------------------------------------------------------------
# Header layout — field by field, against section 4.1's offset table
# ---------------------------------------------------------------------------

def _write_reference(path: Path, **overrides) -> np.ndarray:
    """Write a small, fully-populated CVF and return the values written."""
    values = ramp((5, 4, 3, 2), "<f4")
    kwargs = dict(
        values=values,
        brick_size=(4, 4, 4),
        frame_index=7,
        field_numeric_id=42,
        simulation_time=1.25,
        association=CVF_ASSOC_CELL,
        dtype="float32",
        codec=CODEC_NONE,
        background_value=(1.0, 2.0, 3.0, 4.0),
    )
    kwargs.update(overrides)
    write_cvf(path, **kwargs)
    return values


def test_header_field_offsets(tmp_path: Path):
    """Every field of section 4.1, read back at its documented offset.

    The struct format string is written out here independently of the one in
    ``cvf.py``. If the module's format string drifts, this fails; if this test
    merely re-used the module's constant, it could not.
    """
    path = tmp_path / "reference.cvf"
    _write_reference(path)
    blob = path.read_bytes()

    assert blob[0:8] == b"CFDVOL1\x00"
    assert struct.unpack_from("<I", blob, 8)[0] == 128          # headerBytes
    assert struct.unpack_from("<H", blob, 12)[0] == 1           # majorVersion
    assert struct.unpack_from("<H", blob, 14)[0] == 0           # minorVersion
    assert struct.unpack_from("<I", blob, 16)[0] == 0x01020304  # endianMarker
    assert struct.unpack_from("<I", blob, 20)[0] == 0           # flags
    assert struct.unpack_from("<I", blob, 24)[0] == 7           # frameIndex
    assert struct.unpack_from("<I", blob, 28)[0] == 42          # fieldNumericId
    assert struct.unpack_from("<d", blob, 32)[0] == 1.25        # simulationTime
    assert struct.unpack_from("<I", blob, 40)[0] == 5           # dimensionX
    assert struct.unpack_from("<I", blob, 44)[0] == 4           # dimensionY
    assert struct.unpack_from("<I", blob, 48)[0] == 3           # dimensionZ
    assert struct.unpack_from("<H", blob, 52)[0] == 4           # brickSizeX
    assert struct.unpack_from("<H", blob, 54)[0] == 4           # brickSizeY
    assert struct.unpack_from("<H", blob, 56)[0] == 4           # brickSizeZ
    assert blob[58] == 2                                        # componentCount
    assert blob[59] == CVF_DTYPE_FLOAT32                        # dataType
    assert blob[60] == CVF_ASSOC_CELL                           # association
    assert blob[61] == CODEC_NONE                               # codec
    assert struct.unpack_from("<H", blob, 62)[0] == 0           # reserved
    assert struct.unpack_from("<Q", blob, 64)[0] == 2           # brickCount (2x1x1)
    assert struct.unpack_from("<Q", blob, 72)[0] == 128         # directoryOffset
    payload_offset = struct.unpack_from("<Q", blob, 80)[0]
    assert payload_offset == 128 + 2 * 80                       # payloadOffset
    assert struct.unpack_from("<4f", blob, 88) == (1.0, 2.0, 3.0, 4.0)  # backgroundValue
    assert blob[108:128] == b"\x00" * 20                        # reserved


def test_header_crc_covers_the_header_with_its_own_field_zeroed(tmp_path: Path):
    """Section 4.1: CRC-32C over [0, 128) with [104, 108) set to zero."""
    path = tmp_path / "crc.cvf"
    _write_reference(path)
    blob = path.read_bytes()

    stored = struct.unpack_from("<I", blob, 104)[0]
    blanked = blob[:104] + b"\x00\x00\x00\x00" + blob[108:128]
    assert len(blanked) == 128
    assert stored == crc32c(blanked)

    # The rule is specific: NOT the CRC over the raw header including the field,
    # and NOT the CRC over [0, 104) alone. Both are plausible misreadings.
    assert stored != crc32c(blob[:128])
    assert stored != crc32c(blob[:104])


def test_directory_entry_field_offsets(tmp_path: Path):
    """Every field of section 4.3's 80-byte entry, at its documented offset."""
    path = tmp_path / "directory.cvf"
    # 3x1x1 voxels, brick size 2 => two bricks: a full one and a partial one.
    values = np.array([[[[1.0]]], [[[2.0]]], [[[np.nan]]]], dtype="<f4")
    write_cvf(path, values=values, brick_size=(2, 2, 2), codec=CODEC_NONE)
    blob = path.read_bytes()

    assert struct.unpack_from("<Q", blob, 64)[0] == 2
    directory_offset = struct.unpack_from("<Q", blob, 72)[0]

    entry = blob[directory_offset : directory_offset + 80]
    assert struct.unpack_from("<I", entry, 0)[0] == 0    # brickIndexX
    assert struct.unpack_from("<I", entry, 4)[0] == 0    # brickIndexY
    assert struct.unpack_from("<I", entry, 8)[0] == 0    # brickIndexZ
    assert struct.unpack_from("<H", entry, 12)[0] == 2   # validSizeX
    assert struct.unpack_from("<H", entry, 14)[0] == 1   # validSizeY
    assert struct.unpack_from("<H", entry, 16)[0] == 1   # validSizeZ
    assert struct.unpack_from("<H", entry, 18)[0] == 0   # flags
    absolute = struct.unpack_from("<Q", entry, 20)[0]    # absolutePayloadOffset
    assert struct.unpack_from("<I", entry, 28)[0] == 8   # compressedBytes (2 f32)
    assert struct.unpack_from("<I", entry, 32)[0] == 8   # uncompressedBytes
    assert struct.unpack_from("<4f", entry, 36)[0] == 1.0   # componentMin[0]
    assert struct.unpack_from("<4f", entry, 52)[0] == 2.0   # componentMax[0]
    payload_crc = struct.unpack_from("<I", entry, 68)[0]    # payloadCrc32c
    assert entry[72:80] == b"\x00" * 8                       # reserved

    # payloadCrc32c is over the STORED (compressed) bytes at absolutePayloadOffset.
    assert payload_crc == crc32c(blob[absolute : absolute + 8])

    # The partial edge brick stores exactly one voxel, and its only value is NaN,
    # so section 4.4.7's sentinel applies.
    second = blob[directory_offset + 80 : directory_offset + 160]
    assert struct.unpack_from("<I", second, 0)[0] == 1   # brickIndexX
    assert struct.unpack_from("<H", second, 12)[0] == 1  # validSizeX — not padded
    assert struct.unpack_from("<I", second, 32)[0] == 4  # uncompressedBytes
    assert struct.unpack_from("<f", second, 36)[0] == np.inf
    assert struct.unpack_from("<f", second, 52)[0] == -np.inf


def test_uncompressed_bytes_equals_the_normative_product(tmp_path: Path):
    """Section 4.4.4, for every brick, including the partial ones."""
    path = tmp_path / "sizes.cvf"
    write_cvf(path, values=ramp((7, 5, 3, 3), "<f4"), brick_size=(4, 4, 4),
              codec=CODEC_ZLIB)
    with CVFReader(path) as reader:
        for entry in reader.directory:
            expected = (
                entry.valid_size_x * entry.valid_size_y * entry.valid_size_z
                * reader.header.component_count * reader.header.item_size
            )
            assert entry.uncompressed_bytes == expected


def test_payload_is_x_fastest_then_y_then_z_components_interleaved(tmp_path: Path):
    """Sections 4.4.1 and 4.4.2, asserted on the literal payload bytes.

    A round-trip through a transposing reader looks identical to a correct one,
    so the byte order is read out of the file directly here.
    """
    path = tmp_path / "order.cvf"
    values = np.zeros((2, 2, 2, 2), dtype="<f4")
    for i in range(2):
        for j in range(2):
            for k in range(2):
                values[i, j, k, 0] = 100 * i + 10 * j + k
                values[i, j, k, 1] = -(100 * i + 10 * j + k)
    write_cvf(path, values=values, brick_size=(8, 8, 8), codec=CODEC_NONE)

    blob = path.read_bytes()
    payload_offset = struct.unpack_from("<Q", blob, 80)[0]
    decoded = np.frombuffer(blob, dtype="<f4", count=16, offset=payload_offset)

    expected = []
    for k in range(2):          # Z slowest
        for j in range(2):      # then Y
            for i in range(2):  # X fastest
                expected.append(100 * i + 10 * j + k)          # component 0
                expected.append(-(100 * i + 10 * j + k))       # component 1, interleaved
    assert decoded.tolist() == expected


def test_reader_decodes_a_file_assembled_by_hand_from_the_spec_tables(tmp_path: Path):
    """The strongest layout check available: no writer code is involved.

    Everything below is packed from ``Docs/CFDVIZ_FORMAT.md`` sections 4.1 and
    4.3 directly. If the reader disagrees with the spec on any offset, this
    fails no matter what the writer does.
    """
    payload_values = np.array([1.5, -2.5, 7.0, np.nan], dtype="<f4")
    payload = payload_values.tobytes()

    header = bytearray(128)
    header[0:8] = b"CFDVOL1\x00"
    struct.pack_into("<I", header, 8, 128)          # headerBytes
    struct.pack_into("<H", header, 12, 1)           # majorVersion
    struct.pack_into("<H", header, 14, 0)           # minorVersion
    struct.pack_into("<I", header, 16, 0x01020304)  # endianMarker
    struct.pack_into("<I", header, 20, 0)           # flags
    struct.pack_into("<I", header, 24, 3)           # frameIndex
    struct.pack_into("<I", header, 28, 11)          # fieldNumericId
    struct.pack_into("<d", header, 32, 0.5)         # simulationTime
    struct.pack_into("<I", header, 40, 4)           # dimensionX
    struct.pack_into("<I", header, 44, 1)           # dimensionY
    struct.pack_into("<I", header, 48, 1)           # dimensionZ
    struct.pack_into("<H", header, 52, 8)           # brickSizeX
    struct.pack_into("<H", header, 54, 8)           # brickSizeY
    struct.pack_into("<H", header, 56, 8)           # brickSizeZ
    header[58] = 1                                  # componentCount
    header[59] = 2                                  # dataType = float32
    header[60] = 0                                  # association = cell
    header[61] = 0                                  # codec = none
    struct.pack_into("<H", header, 62, 0)           # reserved
    struct.pack_into("<Q", header, 64, 1)           # brickCount
    struct.pack_into("<Q", header, 72, 128)         # directoryOffset
    struct.pack_into("<Q", header, 80, 208)         # payloadOffset
    struct.pack_into("<4f", header, 88, 0.0, 0.0, 0.0, 0.0)  # backgroundValue

    entry = bytearray(80)
    struct.pack_into("<I", entry, 0, 0)             # brickIndexX
    struct.pack_into("<I", entry, 4, 0)             # brickIndexY
    struct.pack_into("<I", entry, 8, 0)             # brickIndexZ
    struct.pack_into("<H", entry, 12, 4)            # validSizeX
    struct.pack_into("<H", entry, 14, 1)            # validSizeY
    struct.pack_into("<H", entry, 16, 1)            # validSizeZ
    struct.pack_into("<H", entry, 18, 0)            # flags
    struct.pack_into("<Q", entry, 20, 208)          # absolutePayloadOffset
    struct.pack_into("<I", entry, 28, len(payload))  # compressedBytes
    struct.pack_into("<I", entry, 32, len(payload))  # uncompressedBytes
    struct.pack_into("<4f", entry, 36, -2.5, np.inf, np.inf, np.inf)   # componentMin
    struct.pack_into("<4f", entry, 52, 7.0, -np.inf, -np.inf, -np.inf)  # componentMax
    struct.pack_into("<I", entry, 68, crc32c(payload))  # payloadCrc32c

    # headerCrc32c last, over [0,128) with [104,108) zero.
    struct.pack_into("<I", header, 104, crc32c(bytes(header)))

    path = tmp_path / "handmade.cvf"
    path.write_bytes(bytes(header) + bytes(entry) + payload)

    data = read_cvf(path)
    assert data.header.frame_index == 3
    assert data.header.field_numeric_id == 11
    assert data.header.simulation_time == 0.5
    assert data.header.dimensions == (4, 1, 1)
    assert data.header.brick_size == (8, 8, 8)
    assert data.header.component_count == 1
    assert data.values.shape == (4, 1, 1, 1)
    assert_bits_equal(data.values.reshape(-1), payload_values)


# ---------------------------------------------------------------------------
# Round trips
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("dtype", ["float16", "float32", "uint8"])
@pytest.mark.parametrize("codec", [CODEC_NONE, CODEC_ZLIB])
def test_round_trip_every_dtype_and_codec(tmp_path: Path, dtype, codec):
    numpy_dtype = {"float16": "<f2", "float32": "<f4", "uint8": "u1"}[dtype]
    values = ramp((6, 5, 4, 3), numpy_dtype)
    path = tmp_path / f"{dtype}_{codec}.cvf"

    write_cvf(path, values=values, dtype=dtype, codec=codec, brick_size=(4, 4, 4))
    data = read_cvf(path)

    assert data.header.data_type_name == dtype
    assert_bits_equal(data.values, values)


@pytest.mark.skipif(not is_codec_available(CODEC_LZ4), reason="optional lz4 not installed")
def test_round_trip_lz4(tmp_path: Path):
    values = ramp((6, 5, 4, 1), "<f4")
    path = tmp_path / "lz4.cvf"
    write_cvf(path, values=values, codec=CODEC_LZ4, brick_size=(3, 3, 3))
    assert_bits_equal(read_cvf(path).values, values)


@pytest.mark.parametrize("components", [1, 2, 3, 4])
def test_round_trip_every_component_count(tmp_path: Path, components):
    values = ramp((4, 3, 2, components), "<f4")
    path = tmp_path / f"c{components}.cvf"
    write_cvf(path, values=values, brick_size=(2, 2, 2))
    data = read_cvf(path)
    assert data.header.component_count == components
    assert_bits_equal(data.values, values)


def test_more_than_four_components_is_rejected(tmp_path: Path):
    """The header's float32[4] slots cap CVF at 4 components (sections 4.1/4.3)."""
    with pytest.raises(CVFError, match="componentCount"):
        write_cvf(tmp_path / "wide.cvf", values=ramp((2, 2, 2, 5), "<f4"))


def test_nan_and_infinity_survive_bit_exactly(tmp_path: Path):
    """Rule 1.7 and 4.4.6: NaN is legal data and is never coerced to zero."""
    values = np.full((4, 4, 4, 1), 1.0, dtype="<f4")
    values[0, 0, 0, 0] = np.nan
    values[1, 0, 0, 0] = np.inf
    values[2, 0, 0, 0] = -np.inf
    values[3, 0, 0, 0] = -0.0
    # A signalling NaN with a distinctive payload: only a bit-exact path keeps it.
    values[0, 1, 0, 0] = np.frombuffer(struct.pack("<I", 0x7F812345), dtype="<f4")[0]

    path = tmp_path / "nan.cvf"
    write_cvf(path, values=values, codec=CODEC_ZLIB, brick_size=(2, 2, 2))
    data = read_cvf(path)

    assert_bits_equal(data.values, values)
    assert np.isnan(data.values[0, 0, 0, 0])
    assert data.values[1, 0, 0, 0] == np.inf
    assert struct.pack("<f", data.values[3, 0, 0, 0]) == struct.pack("<f", -0.0)


def test_float16_nan_survives(tmp_path: Path):
    values = np.array([np.nan, np.inf, -np.inf, 1.0], dtype="<f2").reshape(4, 1, 1, 1)
    path = tmp_path / "half.cvf"
    write_cvf(path, values=values, dtype="float16", codec=CODEC_ZLIB)
    assert_bits_equal(read_cvf(path).values, values)


def test_scalar_input_may_be_three_dimensional(tmp_path: Path):
    values = ramp((3, 3, 3), "<f4")
    path = tmp_path / "scalar.cvf"
    write_cvf(path, values=values)
    data = read_cvf(path)
    assert data.values.shape == (3, 3, 3, 1)
    assert_bits_equal(data.values[..., 0], values)


def test_partial_edge_bricks_are_not_padded(tmp_path: Path):
    """Section 4.4.3: an edge brick stores exactly its valid extent."""
    values = ramp((5, 5, 5, 1), "<f4")
    path = tmp_path / "edges.cvf"
    write_cvf(path, values=values, brick_size=(4, 4, 4), codec=CODEC_NONE)

    with CVFReader(path) as reader:
        assert reader.header.brick_count == 8  # 2x2x2 bricks over a 5^3 volume
        sizes = sorted(
            (e.valid_size_x, e.valid_size_y, e.valid_size_z) for e in reader.directory
        )
        assert sizes[0] == (1, 1, 1)   # the far corner brick
        assert sizes[-1] == (4, 4, 4)  # the interior brick
        total = sum(
            e.valid_size_x * e.valid_size_y * e.valid_size_z for e in reader.directory
        )
        assert total == 125, "padding would make this larger than the volume"

    assert_bits_equal(read_cvf(path).values, values)


def test_brick_size_larger_than_the_volume_yields_one_brick(tmp_path: Path):
    path = tmp_path / "single.cvf"
    values = ramp((3, 2, 1, 1), "<f4")
    write_cvf(path, values=values, brick_size=(64, 64, 64))
    with CVFReader(path) as reader:
        assert reader.header.brick_count == 1
    assert_bits_equal(read_cvf(path).values, values)


def test_point_association_stores_one_more_value_per_axis(tmp_path: Path):
    """Section 3.2: the half-cell offset, the format's most common bug.

    ``dimensionX/Y/Z`` hold grid **cell** counts, exactly as the manifest
    declares them; the value extent is derived from ``association``. A point
    field on a 3x2x1 grid therefore stores 4x3x2 values.
    """
    values = ramp((4, 3, 2, 1), "<f4")
    path = tmp_path / "point.cvf"
    write_cvf(path, values=values, association="point", dimensions=(3, 2, 1))

    blob = path.read_bytes()
    assert struct.unpack_from("<I", blob, 40)[0] == 3, "dimensionX must be the CELL count"
    assert blob[60] == CVF_ASSOC_POINT

    data = read_cvf(path)
    assert data.header.dimensions == (3, 2, 1)
    assert data.header.value_extent == (4, 3, 2)
    assert data.values.shape == (4, 3, 2, 1)
    assert_bits_equal(data.values, values)


def test_point_association_rejects_a_cell_sized_array(tmp_path: Path):
    """The +1 must be enforced, not silently accepted."""
    with pytest.raises(CVFError, match="point"):
        write_cvf(tmp_path / "bad_point.cvf", values=ramp((3, 2, 1, 1), "<f4"),
                  association="point", dimensions=(3, 2, 1))


def test_cell_association_default_dimensions_are_the_array_shape(tmp_path: Path):
    path = tmp_path / "cell.cvf"
    write_cvf(path, values=ramp((3, 2, 1, 1), "<f4"), association="cell")
    assert read_cvf(path).header.dimensions == (3, 2, 1)


def test_uint8_storage_is_not_rescaled(tmp_path: Path):
    """Rule 1.6: no reader or writer may normalize, quantize or rescale."""
    values = np.array([0, 1, 127, 128, 254, 255], dtype="u1").reshape(6, 1, 1, 1)
    path = tmp_path / "mask.cvf"
    write_cvf(path, values=values, dtype="uint8")
    assert read_cvf(path).values.reshape(-1).tolist() == [0, 1, 127, 128, 254, 255]


def test_float64_storage_is_rejected(tmp_path: Path):
    """Section 3.3: float64 grid-field storage is not supported in 1.x."""
    with pytest.raises(CVFError, match="float64"):
        write_cvf(tmp_path / "f64.cvf", values=ramp((2, 2, 2, 1), "<f8"), dtype="float64")


def test_a_value_that_would_overflow_the_storage_type_is_rejected(tmp_path: Path):
    """Rule 1.6 forbids silent clamping; 1e30 is finite but infinite in float16."""
    values = np.full((2, 2, 2, 1), 1e30, dtype="<f4")
    with pytest.raises(CVFError, match="overflow"):
        write_cvf(tmp_path / "overflow.cvf", values=values, dtype="float16")


# ---------------------------------------------------------------------------
# Per-brick statistics — section 4.4.7
# ---------------------------------------------------------------------------

def test_brick_statistics_ignore_nan(tmp_path: Path):
    values = np.array([1.0, np.nan, 5.0, -2.0], dtype="<f4").reshape(4, 1, 1, 1)
    path = tmp_path / "stats.cvf"
    write_cvf(path, values=values, brick_size=(8, 8, 8))
    with CVFReader(path) as reader:
        entry = reader.directory[0]
        assert entry.component_min[0] == -2.0
        assert entry.component_max[0] == 5.0


def test_all_nan_brick_uses_the_infinite_sentinel(tmp_path: Path):
    """Section 4.4.7: min = +inf, max = -inf means "no valid data"."""
    values = np.full((4, 1, 1, 1), np.nan, dtype="<f4")
    path = tmp_path / "allnan.cvf"
    write_cvf(path, values=values, brick_size=(8, 8, 8))
    with CVFReader(path) as reader:
        entry = reader.directory[0]
        assert entry.component_min[0] == np.inf
        assert entry.component_max[0] == -np.inf


def test_masked_cells_are_excluded_from_statistics(tmp_path: Path):
    """Section 4.4.7: statistics ignore cells rejected by the mask field."""
    values = np.array([1.0, 999.0, 3.0, 2.0], dtype="<f4").reshape(4, 1, 1, 1)
    mask = np.array([True, False, True, True]).reshape(4, 1, 1)
    path = tmp_path / "masked.cvf"
    write_cvf(path, values=values, mask=mask, brick_size=(8, 8, 8))

    with CVFReader(path) as reader:
        entry = reader.directory[0]
        assert entry.component_max[0] == 3.0, "the masked 999 must not set the maximum"
        assert entry.component_min[0] == 1.0

    # The masked value is still STORED — masking affects statistics, not payload.
    assert read_cvf(path).values[1, 0, 0, 0] == 999.0


def test_statistics_are_per_component(tmp_path: Path):
    values = np.zeros((2, 1, 1, 3), dtype="<f4")
    values[0, 0, 0] = (1.0, 10.0, 100.0)
    values[1, 0, 0] = (2.0, 20.0, 200.0)
    path = tmp_path / "percomp.cvf"
    write_cvf(path, values=values, brick_size=(8, 8, 8))
    with CVFReader(path) as reader:
        entry = reader.directory[0]
        assert list(entry.component_min[:3]) == [1.0, 10.0, 100.0]
        assert list(entry.component_max[:3]) == [2.0, 20.0, 200.0]
        # Unused slots carry the "no valid data" sentinel, not a misleading zero.
        assert entry.component_min[3] == np.inf
        assert entry.component_max[3] == -np.inf


def test_statistics_are_computed_over_stored_values_not_the_input(tmp_path: Path):
    """A float16 export must declare the extremes a reader will actually find."""
    values = np.array([0.1, 0.2], dtype="<f8").reshape(2, 1, 1, 1)
    path = tmp_path / "quantized.cvf"
    write_cvf(path, values=values, dtype="float16", brick_size=(8, 8, 8))
    stored = read_cvf(path).values.reshape(-1)
    with CVFReader(path) as reader:
        entry = reader.directory[0]
        assert entry.component_min[0] == np.float32(stored.min())
        assert entry.component_max[0] == np.float32(stored.max())
        assert entry.component_min[0] != np.float32(0.1), "declared the pre-quantized value"


# ---------------------------------------------------------------------------
# Sparse volumes — section 4.4.5
# ---------------------------------------------------------------------------

def test_background_bricks_may_be_omitted_and_read_back_as_background(tmp_path: Path):
    """Section 4.4.5: an absent brick evaluates to backgroundValue everywhere."""
    values = np.full((8, 4, 4, 1), 3.5, dtype="<f4")
    values[0:4] = 1.0  # only the first brick column differs from the background
    path = tmp_path / "sparse.cvf"

    write_cvf(path, values=values, brick_size=(4, 4, 4), background_value=3.5,
              omit_background_bricks=True)

    with CVFReader(path) as reader:
        assert reader.header.brick_count == 1, "the all-background brick was stored anyway"
    assert_bits_equal(read_cvf(path).values, values)


def test_omitting_background_bricks_is_off_by_default(tmp_path: Path):
    values = np.zeros((8, 4, 4, 1), dtype="<f4")
    path = tmp_path / "dense.cvf"
    write_cvf(path, values=values, brick_size=(4, 4, 4))
    with CVFReader(path) as reader:
        assert reader.header.brick_count == 2


def test_nan_background_is_matched_bit_exactly(tmp_path: Path):
    """NaN != NaN, so brick omission must compare bit patterns, not values."""
    values = np.full((8, 4, 4, 1), np.nan, dtype="<f4")
    values[0:4] = 1.0
    path = tmp_path / "nanbg.cvf"
    write_cvf(path, values=values, brick_size=(4, 4, 4), background_value=np.nan,
              omit_background_bricks=True)
    with CVFReader(path) as reader:
        assert reader.header.brick_count == 1
    assert_bits_equal(read_cvf(path).values, values)


# ---------------------------------------------------------------------------
# Random brick access and region loading
# ---------------------------------------------------------------------------

def test_reader_decodes_one_brick_without_reading_the_whole_volume(tmp_path: Path):
    values = ramp((8, 8, 8, 1), "<f4")
    path = tmp_path / "random.cvf"
    write_cvf(path, values=values, brick_size=(4, 4, 4), codec=CODEC_ZLIB)

    with CVFReader(path) as reader:
        brick = reader.read_brick(reader.find_brick(1, 0, 0))
        assert brick.shape == (4, 4, 4, 1)
        assert_bits_equal(brick, values[4:8, 0:4, 0:4, :])


def test_reader_loads_a_region_of_interest(tmp_path: Path):
    values = ramp((8, 8, 8, 2), "<f4")
    path = tmp_path / "roi.cvf"
    write_cvf(path, values=values, brick_size=(4, 4, 4), codec=CODEC_ZLIB)

    with CVFReader(path) as reader:
        region = reader.read_region((2, 3, 1), (7, 5, 6))
    assert_bits_equal(region, values[2:7, 3:5, 1:6, :])


def test_region_bounds_are_validated(tmp_path: Path):
    path = tmp_path / "bounds.cvf"
    write_cvf(path, values=ramp((4, 4, 4, 1), "<f4"))
    with CVFReader(path) as reader:
        with pytest.raises(CVFError, match="outside"):
            reader.read_region((0, 0, 0), (5, 4, 4))
        with pytest.raises(CVFError, match="empty|>="):
            reader.read_region((2, 0, 0), (2, 4, 4))


def test_absent_brick_lookup_returns_none(tmp_path: Path):
    values = np.zeros((8, 4, 4, 1), dtype="<f4")
    values[0:4] = 1.0
    path = tmp_path / "absent.cvf"
    write_cvf(path, values=values, brick_size=(4, 4, 4), background_value=0.0,
              omit_background_bricks=True)
    with CVFReader(path) as reader:
        assert reader.find_brick(0, 0, 0) is not None
        assert reader.find_brick(1, 0, 0) is None


# ---------------------------------------------------------------------------
# Rejection — a corrupt file must raise, never return garbage
# ---------------------------------------------------------------------------

@pytest.fixture
def good_cvf(tmp_path: Path) -> Path:
    path = tmp_path / "good.cvf"
    write_cvf(path, values=ramp((6, 5, 4, 2), "<f4"), brick_size=(4, 4, 4),
              codec=CODEC_ZLIB, frame_index=2, field_numeric_id=9)
    return path


def test_missing_file_is_rejected(tmp_path: Path):
    with pytest.raises(CVFFormatError, match="cannot read"):
        read_cvf(tmp_path / "nope.cvf")


@pytest.mark.parametrize("length", [0, 1, 63, 127])
def test_truncated_header_is_rejected(tmp_path: Path, good_cvf: Path, length):
    short = tmp_path / f"short_{length}.cvf"
    short.write_bytes(good_cvf.read_bytes()[:length])
    with pytest.raises(CVFFormatError, match="shorter than"):
        read_cvf(short)


def test_bad_magic_is_rejected(tmp_path: Path, good_cvf: Path):
    blob = bytearray(good_cvf.read_bytes())
    blob[0:8] = b"CFDMESH1"  # a valid CVM magic — the wrong container, not noise
    path = tmp_path / "wrong_magic.cvf"
    path.write_bytes(bytes(blob))
    with pytest.raises(CVFFormatError, match="bad magic"):
        read_cvf(path)


def test_big_endian_marker_is_rejected_not_byte_swapped(tmp_path: Path, good_cvf: Path):
    """Rule 1.1: reject foreign byte order rather than swapping it."""
    path = tmp_path / "bigendian.cvf"
    path.write_bytes(tamper_cvf(good_cvf.read_bytes(), 16, "<I", 0x04030201))
    with pytest.raises(CVFFormatError, match="byte order not supported"):
        read_cvf(path)


def test_unsupported_major_version_is_rejected(tmp_path: Path, good_cvf: Path):
    path = tmp_path / "v2.cvf"
    path.write_bytes(tamper_cvf(good_cvf.read_bytes(), 12, "<H", 2))
    with pytest.raises(CVFFormatError, match="major version"):
        read_cvf(path)


def test_newer_minor_version_is_accepted(tmp_path: Path, good_cvf: Path):
    """Rule 1.4: a newer minor version loads if every required construct is there."""
    path = tmp_path / "v1_9.cvf"
    path.write_bytes(tamper_cvf(good_cvf.read_bytes(), 14, "<H", 9))
    data = read_cvf(path)
    assert data.header.minor_version == 9


def test_wrong_header_bytes_is_rejected(tmp_path: Path, good_cvf: Path):
    path = tmp_path / "headerbytes.cvf"
    path.write_bytes(tamper_cvf(good_cvf.read_bytes(), 8, "<I", 96))
    with pytest.raises(CVFFormatError, match="headerBytes"):
        read_cvf(path)


def test_header_crc_mismatch_is_rejected(tmp_path: Path, good_cvf: Path):
    """Deliberately does NOT repair the CRC — that is the point of this one."""
    blob = bytearray(good_cvf.read_bytes())
    struct.pack_into("<I", blob, 24, 12345)  # change frameIndex, leave the CRC stale
    path = tmp_path / "badcrc.cvf"
    path.write_bytes(bytes(blob))
    with pytest.raises(CVFFormatError, match="header CRC-32C mismatch"):
        read_cvf(path)


def test_a_flipped_bit_anywhere_in_the_header_is_caught(tmp_path: Path, good_cvf: Path):
    """The header CRC must actually cover the whole header."""
    original = good_cvf.read_bytes()
    for offset in range(0, 104):
        blob = bytearray(original)
        blob[offset] ^= 0x01
        path = tmp_path / "bitflip.cvf"
        path.write_bytes(bytes(blob))
        with pytest.raises(CVFFormatError):
            read_cvf(path)


def test_payload_crc_mismatch_is_rejected(tmp_path: Path, good_cvf: Path):
    blob = bytearray(good_cvf.read_bytes())
    payload_offset = struct.unpack_from("<Q", blob, 80)[0]
    blob[payload_offset] ^= 0xFF
    path = tmp_path / "badpayload.cvf"
    path.write_bytes(bytes(blob))
    with pytest.raises(CVFFormatError, match="payload CRC-32C mismatch"):
        read_cvf(path)


def test_reserved_header_bytes_must_be_zero(tmp_path: Path, good_cvf: Path):
    path = tmp_path / "reserved.cvf"
    path.write_bytes(tamper_cvf(good_cvf.read_bytes(), 62, "<H", 1))
    with pytest.raises(CVFFormatError, match="reserved"):
        read_cvf(path)


def test_unknown_flag_bits_are_rejected(tmp_path: Path, good_cvf: Path):
    path = tmp_path / "flags.cvf"
    path.write_bytes(tamper_cvf(good_cvf.read_bytes(), 20, "<I", 0x80000000))
    with pytest.raises(CVFFormatError, match="flag"):
        read_cvf(path)


def test_unknown_data_type_is_rejected(tmp_path: Path, good_cvf: Path):
    blob = bytearray(good_cvf.read_bytes())
    blob[59] = 7
    path = tmp_path / "dtype.cvf"
    path.write_bytes(tamper_cvf(bytes(blob), 59, "<B", 7))
    with pytest.raises(CVFFormatError, match="dataType"):
        read_cvf(path)


def test_float64_data_type_id_is_rejected_in_cvf(tmp_path: Path, good_cvf: Path):
    """CVA's float64 (ID 4) must not be accepted in a CVF (section 3.3)."""
    path = tmp_path / "cvf_f64.cvf"
    path.write_bytes(tamper_cvf(good_cvf.read_bytes(), 59, "<B", 4))
    with pytest.raises(CVFFormatError, match="dataType"):
        read_cvf(path)


def test_unknown_association_is_rejected(tmp_path: Path, good_cvf: Path):
    """Section 3.2: mesh-vertex, face, particle etc. are reserved and rejected."""
    path = tmp_path / "assoc.cvf"
    path.write_bytes(tamper_cvf(good_cvf.read_bytes(), 60, "<B", 4))
    with pytest.raises(CVFFormatError, match="association"):
        read_cvf(path)


def test_zstd_codec_is_rejected_with_the_exact_message(tmp_path: Path, good_cvf: Path):
    """Section 7: reserved, with mandated wording, and never a silent fallback."""
    path = tmp_path / "zstd.cvf"
    path.write_bytes(tamper_cvf(good_cvf.read_bytes(), 61, "<B", CODEC_ZSTD))
    with pytest.raises(UnsupportedCodecError) as excinfo:
        read_cvf(path)
    assert str(excinfo.value) == ZSTD_REJECTION_MESSAGE


def test_unknown_codec_is_rejected(tmp_path: Path, good_cvf: Path):
    path = tmp_path / "codec.cvf"
    path.write_bytes(tamper_cvf(good_cvf.read_bytes(), 61, "<B", 9))
    with pytest.raises(CVFFormatError, match="codec"):
        read_cvf(path)


def test_brick_count_beyond_the_file_is_rejected_before_allocating(tmp_path: Path,
                                                                   good_cvf: Path):
    """Rule 1.5: a hostile count must not become an allocation.

    2**40 entries is 80 TiB of directory. A reader that allocates first and
    checks afterwards dies here instead of raising.
    """
    path = tmp_path / "hugecount.cvf"
    path.write_bytes(tamper_cvf(good_cvf.read_bytes(), 64, "<Q", 2**40))
    with pytest.raises(CVFFormatError, match="directory"):
        read_cvf(path)


def test_directory_offset_inside_the_header_is_rejected(tmp_path: Path, good_cvf: Path):
    path = tmp_path / "diroverlap.cvf"
    path.write_bytes(tamper_cvf(good_cvf.read_bytes(), 72, "<Q", 64))
    with pytest.raises(CVFFormatError, match="header"):
        read_cvf(path)


def test_directory_offset_past_the_end_is_rejected(tmp_path: Path, good_cvf: Path):
    path = tmp_path / "dirpast.cvf"
    size = good_cvf.stat().st_size
    path.write_bytes(tamper_cvf(good_cvf.read_bytes(), 72, "<Q", size + 4096))
    with pytest.raises(CVFFormatError, match="directory"):
        read_cvf(path)


def test_truncated_directory_is_rejected(tmp_path: Path, good_cvf: Path):
    blob = good_cvf.read_bytes()
    directory_offset = struct.unpack_from("<Q", blob, 72)[0]
    path = tmp_path / "cutdir.cvf"
    path.write_bytes(blob[: directory_offset + 40])
    with pytest.raises(CVFFormatError, match="directory"):
        read_cvf(path)


def test_truncated_payload_is_rejected(tmp_path: Path, good_cvf: Path):
    blob = good_cvf.read_bytes()
    path = tmp_path / "cutpayload.cvf"
    path.write_bytes(blob[: len(blob) - 4])
    with pytest.raises(CVFFormatError, match="payload"):
        read_cvf(path)


def test_lying_uncompressed_bytes_is_rejected_before_allocating(tmp_path: Path,
                                                                good_cvf: Path):
    """Section 4.4.4: the primary defence against a malicious size field."""
    blob = bytearray(good_cvf.read_bytes())
    directory_offset = struct.unpack_from("<Q", blob, 72)[0]
    struct.pack_into("<I", blob, directory_offset + 32, 0xFFFFFF00)
    path = tmp_path / "lyingsize.cvf"
    path.write_bytes(bytes(blob))
    with pytest.raises(CVFFormatError, match="uncompressedBytes"):
        read_cvf(path)


def test_brick_payload_past_the_end_is_rejected(tmp_path: Path, good_cvf: Path):
    blob = bytearray(good_cvf.read_bytes())
    directory_offset = struct.unpack_from("<Q", blob, 72)[0]
    struct.pack_into("<Q", blob, directory_offset + 20, len(blob) + 1024)
    path = tmp_path / "farpayload.cvf"
    path.write_bytes(bytes(blob))
    with pytest.raises(CVFFormatError, match="payload"):
        read_cvf(path)


def test_brick_index_outside_the_volume_is_rejected(tmp_path: Path, good_cvf: Path):
    blob = bytearray(good_cvf.read_bytes())
    directory_offset = struct.unpack_from("<Q", blob, 72)[0]
    struct.pack_into("<I", blob, directory_offset + 0, 9999)
    path = tmp_path / "badbrick.cvf"
    path.write_bytes(bytes(blob))
    with pytest.raises(CVFFormatError, match="brick"):
        read_cvf(path)


def test_duplicate_brick_entries_are_rejected(tmp_path: Path):
    """Two entries for the same brick make "which one wins" undefined."""
    path = tmp_path / "dupe.cvf"
    write_cvf(path, values=ramp((8, 4, 4, 1), "<f4"), brick_size=(4, 4, 4),
              codec=CODEC_NONE)
    blob = bytearray(path.read_bytes())
    directory_offset = struct.unpack_from("<Q", blob, 72)[0]
    struct.pack_into("<I", blob, directory_offset + 80, 0)  # second entry -> brick 0
    dupe = tmp_path / "dupe2.cvf"
    dupe.write_bytes(bytes(blob))
    with pytest.raises(CVFFormatError, match="duplicate"):
        read_cvf(dupe)


def test_verify_crc_false_still_decodes(tmp_path: Path, good_cvf: Path):
    """A validator needs to *report* corruption rather than raise on it."""
    blob = bytearray(good_cvf.read_bytes())
    struct.pack_into("<I", blob, 24, 999)  # stale header CRC
    path = tmp_path / "novalidate.cvf"
    path.write_bytes(bytes(blob))
    data = read_cvf(path, verify_crc=False)
    assert data.header.frame_index == 999


def test_zero_sized_volume_is_rejected(tmp_path: Path):
    with pytest.raises(CVFError, match="at least 1|dimension"):
        write_cvf(tmp_path / "empty.cvf", values=np.zeros((0, 1, 1, 1), dtype="<f4"))


def test_zero_brick_size_is_rejected(tmp_path: Path):
    with pytest.raises(CVFError, match="brick"):
        write_cvf(tmp_path / "zerobrick.cvf", values=ramp((2, 2, 2, 1), "<f4"),
                  brick_size=(0, 4, 4))


def test_brick_size_above_uint16_is_rejected(tmp_path: Path):
    with pytest.raises(CVFError, match="brick"):
        write_cvf(tmp_path / "bigbrick.cvf", values=ramp((2, 2, 2, 1), "<f4"),
                  brick_size=(70000, 4, 4))
