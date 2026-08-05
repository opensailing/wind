"""Tests for the blind-comparison harness.

Most of these are attacks. A blind test is only blind if the critic cannot
recover the identities, and the ways it stops being blind are mundane: the
candidate is always shown first, the filename says "flowviz", the two images
are different sizes so the bigger one is obviously the film reference, or the
answer key is sitting in the directory being reviewed.

Each of those has burned someone, so each gets a test that fails if the
protection is removed.
"""

import json
import os
import struct
import sys
import zlib

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from blind import (  # noqa: E402
    LEAK_DIMENSIONS,
    LEAK_NAME,
    LEAK_RECORD_VISIBLE,
    LEAK_TEXT_CHUNK,
    audit,
    parse_report,
    reveal,
    seal,
    should_continue,
)


def write_png(path, width, height, rgb=(10, 20, 30), text=None):
    """Write a real, minimal PNG. Small enough to be cheap, real enough that
    the dimension reader is parsing an actual IHDR rather than a stub."""
    raw = b""
    for _ in range(height):
        raw += b"\x00" + bytes(rgb) * width

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    if text is not None:
        png += chunk(b"tEXt", text)
    png += chunk(b"IDAT", zlib.compress(raw))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as handle:
        handle.write(png)
    return path


@pytest.fixture
def pair(tmp_path):
    cand = write_png(str(tmp_path / "flowviz_render.png"), 8, 6, (200, 40, 40))
    ref = write_png(str(tmp_path / "avatar_water_still.png"), 8, 6, (40, 40, 200))
    return cand, ref


# --------------------------------------------------------------------------
# The blind mechanism
# --------------------------------------------------------------------------


def test_presented_files_are_named_only_a_and_b(pair, tmp_path):
    """The critic sees the filenames. If the candidate keeps its own name the
    test is not blind, no matter how the images are shuffled."""
    cand, ref = pair
    sealed = seal(cand, ref, str(tmp_path / "shown"), str(tmp_path / "key.json"), seed=1)

    for path in (sealed.a_path, sealed.b_path):
        name = os.path.basename(path).lower()
        assert "flowviz" not in name
        assert "avatar" not in name
        assert "render" not in name
        assert "cand" not in name and "ref" not in name

    assert os.path.basename(sealed.a_path).endswith("A.png")
    assert os.path.basename(sealed.b_path).endswith("B.png")


def test_both_orderings_actually_occur(pair, tmp_path):
    """The single most likely way this breaks: the shuffle is a no-op and the
    candidate is always A. A critic who notices that once knows every answer
    thereafter. This is the test that catches a stubbed-out shuffle."""
    cand, ref = pair
    seen = set()
    for seed in range(40):
        sealed = seal(
            cand,
            ref,
            str(tmp_path / ("shown%d" % seed)),
            str(tmp_path / ("key%d.json" % seed)),
            seed=seed,
        )
        seen.add(sealed.assignment["A"])

    assert seen == {"candidate", "reference"}, (
        "the candidate landed on the same side for all 40 seeds, so the "
        "pairing is not shuffled: %r" % seen
    )


def test_the_same_seed_reproduces_the_same_assignment(pair, tmp_path):
    cand, ref = pair
    first = seal(cand, ref, str(tmp_path / "s1"), str(tmp_path / "k1.json"), seed=7)
    second = seal(cand, ref, str(tmp_path / "s2"), str(tmp_path / "k2.json"), seed=7)
    assert first.assignment == second.assignment


def test_reveal_maps_the_choice_back(pair, tmp_path):
    cand, ref = pair
    sealed = seal(cand, ref, str(tmp_path / "shown"), str(tmp_path / "key.json"), seed=3)

    assert reveal(sealed.record_path, "A") == sealed.assignment["A"]
    assert reveal(sealed.record_path, "B") == sealed.assignment["B"]
    assert {reveal(sealed.record_path, "A"), reveal(sealed.record_path, "B")} == {
        "candidate",
        "reference",
    }


def test_reveal_rejects_a_choice_that_is_not_a_or_b(pair, tmp_path):
    cand, ref = pair
    sealed = seal(cand, ref, str(tmp_path / "shown"), str(tmp_path / "key.json"), seed=3)
    with pytest.raises(ValueError):
        reveal(sealed.record_path, "the left one")


def test_the_answer_key_is_not_written_into_the_reviewed_directory(pair, tmp_path):
    """An answer key inside the directory the critic is pointed at is not a
    blind test; it is an open-book one. The critic lists the directory."""
    cand, ref = pair
    shown = str(tmp_path / "shown")
    sealed = seal(cand, ref, shown, str(tmp_path / "key.json"), seed=5)

    listed = os.listdir(shown)
    assert sorted(listed) == sorted(
        [os.path.basename(sealed.a_path), os.path.basename(sealed.b_path)]
    ), "the reviewed directory contains something other than the two images: %r" % listed


def test_seal_refuses_to_put_the_key_inside_the_reviewed_directory(pair, tmp_path):
    cand, ref = pair
    shown = str(tmp_path / "shown")
    with pytest.raises(ValueError):
        seal(cand, ref, shown, os.path.join(shown, "key.json"), seed=5)


# --------------------------------------------------------------------------
# Leak auditing
# --------------------------------------------------------------------------


