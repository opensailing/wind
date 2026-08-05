"""CVA — mesh-associated array. Format spec section 6.

Section 6.1 gained a normative field-offset table specifically so this container
could be pinned down, and this module asserts it byte by byte. The statistics
section of 6.4 gets the same treatment: its layout is ``8 + 32*C`` bytes with a
documented order, and a mis-ordered section round-trips perfectly while being
unreadable by the Unreal side.

The symmetric-tensor component order (``XX, YY, ZZ, XY, YZ, XZ``) is asserted
explicitly because the spec names the alternative Voigt order as "the classic
way for two solvers to silently disagree".
"""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np
import pytest

from conftest import assert_bits_equal, tamper_cva
from cfdviz.codecs import (
    CODEC_LZ4,
    CODEC_NONE,
    CODEC_ZLIB,
    CODEC_ZSTD,
    ZSTD_REJECTION_MESSAGE,
    UnsupportedCodecError,
    is_codec_available,
)
from cfdviz.crc32c import crc32c
from cfdviz.cva import (
    CVA_ASSOC_ELEMENT,
    CVA_ASSOC_VERTEX,
    CVA_DTYPE_FLOAT16,
    CVA_DTYPE_FLOAT32,
    CVA_DTYPE_FLOAT64,
    CVA_ENDIAN_MARKER,
    CVA_FLAG_FRAME_STATISTICS,
    CVA_FLAG_GLOBAL_STATISTICS,
    CVA_HEADER_BYTES,
    CVA_MAGIC,
    FULL_TENSOR_COMPONENT_ORDER,
    SYMMETRIC_TENSOR_COMPONENT_ORDER,
    VALID_COMPONENT_COUNTS,
    ArrayStatistics,
    CVAError,
    CVAFormatError,
    merge_statistics,
    read_cva,
    write_cva,
)


def test_format_constants_match_the_spec():
    assert CVA_MAGIC == b"CFDARR1\x00"
    assert len(CVA_MAGIC) == 8
    assert CVA_HEADER_BYTES == 96
    assert CVA_ENDIAN_MARKER == 0x01020304
    # Section 6.2 — 3 stays reserved for CVF's uint8.
    assert (CVA_DTYPE_FLOAT16, CVA_DTYPE_FLOAT32, CVA_DTYPE_FLOAT64) == (1, 2, 4)
    assert (CVA_ASSOC_ELEMENT, CVA_ASSOC_VERTEX) == (0, 1)
    assert (CVA_FLAG_FRAME_STATISTICS, CVA_FLAG_GLOBAL_STATISTICS) == (1, 2)
    assert VALID_COMPONENT_COUNTS == (1, 3, 6, 9)


def test_tensor_component_orders_are_the_normative_ones():
    """Section 6.3. The other Voigt order (XX,YY,ZZ,YZ,XZ,XY) must NOT be used."""
    assert SYMMETRIC_TENSOR_COMPONENT_ORDER == ("XX", "YY", "ZZ", "XY", "YZ", "XZ")
    assert SYMMETRIC_TENSOR_COMPONENT_ORDER != ("XX", "YY", "ZZ", "YZ", "XZ", "XY")
    assert FULL_TENSOR_COMPONENT_ORDER == (
        "XX", "XY", "XZ", "YX", "YY", "YZ", "ZX", "ZY", "ZZ",
    )


# ---------------------------------------------------------------------------
# Header layout — section 6.1's offset table
# ---------------------------------------------------------------------------

