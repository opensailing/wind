"""Colormap sampling conventions.

The values themselves are shared with Unreal (``CFDVizColorMaps.cpp``); what is
tested here is the *convention*, because that is where the two implementations
can silently disagree:

- linear interpolation in sRGB, matching matplotlib and ParaView,
- **pixel-center** LUT sampling, ``t = (i + 0.5) / N`` — using ``i / (N - 1)``
  instead shifts the whole map by half a texel,
- viridis, not a rainbow, as the default (``VISUAL_QA.md`` section 1, rule 6).
"""

from __future__ import annotations

import numpy as np
import pytest

from cfdviz.colormaps import (
    COLORMAPS,
    DEFAULT_COLORMAP,
    DIVERGING,
    PERCEPTUALLY_UNIFORM,
    build_lut,
    is_perceptually_uniform,
    sample_colormap,
)


def test_default_is_perceptually_uniform_and_not_a_rainbow():
    """VISUAL_QA rule 6: a rainbow default manufactures visual structure."""
    assert DEFAULT_COLORMAP == "viridis"
    assert is_perceptually_uniform(DEFAULT_COLORMAP)
    assert "turbo" not in PERCEPTUALLY_UNIFORM
    assert DEFAULT_COLORMAP not in DIVERGING


@pytest.mark.parametrize("name", sorted(COLORMAPS))
def test_control_points_are_well_formed(name):
    points = COLORMAPS[name]
    assert len(points) >= 2
    positions = [p[0] for p in points]
    assert positions[0] == 0.0 and positions[-1] == 1.0
    assert positions == sorted(positions), "control points must ascend"
    assert len(set(positions)) == len(positions), "duplicate control positions"
    for _, r, g, b in points:
        assert 0.0 <= r <= 1.0 and 0.0 <= g <= 1.0 and 0.0 <= b <= 1.0


@pytest.mark.parametrize("name", sorted(COLORMAPS))
def test_endpoints_are_exact(name):
    points = COLORMAPS[name]
    assert sample_colormap(name, 0.0) == points[0][1:]
    assert sample_colormap(name, 1.0) == points[-1][1:]


@pytest.mark.parametrize("name", sorted(COLORMAPS))
def test_out_of_range_is_clamped(name):
    assert sample_colormap(name, -5.0) == sample_colormap(name, 0.0)
    assert sample_colormap(name, 5.0) == sample_colormap(name, 1.0)


def test_interpolation_is_linear_in_srgb():
    """Halfway between two control points is the arithmetic mean of the two."""
    points = COLORMAPS["blue-white-red"]
    (_, r0, g0, b0), (_, r1, g1, b1) = points[0], points[1]
    mid = sample_colormap("blue-white-red", 0.25)
    assert mid == pytest.approx(((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2))


def test_grayscale_is_the_identity_ramp():
    for t in (0.0, 0.25, 0.5, 0.75, 1.0):
        assert sample_colormap("grayscale", t) == pytest.approx((t, t, t))


def test_unknown_colormap_raises():
    with pytest.raises(KeyError, match="Unknown colormap"):
        sample_colormap("jet", 0.5)
    with pytest.raises(KeyError):
        build_lut("jet")


def test_lut_uses_pixel_center_sampling():
    """The convention that must match Unreal exactly.

    Entry 0 of a 256-entry LUT samples t = 0.5/256, NOT t = 0. The distinction
    is asserted against grayscale, where the sampled value *is* t.
    """
    lut = build_lut("grayscale", 256)
    assert lut.shape == (256, 3)
    assert lut[0, 0] == pytest.approx(0.5 / 256)
    assert lut[255, 0] == pytest.approx(255.5 / 256)
    # The wrong convention, i / (N-1), would give exactly 0.0 and 1.0.
    assert lut[0, 0] != 0.0
    assert lut[255, 0] != 1.0


def test_lut_is_float32_in_unit_range():
    lut = build_lut("viridis", 64)
    assert lut.dtype == np.float32
    assert lut.min() >= 0.0 and lut.max() <= 1.0


def test_lut_reverse_mirrors_the_map():
    forward = build_lut("viridis", 32)
    backward = build_lut("viridis", 32, reverse=True)
    assert np.allclose(forward, backward[::-1])


def test_banding_quantizes_into_discrete_colors():
    lut = build_lut("viridis", 256, bands=4)
    unique = np.unique(lut, axis=0)
    assert len(unique) == 4
    # Each band is 64 consecutive identical entries.
    for band in range(4):
        block = lut[band * 64 : (band + 1) * 64]
        assert np.all(block == block[0])


def test_banding_uses_band_centers_not_lower_edges():
    """A lower-edge band would make the first band exactly the map's t=0 color."""
    lut = build_lut("grayscale", 256, bands=4)
    assert lut[0, 0] == pytest.approx(0.125)  # centre of [0, 0.25)
    assert lut[0, 0] != 0.0


def test_lut_size_must_be_positive():
    with pytest.raises(ValueError, match="positive"):
        build_lut("viridis", 0)


def test_perceptually_uniform_maps_increase_monotonically_in_luminance():
    """The property that makes them honest: no dark-bright-dark banding."""
    for name in sorted(PERCEPTUALLY_UNIFORM):
        lut = build_lut(name, 128).astype(np.float64)
        luminance = lut @ np.array([0.2126, 0.7152, 0.0722])
        assert np.all(np.diff(luminance) > -1e-6), f"{name} luminance is not monotonic"


def test_turbo_luminance_is_not_monotonic():
    """Proves the previous test can fail: turbo is exactly what it rules out."""
    lut = build_lut("turbo", 128).astype(np.float64)
    luminance = lut @ np.array([0.2126, 0.7152, 0.0722])
    assert np.any(np.diff(luminance) < -1e-6)


def test_diverging_maps_are_light_in_the_middle():
    for name in sorted(DIVERGING):
        low = sample_colormap(name, 0.0)
        mid = sample_colormap(name, 0.5)
        high = sample_colormap(name, 1.0)
        assert sum(mid) > sum(low) and sum(mid) > sum(high)
