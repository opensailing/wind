"""Compression codecs for CFDViz binary payloads.

See ``Docs/CFDVIZ_FORMAT.md`` section 7 and ``Docs/ADR/005-compression-codec.md``.

The shipping codec is **zlib**, not zstd. Unreal Engine 5.8 ships no linkable
C++ zstd — only ``ZstdSharp.dll``, a .NET assembly for build tooling — so a
zstd-mandated format could not be decoded by the Unreal reader that has to read
this project's own sample data. zlib is in the Python standard library and is
engine-native on the Unreal side via ``FCompression``/``NAME_Zlib``.

zlib payloads use the **zlib container format (RFC 1950)** — 2-byte header plus
trailing Adler-32 — which is exactly what ``zlib.compress`` emits and what
``NAME_Zlib`` consumes. Raw DEFLATE (RFC 1951) is not used.
"""

from __future__ import annotations

import zlib
from typing import Final

__all__ = [
    "CODEC_NONE",
    "CODEC_ZSTD",
    "CODEC_LZ4",
    "CODEC_ZLIB",
    "CODEC_NAMES",
    "DEFAULT_CODEC",
    "DEFAULT_ZLIB_LEVEL",
    "ZSTD_REJECTION_MESSAGE",
    "CodecError",
    "UnsupportedCodecError",
    "compress",
    "decompress",
    "codec_id_from_name",
    "is_codec_available",
]

CODEC_NONE: Final = 0
#: Reserved but unimplemented in 1.0. See :data:`ZSTD_REJECTION_MESSAGE`.
CODEC_ZSTD: Final = 1
CODEC_LZ4: Final = 2
CODEC_ZLIB: Final = 3

CODEC_NAMES: Final[dict[int, str]] = {
    CODEC_NONE: "none",
    CODEC_ZSTD: "zstd",
    CODEC_LZ4: "lz4",
    CODEC_ZLIB: "zlib",
}

DEFAULT_CODEC: Final = CODEC_ZLIB
DEFAULT_ZLIB_LEVEL: Final = 6

#: Exact user-facing message required by the spec when zstd data is encountered.
#: The Unreal reader emits the identical string.
ZSTD_REJECTION_MESSAGE: Final = (
    "CVF codec 'zstd' is reserved but not supported in CFDViz 1.0. "
    "Re-export this case with codec 'zlib'."
)


class CodecError(Exception):
    """A payload could not be compressed or decompressed."""


class UnsupportedCodecError(CodecError):
    """The codec is recognised by the format but unavailable in this build."""


def is_codec_available(codec: int) -> bool:
    """Return whether ``codec`` can actually be used in this environment."""
    if codec in (CODEC_NONE, CODEC_ZLIB):
        return True
    if codec == CODEC_LZ4:
        try:
            import lz4.block  # noqa: F401
        except ImportError:
            return False
        return True
    return False  # zstd is reserved and never available in 1.0


def codec_id_from_name(name: str) -> int:
    """Map a codec name to its numeric ID, raising on anything unknown."""
    for codec_id, codec_name in CODEC_NAMES.items():
        if codec_name == name:
            return codec_id
    raise UnsupportedCodecError(
        f"Unknown codec name {name!r}. Supported: {sorted(CODEC_NAMES.values())}"
    )


def compress(data: bytes, codec: int = DEFAULT_CODEC, level: int | None = None) -> bytes:
    """Compress ``data`` with ``codec``.

    Args:
        data: Raw bytes to compress.
        codec: One of the ``CODEC_*`` constants.
        level: Codec-specific level; ``None`` selects the documented default.

    Raises:
        UnsupportedCodecError: For zstd (reserved) or an unavailable codec.
    """
    if codec == CODEC_NONE:
        return data
    if codec == CODEC_ZLIB:
        return zlib.compress(data, DEFAULT_ZLIB_LEVEL if level is None else level)
    if codec == CODEC_ZSTD:
        raise UnsupportedCodecError(ZSTD_REJECTION_MESSAGE)
    if codec == CODEC_LZ4:
        try:
            import lz4.block
        except ImportError as exc:  # pragma: no cover - depends on optional extra
            raise UnsupportedCodecError(
                "Codec 'lz4' requires the optional 'lz4' package: pip install cfdviz[lz4]"
            ) from exc
        return lz4.block.compress(data, store_size=False)
    raise UnsupportedCodecError(f"Unknown codec ID {codec}.")


def decompress(data: bytes, codec: int, uncompressed_size: int) -> bytes:
    """Decompress ``data``, verifying it yields exactly ``uncompressed_size`` bytes.

    The size is always known from the brick directory, so a payload that decodes
    to a different length indicates corruption or a format mismatch and is
    rejected rather than returned.

    Raises:
        UnsupportedCodecError: For zstd (reserved) or an unavailable codec.
        CodecError: If decompression fails or produces the wrong length.
    """
    if codec == CODEC_ZSTD:
        raise UnsupportedCodecError(ZSTD_REJECTION_MESSAGE)

    if codec == CODEC_NONE:
        out = data
    elif codec == CODEC_ZLIB:
        try:
            out = zlib.decompress(data)
        except zlib.error as exc:
            raise CodecError(f"zlib decompression failed: {exc}") from exc
    elif codec == CODEC_LZ4:
        try:
            import lz4.block
        except ImportError as exc:  # pragma: no cover - depends on optional extra
            raise UnsupportedCodecError(
                "Codec 'lz4' requires the optional 'lz4' package: pip install cfdviz[lz4]"
            ) from exc
        try:
            out = lz4.block.decompress(data, uncompressed_size=uncompressed_size)
        except Exception as exc:
            raise CodecError(f"lz4 decompression failed: {exc}") from exc
    else:
        raise UnsupportedCodecError(f"Unknown codec ID {codec}.")

    if len(out) != uncompressed_size:
        raise CodecError(
            f"Decompressed size mismatch: got {len(out)} bytes, "
            f"expected {uncompressed_size} declared in the brick directory."
        )
    return out