def test_header_field_offsets(tmp_path: Path):
    values = np.arange(12, dtype="<f4").reshape(4, 3)
    path = tmp_path / "offsets.cva"
    write_cva(path, values=values, frame_index=5, field_numeric_id=17,
              simulation_time=2.5, association="mesh-element", dtype="float32",
              codec=CODEC_NONE, compute_statistics=True)
    blob = path.read_bytes()

    assert blob[0:8] == b"CFDARR1\x00"
    assert struct.unpack_from("<H", blob, 8)[0] == 1            # majorVersion
    assert struct.unpack_from("<H", blob, 10)[0] == 0           # minorVersion
    assert struct.unpack_from("<I", blob, 12)[0] == 0x01020304  # endianMarker
    assert struct.unpack_from("<I", blob, 16)[0] == CVA_FLAG_FRAME_STATISTICS
    assert struct.unpack_from("<I", blob, 20)[0] == 96          # headerBytes
    assert struct.unpack_from("<I", blob, 24)[0] == 5           # frameIndex
    assert struct.unpack_from("<I", blob, 28)[0] == 17          # fieldNumericId
    assert struct.unpack_from("<d", blob, 32)[0] == 2.5         # simulationTime
    assert struct.unpack_from("<Q", blob, 40)[0] == 4           # valueCount
    assert blob[48] == 3                                        # componentCount
    assert blob[49] == CVA_DTYPE_FLOAT32                        # dataType
    assert blob[50] == CVA_ASSOC_ELEMENT                        # association
    assert blob[51] == CODEC_NONE                               # codec
    payload_crc = struct.unpack_from("<I", blob, 52)[0]         # payloadCrc32c
    payload_offset = struct.unpack_from("<Q", blob, 56)[0]      # payloadOffset
    assert struct.unpack_from("<Q", blob, 64)[0] == 48          # compressedBytes
    assert struct.unpack_from("<Q", blob, 72)[0] == 48          # uncompressedBytes
    assert struct.unpack_from("<I", blob, 84)[0] == 0           # reserved
    assert struct.unpack_from("<Q", blob, 88)[0] != 0           # statisticsOffset

    assert payload_crc == crc32c(blob[payload_offset : payload_offset + 48])


def test_first_24_bytes_are_layout_compatible_with_a_cvm_header(tmp_path: Path):
    """Section 6.1: "identical in meaning and position to a CVM header".

    A shared C++ sniffing routine reads major/minor/endian/flags/headerBytes at
    the same offsets from either container, so those five fields are compared
    across an actual CVA and an actual CVM file.
    """
    from cfdviz.cvm import write_cvm

    cva = tmp_path / "a.cva"
    cvm = tmp_path / "m.cvm"
    write_cva(cva, values=np.zeros((2, 1), dtype="<f4"), compute_statistics=False)
    write_cvm(cvm, positions=np.zeros((3, 3)),
              indices=np.array([[0, 1, 2]], dtype=np.uint32))

    a = cva.read_bytes()
    m = cvm.read_bytes()
    assert a[8:12] == m[8:12], "majorVersion/minorVersion"
    assert a[12:16] == m[12:16], "endianMarker"
    assert struct.unpack_from("<I", a, 20)[0] == struct.unpack_from("<I", m, 20)[0] == 96


def test_header_crc_covers_the_header_with_its_own_field_zeroed(tmp_path: Path):
    path = tmp_path / "crc.cva"
    write_cva(path, values=np.arange(6, dtype="<f4").reshape(6, 1))
    blob = path.read_bytes()
    stored = struct.unpack_from("<I", blob, 80)[0]
    assert stored == crc32c(blob[:80] + b"\x00\x00\x00\x00" + blob[84:96])
    assert stored != crc32c(blob[:96])


def test_statistics_section_layout(tmp_path: Path):
    """Section 6.4: valueCount, then min[C], max[C], mean[C], validCount[C]."""
    values = np.array([[1.0, 10.0], [3.0, 20.0], [np.nan, 30.0]], dtype="<f8")
    values = np.hstack([values, np.array([[100.0], [200.0], [300.0]])])  # 3 components
    path = tmp_path / "stats.cva"
    write_cva(path, values=values, dtype="float64", codec=CODEC_NONE)

    blob = path.read_bytes()
    offset = struct.unpack_from("<Q", blob, 88)[0]
    c = 3
    assert ArrayStatistics.section_bytes(c) == 8 + 32 * c

    assert struct.unpack_from("<Q", blob, offset)[0] == 3            # valueCount
    minimum = struct.unpack_from(f"<{c}d", blob, offset + 8)
    maximum = struct.unpack_from(f"<{c}d", blob, offset + 8 + 8 * c)
    mean = struct.unpack_from(f"<{c}d", blob, offset + 8 + 16 * c)
    valid = struct.unpack_from(f"<{c}Q", blob, offset + 8 + 24 * c)

    assert minimum == (1.0, 10.0, 100.0)
    assert maximum == (3.0, 30.0, 300.0)
    assert mean[0] == pytest.approx(2.0)   # NaN excluded: (1+3)/2, not (1+3+nan)/3
    assert mean[1] == pytest.approx(20.0)
    assert valid == (2, 3, 3), "validCount must count non-NaN entries per component"


