"""Decides whether a capture actually rendered anything.

Imported by _capture_worker.py inside Unreal and by the tests outside it, so it
must not import `unreal` -- keep this module pure.

THE RULE
--------
A capture is a pass only if it DIFFERS from the same frame rendered with all
primitives suppressed. Not if it is bright. Not if it is colourful.

Brightness thresholds cannot work here, and the reason is structural rather than
a matter of tuning. A SkyAtmosphere renders a full-screen dithered gradient
using no primitives whatsoever, so a frame containing zero geometry scores
max=182, unique=1672, mean=88.660 -- comfortably above any threshold anyone
would set. This project shipped `max > 0 and unique > 10` and it certified
captures that were byte-identical to the same level with every mesh deleted.

Worse, the failure modes push the metrics the WRONG way. The
r.SkyAtmosphere.EditorNotifications overlay composites HUD text into scene
colour; that text adds brightness and unique colours, so the most contaminated
capture available scored max=200 unique=3713 while a clean one scored max=148
unique=649. A threshold ranks the broken frame higher than the good one.

So the criterion is differential. The caller renders the shot twice -- once
normally, once with PrimitiveRenderMode suppressing all primitives -- and passes
both here. Identical results mean no primitive contributed a single pixel, which
is precisely the question "did anything render?" and is not answerable from one
observation.

The brightness checks that remain below are floor cases only. They can reject,
never accept.
"""


class Stats(object):
    """One measurement of a rendered frame.

    `checksum` is order-dependent over all pixels: two frames with identical
    histograms but different layouts must not compare equal, or moving an object
    without changing its colours would read as "nothing rendered".
    """

    __slots__ = ("largest", "distinct", "mean", "checksum")

    def __init__(self, largest, distinct, mean, checksum):
        self.largest = largest
        self.distinct = distinct
        self.mean = mean
        self.checksum = checksum

    def __eq__(self, other):
        if not isinstance(other, Stats):
            return NotImplemented
        return (
            self.largest == other.largest
            and self.distinct == other.distinct
            and abs(self.mean - other.mean) < 1e-9
            and self.checksum == other.checksum
        )

    def __repr__(self):
        return "Stats(max=%d unique=%d mean=%.3f crc=%s)" % (
            self.largest,
            self.distinct,
            self.mean,
            self.checksum,
        )

    def as_dict(self):
        return {
            "max": self.largest,
            "unique": self.distinct,
            "mean": round(self.mean, 3),
            "checksum": self.checksum,
        }


class Verdict(object):
    """A pass/fail decision plus the sentence explaining it."""

    __slots__ = ("passed", "reason")

    def __init__(self, passed, reason):
        self.passed = passed
        self.reason = reason

    def __repr__(self):
        return "Verdict(%s, %r)" % ("PASS" if self.passed else "FAIL", self.reason)


def judge(scene, reference, written, byte_size):
    """Return a Verdict for one shot.

    scene     -- Stats for the frame as rendered.
    reference -- Stats for the same frame with all primitives suppressed, or
                 None if that capture could not be taken.
    written   -- whether the PNG reached disk.
    byte_size -- its size on disk.

    Checks are ordered so the reported reason is the root cause: a shot that
    failed to write is reported as unwritten, not as "no primitives rendered".
    """
    if scene is None:
        return Verdict(False, "no measurement was taken for this shot")

    if scene.largest < 0:
        return Verdict(
            False,
            "could not read the render target back; the capture never reached "
            "the CPU, so the PNG (if any) is meaningless",
        )

    if not written:
        return Verdict(False, "no PNG was written to the requested path")

    if byte_size <= 0:
        return Verdict(False, "the PNG was written but is empty (0 bytes)")

    if scene.largest == 0:
        return Verdict(
            False,
            "the frame is pure black (max=0); check that the map has a "
            "SkyAtmosphere, that the DirectionalLight has NEGATIVE pitch, and "
            "that -AllowCommandletRendering was passed",
        )

    if reference is None:
        return Verdict(
            False,
            "no primitive-suppressed reference capture was taken, so this shot "
            "is UNVERIFIED -- a bright frame is indistinguishable from empty "
            "sky without one",
        )

    if scene == reference:
        return Verdict(
            False,
            "no primitives rendered: this frame is identical to the same view "
            "with all primitives suppressed (max=%d unique=%d mean=%.3f both "
            "ways). The camera is aimed at empty sky, the geometry is missing "
            "from the scene, or its materials never resolved."
            % (scene.largest, scene.distinct, scene.mean),
        )

    return Verdict(
        True,
        "primitives rendered: %d pixels-worth of difference from the "
        "primitive-suppressed reference" % abs(scene.checksum - reference.checksum),
    )
