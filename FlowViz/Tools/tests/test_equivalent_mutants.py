#!/usr/bin/env python3
"""Which surviving mutants are EQUIVALENT rather than uncovered?

A mutation campaign reports SURVIVED for two very different situations:

  1. The mutant changes behaviour and no test noticed. A real coverage gap.
  2. The mutant cannot change behaviour for any input. Nothing is missing, and
     no test can ever kill it.

Both print the same word. Treating case 2 as case 1 means writing a test that
cannot fail -- the exact defect this repo's memory warns about -- and then
recording it as coverage. So each survivor gets a decision here, in a model of
the predicate that can be swept exhaustively, before anyone writes C++.

This file is the model, not the implementation. It proves things about the
boolean structure of a guard; it says nothing about whether the C++ compiles to
that structure. That is why a claim of equivalence is only accepted when the
sweep covers every input the guard can distinguish, including the negatives and
the overflow edges, and not merely a few plausible values.

For the one equivalence claimed below, the model was additionally confirmed
against real C++ with real int64_t -- both predicates compiled standalone and
swept over the same 4205 (size, offset, count) triples, with zero
disagreements. A model written in a language with arbitrary-precision integers
is exactly the wrong tool for reasoning about fixed-width overflow, which is
why that check exists and why wrap64 below does.

Run: python3 Tools/tests/test_equivalent_mutants.py
"""

import sys

INT64_MAX = 2**63 - 1
INT64_MIN = -(2**63)


def wrap64(value):
    """Two's-complement int64 wrap.

    Python integers are arbitrary precision, so `offset + count` in this model
    would never overflow -- and overflow is the entire subject of the mutant
    this file exists to distinguish. Without this, the model reported the
    overflow-prone formulation as EQUIVALENT to the safe one, which is the
    opposite of the truth and would have retired the test that catches it.

    That wrong answer was produced, and caught, by the controls below. It is
    the reason a mutant known to differ is swept alongside every mutant claimed
    not to: a model that cannot distinguish anything declares everything
    equivalent, and looks exactly like a proof.

    (Signed overflow is UB in C++, not defined to wrap. Wrapping is what the
    hardware does and what the original defect did; a compiler entitled to
    assume it cannot happen may do something else again. Either way the naive
    form is wrong -- this models the concrete failure that was observed.)
    """
    return ((value + 2**63) % 2**64) - 2**63

# The offsets and counts to sweep. Small values around every boundary, plus the
# hostile magnitudes a header can produce after a uint64 -> int64 narrowing.
# The negatives matter most: a guard clause that looks dead for sensible input
# is often the only thing refusing INT64_MIN.
VALUES = (
    list(range(-3, 20))
    + [INT64_MAX, INT64_MAX - 1, INT64_MIN, INT64_MIN + 1, 2**62, -(2**62)]
)

SIZES = [0, 1, 8, 16, 17]


def sweep(reference, mutant):
    """Return the first (size, offset, count) where the two disagree, or None."""
    for size in SIZES:
        for offset in VALUES:
            for count in VALUES:
                if reference(size, offset, count) != mutant(size, offset, count):
                    return (size, offset, count)
    return None


# --- FByteCursor::CanRead ---------------------------------------------------
#
# The committed guard. Count is subtracted from the remaining length rather
# than added to Offset, because Offset + Count is the addition that overflows
# on a hostile 2^63 length.
def can_read(size, offset, count):
    return offset >= 0 and count >= 0 and offset <= size and count <= size - offset


def mutant_offset_one_past(size, offset, count):
    """`Offset <= Size` weakened to `Offset <= Size + 1`.

    Campaign verdict: SURVIVED. The claim under test is that this is
    equivalent -- when offset == size + 1, `size - offset` is -1, so the count
    clause refuses every count >= 0, and counts < 0 were already refused. The
    mutated clause can therefore never be the deciding one.
    """
    return offset >= 0 and count >= 0 and offset <= size + 1 and count <= size - offset


def mutant_naive_addition(size, offset, count):
    """The overflow-prone formulation. Must NOT be equivalent."""
    return (offset >= 0 and count >= 0 and offset <= size
            and wrap64(offset + count) <= size)