def test_reader_decodes_a_file_assembled_by_hand_from_the_spec_table(tmp_path: Path):
    values = np.array([[1.0, 2.0, 3.0], [np.nan, 5.0, 6.0]], dtype="<f4")
    payload = values.tobytes()

    header = bytearray(96)
    header[0:8] = b"CFDARR1\x00"
    struct.pack_into("<H", header, 8, 1)
    struct.pack_into("<H", header, 10, 0)
    struct.pack_into("<I", header, 12, 0x01020304)
    struct.pack_into("<I", header, 16, 0)            # flags: no statistics
    struct.pack_into("<I", header, 20, 96)           # headerBytes
    struct.pack_into("<I", header, 24, 4)            # frameIndex
    struct.pack_into("<I", header, 28, 21)           # fieldNumericId
    struct.pack_into("<d", header, 32, 3.75)         # simulationTime
    struct.pack_into("<Q", header, 40, 2)            # valueCount
    header[48] = 3                                   # componentCount
    header[49] = 2                                   # dataType = float32
    header[50] = 1                                   # association = mesh-vertex
    header[51] = 0                                   # codec = none
    struct.pack_into("<I", header, 52, crc32c(payload))
    struct.pack_into("<Q", header, 56, 96)           # payloadOffset
    struct.pack_into("<Q", header, 64, len(payload))  # compressedBytes
    struct.pack_into("<Q", header, 72, len(payload))  # uncompressedBytes
    struct.pack_into("<I", header, 84, 0)            # reserved
    struct.pack_into("<Q", header, 88, 0)            # statisticsOffset
    struct.pack_into("<I", header, 80, crc32c(bytes(header)))

    path = tmp_path / "handmade.cva"
    path.write_bytes(bytes(header) + payload)

    data = read_cva(path)
    assert data.header.frame_index == 4
    assert data.header.field_numeric_id == 21
    assert data.header.simulation_time == 3.75
    assert data.header.association_name == "mesh-vertex"
    assert data.frame_statistics is None
    assert_bits_equal(data.values, values)


# ---------------------------------------------------------------------------
# Round trips
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("dtype", ["float16", "float32", "float64"])
@pytest.mark.parametrize("codec", [CODEC_NONE, CODEC_ZLIB])
@pytest.mark.parametrize("components", [1, 3, 6, 9])
def test_round_trip_every_dtype_codec_and_component_count(
    tmp_path: Path, dtype, codec, components
):
    numpy_dtype = {"float16": "<f2", "float32": "<f4", "float64": "<f8"}[dtype]
    values = (np.arange(components * 20, dtype=np.float64) * 0.5 - 7.0)
    values = values.reshape(20, components).astype(numpy_dtype)

    path = tmp_path / f"{dtype}_{codec}_{components}.cva"
    write_cva(path, values=values, dtype=dtype, codec=codec)
    data = read_cva(path)

    assert data.header.data_type_name == dtype
    assert data.component_count == components
    assert_bits_equal(data.values, values)


@pytest.mark.skipif(not is_codec_available(CODEC_LZ4), reason="optional lz4 not installed")
def test_round_trip_lz4(tmp_path: Path):
    values = np.arange(60, dtype="<f4").reshape(20, 3)
    path = tmp_path / "lz4.cva"
    write_cva(path, values=values, codec=CODEC_LZ4)
    assert_bits_equal(read_cva(path).values, values)


@pytest.mark.parametrize("components", [2, 4, 5, 7, 8, 10])
def test_illegal_component_counts_are_rejected(tmp_path: Path, components):
    """Section 6: 1, 3, 6, 9 only. A 4-component array is a CVF concern."""
    with pytest.raises(CVAError, match="componentCount"):
        write_cva(tmp_path / "bad.cva", values=np.zeros((3, components), dtype="<f4"))


def test_one_dimensional_input_becomes_a_scalar_array(tmp_path: Path):
    path = tmp_path / "scalar.cva"
    write_cva(path, values=np.arange(5, dtype="<f4"))
    data = read_cva(path)
    assert data.values.shape == (5, 1)
    assert data.component_names == ("",)


def test_nan_and_infinity_survive_bit_exactly(tmp_path: Path):
    values = np.array(
        [[np.nan, 1.0, -1.0], [np.inf, -np.inf, 0.0], [-0.0, 2.5, np.nan]],
        dtype="<f4",
    )
    path = tmp_path / "nan.cva"
    write_cva(path, values=values, codec=CODEC_ZLIB)
    data = read_cva(path)
    assert_bits_equal(data.values, values)
    assert struct.pack("<f", data.values[2, 0]) == struct.pack("<f", -0.0)


