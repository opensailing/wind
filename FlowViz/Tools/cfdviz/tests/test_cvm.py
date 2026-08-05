"""CVM — boundary / structure triangle mesh. Format spec section 5.

As in ``test_cvf.py``, header offsets are asserted against the spec's table
directly and a file is decoded that was assembled by hand, so a shared
misunderstanding between writer and reader cannot pass.

The geometry helpers get a real check too: triangle winding is
counter-clockwise viewed from outside (section 5.3), which is asserted from the
geometry — signed volume and per-face normals — rather than by trusting the
vertex order that was typed in.
"""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np
import pytest

from conftest import assert_bits_equal, tamper_cvm
from cfdviz.crc32c import crc32c
from cfdviz.cvm import (
    CVM_ENDIAN_MARKER,
    CVM_FLAG_FLOAT64_POSITIONS,
    CVM_FLAG_NODE_IDS,
    CVM_FLAG_NORMALS,
    CVM_FLAG_PATCH_IDS,
    CVM_HEADER_BYTES,
    CVM_MAGIC,
    CVMError,
    CVMFormatError,
    make_box_mesh,
    make_cylinder_mesh,
    outward_winding_is_consistent,
    read_cvm,
    write_cvm,
)

TRIANGLE_POSITIONS = np.array(
    [[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]],
    dtype=np.float64,
)
TRIANGLE_INDICES = np.array([[0, 1, 2], [0, 2, 3]], dtype=np.uint32)


def test_format_constants_match_the_spec():
    assert CVM_MAGIC == b"CFDMESH1"
    assert len(CVM_MAGIC) == 8
    assert CVM_HEADER_BYTES == 96
    assert CVM_ENDIAN_MARKER == 0x01020304
    assert (CVM_FLAG_NORMALS, CVM_FLAG_PATCH_IDS, CVM_FLAG_NODE_IDS,
            CVM_FLAG_FLOAT64_POSITIONS) == (1, 2, 4, 8)


# ---------------------------------------------------------------------------
# Header layout — section 5.1's offset table
# ---------------------------------------------------------------------------

def test_header_field_offsets(tmp_path: Path):
    path = tmp_path / "offsets.cvm"
    write_cvm(
        path,
        positions=TRIANGLE_POSITIONS,
        indices=TRIANGLE_INDICES,
        normals=np.tile([0.0, 0.0, 1.0], (4, 1)),
        patch_ids=np.array([3, 4], dtype=np.uint32),
        node_ids=np.array([10, 11, 12, 13], dtype=np.uint64),
    )
    blob = path.read_bytes()

    assert blob[0:8] == b"CFDMESH1"
    assert struct.unpack_from("<H", blob, 8)[0] == 1            # majorVersion
    assert struct.unpack_from("<H", blob, 10)[0] == 0           # minorVersion
    assert struct.unpack_from("<I", blob, 12)[0] == 0x01020304  # endianMarker
    assert struct.unpack_from("<I", blob, 16)[0] == 0b0111      # flags
    assert struct.unpack_from("<I", blob, 20)[0] == 96          # headerBytes
    assert struct.unpack_from("<Q", blob, 24)[0] == 4           # vertexCount
    assert struct.unpack_from("<Q", blob, 32)[0] == 2           # triangleCount
    positions_offset = struct.unpack_from("<Q", blob, 40)[0]
    normals_offset = struct.unpack_from("<Q", blob, 48)[0]
    indices_offset = struct.unpack_from("<Q", blob, 56)[0]
    patch_offset = struct.unpack_from("<Q", blob, 64)[0]
    node_offset = struct.unpack_from("<Q", blob, 72)[0]
    assert blob[84:96] == b"\x00" * 12                          # reserved

    assert positions_offset >= 96
    assert all(o > 0 for o in (normals_offset, indices_offset, patch_offset, node_offset))

    # Arrays decode from those offsets with the documented per-element types.
    assert np.frombuffer(blob, "<f4", 12, positions_offset).reshape(4, 3).tolist() == \
        TRIANGLE_POSITIONS.tolist()
    assert np.frombuffer(blob, "<u4", 6, indices_offset).reshape(2, 3).tolist() == \
        TRIANGLE_INDICES.tolist()
    assert np.frombuffer(blob, "<u4", 2, patch_offset).tolist() == [3, 4]
    assert np.frombuffer(blob, "<u8", 4, node_offset).tolist() == [10, 11, 12, 13]


