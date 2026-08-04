"""Canonical colormap definitions, shared by the Python tools and Unreal.

These control points are the single source of truth for both implementations. A
Python-generated reference figure and an Unreal render of the same data must
produce the same colors, so the values live here and are exported to the engine
rather than being re-typed on each side.

Sampling convention (normative — both implementations must match):

- A colormap is a list of ``(t, r, g, b)`` control points with ``t`` ascending
  over ``[0, 1]`` and channels in ``[0, 1]``.
- Values between control points are **linearly interpolated in sRGB space**.
  This is not the perceptually ideal interpolation, but it is what matplotlib,
  ParaView, and GPU texture filtering all do, so it is what makes the three
  agree. Choosing "better" interpolation here would make our figures disagree
  with every reference tool.
- Sampling to a lookup texture uses **pixel centers**: entry ``i`` of an
  ``N``-entry LUT samples ``t = (i + 0.5) / N``. Using ``i / (N - 1)`` instead
  shifts the whole map by half a texel and is a classic off-by-one that shows
  up as a subtle hue shift against reference figures.

The perceptually uniform maps (viridis, plasma, inferno, magma) are from
matplotlib, released under a CC0 dedication. Turbo is Google's rainbow
replacement, Apache 2.0. See ``Docs/THIRD_PARTY_NOTICES.md``.
"""

from __future__ import annotations

from typing import Final

__all__ = [
    "ColorMap",
    "COLORMAPS",
    "DEFAULT_COLORMAP",
    "PERCEPTUALLY_UNIFORM",
    "DIVERGING",
    "sample_colormap",
    "build_lut",
    "is_perceptually_uniform",
]

#: A colormap is a tuple of (t, r, g, b) control points, t ascending over [0,1].
ColorMap = tuple[tuple[float, float, float, float], ...]

#: Default for all scalar fields. Perceptually uniform, colorblind-safe, and
#: legible in greyscale print. Never default to a rainbow map: rainbows create
#: visual edges where data is smooth and flatten real structure where it is
#: steep, which is actively misleading in a scientific figure.
DEFAULT_COLORMAP: Final = "viridis"

# Control points subsampled from matplotlib's 256-entry tables at every 32nd
# entry plus the endpoints. Nine points reproduce these maps to within ~1/255
# under linear interpolation, which is below the quantization of an 8-bit LUT.
_VIRIDIS: Final[ColorMap] = (
    (0.000, 0.267004, 0.004874, 0.329415),
    (0.125, 0.282623, 0.140926, 0.457517),
    (0.250, 0.253935, 0.265254, 0.529983),
    (0.375, 0.206756, 0.371758, 0.553117),
    (0.500, 0.163625, 0.471133, 0.558148),
    (0.625, 0.127568, 0.566949, 0.550556),
    (0.750, 0.134692, 0.658636, 0.517649),
    (0.875, 0.266941, 0.748751, 0.440573),
    (1.000, 0.993248, 0.906157, 0.143936),
)

_PLASMA: Final[ColorMap] = (
    (0.000, 0.050383, 0.029803, 0.527975),
    (0.125, 0.253935, 0.014979, 0.617331),
    (0.250, 0.417642, 0.000564, 0.658390),
    (0.375, 0.562738, 0.051545, 0.641509),
    (0.500, 0.692840, 0.165141, 0.564522),
    (0.625, 0.798216, 0.280197, 0.469538),
    (0.750, 0.881443, 0.392529, 0.383229),
    (0.875, 0.949217, 0.517763, 0.295662),
    (1.000, 0.940015, 0.975158, 0.131326),
)

_INFERNO: Final[ColorMap] = (
    (0.000, 0.001462, 0.000466, 0.013866),
    (0.125, 0.087411, 0.044556, 0.224813),
    (0.250, 0.229739, 0.036590, 0.360847),
    (0.375, 0.374915, 0.081348, 0.412415),
    (0.500, 0.517933, 0.132268, 0.408558),
    (0.625, 0.665859, 0.198401, 0.370587),
    (0.750, 0.798216, 0.280197, 0.469538),
    (0.875, 0.930513, 0.411474, 0.145367),
    (1.000, 0.988362, 0.998364, 0.644924),
)

_MAGMA: Final[ColorMap] = (
    (0.000, 0.001462, 0.000466, 0.013866),
    (0.125, 0.078815, 0.054184, 0.211667),
    (0.250, 0.207677, 0.063327, 0.379497),
    (0.375, 0.345164, 0.106815, 0.453077),
    (0.500, 0.482930, 0.146968, 0.472039),
    (0.625, 0.629101, 0.192902, 0.457755),
    (0.750, 0.775059, 0.257322, 0.406990),
    (0.875, 0.912966, 0.381636, 0.359630),
    (1.000, 0.987053, 0.991438, 0.749504),
)

#: Google's Turbo — a rainbow replacement without the worst artifacts of jet.
#: Offered for comparison with legacy figures, but never the default: any
#: rainbow map still manufactures apparent structure.
_TURBO: Final[ColorMap] = (
    (0.000, 0.18995, 0.07176, 0.23217),
    (0.125, 0.25107, 0.25237, 0.63374),
    (0.250, 0.27628, 0.42118, 0.89123),
    (0.375, 0.25862, 0.57958, 0.99876),
    (0.500, 0.15844, 0.73551, 0.92305),
    (0.625, 0.09267, 0.86554, 0.71000),
    (0.750, 0.19659, 0.94901, 0.47605),
    (0.875, 0.52795, 0.98727, 0.23573),
    (1.000, 0.47960, 0.01583, 0.01055),
)