def test_symmetric_tensor_component_order_is_preserved_positionally(tmp_path: Path):
    """A per-component fingerprint: any reordering shows up immediately."""
    values = np.array([[11.0, 22.0, 33.0, 12.0, 23.0, 13.0]], dtype="<f8")
    path = tmp_path / "sym.cva"
    write_cva(path, values=values, dtype="float64", codec=CODEC_NONE)

    blob = path.read_bytes()
    offset = struct.unpack_from("<Q", blob, 56)[0]
    on_disk = struct.unpack_from("<6d", blob, offset)
    assert on_disk == (11.0, 22.0, 33.0, 12.0, 23.0, 13.0), "components were reordered"

    data = read_cva(path)
    assert data.component_names == ("XX", "YY", "ZZ", "XY", "YZ", "XZ")
    assert dict(zip(data.component_names, data.values[0])) == {
        "XX": 11.0, "YY": 22.0, "ZZ": 33.0, "XY": 12.0, "YZ": 23.0, "XZ": 13.0,
    }


def test_components_are_interleaved_per_entity(tmp_path: Path):
    """Section 6.3.1: v0.x, v0.y, v0.z, v1.x — not component-planar."""
    values = np.array([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]], dtype="<f8")
    path = tmp_path / "interleaved.cva"
    write_cva(path, values=values, dtype="float64", codec=CODEC_NONE)
    blob = path.read_bytes()
    offset = struct.unpack_from("<Q", blob, 56)[0]
    assert struct.unpack_from("<6d", blob, offset) == (1.0, 2.0, 3.0, 4.0, 5.0, 6.0)
    # Planar storage would be (1,4,2,5,3,6).
    assert struct.unpack_from("<6d", blob, offset) != (1.0, 4.0, 2.0, 5.0, 3.0, 6.0)


@pytest.mark.parametrize("association, code", [
    ("mesh-vertex", 1), ("mesh-element", 0), ("vertex", 1), ("element", 0),
    ("point", 1), ("cell", 0),
])
def test_association_spellings(tmp_path: Path, association, code):
    path = tmp_path / f"{code}.cva"
    write_cva(path, values=np.zeros((2, 1), dtype="<f4"), association=association)
    assert path.read_bytes()[50] == code


def test_empty_array_round_trips(tmp_path: Path):
    path = tmp_path / "empty.cva"
    write_cva(path, values=np.zeros((0, 3), dtype="<f4"))
    data = read_cva(path)
    assert data.values.shape == (0, 3)
    assert data.frame_statistics.value_count == 0
    assert data.frame_statistics.minimum.tolist() == [np.inf] * 3


def test_overflowing_the_storage_type_is_rejected(tmp_path: Path):
    """Rule 1.6: never silently clamp. 1e30 is finite in f32, infinite in f16."""
    with pytest.raises(CVAError, match="overflow"):
        write_cva(tmp_path / "of.cva", values=np.array([[1e30]], dtype="<f4"),
                  dtype="float16")


def test_uint8_data_type_is_rejected_in_cva(tmp_path: Path):
    """Section 6.2: 3 is RESERVED for CVF's uint8, which CVA does not offer."""
    with pytest.raises(CVAError, match="unknown CVA dataType ID 3|unsupported"):
        write_cva(tmp_path / "u8.cva", values=np.zeros((2, 1)), dtype=3)


# ---------------------------------------------------------------------------
# Statistics — section 6.4
# ---------------------------------------------------------------------------

def test_frame_statistics_round_trip(tmp_path: Path):
    values = np.array([[1.0, -1.0], [3.0, -3.0], [2.0, -2.0]], dtype="<f4")
    values = np.hstack([values, values[:, :1]])  # 3 components
    path = tmp_path / "fs.cva"
    write_cva(path, values=values)
    stats = read_cva(path).frame_statistics

    assert stats.value_count == 3
    assert stats.minimum.tolist() == [1.0, -3.0, 1.0]
    assert stats.maximum.tolist() == [3.0, -1.0, 3.0]
    assert stats.mean.tolist() == pytest.approx([2.0, -2.0, 2.0])
    assert stats.valid_count.tolist() == [3, 3, 3]


