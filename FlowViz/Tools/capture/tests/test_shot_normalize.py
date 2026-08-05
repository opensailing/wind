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
