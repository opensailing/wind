"""Tests for the capture verdict logic.

These exist because the previous criterion -- `max > 0 and unique > 10` -- could
not fail. A SkyAtmosphere renders a bright, richly dithered gradient using zero
primitives, so an empty frame cleared both thresholds and the tool reported
success on captures that contained no geometry at all.

Every test below that ends in `_is_not_a_pass` is a known-bad input. If one of
them starts passing, the criterion has regressed to something that cannot fail.
"""

import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from verdict import Stats, judge  # noqa: E402


def stats(largest=200, distinct=1672, mean=88.66, checksum=12345):
    """A healthy-looking measurement. Individual fields overridden per test."""
    return Stats(largest=largest, distinct=distinct, mean=mean, checksum=checksum)


# --- The regression that motivated this module -------------------------------


def test_frame_identical_to_the_no_primitive_reference_is_not_a_pass():
    """The exact false positive: bright, dithered, and completely empty.

    Observed on this project as max=182 unique=1672 mean=88.660, byte-identical
    before and after deleting every mesh in the level.
    """
    empty = stats(largest=182, distinct=1672, mean=88.660, checksum=0xABCD)
    verdict = judge(scene=empty, reference=empty, written=True, byte_size=1173227)

    assert not verdict.passed
    assert "no primitive" in verdict.reason.lower()


def test_frame_that_differs_from_the_reference_is_a_pass():
    scene = stats(checksum=0x1111)
    reference = stats(checksum=0x2222)

    verdict = judge(scene=scene, reference=reference, written=True, byte_size=1173227)

    assert verdict.passed, verdict.reason


def test_bright_and_varied_does_not_rescue_an_empty_frame():
    """Brightness and colour count must not be able to override the differential.

    The sky-atmosphere overlay bug ADDS brightness and unique colours, so the
    most contaminated captures score the highest on both.
    """
    empty = stats(largest=255, distinct=60000, mean=200.0, checksum=7)
    verdict = judge(scene=empty, reference=empty, written=True, byte_size=9999999)

    assert not verdict.passed


# --- The floor cases the old criterion did get right -------------------------


def test_pure_black_frame_is_not_a_pass():
    scene = stats(largest=0, distinct=1, mean=0.0, checksum=0)
    reference = stats(largest=0, distinct=1, mean=0.0, checksum=1)

    verdict = judge(scene=scene, reference=reference, written=True, byte_size=5000)

    assert not verdict.passed
    assert "black" in verdict.reason.lower()


def test_unwritten_file_is_not_a_pass():
    verdict = judge(scene=stats(), reference=stats(checksum=99), written=False, byte_size=0)

    assert not verdict.passed
    assert "written" in verdict.reason.lower()


def test_empty_file_on_disk_is_not_a_pass():
    verdict = judge(scene=stats(), reference=stats(checksum=99), written=True, byte_size=0)

    assert not verdict.passed


def test_failed_readback_is_not_a_pass():
    """measure() returns largest=-1 when the render target cannot be read."""
    unreadable = Stats(largest=-1, distinct=0, mean=0.0, checksum=0)

    verdict = judge(scene=unreadable, reference=stats(), written=True, byte_size=1000)

    assert not verdict.passed
    assert "read" in verdict.reason.lower()


# --- Absence of a reference must not silently pass ---------------------------


def test_missing_reference_is_not_a_pass():
    """If the reference capture could not be taken, the shot is unverified.

    Unverified must not report as PASS -- that is how the original bug shipped.
    """
    verdict = judge(scene=stats(), reference=None, written=True, byte_size=1173227)

    assert not verdict.passed
    assert "reference" in verdict.reason.lower()


# --- Reason strings are the operator-facing product of a failure -------------


@pytest.mark.parametrize(
    "scene,reference",
    [
        (stats(largest=0, distinct=1, mean=0.0, checksum=0), stats(checksum=5)),
        (stats(), stats()),
        (stats(), None),
    ],
)
def test_every_failure_explains_itself(scene, reference):
    verdict = judge(scene=scene, reference=reference, written=True, byte_size=1000)

    assert not verdict.passed
    assert verdict.reason
    assert len(verdict.reason) > 20, "a bare 'FAIL' sends the reader to the source"
