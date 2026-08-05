"""Tests for `capture_frame.normalize`, specifically that it does not EAT keys.

`normalize` builds its result by naming each key it wants. That is a reasonable
way to apply defaults and a dangerous way to forward a request: any key it does
not name is silently dropped, and the shot still renders. The failure is
therefore not an error but a picture -- a capture with no volume in it, which
looks exactly like a capture whose volume failed to render, and has nothing in
common with it as a fix.

The `volume` key is the one that matters here. It carries the case path, the
field and the frame, and if it does not survive normalization the worker never
calls spawn_case_actor, the shot is judged by `judge` instead of
`judge_marcher`, and it PASSES -- on the strength of the map's floor and sky.
That is a green result for a run that never placed the thing under test.
"""

import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from capture_frame import normalize  # noqa: E402


BASE = {"map": "/Game/FlowViz/Maps/L_CapTest", "output": "/tmp/shot.png"}


def test_volume_spec_survives_normalization():
    """The regression this file exists for."""
    volume = {
        "case": "/abs/Case.cfdviz",
        "field": "speed",
        "frame": 3,
        "location": [10.0, 20.0, 30.0],
        "draw_bounding_box": False,
    }

    result = normalize(dict(BASE, volume=volume))

    assert "volume" in result, (
        "normalize dropped the volume spec, so the worker would never place a "
        "case actor and the shot would be judged -- and would PASS -- on the "
        "map's floor and sky alone"
    )
    assert result["volume"] == volume


def test_volume_spec_is_absent_when_not_requested():
    """A shot without a volume must not acquire one, or every plain capture
    would start demanding a marcher-suppressed control it has no use for."""
    assert "volume" not in normalize(dict(BASE))


def test_volume_spec_is_copied_not_aliased():
    """The worker mutates nothing, but the caller may reuse a spec across shots;
    aliasing would make a later edit reach backwards into an earlier shot."""
    volume = {"case": "/abs/Case.cfdviz", "field": "speed"}
    result = normalize(dict(BASE, volume=volume))

    volume["field"] = "pressure"
    assert result["volume"]["field"] == "speed"


@pytest.mark.parametrize("missing", ["case", "field"])
def test_volume_spec_without_the_essentials_is_rejected(missing):
    """Refused at normalization, where the caller still has a stack trace.

    Both of these fail silently downstream: no `case` means no volume, and no
    `field` means the worker refuses to guess (because the manifest's first
    field is typically a vector, which loads, uploads and marches nothing). In
    both cases the shot renders and the PNG looks like an empty room. Rejecting
    here turns a picture into an error message.
    """
    volume = {"case": "/abs/Case.cfdviz", "field": "speed"}
    del volume[missing]

    with pytest.raises(ValueError) as excinfo:
        normalize(dict(BASE, volume=volume))

    assert missing in str(excinfo.value)


def test_existing_keys_are_unaffected():
    """The control. If normalize were broken outright these would fail too, and
    the volume assertions above would be measuring the wrong thing."""
    result = normalize(dict(BASE, location=[1.0, 2.0, 3.0], fov=42.0))

    assert result["map"] == BASE["map"]
    assert result["location"] == [1.0, 2.0, 3.0]
    assert result["fov"] == 42.0
    assert os.path.isabs(result["output"])


# ---------------------------------------------------------------------------
# Render settings
#
# The same hazard as the volume spec above, one level in: these keys reach the
# engine through `dict(volume)`, so forwarding is not the risk. The risk is a
# VALUE that the engine refuses. A refused composite mode leaves the volume in
# its previous mode and renders a complete, plausible picture, so a shot
# labelled "iso-surface" would contain alpha compositing -- and no pixel check,
# no reference diff and no critic can tell those apart afterwards.
#
# Caught here, where the caller still has a stack trace, rather than 90 seconds
# into an engine launch.
# ---------------------------------------------------------------------------

VALID_VOLUME = {"case": "/abs/Case.cfdviz", "field": "speed"}


def test_render_settings_survive_normalization():
    """The forwarding assertion, so the rejection tests below are about values."""
    volume = dict(VALID_VOLUME, composite_mode=4, iso_value=2.5, lighting=True)

    result = normalize(dict(BASE, volume=volume))

    assert result["volume"]["composite_mode"] == 4
    assert result["volume"]["iso_value"] == 2.5
    assert result["volume"]["lighting"] is True


def test_render_settings_are_absent_when_not_requested():
    """THE IDENTITY CONTROL, and the reason absent must not mean "the default".

    Every reference image in the repo was captured before these keys existed. A
    normalize that helpfully filled in composite_mode=0 would look identical
    here and would make the worker WRITE the default -- turning "untouched" into
    "explicitly overwritten", which is a different thing the moment anything
    else sets a mode earlier in the same shot.
    """
    result = normalize(dict(BASE, volume=dict(VALID_VOLUME)))

    assert "composite_mode" not in result["volume"]
    assert "iso_value" not in result["volume"]
    assert "lighting" not in result["volume"]


@pytest.mark.parametrize("mode", [-1, 6, 99, 2**31])
def test_out_of_range_composite_mode_is_rejected(mode):
    """Refused, because the engine's refusal renders as a successful capture.

    UFlowVizCaptureLibrary::SetVolumeCompositeMode rejects these and KEEPS the
    previous mode by design -- a control given a bad number must not become a
    control that does nothing. That is right for the engine and useless for a
    capture script: the shot completes, the PNG is full of a plausible volume,
    and it is the wrong mode.
    """
    volume = dict(VALID_VOLUME, composite_mode=mode)

    with pytest.raises(ValueError) as excinfo:
        normalize(dict(BASE, volume=volume))

    assert "composite_mode" in str(excinfo.value)


@pytest.mark.parametrize("mode", [0, 1, 2, 3, 4, 5])
def test_every_mode_the_shader_implements_is_accepted(mode):
    """The control for the rejection above.

    Without this, `raise ValueError` on every composite_mode would pass the
    rejection test and silently make the whole control unusable. Six values,
    matching EFlowVizCompositeMode and the FLOWVIZ_MODE_* defines in the .usf --
    if the enum grows a seventh, this test is where the omission surfaces.
    """
    result = normalize(dict(BASE, volume=dict(VALID_VOLUME, composite_mode=mode)))

    assert result["volume"]["composite_mode"] == mode


@pytest.mark.parametrize("bad", [float("nan"), float("inf"), float("-inf")])
def test_non_finite_iso_value_is_rejected(bad):
    """NaN compares false against everything, so an iso-surface at NaN finds no
    crossing and renders EMPTY -- the same picture as a threshold outside the
    data range, and a completely different fix."""
    volume = dict(VALID_VOLUME, composite_mode=4, iso_value=bad)

    with pytest.raises(ValueError) as excinfo:
        normalize(dict(BASE, volume=volume))

    assert "iso_value" in str(excinfo.value)


def test_a_finite_iso_value_is_accepted():
    """The control for the rejection above, and it uses a NEGATIVE value on
    purpose: a guard written as `if not iso_value` or `if iso_value <= 0` would
    pass every case above and reject a legitimate threshold on a signed field."""
    result = normalize(
        dict(BASE, volume=dict(VALID_VOLUME, composite_mode=4, iso_value=-3.25))
    )

    assert result["volume"]["iso_value"] == -3.25
