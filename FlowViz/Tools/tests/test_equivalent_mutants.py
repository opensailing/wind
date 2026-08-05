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

    total = len(CASES)
    print()
    print("equivalence tests: %d passed, %d failed" % (total - failures, failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