def mutant_drop_negative_count(size, offset, count):
    """`Count >= 0` removed. Must NOT be equivalent."""
    return offset >= 0 and offset <= size and count <= size - offset


CASES = [
    # (name, mutant, expected_equivalent)
    ("CanRead: offset one past the end accepted",
     mutant_offset_one_past, True),
    # Controls. A sweep that declared everything equivalent would pass the
    # case above on its own, so two mutants known to differ are swept too.
    ("CanRead: naive addition that overflows",
     mutant_naive_addition, False),
    ("CanRead: negative count accepted",
     mutant_drop_negative_count, False),
]


# --- Playback: FFlowVizTimeline / BuildRequestList --------------------------
#
# Two survivors from the FlowViz.Playback campaign. Both are claimed equivalent
# below, and each claim is swept against a control that must be distinguished.

def try_bracket(times, t):
    """Model of FFlowVizTimeline::TryBracket. Returns (A, B, alpha)."""
    n = len(times)
    if n == 0:
        return None
    last = n - 1
    if t <= times[0]:
        return (0, 0, 0.0)
    if t >= times[last]:
        return (last, last, 0.0)
    lo, hi = 0, last
    while lo + 1 < hi:
        mid = lo + (hi - lo) // 2
        if times[mid] <= t:
            lo = mid
        else:
            hi = mid
    if t == times[lo]:
        return (lo, lo, 0.0)
    if t == times[hi]:
        return (hi, hi, 0.0)
    return (lo, hi, (t - times[lo]) / (times[hi] - times[lo]))


TIMELINES = [
    [0.0, 1.0, 3.0, 6.0, 10.0],   # the test fixture: no two gaps equal
    [0.0, 0.05, 0.10, 0.15],      # the shipped sample: uniform
    [0.0, 1.0],
    [5.0],
    [0.0, 2.0, 2.5],
]


def bracket_probes(times):
    probes = list(times)
    for a, b in zip(times, times[1:]):
        width = b - a
        probes += [(a + b) / 2.0, a + width * 1e-12, b - width * 1e-12,
                   a + width * 0.25, a + width * 0.75]
    probes += [times[0] - 1.0, times[-1] + 1.0, times[0] - 1e-9, times[-1] + 1e-9]
    return probes


def sweep_interpolated(reference, mutant):
    """First time where the two bInterpolated spellings disagree, or None."""
    for times in TIMELINES:
        for t in bracket_probes(times):
            r = try_bracket(times, t)
            if r is None:
                continue
            a, b, alpha = r
            if reference(a, b, alpha) != mutant(a, b, alpha):
                return (times, t, a, b, alpha)
    return None


def interpolated_real(a, b, alpha):
    """bInterpolated is true exactly when alpha is neither 0 nor 1 (VISUAL_QA
    rule 5). TryBracket already collapses an exact landing to (F, F, 0), so the
    alpha bounds cannot be the deciding clause."""
    return a != b and alpha > 0.0 and alpha < 1.0


def interpolated_mutant_drop_bounds(a, b, alpha):
    """Campaign verdict: SURVIVED. Claimed equivalent -- TryBracket never
    returns A != B together with an alpha of exactly 0 or 1, so the dropped
    clauses can never decide the result."""
    return a != b


def interpolated_control_always(a, b, alpha):
    """Control: must NOT be equivalent."""
    return True


def build_request_list(last, a, b, forward, ahead, behind, clamp):
    """Model of FlowVizPlayback::BuildRequestList. `clamp` selects the mutant
    that clamps out-of-range frames instead of dropping them."""
    out = []

    def add(frame):
        if clamp:
            frame = min(max(frame, 0), last)
        elif frame < 0 or frame > last:
            return
        if frame not in out:
            out.append(frame)

    add(a)
    add(b)
    lead = 1 if forward else -1
    lead_anchor = b if forward else a
    trail_anchor = a if forward else b
    for radius in range(1, max(ahead, behind) + 1):
        if radius <= ahead:
            add(lead_anchor + lead * radius)
        if radius <= behind:
            add(trail_anchor - lead * radius)
    return out


