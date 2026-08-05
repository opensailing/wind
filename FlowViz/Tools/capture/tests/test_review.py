"""Tests for the review session runner.

The runner is the thing that decides whether a /loop iteration is done, so
the failure that matters is not "it crashed" -- it is "it reported PASS when
it should not have". Every test here is built around that.

The specific ways a review loop lies to itself, each of which gets a test:

  * It reviews a capture that failed verification, and grades empty sky.
  * It reviews against an empty reference corpus and calls the absence of a
    competitor a win.
  * It accepts a verdict from a comparison that leaked the identities.
  * It counts WEAK as done.
  * It stops after N iterations and reports the last verdict as the outcome
    rather than as a timeout.
"""

import json
import os
import struct
import sys
import zlib

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from review import (  # noqa: E402
    ReviewError,
    Session,
    iterate,
)


def write_png(path, width=8, height=6, rgb=(10, 20, 30)):
    raw = b""
    for _ in range(height):
        raw += b"\x00" + bytes(rgb) * width

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as handle:
        handle.write(png)
    return path


PASS_REPORT = """
PROFILE:  Presentation
VERDICT:  PASS
BLIND:    I could not separate them; A's edge detail matched B's.

HARD FAILURES
  - none

QUALITY GAPS
  - A's shadow falloff is marginally denser.

WHAT WORKS
  - Multiple-scattering falloff on the lit side is convincing.
"""

REJECT_REPORT = """
PROFILE:  Presentation
VERDICT:  REJECT
BLIND:    B is obviously the film frame; A has visible step banding.

HARD FAILURES
  - Step banding across the plume, roughly 8px period, in A.

QUALITY GAPS
  - A's silhouette is soft and blobby.

WHAT WORKS
  - none
"""

WEAK_REPORT = REJECT_REPORT.replace("VERDICT:  REJECT", "VERDICT:  WEAK").replace(
    "  - Step banding across the plume, roughly 8px period, in A.\n", "  - none\n"
)


@pytest.fixture
def session(tmp_path):
    refs = tmp_path / "refs"
    refs.mkdir()
    write_png(str(refs / "film_still.png"))
    return Session(
        profile="Presentation",
        reference_dir=str(refs),
        work_dir=str(tmp_path / "work"),
        max_iterations=5,
    )


# --------------------------------------------------------------------------
# Refusing to review something that is not reviewable
# --------------------------------------------------------------------------


def test_a_failed_capture_is_never_reviewed(session, tmp_path):
    """The capture tool already decides whether a frame contains anything.
    If it said no, grading that frame grades empty sky -- and the critic will
    happily write paragraphs about it."""
    shot = write_png(str(tmp_path / "shot.png"))

    def critic(_):
        raise AssertionError("the critic must not be asked about a failed capture")

    with pytest.raises(ReviewError) as excinfo:
        iterate(session, shot, capture_passed=False,
                capture_reason="no primitives rendered", critic=critic)

    assert "capture" in str(excinfo.value).lower()
    assert "no primitives rendered" in str(excinfo.value)


def test_an_empty_reference_corpus_is_refused(tmp_path):
    """A comparison with nothing to compare against is not a win. Without
    this the loop 'passes' on a clean checkout, where references/ is empty by
    design because the stills are third-party."""
    empty = tmp_path / "refs"
    empty.mkdir()
    session = Session(
        profile="Presentation",
        reference_dir=str(empty),
        work_dir=str(tmp_path / "work"),
        max_iterations=5,
    )
    shot = write_png(str(tmp_path / "shot.png"))

    def critic(_):
        raise AssertionError("the critic must not be asked without a reference")

    with pytest.raises(ReviewError) as excinfo:
        iterate(session, shot, capture_passed=True, capture_reason="ok", critic=critic)

    assert "reference" in str(excinfo.value).lower()


def test_a_leaked_comparison_is_refused(tmp_path):
    """A resolution mismatch identifies the film still outright. audit()
    reports it; the runner must act on that rather than log it and continue,
    because a leaked verdict is indistinguishable from a real one afterwards."""
    refs = tmp_path / "refs"
    refs.mkdir()
    write_png(str(refs / "film.png"), width=64, height=48)  # different size
    session = Session(
        profile="Presentation",
        reference_dir=str(refs),
        work_dir=str(tmp_path / "work"),
        max_iterations=5,
    )
    shot = write_png(str(tmp_path / "shot.png"), width=8, height=6)

    def critic(_):
        raise AssertionError("the critic must not be asked about a leaked pair")

    with pytest.raises(ReviewError) as excinfo:
        iterate(session, shot, capture_passed=True, capture_reason="ok", critic=critic)

    message = str(excinfo.value).lower()
    assert "leak" in message or "blind" in message


# --------------------------------------------------------------------------
# The critic sees a blinded pair and nothing else
# --------------------------------------------------------------------------


def test_the_critic_is_handed_only_the_two_blinded_images(session, tmp_path):
    shot = write_png(str(tmp_path / "shot.png"))
    seen = {}

    def critic(prompt):
        seen["dir"] = prompt.shown_dir
        seen["listing"] = sorted(os.listdir(prompt.shown_dir))
        seen["profile"] = prompt.profile
        return PASS_REPORT

    iterate(session, shot, capture_passed=True, capture_reason="ok", critic=critic)

    assert seen["listing"] == ["A.png", "B.png"]
    assert seen["profile"] == "Presentation"