def test_header_crc_covers_the_header_with_its_own_field_zeroed(tmp_path: Path):
    """Section 5.1: CRC-32C over [0, 96) with [80, 84) zeroed."""
    path = tmp_path / "crc.cvm"
    write_cvm(path, positions=TRIANGLE_POSITIONS, indices=TRIANGLE_INDICES)
    blob = path.read_bytes()

    stored = struct.unpack_from("<I", blob, 80)[0]
    assert stored == crc32c(blob[:80] + b"\x00\x00\x00\x00" + blob[84:96])
    assert stored != crc32c(blob[:96])


def test_cvm_and_cva_share_the_crc_offset(tmp_path: Path):
    """Section 6.1: "byte-for-byte the same rule as CVM 5.1"."""
    from cfdviz.cva import CVA_HEADER_BYTES

    assert CVA_HEADER_BYTES == CVM_HEADER_BYTES == 96


def test_reader_decodes_a_file_assembled_by_hand_from_the_spec_table(tmp_path: Path):
    positions = np.array([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0], [7.0, 8.0, 9.0]], dtype="<f4")
    indices = np.array([[0, 1, 2]], dtype="<u4")

    header = bytearray(96)
    header[0:8] = b"CFDMESH1"
    struct.pack_into("<H", header, 8, 1)            # majorVersion
    struct.pack_into("<H", header, 10, 0)           # minorVersion
    struct.pack_into("<I", header, 12, 0x01020304)  # endianMarker
    struct.pack_into("<I", header, 16, 0)           # flags — no optional arrays
    struct.pack_into("<I", header, 20, 96)          # headerBytes
    struct.pack_into("<Q", header, 24, 3)           # vertexCount
    struct.pack_into("<Q", header, 32, 1)           # triangleCount
    struct.pack_into("<Q", header, 40, 96)          # positionsOffset
    struct.pack_into("<Q", header, 48, 0)           # normalsOffset — absent -> 0
    struct.pack_into("<Q", header, 56, 96 + 36)     # indicesOffset
    struct.pack_into("<Q", header, 64, 0)           # patchIdsOffset
    struct.pack_into("<Q", header, 72, 0)           # nodeIdsOffset
    struct.pack_into("<I", header, 80, crc32c(bytes(header)))

    path = tmp_path / "handmade.cvm"
    path.write_bytes(bytes(header) + positions.tobytes() + indices.tobytes())

    mesh = read_cvm(path)
    assert mesh.vertex_count == 3
    assert mesh.triangle_count == 1
    assert mesh.positions.tolist() == positions.tolist()
    assert mesh.indices.tolist() == indices.tolist()
    assert mesh.normals is None and mesh.patch_ids is None and mesh.node_ids is None


# ---------------------------------------------------------------------------
# Round trips
# ---------------------------------------------------------------------------

def test_minimal_round_trip(tmp_path: Path):
    path = tmp_path / "min.cvm"
    write_cvm(path, positions=TRIANGLE_POSITIONS, indices=TRIANGLE_INDICES)
    mesh = read_cvm(path)
    assert mesh.positions.dtype == np.dtype("<f4")
    assert mesh.positions.tolist() == TRIANGLE_POSITIONS.astype("<f4").tolist()
    assert mesh.indices.tolist() == TRIANGLE_INDICES.tolist()
    assert mesh.header.flags == 0


def test_full_round_trip_with_every_optional_array(tmp_path: Path):
    normals = np.array([[0, 0, 1], [0, 1, 0], [1, 0, 0], [0, 0, -1]], dtype=np.float64)
    patch_ids = np.array([7, 9], dtype=np.uint32)
    node_ids = np.array([2**40, 2**41, 2**42, 2**63 + 1], dtype=np.uint64)

    path = tmp_path / "full.cvm"
    write_cvm(path, positions=TRIANGLE_POSITIONS, indices=TRIANGLE_INDICES,
              normals=normals, patch_ids=patch_ids, node_ids=node_ids)
    mesh = read_cvm(path)

    assert mesh.normals.tolist() == normals.astype("<f4").tolist()
    assert mesh.patch_ids.tolist() == patch_ids.tolist()
    assert mesh.node_ids.tolist() == node_ids.tolist(), "uint64 node IDs must not narrow"


