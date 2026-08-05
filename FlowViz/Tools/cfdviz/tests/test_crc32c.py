"""CRC-32C conformance — format spec section 8.

The check value is mandated by the spec and asserted identically on the Unreal
side (``CFDVizCrc32CTest.cpp``). A CRC that fails it is wrong, and every file it
has written is unreadable by a correct implementation.

The tests that matter here are the *discriminating* ones: several wrong CRC
variants (zlib's CRC-32, an unreflected Castagnoli, a missing final XOR) all
produce plausible-looking 32-bit numbers, so this module asserts against the
values those variants would produce and requires that ours is not one of them.
"""

from __future__ import annotations

import zlib
from pathlib import Path

import pytest

from cfdviz.crc32c import CHECK_VALUE, CRC32C_POLY_REFLECTED, crc32c, self_check


def test_check_value_from_the_spec():
    """The one assertion section 8 makes mandatory."""
    assert crc32c(b"123456789") == 0xE3069283
    assert CHECK_VALUE == 0xE3069283


def test_reflected_polynomial_is_castagnoli():
    assert CRC32C_POLY_REFLECTED == 0x82F63B78


def test_self_check_passes():
    self_check()  # raises RuntimeError if the implementation is broken


def test_empty_input_is_zero():
    """CRC-32C of nothing is 0: init 0xFFFFFFFF xor final 0xFFFFFFFF."""
    assert crc32c(b"") == 0x00000000


def test_is_not_zlib_crc32():
    """Guards the single most likely mix-up: CRC-32 (0x04C11DB7) vs CRC-32C.

    Spec section 8 calls this out by name. If someone swaps in ``zlib.crc32``,
    every other CRC test in this suite still passes — writer and reader would
    agree — and only this assertion catches it.
    """
    assert crc32c(b"123456789") != zlib.crc32(b"123456789")
    assert zlib.crc32(b"123456789") == 0xCBF43926  # the value we must NOT produce


@pytest.mark.parametrize(
    "wrong_value, description",
    [
        (0xCBF43926, "zlib CRC-32 (polynomial 0x04C11DB7)"),
        (0x1CF96D7C, "unreflected CRC-32C"),
        (0x00000000, "no checksum at all"),
        (~0xE3069283 & 0xFFFFFFFF, "CRC-32C with the final XOR omitted"),
    ],
)
def test_is_not_a_known_wrong_variant(wrong_value, description):
    assert crc32c(b"123456789") != wrong_value, f"produced the {description} value"


def test_streaming_matches_one_shot():
    """A chunked CRC must equal the one-shot CRC, or large files diverge."""
    data = bytes(range(256)) * 13
    one_shot = crc32c(data)

    running = 0
    for start in range(0, len(data), 97):
        running = crc32c(data[start : start + 97], running)
    assert running == one_shot


def test_streaming_seed_of_zero_is_the_identity():
    """Seeding with 0 must mean "nothing hashed yet"."""
    assert crc32c(b"abc", 0) == crc32c(b"abc")


def test_single_bit_flip_changes_the_crc():
    """The check must actually depend on every input bit."""
    base = bytearray(b"CFDViz brick payload, 64 bytes of nothing in particular.")
    reference = crc32c(bytes(base))
    for index in range(len(base)):
        for bit in (0, 3, 7):
            mutated = bytearray(base)
            mutated[index] ^= 1 << bit
            assert crc32c(bytes(mutated)) != reference


def test_accepts_memoryview_and_bytearray():
    """Readers pass slices of an mmap'd file, not always bytes."""
    data = b"123456789"
    assert crc32c(bytearray(data)) == CHECK_VALUE
    assert crc32c(memoryview(data)) == CHECK_VALUE


def test_result_is_an_unsigned_32_bit_int():
    """A negative or wide value would be packed wrong by ``struct.pack('<I')``."""
    for payload in (b"", b"\xff" * 3, b"123456789", bytes(range(256))):
        value = crc32c(payload)
        assert isinstance(value, int)
        assert 0 <= value <= 0xFFFFFFFF


def test_self_check_passes_on_the_shipped_implementation():
    """``self_check`` runs at import; calling it again must stay silent."""
    self_check()


def test_self_check_raises_when_the_crc_is_wrong(monkeypatch):
    """The guard that makes a broken CRC un-ignorable.

    Every format module calls ``self_check()`` at import, so a miscompiled or
    mis-edited CRC aborts the import rather than quietly writing files no other
    implementation can read. That is only true if the check can actually fail —
    which is what this asserts, by making ``crc32c`` return zlib's value.
    """
    import importlib

    module = importlib.import_module("cfdviz.crc32c")
    monkeypatch.setattr(module, "crc32c", lambda *a, **k: 0xCBF43926)
    with pytest.raises(RuntimeError, match=r"self-check FAILED.*0xCBF43926"):
        module.self_check()


def test_a_broken_crc_aborts_the_import_of_the_format_modules():
    """Spec 2: a wrong CRC must not be usable to write files.

    Imported in a subprocess with a mutated source tree, because the guard fires
    at import time and cannot be observed from inside an already-imported
    package. This is the check that ties the loud failure to the modules that
    actually depend on it.
    """
    import shutil
    import subprocess
    import sys
    import tempfile

    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory() as work:
        staged = Path(work) / "cfdviz"
        shutil.copytree(root / "src" / "cfdviz", staged)
        source = staged / "crc32c.py"
        source.write_text(
            source.read_text(encoding="utf-8").replace(
                "CRC32C_POLY_REFLECTED = 0x82F63B78",
                "CRC32C_POLY_REFLECTED = 0xEDB88320",
                1,
            ),
            encoding="utf-8",
        )
        result = subprocess.run(
            [sys.executable, "-B", "-c", "import cfdviz.cvf"],
            capture_output=True, text=True,
            env={"PYTHONPATH": work, "PATH": "/usr/bin:/bin",
                 "PYTHONDONTWRITEBYTECODE": "1"},
        )
    assert result.returncode != 0, "a wrong polynomial imported cleanly"
    assert "self-check FAILED" in result.stderr
    assert "0xE3069283" in result.stderr, "the error must name the expected value"
