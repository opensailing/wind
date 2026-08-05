"""Tests for the RAY-MARCHER verdict, which is a different question from `judge`.

`judge` answers "did any primitive render?" by comparing the frame against the
same view with every primitive suppressed. That is the right question for a
scene of static meshes and the WRONG question for the volume ray-marcher, and
the difference is what these tests pin down.

WHY THE PRIMITIVE DIFFERENTIAL CANNOT SEE THE MARCHER
-----------------------------------------------------
FFlowVizVolumeSceneProxy::GetDynamicMeshElements draws three things from one
proxy:

  1. a wireframe bounding box, when bDrawBoundingBox is set;
  2. the hull -- a solid triangle box with GEngine->DebugMeshMaterial -- which is
     drawn on EVERY non-wireframe view with no flag guarding it at all; and
  3. the ray-march dispatch.

The primitive-suppressed reference removes the whole proxy, so it removes all
three together. `scene != reference` therefore goes true the moment the HULL
rasterizes, which it does unconditionally, whether or not the marcher
contributed a single pixel. Turning bDrawBoundingBox off does not help: the hull
is not the wire box and is not gated on that flag.

That is a pass criterion that cannot fail for the thing under test. It would
certify a completely dead ray-marcher, and the hull is an opaque box, so the
certified image would even look like "a volume".

THE CRITERION THAT CAN FAIL
---------------------------
Hold the proxy fixed and move exactly one variable: whether a dispatcher is
installed. FlowVizVolumeRayMarch::SetDispatcher(nullptr) removes the marcher and
changes nothing else -- same actor, same hull, same wireframe, same camera, same
lighting. Any pixel that differs between those two captures was produced by the
ray-marcher and by nothing else.

So the shot is captured three ways and all three are reported:

  A  marcher installed      the frame under test
  C  marcher uninstalled    identical scene, no ray-march  <- the control
  B  primitives suppressed  the existing empty-scene baseline

and the headline verdict is A != C. B is retained because it answers a question
C cannot: whether the volume actor reached the render scene at all.

Every test below ending in `_is_not_a_pass` is a known-bad input. If one starts
passing, the criterion has regressed to something that cannot fail.
"""

import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from verdict import Stats, judge_marcher  # noqa: E402


def stats(largest=200, distinct=1672, mean=88.66, checksum=12345):
    """A healthy-looking measurement. Individual fields overridden per test."""
    return Stats(largest=largest, distinct=distinct, mean=mean, checksum=checksum)


# A distinct sentinel, because None is a MEANINGFUL value for `reference` -- it
# is the "no reference could be taken" case, which must not pass. A `None`
# default would have the helper silently substitute a healthy reference and
# defeat the very test that checks it.
UNSET = object()


def call(with_marcher, without_marcher, reference=UNSET, written=True, byte_size=1173227):
    if reference is UNSET:
        reference = stats(largest=182, distinct=1672, mean=88.660, checksum=0xEEEE)
    return judge_marcher(
        with_marcher=with_marcher,
        without_marcher=without_marcher,
        reference=reference,
        written=written,
        byte_size=byte_size,
    )


# --- The confound this module exists to catch --------------------------------


def test_frame_identical_with_and_without_the_marcher_is_not_a_pass():
    """The whole point. Hull and wireframe drew; the ray-marcher did not.

    Both captures contain a bright, opaque, entirely convincing box. They also
    differ from the primitive-suppressed reference, so the ordinary primitive
    differential calls this a PASS. It is not one.
    """
    box_only = stats(largest=214, distinct=9001, mean=97.5, checksum=0xB0B0)

    verdict = call(with_marcher=box_only, without_marcher=box_only)

    assert not verdict.passed
    assert "marcher" in verdict.reason.lower()


def test_differing_from_the_primitive_reference_does_not_rescue_a_dead_marcher():
    """Explicitly: passing `judge` must not imply passing `judge_marcher`.

    This is the exact upgrade path a reader needs to see fail, because the old
    criterion is still right about its own question and will still say PASS.
    """
    from verdict import judge

    box_only = stats(checksum=0xB0B0)
    empty = stats(largest=182, distinct=1672, mean=88.660, checksum=0xEEEE)

    assert judge(scene=box_only, reference=empty, written=True, byte_size=999).passed

    verdict = call(with_marcher=box_only, without_marcher=box_only, reference=empty)
    assert not verdict.passed