def test_statistics_ignore_nan_and_report_honest_counts(tmp_path: Path):
    """Section 6.4: "a partially invalid field still reports honest statistics"."""
    values = np.array([[1.0], [np.nan], [3.0], [np.nan]], dtype="<f4")
    path = tmp_path / "nanstats.cva"
    write_cva(path, values=values)
    stats = read_cva(path).frame_statistics

    assert stats.valid_count.tolist() == [2]
    assert stats.value_count == 4
    assert stats.nan_count.tolist() == [2]
    assert stats.mean[0] == pytest.approx(2.0), "NaN must not be averaged in"
    assert not np.isnan(stats.mean[0])


def test_all_nan_component_uses_the_infinite_sentinel(tmp_path: Path):
    """Mirrors CVF section 4.4.7: min=+inf, max=-inf, mean=NaN."""
    values = np.array([[np.nan, 1.0, np.nan]], dtype="<f4")
    path = tmp_path / "allnan.cva"
    write_cva(path, values=values)
    stats = read_cva(path).frame_statistics

    assert stats.minimum[0] == np.inf
    assert stats.maximum[0] == -np.inf
    assert np.isnan(stats.mean[0])
    assert stats.valid_count[0] == 0
    assert stats.minimum[1] == 1.0


def test_global_statistics_round_trip(tmp_path: Path):
    frame_a = ArrayStatistics.from_values(np.array([[1.0], [2.0]]))
    frame_b = ArrayStatistics.from_values(np.array([[10.0], [20.0]]))
    global_stats = merge_statistics([frame_a, frame_b])

    path = tmp_path / "gs.cva"
    write_cva(path, values=np.array([[1.0], [2.0]], dtype="<f4"),
              global_statistics=global_stats)
    data = read_cva(path)

    assert data.header.has_frame_statistics and data.header.has_global_statistics
    assert data.global_statistics.minimum.tolist() == [1.0]
    assert data.global_statistics.maximum.tolist() == [20.0]
    assert data.global_statistics.mean.tolist() == pytest.approx([8.25])
    assert data.global_statistics.value_count == 4


def test_merging_frame_statistics_equals_one_pass_over_the_concatenation():
    """The property that makes incremental global statistics trustworthy."""
    a = np.array([[1.0, 5.0], [np.nan, 6.0], [3.0, 7.0]])
    b = np.array([[10.0, np.nan], [-2.0, 8.0]])

    merged = merge_statistics([
        ArrayStatistics.from_values(a), ArrayStatistics.from_values(b)
    ])
    one_pass = ArrayStatistics.from_values(np.vstack([a, b]))

    assert merged.minimum.tolist() == one_pass.minimum.tolist()
    assert merged.maximum.tolist() == one_pass.maximum.tolist()
    assert merged.mean.tolist() == pytest.approx(one_pass.mean.tolist())
    assert merged.valid_count.tolist() == one_pass.valid_count.tolist()
    assert merged.value_count == one_pass.value_count


def test_merging_an_all_nan_block_leaves_the_other_intact():
    real = ArrayStatistics.from_values(np.array([[1.0], [3.0]]))
    empty = ArrayStatistics.from_values(np.array([[np.nan], [np.nan]]))
    merged = merge_statistics([real, empty])
    assert merged.minimum.tolist() == [1.0]
    assert merged.maximum.tolist() == [3.0]
    assert merged.mean.tolist() == pytest.approx([2.0])


def test_merging_two_empty_blocks_keeps_the_sentinel():
    empty = ArrayStatistics.from_values(np.full((2, 1), np.nan))
    merged = merge_statistics([empty, empty])
    assert merged.minimum.tolist() == [np.inf]
    assert merged.maximum.tolist() == [-np.inf]
    assert np.isnan(merged.mean[0])


def test_merge_statistics_rejects_an_empty_sequence():
    with pytest.raises(CVAError, match="at least one"):
        merge_statistics([])


def test_merge_rejects_mismatched_component_counts():
    a = ArrayStatistics.from_values(np.zeros((2, 3)))
    b = ArrayStatistics.from_values(np.zeros((2, 6)))
    with pytest.raises(CVAError, match="components"):
        a.merge(b)


def test_statistics_can_be_suppressed(tmp_path: Path):
    path = tmp_path / "nostats.cva"
    write_cva(path, values=np.zeros((3, 1), dtype="<f4"), compute_statistics=False)
    blob = path.read_bytes()
    assert struct.unpack_from("<I", blob, 16)[0] == 0, "flags must be clear"
    assert struct.unpack_from("<Q", blob, 88)[0] == 0, "statisticsOffset must be 0"
    assert read_cva(path).frame_statistics is None