def test_the_critic_is_not_told_which_is_which(session, tmp_path):
    """Guards the whole point. The prompt object must not carry the answer,
    or a critic implementation reads it straight off the attribute."""
    shot = write_png(str(tmp_path / "shot.png"))
    captured = {}

    def critic(prompt):
        captured["fields"] = {
            name: getattr(prompt, name) for name in dir(prompt)
            if not name.startswith("_")
        }
        return PASS_REPORT

    iterate(session, shot, capture_passed=True, capture_reason="ok", critic=critic)

    blob = json.dumps(captured["fields"], default=str).lower()
    assert "candidate" not in blob
    assert "assignment" not in blob
    assert "shot.png" not in blob


# --------------------------------------------------------------------------
# The loop rule
# --------------------------------------------------------------------------


def test_a_pass_ends_the_loop(session, tmp_path):
    shot = write_png(str(tmp_path / "shot.png"))
    result = iterate(session, shot, capture_passed=True, capture_reason="ok",
                     critic=lambda _: PASS_REPORT)
    assert result.verdict == "PASS"
    assert result.done is True


def test_weak_does_not_end_the_loop(session, tmp_path):
    """WEAK is the self-deception the protocol names explicitly. A runner
    that treats "no hard failures" as done is exactly the bug."""
    shot = write_png(str(tmp_path / "shot.png"))
    result = iterate(session, shot, capture_passed=True, capture_reason="ok",
                     critic=lambda _: WEAK_REPORT)
    assert result.verdict == "WEAK"
    assert result.done is False


def test_a_reject_does_not_end_the_loop(session, tmp_path):
    shot = write_png(str(tmp_path / "shot.png"))
    result = iterate(session, shot, capture_passed=True, capture_reason="ok",
                     critic=lambda _: REJECT_REPORT)
    assert result.verdict == "REJECT"
    assert result.done is False
    assert result.hard_failures


def test_exhausting_the_iteration_budget_is_not_a_pass(tmp_path):
    """A loop that stops after N tries has TIMED OUT. Reporting the last
    verdict as the outcome converts "we ran out of patience" into "done"."""
    refs = tmp_path / "refs"
    refs.mkdir()
    write_png(str(refs / "film.png"))
    session = Session(
        profile="Presentation",
        reference_dir=str(refs),
        work_dir=str(tmp_path / "work"),
        max_iterations=2,
    )
    shot = write_png(str(tmp_path / "shot.png"))

    for _ in range(2):
        result = iterate(session, shot, capture_passed=True, capture_reason="ok",
                         critic=lambda _: WEAK_REPORT)
        assert result.done is False

    with pytest.raises(ReviewError) as excinfo:
        iterate(session, shot, capture_passed=True, capture_reason="ok",
                critic=lambda _: WEAK_REPORT)

    message = str(excinfo.value).lower()
    assert "exhaust" in message or "budget" in message or "iteration" in message
    assert "pass" not in message.split("not a")[0][:20]


# --------------------------------------------------------------------------
# The audit trail
# --------------------------------------------------------------------------


def test_each_iteration_is_recorded_with_its_reference(session, tmp_path):
    """A verdict you cannot attribute to a specific reference image is not
    reproducible, and the protocol requires findings be reproducible."""
    shot = write_png(str(tmp_path / "shot.png"))
    result = iterate(session, shot, capture_passed=True, capture_reason="ok",
                     critic=lambda _: REJECT_REPORT)

    with open(result.record_path) as handle:
        record = json.load(handle)

    assert record["profile"] == "Presentation"
    assert record["verdict"] == "REJECT"
    assert record["candidate_source"].endswith("shot.png")
    assert record["reference_source"].endswith("film_still.png")
    assert record["iteration"] == 1
    assert "assignment" in record


def test_the_answer_key_is_not_written_where_the_critic_looks(session, tmp_path):
    shot = write_png(str(tmp_path / "shot.png"))
    result = iterate(session, shot, capture_passed=True, capture_reason="ok",
                     critic=lambda _: PASS_REPORT)

    record_dir = os.path.dirname(os.path.abspath(result.record_path))
    shown_dir = os.path.abspath(result.shown_dir)
    assert record_dir != shown_dir
    assert not result.record_path.startswith(shown_dir + os.sep)


def test_references_rotate_across_iterations(tmp_path):
    """Grading against one image repeatedly overfits to that image. With
    several available the loop must use more than the first."""
    refs = tmp_path / "refs"
    refs.mkdir()
    for name in ("a_film.png", "b_film.png", "c_film.png"):
        write_png(str(refs / name))
    session = Session(
        profile="Presentation",
        reference_dir=str(refs),
        work_dir=str(tmp_path / "work"),
        max_iterations=10,
    )
    shot = write_png(str(tmp_path / "shot.png"))

    used = set()
    for _ in range(3):
        result = iterate(session, shot, capture_passed=True, capture_reason="ok",
                         critic=lambda _: REJECT_REPORT)
        used.add(os.path.basename(result.reference_source))

    assert len(used) > 1, (
        "every iteration used the same reference %r, so the loop overfits to "
        "one image" % used
    )