def test_float64_positions_round_trip_at_full_precision(tmp_path: Path):
    """Flag bit 3. Coordinates that float32 cannot represent must survive."""
    positions = np.array(
        [[1234567.891234, 0.1234567890123, -9876543.210987],
         [1.0000000001, 2.0000000002, 3.0000000003],
         [0.0, 0.0, 0.0]],
        dtype=np.float64,
    )
    indices = np.array([[0, 1, 2]], dtype=np.uint32)

    path = tmp_path / "f64.cvm"
    write_cvm(path, positions=positions, indices=indices, use_float64=True)
    mesh = read_cvm(path)

    assert mesh.header.positions_are_float64
    assert mesh.header.flags & CVM_FLAG_FLOAT64_POSITIONS
    assert mesh.positions.dtype == np.dtype("<f8")
    assert_bits_equal(mesh.positions, positions.astype("<f8"))
    # And float32 storage would genuinely have lost this — proving the test bites.
    assert positions.astype("<f4").astype("<f8")[0, 0] != positions[0, 0]


def test_nan_positions_survive(tmp_path: Path):
    positions = TRIANGLE_POSITIONS.copy()
    positions[1, 1] = np.nan
    path = tmp_path / "nan.cvm"
    write_cvm(path, positions=positions, indices=TRIANGLE_INDICES)
    assert np.isnan(read_cvm(path).positions[1, 1])


def test_empty_mesh_round_trips(tmp_path: Path):
    path = tmp_path / "empty.cvm"
    write_cvm(path, positions=np.zeros((0, 3)), indices=np.zeros((0, 3), dtype=np.uint32))
    mesh = read_cvm(path)
    assert mesh.vertex_count == 0
    assert mesh.triangle_count == 0


def test_absent_arrays_have_offset_zero(tmp_path: Path):
    """Section 5.2: an absent array's offset MUST be written as 0."""
    path = tmp_path / "sparse.cvm"
    write_cvm(path, positions=TRIANGLE_POSITIONS, indices=TRIANGLE_INDICES,
              patch_ids=np.array([1, 2], dtype=np.uint32))
    blob = path.read_bytes()
    assert struct.unpack_from("<Q", blob, 48)[0] == 0, "normalsOffset"
    assert struct.unpack_from("<Q", blob, 72)[0] == 0, "nodeIdsOffset"
    assert struct.unpack_from("<Q", blob, 64)[0] != 0, "patchIdsOffset"


# ---------------------------------------------------------------------------
# Association: patchIds per TRIANGLE, nodeIds per VERTEX (section 5.3)
# ---------------------------------------------------------------------------

def test_patch_ids_are_per_triangle(tmp_path: Path):
    """4 vertices, 2 triangles — a per-vertex patch array must be rejected."""
    with pytest.raises(CVMError, match="triangle"):
        write_cvm(tmp_path / "bad.cvm", positions=TRIANGLE_POSITIONS,
                  indices=TRIANGLE_INDICES,
                  patch_ids=np.array([1, 2, 3, 4], dtype=np.uint32))


def test_node_ids_are_per_vertex(tmp_path: Path):
    with pytest.raises(CVMError, match="vertex|vertices"):
        write_cvm(tmp_path / "bad.cvm", positions=TRIANGLE_POSITIONS,
                  indices=TRIANGLE_INDICES,
                  node_ids=np.array([1, 2], dtype=np.uint64))


def test_normals_are_per_vertex(tmp_path: Path):
    with pytest.raises(CVMError, match="vertex|vertices"):
        write_cvm(tmp_path / "bad.cvm", positions=TRIANGLE_POSITIONS,
                  indices=TRIANGLE_INDICES, normals=np.zeros((2, 3)))


# ---------------------------------------------------------------------------
# Index validation — section 5.3
# ---------------------------------------------------------------------------

def test_out_of_range_index_is_rejected_on_write(tmp_path: Path):
    with pytest.raises(CVMError, match="vertexCount"):
        write_cvm(tmp_path / "bad.cvm", positions=TRIANGLE_POSITIONS,
                  indices=np.array([[0, 1, 4]], dtype=np.uint32))


def test_negative_index_is_rejected_before_the_uint32_cast(tmp_path: Path):
    """Casting -1 to uint32 gives 4294967295, which then looks like a big index."""
    with pytest.raises(CVMError, match="negative"):
        write_cvm(tmp_path / "bad.cvm", positions=TRIANGLE_POSITIONS,
                  indices=np.array([[0, 1, -1]], dtype=np.int64))


