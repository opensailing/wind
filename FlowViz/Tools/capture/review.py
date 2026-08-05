"""Runs one iteration of the visual review loop.

Ties together the three pieces that already exist: `capture_frame.py` decides
whether a frame contains anything, `blind.py` presents it against a reference
without revealing which is which, and `Docs/VISUAL_QA.md` says what a verdict
means. This module is the part that decides whether an iteration is DONE.

That makes its failure mode specific: not crashing, but reporting PASS when it
should not have. Everything here is shaped around refusing to do that.

WHAT IT REFUSES TO REVIEW

  * A capture that failed verification. The capture tool already answered
    "did anything render?"; grading a frame it rejected means grading empty
    sky, and a critic will write confident paragraphs about empty sky.
  * An empty reference corpus. A comparison with nothing to compare against
    is not a win. This matters on a clean checkout, where `references/` is
    empty by design because the stills are third-party.
  * A pairing that `audit()` says leaks the identities. A leaked verdict is
    not a weaker signal -- it is worthless, and afterwards it is
    indistinguishable from a real one.

WHY EXHAUSTION IS AN ERROR AND NOT A VERDICT

Running out of iterations means the loop TIMED OUT. Returning the last
verdict as the outcome quietly converts "we ran out of patience" into "this
is the result", which is the same class of mistake as treating WEAK as a
pass. So `iterate()` raises once the budget is spent rather than returning.
"""

import json
import os

from blind import audit, parse_report, seal, should_continue


class ReviewError(Exception):
    """The iteration could not produce a trustworthy verdict."""


class Session(object):
    """State carried across iterations of one review loop."""

    __slots__ = ("profile", "reference_dir", "work_dir", "max_iterations",
                 "iteration")

    def __init__(self, profile, reference_dir, work_dir, max_iterations=10):
        self.profile = profile
        self.reference_dir = os.path.abspath(reference_dir)
        self.work_dir = os.path.abspath(work_dir)
        self.max_iterations = max_iterations
        self.iteration = 0

    def references(self):
        """Every reference image available, sorted for determinism."""
        if not os.path.isdir(self.reference_dir):
            return []
        found = []
        for root, _dirs, files in os.walk(self.reference_dir):
            for name in sorted(files):
                if name.lower().endswith((".png", ".jpg", ".jpeg", ".exr")):
                    found.append(os.path.join(root, name))
        return sorted(found)


class Prompt(object):
    """What the critic is given. Deliberately narrow.

    It carries the directory and the profile and NOTHING that identifies the
    images -- no candidate path, no assignment. A critic implementation that
    can read the answer off an attribute is not blind, and the temptation to
    add "just for logging" is exactly how that happens.
    """

    __slots__ = ("shown_dir", "profile", "iteration")

    def __init__(self, shown_dir, profile, iteration):
        self.shown_dir = shown_dir
        self.profile = profile
        self.iteration = iteration


class Outcome(object):
    """The result of one iteration."""

    __slots__ = ("verdict", "done", "profile", "iteration", "hard_failures",
                 "quality_gaps", "what_works", "blind", "shown_dir",
                 "record_path", "reference_source", "preferred")

    def __init__(self, **kwargs):
        for name in self.__slots__:
            setattr(self, name, kwargs.get(name))

    def __repr__(self):
        return "Outcome(iteration=%s, %s, done=%s)" % (
            self.iteration,
            self.verdict,
            self.done,
        )


def iterate(session, candidate_png, capture_passed, capture_reason, critic,
            seed=None):
    """Run one blind review of `candidate_png`. Returns an Outcome.

    `critic` is called with a Prompt and must return a report in the format of
    `VISUAL_QA.md` section 3. Raises ReviewError rather than returning a
    verdict whenever the comparison cannot be trusted.
    """
    if session.iteration >= session.max_iterations:
        raise ReviewError(
            "the iteration budget of %d is exhausted; this is a TIMEOUT, not a "
            "verdict. The last graded result is not the outcome -- the loop "
            "never reached a passing grade." % session.max_iterations
        )

    if not capture_passed:
        raise ReviewError(
            "refusing to review a capture that failed verification (%s). The "
            "capture tool already determined this frame contains no rendered "
            "primitives; a critic asked about it would grade empty sky."
            % (capture_reason or "no reason given")
        )

    references = session.references()
    if not references:
        raise ReviewError(
            "no reference images in %s, so there is nothing to compare "
            "against. An unopposed render is not a passing render -- see "
            "Tools/capture/references/README.md for what belongs there and "
            "why it is not committed." % session.reference_dir
        )

    session.iteration += 1
    iteration = session.iteration

    # Rotate references so the loop cannot overfit to a single image.
    reference = references[(iteration - 1) % len(references)]

    shown_dir = os.path.join(session.work_dir, "iteration_%03d" % iteration)
    record_path = os.path.join(session.work_dir, "key_%03d.json" % iteration)
    if not os.path.isdir(session.work_dir):
        os.makedirs(session.work_dir)

    sealed = seal(candidate_png, reference, shown_dir, record_path, seed=seed)

    leaks = audit(sealed)
    if leaks:
        raise ReviewError(
            "the blind comparison leaks the identities, so any verdict from it "
            "would be worthless:\n  - %s"
            % "\n  - ".join("%s: %s" % (leak.kind, leak.detail) for leak in leaks)
        )

    report_text = critic(Prompt(shown_dir, session.profile, iteration))
    report = parse_report(report_text)

    preferred = None
    for token in ("A", "B"):
        if report.blind.strip().upper().startswith(token):
            preferred = sealed.assignment[token]
            break

    with open(record_path, "w") as handle:
        json.dump(
            {
                "iteration": iteration,
                "profile": report.profile or session.profile,
                "verdict": report.verdict,
                "blind": report.blind,
                "hard_failures": report.hard_failures,
                "quality_gaps": report.quality_gaps,
                "what_works": report.what_works,
                "candidate_source": os.path.abspath(candidate_png),
                "reference_source": os.path.abspath(reference),
                "assignment": sealed.assignment,
                "preferred": preferred,
                "shown_dir": shown_dir,
            },
            handle,
            indent=2,
        )

    return Outcome(
        verdict=report.verdict,
        done=not should_continue(report.verdict),
        profile=report.profile or session.profile,
        iteration=iteration,
        hard_failures=report.hard_failures,
        quality_gaps=report.quality_gaps,
        what_works=report.what_works,
        blind=report.blind,
        shown_dir=shown_dir,
        record_path=record_path,
        reference_source=os.path.abspath(reference),
        preferred=preferred,
    )
