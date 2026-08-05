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


def judge_marcher(with_marcher, without_marcher, reference, written, byte_size):
    """Return a Verdict for one shot of the VOLUME RAY-MARCHER specifically.

    `judge` above answers "did any primitive render?". That is the wrong
    question for the ray-marcher, and the reason is structural rather than a
    matter of strictness.

    FFlowVizVolumeSceneProxy::GetDynamicMeshElements draws three things from one
    proxy: a wireframe bounding box (under bDrawBoundingBox), the HULL -- a
    solid triangle box using GEngine->DebugMeshMaterial, drawn on every
    non-wireframe view with no flag guarding it -- and the ray-march dispatch.
    The primitive-suppressed reference removes the entire proxy, so it removes
    all three at once. `scene != reference` therefore goes true the instant the
    hull rasterizes, which it does unconditionally. It cannot distinguish a
    working marcher from a dead one, and because the hull is an opaque box, the
    frame it certifies even LOOKS like a rendered volume.

    Turning bDrawBoundingBox off does not fix that: the wire box is not the
    hull, and only the wire box is behind that flag.

    So the criterion here holds the proxy fixed and moves exactly one variable:
    whether a dispatcher is installed. With SetDispatcher(nullptr) the same
    actor, the same hull, the same wireframe, the same camera and the same
    lighting all still render; only the ray-march is gone. Any pixel that
    differs between the two captures was produced by the marcher and by nothing
    else, which is precisely the claim being made.

    with_marcher    -- Stats with the production dispatcher installed.
    without_marcher -- Stats for the identical scene with it uninstalled. The
                       control. None means it could not be taken, which is
                       UNSCORED, not a pass.
    reference       -- Stats with all primitives suppressed. Still required: it
                       answers a question the control cannot, namely whether the
                       volume reached the render scene at all.
    written         -- whether the PNG reached disk.
    byte_size       -- its size on disk.

    Checks are ordered so the reported reason is the root cause.
    """
    # Everything `judge` rejects, this must reject too: a shot that failed to
    # read back or never hit disk says nothing about the marcher. Delegating
    # rather than restating keeps one copy of those rules.
    base = judge(with_marcher, reference, written, byte_size)
    if not base.passed:
        return base

    if without_marcher is None:
        return Verdict(
            False,
            "no marcher-suppressed control capture was taken, so this shot is "
            "UNSCORED: the volume proxy draws an opaque hull box whether or not "
            "the ray-marcher ran, and without the control there is no way to "
            "attribute a single pixel to the marcher",
        )

    # The control matching the empty-scene reference means no volume primitive
    # was in the scene at all -- the case never loaded, or the actor was never
    # spawned. Whatever the marcher-on frame holds, it was not composited over a
    # volume that is present, so "the volume rendered" would misdescribe it.
    if without_marcher == reference:
        return Verdict(
            False,
            "the volume is not in the scene: with the marcher suppressed the "
            "frame is identical to the primitive-suppressed reference (max=%d "
            "unique=%d mean=%.3f), but the proxy draws its hull unconditionally, "
            "so a present volume could not look empty. The case failed to load "
            "or the actor was never spawned."
            % (without_marcher.largest, without_marcher.distinct, without_marcher.mean),
        )

    if with_marcher == without_marcher:
        return Verdict(
            False,
            "the ray-marcher contributed nothing: this frame is identical to "
            "the same scene with the dispatcher uninstalled (max=%d unique=%d "
            "mean=%.3f both ways). Everything visible is the proxy's hull and "
            "wireframe box. Check the capture log for 'AddRayMarchPass' and for "
            "Error lines, and confirm a frame was uploaded -- without one, "
            "bHasParameters is false and the dispatch is skipped."
            % (with_marcher.largest, with_marcher.distinct, with_marcher.mean),
        )

    return Verdict(
        True,
        "the ray-marcher rendered: %d pixels-worth of difference from the "
        "marcher-suppressed control, which is the same scene with the same hull "
        "and the same box (control differs from the empty reference by %d)"
        % (
            abs(with_marcher.checksum - without_marcher.checksum),
            abs(without_marcher.checksum - reference.checksum),
        ),
    )