def test_out_of_range_index_is_rejected_on_read(tmp_path: Path):
    path = tmp_path / "good.cvm"
    write_cvm(path, positions=TRIANGLE_POSITIONS, indices=TRIANGLE_INDICES)
    blob = bytearray(path.read_bytes())
    indices_offset = struct.unpack_from("<Q", blob, 56)[0]
    struct.pack_into("<I", blob, indices_offset, 99)
    bad = tmp_path / "bad.cvm"
    bad.write_bytes(bytes(blob))
    with pytest.raises(CVMFormatError, match="vertexCount"):
        read_cvm(bad)


# ---------------------------------------------------------------------------
# Rejection
# ---------------------------------------------------------------------------

@pytest.fixture
def good_cvm(tmp_path: Path) -> Path:
    path = tmp_path / "good.cvm"
    write_cvm(path, positions=TRIANGLE_POSITIONS, indices=TRIANGLE_INDICES,
              normals=np.tile([0.0, 0.0, 1.0], (4, 1)),
              patch_ids=np.array([1, 2], dtype=np.uint32))
    return path


def test_missing_file_is_rejected(tmp_path: Path):
    with pytest.raises(CVMFormatError, match="cannot read"):
        read_cvm(tmp_path / "nope.cvm")


@pytest.mark.parametrize("length", [0, 1, 95])
def test_truncated_header_is_rejected(tmp_path: Path, good_cvm: Path, length):
    short = tmp_path / "short.cvm"
    short.write_bytes(good_cvm.read_bytes()[:length])
    with pytest.raises(CVMFormatError, match="shorter than"):
        read_cvm(short)


def test_bad_magic_is_rejected(tmp_path: Path, good_cvm: Path):
    blob = bytearray(good_cvm.read_bytes())
    blob[0:8] = b"CFDVOL1\x00"  # a valid CVF magic — the wrong container
    path = tmp_path / "bad.cvm"
    path.write_bytes(bytes(blob))
    with pytest.raises(CVMFormatError, match="bad magic"):
        read_cvm(path)


def test_big_endian_marker_is_rejected(tmp_path: Path, good_cvm: Path):
    path = tmp_path / "be.cvm"
    path.write_bytes(tamper_cvm(good_cvm.read_bytes(), 12, "<I", 0x04030201))
    with pytest.raises(CVMFormatError, match="byte order not supported"):
        read_cvm(path)


def test_unsupported_major_version_is_rejected(tmp_path: Path, good_cvm: Path):
    path = tmp_path / "v3.cvm"
    path.write_bytes(tamper_cvm(good_cvm.read_bytes(), 8, "<H", 3))
    with pytest.raises(CVMFormatError, match="major version"):
        read_cvm(path)


def test_header_crc_mismatch_is_rejected(tmp_path: Path, good_cvm: Path):
    blob = bytearray(good_cvm.read_bytes())
    struct.pack_into("<Q", blob, 24, 4)  # rewrite vertexCount, leave the CRC stale
    blob[26] ^= 0x01
    path = tmp_path / "badcrc.cvm"
    path.write_bytes(bytes(blob))
    with pytest.raises(CVMFormatError, match="header CRC-32C mismatch"):
        read_cvm(path)


def test_truncated_arrays_are_rejected(tmp_path: Path, good_cvm: Path):
    blob = good_cvm.read_bytes()
    path = tmp_path / "cut.cvm"
    path.write_bytes(blob[: len(blob) - 8])
    with pytest.raises(CVMFormatError, match="truncated|only"):
        read_cvm(path)


def test_a_vertex_count_beyond_the_file_is_rejected_before_allocating(
    tmp_path: Path, good_cvm: Path
):
    """Rule 1.5: 2**40 vertices is 12 TiB. Reject, do not allocate."""
    path = tmp_path / "huge.cvm"
    path.write_bytes(tamper_cvm(good_cvm.read_bytes(), 24, "<Q", 2**40))
    with pytest.raises(CVMFormatError, match="truncated|only"):
        read_cvm(path)


def test_unknown_flag_bits_are_rejected(tmp_path: Path, good_cvm: Path):
    path = tmp_path / "flags.cvm"
    path.write_bytes(tamper_cvm(good_cvm.read_bytes(), 16, "<I", 0xFF))
    with pytest.raises(CVMFormatError, match="flag"):
        read_cvm(path)