#: ParaView's default diverging map. Use for signed data centered on zero.
_COOLWARM: Final[ColorMap] = (
    (0.000, 0.229806, 0.298718, 0.753683),
    (0.125, 0.365375, 0.450915, 0.859997),
    (0.250, 0.508937, 0.588019, 0.936277),
    (0.375, 0.653372, 0.700669, 0.977678),
    (0.500, 0.865003, 0.865003, 0.865003),
    (0.625, 0.938156, 0.680509, 0.615520),
    (0.750, 0.930204, 0.520737, 0.416470),
    (0.875, 0.867254, 0.354740, 0.259232),
    (1.000, 0.705673, 0.015556, 0.150233),
)

_BLUE_WHITE_RED: Final[ColorMap] = (
    (0.000, 0.000000, 0.000000, 1.000000),
    (0.500, 1.000000, 1.000000, 1.000000),
    (1.000, 1.000000, 0.000000, 0.000000),
)

_GRAYSCALE: Final[ColorMap] = (
    (0.000, 0.000000, 0.000000, 0.000000),
    (1.000, 1.000000, 1.000000, 1.000000),
)

COLORMAPS: Final[dict[str, ColorMap]] = {
    "viridis": _VIRIDIS,
    "plasma": _PLASMA,
    "inferno": _INFERNO,
    "magma": _MAGMA,
    "turbo": _TURBO,
    "coolwarm": _COOLWARM,
    "blue-white-red": _BLUE_WHITE_RED,
    "grayscale": _GRAYSCALE,
}

#: Maps safe to use without visually fabricating structure. The UI should mark
#: anything outside this set, so a user choosing turbo knows what they chose.
PERCEPTUALLY_UNIFORM: Final[frozenset[str]] = frozenset(
    {"viridis", "plasma", "inferno", "magma", "grayscale"}
)

#: Maps intended for signed data about a meaningful zero. The UI should center
#: these on zero by default rather than on the data midpoint.
DIVERGING: Final[frozenset[str]] = frozenset({"coolwarm", "blue-white-red"})


def is_perceptually_uniform(name: str) -> bool:
    """Return whether ``name`` is safe to use without qualification."""
    return name in PERCEPTUALLY_UNIFORM


def sample_colormap(name: str, t: float) -> tuple[float, float, float]:
    """Sample a colormap at position ``t``, clamped to ``[0, 1]``.

    Args:
        name: A key of :data:`COLORMAPS`.
        t: Position along the map. Values outside ``[0, 1]`` are clamped; the
            caller is responsible for under/over-range colors, which are a
            display decision rather than a colormap property.

    Returns:
        Linear ``(r, g, b)`` in ``[0, 1]``.

    Raises:
        KeyError: If ``name`` is not a known colormap.
    """
    if name not in COLORMAPS:
        raise KeyError(f"Unknown colormap {name!r}. Available: {sorted(COLORMAPS)}")

    points = COLORMAPS[name]
    t = min(1.0, max(0.0, t))

    # Endpoints are exact, so a full-range LUT reproduces the map's true ends.
    if t <= points[0][0]:
        return points[0][1], points[0][2], points[0][3]
    if t >= points[-1][0]:
        return points[-1][1], points[-1][2], points[-1][3]

    for i in range(len(points) - 1):
        t0, r0, g0, b0 = points[i]
        t1, r1, g1, b1 = points[i + 1]
        if t0 <= t <= t1:
            span = t1 - t0
            f = 0.0 if span <= 0.0 else (t - t0) / span
            return (r0 + (r1 - r0) * f, g0 + (g1 - g0) * f, b0 + (b1 - b0) * f)

    # Unreachable given the clamping above, but returning the top end is the
    # only sane fallback if a table is ever edited into a non-ascending state.
    return points[-1][1], points[-1][2], points[-1][3]


def build_lut(name: str, size: int = 256, *, reverse: bool = False, bands: int = 0):
    """Build a lookup table for upload as a 1D texture.

    Uses **pixel-center** sampling: entry ``i`` samples ``t = (i + 0.5) / size``.
    The Unreal side must sample identically or the two renders disagree by half
    a texel — visible as a subtle but real hue shift against reference figures.

    Args:
        name: A key of :data:`COLORMAPS`.
        size: Number of entries. 256 matches an 8-bit texture exactly.
        reverse: Flip the map end-for-end.
        bands: If > 0, quantize into this many discrete color bands. Banding is
            a legitimate scientific display choice (it makes level sets
            readable), so it is applied here rather than being faked with a
            posterize post-process, which would also affect the UI and legend.

    Returns:
        A ``numpy`` array of shape ``(size, 3)``, dtype float32, in ``[0, 1]``.

    Raises:
        ValueError: If ``size`` is not positive.
        KeyError: If ``name`` is not a known colormap.
    """
    import numpy as np

    if size <= 0:
        raise ValueError(f"LUT size must be positive, got {size}.")

    lut = np.empty((size, 3), dtype=np.float32)
    for i in range(size):
        t = (i + 0.5) / size
        if bands > 0:
            # Snap to the band center so each band shows its representative
            # color rather than the color at its lower edge.
            band = min(bands - 1, int(t * bands))
            t = (band + 0.5) / bands
        if reverse:
            t = 1.0 - t
        lut[i] = sample_colormap(name, t)
    return lut