def test_frame_that_differs_from_the_marcher_suppressed_control_is_a_pass():
    verdict = call(
        with_marcher=stats(checksum=0x1111),
        without_marcher=stats(checksum=0x2222),
    )

    assert verdict.passed, verdict.reason


def test_a_pass_reports_the_numeric_delta():
    """A bare PASS is not reviewable; the size of the contribution is the datum."""
    verdict = call(
        with_marcher=stats(checksum=0x3000),
        without_marcher=stats(checksum=0x1000),
    )

    assert verdict.passed
    assert str(0x2000) in verdict.reason


def test_brightness_cannot_override_the_marcher_differential():
    """The failure modes push brightness the wrong way -- see verdict.py."""
    bright = stats(largest=255, distinct=60000, mean=200.0, checksum=7)

    verdict = call(with_marcher=bright, without_marcher=bright)

    assert not verdict.passed


# --- A missing control is UNSCORED, never a pass -----------------------------


def test_missing_marcher_suppressed_control_is_not_a_pass():
    """Without the control there is no way to attribute the pixels. Not a pass."""
    verdict = call(with_marcher=stats(checksum=1), without_marcher=None)

    assert not verdict.passed
    assert "control" in verdict.reason.lower()


def test_missing_primitive_reference_is_not_a_pass():
    """Inherited from `judge`: an unverified shot must not report green."""
    verdict = call(
        with_marcher=stats(checksum=1),
        without_marcher=stats(checksum=2),
        reference=None,
    )

    assert not verdict.passed
    assert "reference" in verdict.reason.lower()


# --- The volume must actually be in the scene --------------------------------


def test_control_identical_to_the_empty_reference_is_not_a_pass():
    """If the marcher-off frame is empty, the volume primitive never rendered.

    The proxy draws its hull unconditionally, so a control that matches the
    primitive-suppressed reference means no proxy was in the scene -- the case
    failed to load, or the actor was never spawned. Whatever the marcher-on
    frame then contains, it was not composited over a volume that is present,
    and calling it "the volume rendered" would be a lie about the scene.
    """
    empty = stats(largest=182, distinct=1672, mean=88.660, checksum=0xEEEE)

    verdict = call(
        with_marcher=stats(checksum=0x1234),
        without_marcher=empty,
        reference=empty,
    )

    assert not verdict.passed
    assert "not in the scene" in verdict.reason.lower()


# --- The floor cases still apply ---------------------------------------------


def test_pure_black_frame_is_not_a_pass():
    verdict = call(
        with_marcher=stats(largest=0, distinct=1, mean=0.0, checksum=0),
        without_marcher=stats(checksum=5),
    )

    assert not verdict.passed
    assert "black" in verdict.reason.lower()


def test_failed_readback_is_not_a_pass():
    verdict = call(
        with_marcher=Stats(largest=-1, distinct=0, mean=0.0, checksum=0),
        without_marcher=stats(checksum=5),
    )

    assert not verdict.passed
    assert "read" in verdict.reason.lower()


def test_unwritten_file_is_not_a_pass():
    verdict = call(
        with_marcher=stats(checksum=1),
        without_marcher=stats(checksum=2),
        written=False,
        byte_size=0,
    )

    assert not verdict.passed
    assert "written" in verdict.reason.lower()


def test_no_measurement_at_all_is_not_a_pass():
    verdict = call(with_marcher=None, without_marcher=stats(checksum=2))

    assert not verdict.passed


# --- Reason strings are the operator-facing product of a failure -------------


@pytest.mark.parametrize(
    "with_marcher,without_marcher,reference",
    [
        (stats(checksum=1), stats(checksum=1), stats(checksum=9)),
        (stats(checksum=1), None, stats(checksum=9)),
        (stats(checksum=1), stats(checksum=2), None),
        (stats(checksum=1), stats(checksum=9), stats(checksum=9)),
        (stats(largest=0, distinct=1, mean=0.0, checksum=0), stats(checksum=2), stats(checksum=9)),
    ],
)
def test_every_failure_explains_itself(with_marcher, without_marcher, reference):
    verdict = judge_marcher(
        with_marcher=with_marcher,
        without_marcher=without_marcher,
        reference=reference,
        written=True,
        byte_size=1000,
    )

    assert not verdict.passed
    assert verdict.reason
    assert len(verdict.reason) > 20, "a bare 'FAIL' sends the reader to the source"