def test_nonzero_offset_behind_a_clear_flag_is_rejected(tmp_path: Path):
    """Section 5.2: an absent array's offset MUST be 0 and MUST be ignored."""
    path = tmp_path / "stale.cvm"
    write_cvm(path, positions=TRIANGLE_POSITIONS, indices=TRIANGLE_INDICES)
    bad = tmp_path / "stalebad.cvm"
    bad.write_bytes(tamper_cvm(path.read_bytes(), 48, "<Q", 96))
    with pytest.raises(CVMFormatError, match="presence flag"):
        read_cvm(bad)


def test_wrong_header_bytes_is_rejected(tmp_path: Path, good_cvm: Path):
    path = tmp_path / "hb.cvm"
    path.write_bytes(tamper_cvm(good_cvm.read_bytes(), 20, "<I", 128))
    with pytest.raises(CVMFormatError, match="headerBytes"):
        read_cvm(path)


def test_reserved_bytes_must_be_zero(tmp_path: Path, good_cvm: Path):
    path = tmp_path / "res.cvm"
    path.write_bytes(tamper_cvm(good_cvm.read_bytes(), 84, "<I", 1))
    with pytest.raises(CVMFormatError, match="reserved"):
        read_cvm(path)


def test_error_messages_name_the_file_and_offset(tmp_path: Path, good_cvm: Path):
    """Section 10: a corrupt case must identify the file and the byte offset."""
    path = tmp_path / "be.cvm"
    path.write_bytes(tamper_cvm(good_cvm.read_bytes(), 12, "<I", 0x04030201))
    with pytest.raises(CVMFormatError) as excinfo:
        read_cvm(path)
    assert "be.cvm" in str(excinfo.value)
    assert "byte offset 12" in str(excinfo.value)
    assert excinfo.value.offset == 12


# ---------------------------------------------------------------------------
# Geometry helpers — winding is asserted from the geometry, not from the code
# ---------------------------------------------------------------------------

def _signed_volume(positions: np.ndarray, indices: np.ndarray) -> float:
    """Six times the signed volume of a closed mesh.

    Positive means every triangle is counter-clockwise viewed from outside
    (section 5.3). This is derived from the actual vertex data, so it fails if
    the generator's winding is wrong even when its own normals agree with it.
    """
    a = positions[indices[:, 0]]
    b = positions[indices[:, 1]]
    c = positions[indices[:, 2]]
    return float(np.einsum("ij,ij->i", a, np.cross(b, c)).sum())


def test_box_mesh_is_closed_and_wound_counter_clockwise_from_outside():
    positions, indices, normals, patch_ids = make_box_mesh((-1, -2, -3), (1, 2, 3))
    assert positions.shape == (24, 3)
    assert indices.shape == (12, 3)
    assert patch_ids.shape == (12,)

    # 2x4x6 = 48; the signed volume is 6V for an outward-wound closed mesh.
    assert _signed_volume(positions, indices) == pytest.approx(6 * 48.0)
    assert outward_winding_is_consistent(positions, indices, normals)


def test_inward_box_reverses_the_winding():
    """A domain boundary seen from inside the fluid must flip, not just recolour."""
    _, outward_idx, _, _ = make_box_mesh((0, 0, 0), (1, 1, 1))
    positions, inward_idx, normals, _ = make_box_mesh((0, 0, 0), (1, 1, 1), inward=True)

    assert _signed_volume(positions, outward_idx) > 0
    assert _signed_volume(positions, inward_idx) < 0
    assert outward_winding_is_consistent(positions, inward_idx, normals)


def test_box_patch_ids_use_the_spec_named_patches():
    """Section 5.3 requires inlet, outlet, sideWalls and cylinderWall to exist."""
    _, _, _, patch_ids = make_box_mesh(
        (0, 0, 0), (10, 4, 4), {"inlet": 1, "outlet": 2, "sideWalls": 3}
    )
    # Face order is -x, +x, -y, +y, -z, +z, two triangles each.
    assert patch_ids.tolist() == [1, 1, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3]


def test_a_specific_face_overrides_a_group_regardless_of_dict_order():
    _, _, _, a = make_box_mesh((0, 0, 0), (1, 1, 1), {"sideWalls": 3, "-y": 9})
    _, _, _, b = make_box_mesh((0, 0, 0), (1, 1, 1), {"-y": 9, "sideWalls": 3})
    assert a.tolist() == b.tolist()
    assert a[4] == 9 and a[5] == 9  # the -y face
    assert a[6] == 3                # +y still the group value