def test_statistics_are_computed_over_stored_values(tmp_path: Path):
    """A float16 export must declare the extremes a reader will find in the file."""
    values = np.array([[0.1], [0.7]], dtype="<f8")
    path = tmp_path / "quant.cva"
    write_cva(path, values=values, dtype="float16")
    data = read_cva(path)
    assert data.frame_statistics.minimum[0] == float(np.float16(0.1))
    assert data.frame_statistics.minimum[0] != 0.1


# ---------------------------------------------------------------------------
# Rejection
# ---------------------------------------------------------------------------

@pytest.fixture
def good_cva(tmp_path: Path) -> Path:
    path = tmp_path / "good.cva"
    write_cva(path, values=np.arange(60, dtype="<f4").reshape(20, 3), codec=CODEC_ZLIB,
              frame_index=3, field_numeric_id=8)
    return path


def test_missing_file_is_rejected(tmp_path: Path):
    with pytest.raises(CVAFormatError, match="cannot read"):
        read_cva(tmp_path / "nope.cva")


@pytest.mark.parametrize("length", [0, 1, 95])
def test_truncated_header_is_rejected(tmp_path: Path, good_cva: Path, length):
    short = tmp_path / "short.cva"
    short.write_bytes(good_cva.read_bytes()[:length])
    with pytest.raises(CVAFormatError, match="shorter than"):
        read_cva(short)


def test_bad_magic_is_rejected(tmp_path: Path, good_cva: Path):
    blob = bytearray(good_cva.read_bytes())
    blob[0:8] = b"CFDVOL1\x00"
    path = tmp_path / "bad.cva"
    path.write_bytes(bytes(blob))
    with pytest.raises(CVAFormatError, match="bad magic"):
        read_cva(path)


def test_big_endian_marker_is_rejected(tmp_path: Path, good_cva: Path):
    path = tmp_path / "be.cva"
    path.write_bytes(tamper_cva(good_cva.read_bytes(), 12, "<I", 0x04030201))
    with pytest.raises(CVAFormatError, match="byte order not supported"):
        read_cva(path)


def test_unsupported_major_version_is_rejected(tmp_path: Path, good_cva: Path):
    path = tmp_path / "v2.cva"
    path.write_bytes(tamper_cva(good_cva.read_bytes(), 8, "<H", 2))
    with pytest.raises(CVAFormatError, match="major version"):
        read_cva(path)


def test_header_crc_mismatch_is_rejected(tmp_path: Path, good_cva: Path):
    blob = bytearray(good_cva.read_bytes())
    struct.pack_into("<I", blob, 24, 4242)
    path = tmp_path / "crc.cva"
    path.write_bytes(bytes(blob))
    with pytest.raises(CVAFormatError, match="header CRC-32C mismatch"):
        read_cva(path)


def test_payload_crc_mismatch_is_rejected(tmp_path: Path, good_cva: Path):
    blob = bytearray(good_cva.read_bytes())
    offset = struct.unpack_from("<Q", blob, 56)[0]
    blob[offset] ^= 0xFF
    path = tmp_path / "payload.cva"
    path.write_bytes(bytes(blob))
    with pytest.raises(CVAFormatError, match="payload CRC-32C mismatch"):
        read_cva(path)


def test_inconsistent_uncompressed_bytes_is_rejected(tmp_path: Path, good_cva: Path):
    """Section 6.3.2, checked before allocating."""
    path = tmp_path / "size.cva"
    path.write_bytes(tamper_cva(good_cva.read_bytes(), 72, "<Q", 2**40))
    with pytest.raises(CVAFormatError, match="uncompressedBytes"):
        read_cva(path)


def test_a_huge_value_count_is_rejected_before_allocating(tmp_path: Path, good_cva: Path):
    path = tmp_path / "huge.cva"
    path.write_bytes(tamper_cva(good_cva.read_bytes(), 40, "<Q", 2**48))
    with pytest.raises(CVAFormatError, match="uncompressedBytes"):
        read_cva(path)


def test_truncated_payload_is_rejected(tmp_path: Path, good_cva: Path):
    blob = good_cva.read_bytes()
    path = tmp_path / "cut.cva"
    path.write_bytes(blob[: len(blob) - 4])
    with pytest.raises(CVAFormatError, match="truncated|only"):
        read_cva(path)