def sweep_request_list(reference, mutant):
    """First (last, A, B, forward, ahead, behind) where the lists differ."""
    for last in range(0, 8):
        for a in range(0, last + 1):
            for b in (a, min(a + 1, last)):
                for forward in (True, False):
                    for ahead in range(0, 5):
                        for behind in range(0, 5):
                            args = (last, a, b, forward, ahead, behind)
                            if reference(*args) != mutant(*args):
                                return args
    return None


def request_real(last, a, b, forward, ahead, behind):
    return build_request_list(last, a, b, forward, ahead, behind, clamp=False)


def request_mutant_clamp(last, a, b, forward, ahead, behind):
    """Campaign verdict: SURVIVED. Claimed equivalent -- a clamped frame is
    always 0 or `last`, and the walk only leaves the range after it has already
    added that endpoint, so AddUnique folds the clamped value into the entry
    already present."""
    return build_request_list(last, a, b, forward, ahead, behind, clamp=True)


def request_control_ignore_direction(last, a, b, forward, ahead, behind):
    """Control: `Lead` pinned to +1 regardless of travel. This mutant was
    KILLED by the suite, so the model must distinguish it too."""
    out = []

    def add(frame):
        if frame < 0 or frame > last:
            return
        if frame not in out:
            out.append(frame)

    add(a)
    add(b)
    lead_anchor = b if forward else a
    trail_anchor = a if forward else b
    for radius in range(1, max(ahead, behind) + 1):
        if radius <= ahead:
            add(lead_anchor + 1 * radius)
        if radius <= behind:
            add(trail_anchor - 1 * radius)
    return out


PLAYBACK_CASES = [
    # (name, sweep, reference, mutant, expected_equivalent)
    ("SelectFrames: bInterpolated drops the alpha bounds",
     sweep_interpolated, interpolated_real, interpolated_mutant_drop_bounds, True),
    ("SelectFrames: bInterpolated always true",
     sweep_interpolated, interpolated_real, interpolated_control_always, False),
    ("BuildRequestList: clamps out-of-range instead of dropping",
     sweep_request_list, request_real, request_mutant_clamp, True),
    ("BuildRequestList: preload direction ignored",
     sweep_request_list, request_real, request_control_ignore_direction, False),
]


# --- SetVolumeCompositeMode: the negative guard -----------------------------
#
# One survivor from the FlowViz.Capture campaign. The Blueprint/Python entry
# point takes a SIGNED int32 -- callers are people typing numbers -- and the
# view model's setter takes uint32. The guard refuses negatives before the cast.

UINT32_MODULUS = 2**32
INT32_MAX = 2**31 - 1
INT32_MIN = -(2**31)

# Alpha=0 .. Diagnostic=5. Pinned to the FLOWVIZ_MODE_* defines in the .usf.
MODE_MAX = 5


def to_uint32(value):
    """C++ `static_cast<uint32>` of an int32.

    The same trap wrap64 exists for, one width down, and this time the wrap is
    not a hazard being modelled but the entire mechanism the equivalence rests
    on. Without it -1 stays -1, `-1 <= 5` is true, and the model reports the
    mutant as DISTINGUISHED -- claiming a coverage gap that is not there.

    That is the safe direction to fail in, which is why this is the one
    conversion written out rather than left to Python: a broken wrap here
    manufactures work instead of retiring a test that catches something.

    Measured against real C++ on this toolchain rather than assumed: -1 ->
    4294967295, -2 -> 4294967294, INT32_MIN -> 2147483648, 99 -> 99. (The cast
    is what was measured. The predicates below are the model.)
    """
    return value % UINT32_MODULUS


MODE_VALUES = (
    list(range(-8, 12))
    + [99, INT32_MAX, INT32_MAX - 1, INT32_MIN, INT32_MIN + 1, 2**30, -(2**30)]
)


def sweep_modes(reference, mutant):
    """First int32 where the two spellings disagree, or None."""
    for mode in MODE_VALUES:
        if reference(mode) != mutant(mode):
            return mode
    return None


def set_mode_by_value(unsigned):
    """Model of FFlowVizRenderSettingsViewModel::SetCompositeModeByValue.

    An exhaustive switch over the enumerators; anything else is refused. The
    switch is what makes the claim below hold, so it is modelled rather than
    assumed away.
    """
    return unsigned <= MODE_MAX