def test_unknown_box_face_key_is_rejected():
    with pytest.raises(CVMError, match="unknown box face key"):
        make_box_mesh((0, 0, 0), (1, 1, 1), {"topSide": 1})


def test_degenerate_box_is_rejected():
    with pytest.raises(CVMError, match="degenerate|inverted"):
        make_box_mesh((1, 1, 1), (1, 2, 3))


def test_cylinder_is_closed_and_outward_wound():
    positions, indices, normals, patch_ids = make_cylinder_mesh(
        (0, 0, 0), radius=2.0, height=4.0, axis="z", segments=64, patch_id=4
    )
    # A 64-gon prism approximates pi*r^2*h = 50.265; the inscribed polygon is
    # slightly smaller, so an exact match would be wrong to assert.
    volume = _signed_volume(positions, indices) / 6.0
    assert volume == pytest.approx(np.pi * 4.0 * 4.0, rel=0.01)
    assert volume > 0, "cylinder is wound inside-out"
    assert outward_winding_is_consistent(positions, indices, normals)
    assert set(patch_ids.tolist()) == {4}


@pytest.mark.parametrize("axis", ["x", "y", "z", 0, 1, 2])
def test_cylinder_winding_is_correct_on_every_axis(axis):
    positions, indices, normals, _ = make_cylinder_mesh(
        (1, 2, 3), radius=1.0, height=2.0, axis=axis, segments=16
    )
    assert _signed_volume(positions, indices) > 0
    assert outward_winding_is_consistent(positions, indices, normals)


def test_uncapped_cylinder_has_no_caps():
    _, capped, _, _ = make_cylinder_mesh((0, 0, 0), 1.0, 2.0, segments=8)
    _, open_ended, _, _ = make_cylinder_mesh((0, 0, 0), 1.0, 2.0, segments=8, capped=False)
    assert open_ended.shape[0] == 16          # 2 per segment, side wall only
    assert capped.shape[0] == 16 + 2 * 8      # plus one fan triangle per segment per cap


def test_cylinder_side_normals_are_radial_and_unit_length():
    positions, _, normals, _ = make_cylinder_mesh((5, 0, 0), 2.0, 4.0, "z", segments=12)
    side = normals[:24]
    assert np.allclose(np.linalg.norm(side, axis=1), 1.0)
    assert np.allclose(side[:, 2], 0.0), "a Z-axis cylinder's wall normals have no Z"
    radial = positions[:24] - np.array([5.0, 0.0, 0.0])
    radial[:, 2] = 0.0
    radial /= np.linalg.norm(radial, axis=1)[:, None]
    assert np.allclose(radial, side, atol=1e-12)


@pytest.mark.parametrize(
    "kwargs, message",
    [
        ({"radius": 0.0}, "radius"),
        ({"radius": -1.0}, "radius"),
        ({"height": 0.0}, "height"),
        ({"segments": 2}, "segments"),
    ],
)
def test_degenerate_cylinder_is_rejected(kwargs, message):
    args = {"center": (0, 0, 0), "radius": 1.0, "height": 1.0, "segments": 8}
    args.update(kwargs)
    with pytest.raises(CVMError, match=message):
        make_cylinder_mesh(**args)


def test_unknown_axis_is_rejected():
    with pytest.raises(CVMError, match="axis"):
        make_cylinder_mesh((0, 0, 0), 1.0, 1.0, axis="w")


def test_winding_check_actually_rejects_a_flipped_mesh():
    """The check that the winding check can fail — VISUAL_QA section 4."""
    positions, indices, normals, _ = make_box_mesh((0, 0, 0), (1, 1, 1))
    assert outward_winding_is_consistent(positions, indices, normals)
    flipped = indices[:, ::-1].copy()
    assert not outward_winding_is_consistent(positions, flipped, normals)


def test_generated_box_round_trips_through_the_file(tmp_path: Path):
    positions, indices, normals, patch_ids = make_box_mesh(
        (0, 0, 0), (10, 4, 4), {"inlet": 1, "outlet": 2, "sideWalls": 3}
    )
    path = tmp_path / "box.cvm"
    write_cvm(path, positions=positions, indices=indices, normals=normals,
              patch_ids=patch_ids)
    mesh = read_cvm(path)

    assert mesh.patch_ids.tolist() == patch_ids.tolist()
    assert _signed_volume(mesh.positions.astype(np.float64), mesh.indices) > 0
    assert outward_winding_is_consistent(mesh.positions, mesh.indices, mesh.normals)