def test_payload_offset_inside_the_header_is_rejected(tmp_path: Path, good_cva: Path):
    path = tmp_path / "overlap.cva"
    path.write_bytes(tamper_cva(good_cva.read_bytes(), 56, "<Q", 8))
    with pytest.raises(CVAFormatError, match="header"):
        read_cva(path)


def test_zstd_codec_is_rejected_with_the_exact_message(tmp_path: Path, good_cva: Path):
    path = tmp_path / "zstd.cva"
    path.write_bytes(tamper_cva(good_cva.read_bytes(), 51, "<B", CODEC_ZSTD))
    with pytest.raises(UnsupportedCodecError) as excinfo:
        read_cva(path)
    assert str(excinfo.value) == ZSTD_REJECTION_MESSAGE


def test_unknown_codec_is_rejected(tmp_path: Path, good_cva: Path):
    path = tmp_path / "codec.cva"
    path.write_bytes(tamper_cva(good_cva.read_bytes(), 51, "<B", 200))
    with pytest.raises(CVAFormatError, match="codec"):
        read_cva(path)


def test_unknown_data_type_is_rejected(tmp_path: Path, good_cva: Path):
    path = tmp_path / "dtype.cva"
    path.write_bytes(tamper_cva(good_cva.read_bytes(), 49, "<B", 3))
    with pytest.raises(CVAFormatError, match="dataType"):
        read_cva(path)


def test_unknown_association_is_rejected(tmp_path: Path, good_cva: Path):
    path = tmp_path / "assoc.cva"
    path.write_bytes(tamper_cva(good_cva.read_bytes(), 50, "<B", 5))
    with pytest.raises(CVAFormatError, match="association"):
        read_cva(path)


def test_illegal_component_count_in_the_header_is_rejected(tmp_path: Path, good_cva: Path):
    path = tmp_path / "cc.cva"
    path.write_bytes(tamper_cva(good_cva.read_bytes(), 48, "<B", 4))
    with pytest.raises(CVAFormatError, match="componentCount"):
        read_cva(path)


def test_unknown_flag_bits_are_rejected(tmp_path: Path, good_cva: Path):
    path = tmp_path / "flags.cva"
    path.write_bytes(tamper_cva(good_cva.read_bytes(), 16, "<I", 0xF0))
    with pytest.raises(CVAFormatError, match="flag"):
        read_cva(path)


def test_reserved_bytes_must_be_zero(tmp_path: Path, good_cva: Path):
    path = tmp_path / "res.cva"
    path.write_bytes(tamper_cva(good_cva.read_bytes(), 84, "<I", 7))
    with pytest.raises(CVAFormatError, match="reserved"):
        read_cva(path)


def test_statistics_offset_without_a_flag_is_rejected(tmp_path: Path):
    path = tmp_path / "nostats.cva"
    write_cva(path, values=np.zeros((2, 1), dtype="<f4"), compute_statistics=False)
    bad = tmp_path / "stale.cva"
    bad.write_bytes(tamper_cva(path.read_bytes(), 88, "<Q", 96))
    with pytest.raises(CVAFormatError, match="statisticsOffset"):
        read_cva(bad)


def test_statistics_past_the_end_is_rejected(tmp_path: Path, good_cva: Path):
    path = tmp_path / "farstats.cva"
    size = good_cva.stat().st_size
    path.write_bytes(tamper_cva(good_cva.read_bytes(), 88, "<Q", size + 1024))
    with pytest.raises(CVAFormatError, match="statistics"):
        read_cva(path)


def test_error_messages_name_the_file_and_offset(tmp_path: Path, good_cva: Path):
    path = tmp_path / "be.cva"
    path.write_bytes(tamper_cva(good_cva.read_bytes(), 12, "<I", 0x04030201))
    with pytest.raises(CVAFormatError) as excinfo:
        read_cva(path)
    assert "be.cva" in str(excinfo.value)
    assert "byte offset 12" in str(excinfo.value)


def test_verify_crc_false_still_decodes(tmp_path: Path, good_cva: Path):
    blob = bytearray(good_cva.read_bytes())
    struct.pack_into("<I", blob, 24, 555)
    path = tmp_path / "skip.cva"
    path.write_bytes(bytes(blob))
    assert read_cva(path, verify_crc=False).header.frame_index == 555