def accepts_mode_real(mode):
    """The committed guard: refuse negatives, then let the view model validate."""
    return mode >= 0 and set_mode_by_value(to_uint32(mode))


def mode_mutant_drop_negative_guard(mode):
    """Campaign verdict: SURVIVED. Claimed equivalent -- every negative int32
    casts to a uint32 at or above 2^31, which the exhaustive switch refuses
    anyway, so the dropped clause can never be the deciding one.

    The guard stays in the C++ regardless. Its redundancy is a property of the
    switch being exhaustive, not of the guard itself, and a mode added without
    a case label would make it load-bearing again with nothing to announce that.
    """
    return set_mode_by_value(to_uint32(mode))


def mode_control_signed_compare(mode):
    """Control: the range check written on the SIGNED value, without the cast.

    Must NOT be equivalent -- this is the hazard the guard is mistaken for, and
    the one direction where dropping the guard would matter. If the sweep ever
    calls this equivalent, to_uint32 has stopped wrapping.
    """
    return mode <= MODE_MAX


def mode_control_cast_without_validation(mode):
    """Control: validated by cast alone, so 99 becomes a live mode. This arm was
    KILLED by the suite, so the model must distinguish it too."""
    return True


MODE_CASES = [
    # (name, mutant, expected_equivalent)
    ("SetVolumeCompositeMode: the negative guard dropped",
     mode_mutant_drop_negative_guard, True),
    ("SetVolumeCompositeMode: range checked on the signed value",
     mode_control_signed_compare, False),
    ("SetVolumeCompositeMode: validated by cast alone",
     mode_control_cast_without_validation, False),
]


def main():
    failures = 0
    print("equivalent-mutant analysis (model of FByteCursor::CanRead):")
    for name, mutant, expect_equivalent in CASES:
        witness = sweep(can_read, mutant)
        equivalent = witness is None
        if equivalent == expect_equivalent:
            if equivalent:
                print("  ok    EQUIVALENT   %s" % name)
                print("        no (size, offset, count) in the sweep distinguishes it;")
                print("        a test for this could not fail, so none is written.")
            else:
                size, offset, count = witness
                print("  ok    DISTINGUISHED %s" % name)
                print("        size=%d offset=%d count=%d -> real=%s mutant=%s"
                      % (size, offset, count,
                         can_read(size, offset, count), mutant(size, offset, count)))
        else:
            failures += 1
            print("  FAIL  %s: expected equivalent=%s, got %s (witness %s)"
                  % (name, expect_equivalent, equivalent, witness))

    print()
    print("equivalent-mutant analysis (models of the playback frame selection):")
    for name, sweep_fn, reference, mutant, expect_equivalent in PLAYBACK_CASES:
        witness = sweep_fn(reference, mutant)
        equivalent = witness is None
        if equivalent == expect_equivalent:
            if equivalent:
                print("  ok    EQUIVALENT   %s" % name)
                print("        nothing in the sweep distinguishes it; a test for")
                print("        this could not fail, so none is written.")
            else:
                print("  ok    DISTINGUISHED %s" % name)
                print("        witness: %s" % (witness,))
        else:
            failures += 1
            print("  FAIL  %s: expected equivalent=%s, got %s (witness %s)"
                  % (name, expect_equivalent, equivalent, witness))

    print()
    print("equivalent-mutant analysis (model of the composite-mode guard):")
    for name, mutant, expect_equivalent in MODE_CASES:
        witness = sweep_modes(accepts_mode_real, mutant)
        equivalent = witness is None
        if equivalent == expect_equivalent:
            if equivalent:
                print("  ok    EQUIVALENT   %s" % name)
                print("        no int32 in the sweep distinguishes it; a test for")
                print("        this could not fail, so none is written.")
            else:
                print("  ok    DISTINGUISHED %s" % name)
                print("        mode=%d -> real=%s mutant=%s"
                      % (witness, accepts_mode_real(witness), mutant(witness)))
        else:
            failures += 1
            print("  FAIL  %s: expected equivalent=%s, got %s (witness %s)"
                  % (name, expect_equivalent, equivalent, witness))

    total = len(CASES) + len(PLAYBACK_CASES) + len(MODE_CASES)
    print()
    print("equivalence tests: %d passed, %d failed" % (total - failures, failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