def test_mismatched_resolution_is_reported_as_a_leak(tmp_path):
    """VISUAL_QA.md section 4: a resolution mismatch invalidates a blind
    comparison. It is also a dead giveaway -- the 3840-wide one is the film
    still. This must be caught before a critic is ever asked."""
    cand = write_png(str(tmp_path / "c.png"), 8, 6)
    ref = write_png(str(tmp_path / "r.png"), 16, 12)
    sealed = seal(cand, ref, str(tmp_path / "shown"), str(tmp_path / "k.json"), seed=1)

    leaks = audit(sealed)
    assert LEAK_DIMENSIONS in [leak.kind for leak in leaks]
    assert any("8x6" in leak.detail and "16x12" in leak.detail for leak in leaks)


def test_matched_resolution_is_clean(pair, tmp_path):
    cand, ref = pair
    sealed = seal(cand, ref, str(tmp_path / "shown"), str(tmp_path / "k.json"), seed=1)
    assert audit(sealed) == []


def test_a_png_text_chunk_naming_the_source_is_a_leak(tmp_path):
    """Renderers love to stamp their name into PNG metadata. `Software:
    Unreal Engine` in one image and `Adobe` in the other ends the blind."""
    cand = write_png(str(tmp_path / "c.png"), 8, 6, text=b"Software\x00Unreal Engine 5")
    ref = write_png(str(tmp_path / "r.png"), 8, 6)
    sealed = seal(cand, ref, str(tmp_path / "shown"), str(tmp_path / "k.json"), seed=1)

    leaks = audit(sealed)
    assert LEAK_TEXT_CHUNK in [leak.kind for leak in leaks]


def test_an_identifying_filename_is_a_leak(pair, tmp_path):
    """Guards the guard: if presented names ever stop being A/B, audit must
    say so rather than silently approving a non-blind pair."""
    cand, ref = pair
    sealed = seal(cand, ref, str(tmp_path / "shown"), str(tmp_path / "k.json"), seed=1)

    leaked = os.path.join(os.path.dirname(sealed.a_path), "flowviz_A.png")
    os.rename(sealed.a_path, leaked)
    sealed.a_path = leaked

    leaks = audit(sealed)
    assert LEAK_NAME in [leak.kind for leak in leaks]


def test_a_key_that_appears_beside_the_images_is_a_leak(pair, tmp_path):
    cand, ref = pair
    shown = str(tmp_path / "shown")
    sealed = seal(cand, ref, shown, str(tmp_path / "k.json"), seed=1)

    with open(os.path.join(shown, "key.json"), "w") as handle:
        json.dump(sealed.assignment, handle)

    leaks = audit(sealed)
    assert LEAK_RECORD_VISIBLE in [leak.kind for leak in leaks]


# --------------------------------------------------------------------------
# Verdict parsing and the loop rule
# --------------------------------------------------------------------------


GOOD_REPORT = """
PROFILE:  Presentation
VERDICT:  PASS
BLIND:    B read as the film reference at first, but A's edge detail held up
          at 200% zoom and I could not call it.

HARD FAILURES
  - none

QUALITY GAPS
  - A's shadow falloff on the lower plume is slightly denser than B's.

WHAT WORKS
  - Multiple-scattering falloff on the plume's lit side is convincing.
  - Wispy silhouette detail survives at the pixel scale.
"""


def test_a_well_formed_report_parses(tmp_path):
    report = parse_report(GOOD_REPORT)
    assert report.verdict == "PASS"
    assert report.profile == "Presentation"
    assert "edge detail" in report.blind
    assert len(report.what_works) == 2


def test_an_unknown_verdict_is_refused():
    with pytest.raises(ValueError):
        parse_report(GOOD_REPORT.replace("VERDICT:  PASS", "VERDICT:  LOOKS GOOD"))


def test_a_missing_verdict_is_refused():
    with pytest.raises(ValueError):
        parse_report(GOOD_REPORT.replace("VERDICT:  PASS", ""))


def test_a_pass_with_no_specific_praise_is_refused():
    """VISUAL_QA.md section 3: "Looks good" is not a passing verdict; a pass
    requires stating specifically what makes it good. A PASS whose WHAT WORKS
    is empty or a shrug is the exact self-deception the protocol exists to
    stop, so it is rejected at parse time rather than believed."""
    vague = GOOD_REPORT.replace(
        "  - Multiple-scattering falloff on the plume's lit side is convincing.\n"
        "  - Wispy silhouette detail survives at the pixel scale.\n",
        "  - Looks good.\n",
    )
    with pytest.raises(ValueError):
        parse_report(vague)


def test_a_reject_needs_no_praise():
    """The symmetric case must still parse -- a critic rejecting a render is
    not required to compliment it, and a parser that demanded praise would
    make REJECT unreportable."""
    report = parse_report(
        GOOD_REPORT.replace("VERDICT:  PASS", "VERDICT:  REJECT").replace(
            "  - Multiple-scattering falloff on the plume's lit side is convincing.\n"
            "  - Wispy silhouette detail survives at the pixel scale.\n",
            "  - none\n",
        )
    )
    assert report.verdict == "REJECT"


def test_the_loop_continues_on_weak():
    """WEAK is explicitly not a passing grade. It is called out in the
    protocol as the most common self-deception in this work, so the loop
    must not treat "no hard failures" as done."""
    assert should_continue("REJECT") is True
    assert should_continue("WEAK") is True
    assert should_continue("PASS") is False
    assert should_continue("EXCEEDS") is False
