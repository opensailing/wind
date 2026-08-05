"""Compression codecs — format spec section 7 and ADR 005.

The load-bearing facts here are not "compression works" but:

- zlib payloads are the **RFC 1950 container**, not raw DEFLATE. Unreal decodes
  with ``NAME_Zlib``, which requires the 2-byte header and the trailing Adler-32.
  Emitting raw DEFLATE would round-trip perfectly in Python and be undecodable
  by the shipping reader.
- zstd is reserved and must be rejected with an **exact** user-facing string,
  matched character for character against the Unreal constant.
- ``decompress`` verifies the declared uncompressed size before returning, so a
  truncated or substituted payload cannot silently yield a short buffer.
"""

from __future__ import annotations

import zlib

import pytest

from cfdviz.codecs import (
    CODEC_LZ4,
    CODEC_NAMES,
    CODEC_NONE,
    CODEC_ZLIB,
    CODEC_ZSTD,
    DEFAULT_CODEC,
    DEFAULT_ZLIB_LEVEL,
    ZSTD_REJECTION_MESSAGE,
    CodecError,
    UnsupportedCodecError,
    codec_id_from_name,
    compress,
    decompress,
    is_codec_available,
)

PAYLOAD = (b"CFDViz brick payload " * 200) + bytes(range(256))


def test_codec_ids_match_the_spec_table():
    """Section 4.2: none=0, zstd=1, lz4=2, zlib=3. These are on-disk bytes."""
    assert CODEC_NONE == 0
    assert CODEC_ZSTD == 1
    assert CODEC_LZ4 == 2
    assert CODEC_ZLIB == 3
    assert CODEC_NAMES == {0: "none", 1: "zstd", 2: "lz4", 3: "zlib"}


def test_zlib_is_the_shipping_default():
    """Section 7: the shipping default is codec 3 at level 6."""
    assert DEFAULT_CODEC == CODEC_ZLIB
    assert DEFAULT_ZLIB_LEVEL == 6


def test_zlib_output_is_the_rfc1950_container_not_raw_deflate():
    """The single most consequential detail in section 7.

    ``NAME_Zlib`` on the Unreal side consumes RFC 1950: a 2-byte header whose
    first nibble is 8 (deflate), a 32 KiB window, and a trailing Adler-32.
    Raw DEFLATE (RFC 1951) round-trips fine through Python and is undecodable
    by the engine, so the bytes themselves are asserted here.
    """
    blob = compress(PAYLOAD, CODEC_ZLIB)

    assert blob[0] & 0x0F == 8, "CMF low nibble must be 8 (deflate)"
    assert (blob[0] << 8 | blob[1]) % 31 == 0, "RFC 1950 header checksum failed"
    assert blob[-4:] == zlib.adler32(PAYLOAD).to_bytes(4, "big"), "missing trailing Adler-32"

    # And a raw-DEFLATE stream would fail every one of those.
    raw = zlib.compressobj(DEFAULT_ZLIB_LEVEL, zlib.DEFLATED, -zlib.MAX_WBITS)
    raw_deflate = raw.compress(PAYLOAD) + raw.flush()
    assert blob != raw_deflate
    assert zlib.decompress(blob) == PAYLOAD


def test_zlib_default_level_is_six():
    """Level 6 is normative, so two writers produce identical bytes."""
    assert compress(PAYLOAD, CODEC_ZLIB) == zlib.compress(PAYLOAD, 6)
    assert compress(PAYLOAD, CODEC_ZLIB) != zlib.compress(PAYLOAD, 9)


@pytest.mark.parametrize("codec", [CODEC_NONE, CODEC_ZLIB])
def test_round_trip(codec):
    blob = compress(PAYLOAD, codec)
    assert decompress(blob, codec, len(PAYLOAD)) == PAYLOAD


def test_none_is_a_verbatim_copy():
    assert compress(PAYLOAD, CODEC_NONE) == PAYLOAD


@pytest.mark.skipif(not is_codec_available(CODEC_LZ4), reason="optional lz4 not installed")
def test_lz4_round_trip():
    blob = compress(PAYLOAD, CODEC_LZ4)
    assert decompress(blob, CODEC_LZ4, len(PAYLOAD)) == PAYLOAD


