"""CRC-32C (Castagnoli) — the checksum used by every CFDViz binary format.

This is **not** the CRC-32 used by zip/zlib (polynomial 0x04C11DB7) and **not**
Unreal's ``FCrc::MemCrc32``. Mixing them up produces files that one
implementation writes and the other rejects, so the check value in
:func:`self_check` is asserted by the test suite on both the Python and the
C++ side.

Parameters (see ``Docs/CFDVIZ_FORMAT.md`` section 8)::

    polynomial (reflected)  0x82F63B78
    initial value           0xFFFFFFFF
    input/output reflected  yes
    final XOR               0xFFFFFFFF
"""

from __future__ import annotations

__all__ = ["CRC32C_POLY_REFLECTED", "CHECK_VALUE", "crc32c", "self_check"]

#: Reflected Castagnoli polynomial.
CRC32C_POLY_REFLECTED = 0x82F63B78

#: Standard check value: ``crc32c(b"123456789")``. Any implementation that
#: fails this is wrong, and every file it has written is unreadable.
CHECK_VALUE = 0xE3069283


def _build_table() -> list[int]:
    """Build the 256-entry byte-at-a-time lookup table."""
    table = []
    for byte in range(256):
        crc = byte
        for _ in range(8):
            # Reflected algorithm: shift right, xor polynomial on low bit set.
            crc = (crc >> 1) ^ (CRC32C_POLY_REFLECTED if crc & 1 else 0)
        table.append(crc)
    return table


_TABLE = _build_table()


def crc32c(data: bytes | bytearray | memoryview, crc: int = 0) -> int:
    """Compute CRC-32C over ``data``.

    Args:
        data: Bytes to checksum.
        crc: Running CRC from a previous call, for streaming. Pass the previous
            return value directly; the pre/post conditioning is handled here.

    Returns:
        The CRC-32C as an unsigned 32-bit integer.
    """
    crc ^= 0xFFFFFFFF  # undo the final XOR of any previous chunk
    for byte in memoryview(data).tobytes():
        crc = _TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def self_check() -> None:
    """Raise :class:`RuntimeError` if this CRC implementation is incorrect.

    Called at import time by the format modules so a broken build fails loudly
    and immediately rather than silently emitting corrupt files.
    """
    actual = crc32c(b"123456789")
    if actual != CHECK_VALUE:
        raise RuntimeError(
            f"CRC-32C self-check FAILED: got 0x{actual:08X}, expected 0x{CHECK_VALUE:08X}. "
            "This implementation is incorrect and must not be used to write files."
        )


self_check()