@pytest.mark.skipif(not is_codec_available(CODEC_LZ4), reason="optional lz4 not installed")
def test_lz4_is_a_raw_block_with_no_size_prefix():
    """The same trap as raw-DEFLATE-vs-zlib, one codec over.

    Unreal decompresses with ``LZ4_decompress_safe``, a raw block that carries
    no length header — the size comes from the brick's ``uncompressedBytes``.
    ``lz4.block.compress`` defaults to ``store_size=True``, which prepends four
    little-endian bytes. That round-trips perfectly through Python and fails in
    the engine, so a round-trip test alone cannot catch it; the leading bytes
    are asserted instead.
    """
    import lz4.block

    blob = compress(PAYLOAD, CODEC_LZ4)
    prefixed = lz4.block.compress(PAYLOAD, store_size=True)

    assert blob == lz4.block.compress(PAYLOAD, store_size=False)
    assert blob != prefixed, "a size prefix leaked into the stored block"
    assert prefixed[:4] == len(PAYLOAD).to_bytes(4, "little")
    assert blob[:4] != len(PAYLOAD).to_bytes(4, "little")
    # Decodable knowing only the size from the brick directory, which is all
    # LZ4_decompress_safe is given.
    assert lz4.block.decompress(blob, uncompressed_size=len(PAYLOAD)) == PAYLOAD


def test_empty_payload_round_trips():
    for codec in (CODEC_NONE, CODEC_ZLIB):
        assert decompress(compress(b"", codec), codec, 0) == b""


# --- zstd: reserved, and the rejection wording is normative -----------------

def test_compress_rejects_zstd_with_the_exact_message():
    with pytest.raises(UnsupportedCodecError) as excinfo:
        compress(PAYLOAD, CODEC_ZSTD)
    assert str(excinfo.value) == ZSTD_REJECTION_MESSAGE


def test_decompress_rejects_zstd_with_the_exact_message():
    with pytest.raises(UnsupportedCodecError) as excinfo:
        decompress(b"\x28\xb5\x2f\xfd", CODEC_ZSTD, 16)
    assert str(excinfo.value) == ZSTD_REJECTION_MESSAGE


def test_zstd_rejection_message_is_character_for_character_the_spec_string():
    """Section 7 fixes this wording; the Unreal reader emits the same literal.

    Written out in full rather than referencing the constant, so an edit to the
    constant fails here instead of silently redefining the contract.
    """
    assert ZSTD_REJECTION_MESSAGE == (
        "CVF codec 'zstd' is reserved but not supported in CFDViz 1.0. "
        "Re-export this case with codec 'zlib'."
    )


def test_zstd_is_never_available():
    assert is_codec_available(CODEC_ZSTD) is False


def test_zstd_never_silently_falls_back():
    """Section 7: "MUST NOT silently fall back to another codec"."""
    for attempt in (lambda: compress(PAYLOAD, CODEC_ZSTD),
                    lambda: decompress(zlib.compress(PAYLOAD), CODEC_ZSTD, len(PAYLOAD))):
        with pytest.raises(UnsupportedCodecError):
            attempt()


# --- failure paths ----------------------------------------------------------

def test_decompress_rejects_a_size_mismatch():
    """A payload that decodes to the wrong length is corruption, not data."""
    blob = compress(PAYLOAD, CODEC_ZLIB)
    with pytest.raises(CodecError, match="size mismatch"):
        decompress(blob, CODEC_ZLIB, len(PAYLOAD) + 1)


def test_decompress_rejects_corrupt_zlib_data():
    blob = bytearray(compress(PAYLOAD, CODEC_ZLIB))
    blob[len(blob) // 2] ^= 0xFF
    with pytest.raises(CodecError, match="zlib decompression failed"):
        decompress(bytes(blob), CODEC_ZLIB, len(PAYLOAD))


def test_decompress_rejects_raw_deflate_offered_as_zlib():
    """Proves the container check is real, not incidental."""
    raw = zlib.compressobj(6, zlib.DEFLATED, -zlib.MAX_WBITS)
    blob = raw.compress(PAYLOAD) + raw.flush()
    with pytest.raises(CodecError):
        decompress(blob, CODEC_ZLIB, len(PAYLOAD))


def test_unknown_codec_id_is_rejected():
    with pytest.raises(UnsupportedCodecError, match="Unknown codec ID 99"):
        compress(PAYLOAD, 99)
    with pytest.raises(UnsupportedCodecError, match="Unknown codec ID 99"):
        decompress(PAYLOAD, 99, len(PAYLOAD))


def test_codec_id_from_name():
    assert codec_id_from_name("zlib") == CODEC_ZLIB
    assert codec_id_from_name("none") == CODEC_NONE
    with pytest.raises(UnsupportedCodecError, match="Unknown codec name"):
        codec_id_from_name("brotli")
