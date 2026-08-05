"""Tests for the Scientific-profile reference figure generator.

`Docs/VISUAL_QA.md` section 1 lists nine hard requirements for a Scientific
render, and `references/README.md` warns that a reference nobody checked is
worse than no reference at all -- it produces a verdict that sounds
authoritative and means nothing. These figures ARE the bar the blind harness
judges against, so every requirement they claim to satisfy is asserted here.

Three of these tests are negative controls, and they are the point of the file:

  * `test_invalid_check_fails_when_nan_is_painted_as_the_colormap_minimum`
    repaints NaN as viridis's darkest purple -- exactly the bug requirement 4
    forbids -- and requires the distinctness check to FAIL. Without it,
    `test_invalid_cells_are_visibly_distinct` could be passing because every
    image trivially satisfies it.
  * `test_vortex_street_detector_rejects_a_featureless_field` feeds the street
    detector a monotone ramp and requires it to find nothing. A detector that
    fires on any image is not detecting a vortex street.
  * `test_data_midpoint_norm_is_not_centered_on_zero` builds the wrong norm on
    purpose and shows the centering assertion catches it.

The image-level tests read pixels back out of the written PNG rather than
inspecting the figure in memory. A PNG that exists is not a PNG that shows
something, and the artist tree cannot tell you what landed on disk.
"""

from __future__ import annotations

import json
import os
import sys

import numpy as np
import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import matplotlib  # noqa: E402

matplotlib.use("Agg")

import matplotlib.image as mpimg  # noqa: E402

import make_reference_figures as mrf  # noqa: E402
from blind import audit, seal  # noqa: E402

# tests/ -> capture/ -> Tools/ -> FlowViz/
_FLOWVIZ = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)
CASE_ROOT = os.path.join(_FLOWVIZ, "Samples", "MockCylinderWake.cfdviz")

# Cylinder geometry, read off meshes/obstacle.cvm. The wake bands below are
# placed relative to this, so a test does not silently drift onto the cylinder.
CYLINDER_CENTER = (4.0, 2.0)
CYLINDER_RADIUS = 0.45


# ---------------------------------------------------------------------------
# Perceptual distance, so "visibly distinct" is a number and not an opinion
# ---------------------------------------------------------------------------

def _srgb_to_linear(channel: np.ndarray) -> np.ndarray:
    channel = np.asarray(channel, dtype=np.float64)
    return np.where(channel <= 0.04045, channel / 12.92, ((channel + 0.055) / 1.055) ** 2.4)


def oklab(rgb: np.ndarray) -> np.ndarray:
    """Convert sRGB in [0,1] to OKLab, for perceptual distances.

    Euclidean distance in OKLab (x100) is the same scale the project's dataviz
    guidance uses: >= 8 is "distinguishable", >= 15 clears the normal-vision
    floor. Comparing sRGB channels instead would call navy and black different
    and light grey and white the same.
    """
    linear = _srgb_to_linear(np.asarray(rgb, dtype=np.float64))
    r, g, b = linear[..., 0], linear[..., 1], linear[..., 2]
    long_ = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b
    medium = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b
    short = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b
    l_, m_, s_ = np.cbrt(long_), np.cbrt(medium), np.cbrt(short)
    return np.stack(
        [
            0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
            1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
            0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_,
        ],
        axis=-1,
    )


def perceptual_distance(color, others) -> float:
    """Smallest OKLab distance (x100) between ``color`` and any of ``others``."""
    target = oklab(np.asarray(color, dtype=np.float64))
    field = oklab(np.asarray(others, dtype=np.float64))
    return float(np.sqrt(((field - target) ** 2).sum(axis=-1)).min()) * 100.0


# ---------------------------------------------------------------------------
# Reading the written PNG back
# ---------------------------------------------------------------------------

def load_rgb(path: str) -> np.ndarray:
    """The written PNG as (rows, cols, 3) float in [0, 1], top row first."""
    image = mpimg.imread(path)
    return np.asarray(image, dtype=np.float64)[..., :3]


def axes_pixels(result, image: np.ndarray) -> np.ndarray:
    """Crop ``image`` to the data axes, using the mapping the generator recorded."""
    x0, y0, x1, y1 = (int(round(v)) for v in result.axes_bbox_px)
    return image[y0:y1, x0:x1]


def data_to_pixel(result, x: float, y: float) -> tuple[int, int]:
    """Physical (x, y) -> (column, row) in the *cropped* axes image."""
    xmin, xmax, ymin, ymax = result.extent
    x0, y0, x1, y1 = result.axes_bbox_px
    width, height = x1 - x0, y1 - y0
    column = (x - xmin) / (xmax - xmin) * width
    # Data y increases upward; PNG rows increase downward.
    row = (ymax - y) / (ymax - ymin) * height
    return int(round(column)), int(round(row))


def exact_color_count(image: np.ndarray, color) -> int:
    """Pixels matching ``color`` exactly after 8-bit quantization."""
    target = np.round(np.asarray(color, dtype=np.float64) * 255.0)
    quantized = np.round(image * 255.0)
    return int(np.all(quantized == target, axis=-1).sum())


# ---------------------------------------------------------------------------
# The two detectors under test, plus their controls
# ---------------------------------------------------------------------------

def invalid_is_visibly_distinct(result, image: np.ndarray, minimum_pixels: int = 100):
    """Whether the figure's invalid cells read as invalid.

    Two conditions, and requirement 4 needs both:

    1. Pixels **inside the data axes** actually carry the invalid color -- a
       figure that drew NaN as data has none, but so does a figure whose NaNs
       vanished entirely.
    2. That color is perceptually far from every color the colormap can
       produce. This is the half that catches "NaN rendered as the colormap
       minimum": those pixels exist and are numerous, they just mean nothing.

    Counting over the whole PNG instead of the data axes is a trap that this
    check fell into once: the legend's own invalid swatch contributes ~841
    magenta pixels, which cleared the threshold on its own and let a figure
    that painted every NaN as viridis's darkest purple pass. The swatch proves
    the legend exists, not that any cell was marked invalid.
    """
    painted = exact_color_count(axes_pixels(result, image), result.invalid_color)
    if painted < minimum_pixels:
        return False, (
            "only %d pixels inside the data axes carry the invalid color %r; "
            "the NaN cells were drawn as data or dropped"
            % (painted, result.invalid_color)
        )
    ramp = mrf.colormap_samples(result.colormap)
    distance = perceptual_distance(result.invalid_color, ramp)
    if distance < 15.0:
        return False, (
            "the invalid color %r sits %.1f OKLab units from the nearest %s "
            "color, so an invalid cell is indistinguishable from a valid one"
            % (result.invalid_color, distance, result.colormap)
        )
    return True, "%d invalid pixels, %.1f OKLab units clear of the ramp" % (
        painted, distance
    )


def _runs(flags: np.ndarray, minimum_length: int) -> list[tuple[int, int]]:
    """Contiguous True runs of at least ``minimum_length``, as (start, stop)."""
    found = []
    start = None
    for index, flag in enumerate(flags):
        if flag and start is None:
            start = index
        elif not flag and start is not None:
            if index - start >= minimum_length:
                found.append((start, index))
            start = None
    if start is not None and len(flags) - start >= minimum_length:
        found.append((start, len(flags)))
    return found


def vortex_street_lobes(result, image: np.ndarray):
    """Signed vortex cores found in the wake of the rendered vorticity figure.

    Works on pixels, not on the data, because the question is whether the
    FIGURE shows the street. A diverging map puts one pole warm and the other
    cool with a neutral middle, so a core is a run of pixels whose red channel
    clearly beats its blue channel (or the reverse).

    Returns a list of (physical_x, sign) sorted downstream, taking cool cores
    from the band above the wake centerline and warm cores from the band below
    it -- which is where the alternating Karman street puts them.
    """
    cropped = axes_pixels(result, image)
    red, blue = cropped[..., 0], cropped[..., 2]
    warm = (red - blue) > 0.20
    cool = (blue - red) > 0.20

    center_x, center_y = CYLINDER_CENTER
    downstream = center_x + 2.0 * CYLINDER_RADIUS
    xmin, xmax, _ymin, _ymax = result.extent
    start_column, _ = data_to_pixel(result, downstream, center_y)
    _, warm_row = data_to_pixel(result, downstream, center_y - 0.5)
    _, cool_row = data_to_pixel(result, downstream, center_y + 0.5)

    width = cropped.shape[1]
    minimum_length = max(3, int(0.01 * width))

    lobes = []
    for row, mask, sign in ((cool_row, cool, -1), (warm_row, warm, +1)):
        row = int(np.clip(row, 0, cropped.shape[0] - 1))
        line = mask[row][start_column:]
        for begin, end in _runs(line, minimum_length):
            column = start_column + 0.5 * (begin + end)
            physical = xmin + column / width * (xmax - xmin)
            lobes.append((physical, sign))
    return sorted(lobes)


def alternation_count(lobes) -> int:
    """How many times the sign flips walking downstream through ``lobes``."""
    signs = [sign for _x, sign in lobes]
    return sum(1 for a, b in zip(signs, signs[1:]) if a != b)


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(scope="session")
def figures(tmp_path_factory):
    """The whole default corpus, generated once."""
    out = tmp_path_factory.mktemp("references")
    results = mrf.generate(CASE_ROOT, str(out))
    return {result.key: result for result in results}


@pytest.fixture(scope="session")
def images(figures):
    return {key: load_rgb(result.path) for key, result in figures.items()}


# ---------------------------------------------------------------------------
# Requirement 6 -- perceptually uniform by default, never rainbow
# ---------------------------------------------------------------------------

def test_every_default_figure_uses_a_safe_colormap(figures):
    from cfdviz.colormaps import DIVERGING, is_perceptually_uniform

    assert figures, "the generator produced no figures at all"
    for key, result in figures.items():
        safe = is_perceptually_uniform(result.colormap) or result.colormap in DIVERGING
        assert safe, (
            "%s defaults to %r, which is neither perceptually uniform nor a "
            "diverging map for signed data (VISUAL_QA 1.6)" % (key, result.colormap)
        )


def test_no_figure_defaults_to_a_rainbow_map(figures):
    for key, result in figures.items():
        assert result.colormap not in ("turbo", "jet", "rainbow"), (
            "%s uses the rainbow map %r as a default" % (key, result.colormap)
        )


def test_velocity_magnitude_uses_viridis(figures):
    assert figures["velocity_magnitude_slice"].colormap == "viridis"


# ---------------------------------------------------------------------------
# The colormap is the project's table, not matplotlib's
# ---------------------------------------------------------------------------

def test_colormap_comes_from_the_project_table(figures):
    """The figures must sample cfdviz/colormaps.py, the table Unreal shares.

    The control below is what gives this test teeth: the project's nine-point
    viridis differs from matplotlib's 256-entry one by up to 103/255 in a
    channel, so a generator that quietly used `plt.get_cmap("viridis")` would
    produce a reference the Unreal render can never match, and this assertion
    catches it.
    """
    from cfdviz.colormaps import build_lut

    ours = mrf.colormap_samples("viridis", 256)
    expected = build_lut("viridis", 256)
    assert np.allclose(ours, expected, atol=1e-6)


def test_matplotlib_builtin_viridis_would_fail_that_test():
    """Control: the two viridis tables are genuinely different."""
    from cfdviz.colormaps import build_lut

    builtin = matplotlib.colormaps["viridis"](
        (np.arange(256) + 0.5) / 256.0
    )[:, :3]
    assert not np.allclose(builtin, build_lut("viridis", 256), atol=1e-6), (
        "if these matched, the previous test could not tell the two apart"
    )


# ---------------------------------------------------------------------------
# Requirement 7 -- diverging data centered on the meaningful zero
# ---------------------------------------------------------------------------

def test_vorticity_is_diverging_and_centered_on_zero(figures):
    from cfdviz.colormaps import DIVERGING

    result = figures["vorticity_slice"]
    assert result.diverging
    assert result.colormap in DIVERGING
    assert result.vmin == pytest.approx(-result.vmax)
    assert result.vmin < 0.0 < result.vmax
    assert result.norm_of_zero == pytest.approx(0.5), (
        "zero must land on the neutral middle of the diverging map, not "
        "wherever the data midpoint happens to fall"
    )


def test_data_midpoint_norm_is_not_centered_on_zero():
    """Control: the centering assertion above rejects the wrong norm.

    An auto-ranged norm over asymmetric signed data puts its neutral color at
    the data midpoint, so zero is painted as "somewhat negative" and the sign
    of the field is a lie. If that passed the test above, the test above would
    be checking nothing.
    """
    import matplotlib.colors as mcolors

    asymmetric = mcolors.Normalize(vmin=-4.0, vmax=16.0)
    assert asymmetric(0.0) != pytest.approx(0.5)

    centered = mrf.centered_norm(-4.0, 16.0)
    assert centered(0.0) == pytest.approx(0.5)
    assert centered.vmin == pytest.approx(-centered.vmax)


def test_velocity_magnitude_is_not_treated_as_diverging(figures):
    result = figures["velocity_magnitude_slice"]
    assert not result.diverging
    assert result.vmin >= 0.0, "a magnitude has no meaningful negative half"


# ---------------------------------------------------------------------------
# Requirement 4 -- invalid data is visibly invalid
# ---------------------------------------------------------------------------

def test_the_case_actually_contains_invalid_cells():
    """Degenerate-data guard: without NaNs, every NaN test below is vacuous."""
    field = mrf.velocity_magnitude_slice(CASE_ROOT, frame=mrf.DEFAULT_FRAME)
    assert np.isnan(field.values).any(), (
        "the sample case has no NaN in this slice, so nothing below is testing "
        "NaN handling"
    )


@pytest.mark.parametrize(
    "key", ["velocity_magnitude_slice", "vorticity_slice", "vorticity_magnitude_mip"]
)
def test_invalid_cells_are_visibly_distinct(figures, images, key):
    verdict, reason = invalid_is_visibly_distinct(figures[key], images[key])
    assert verdict, "%s: %s" % (key, reason)


def test_invalid_check_fails_when_nan_is_painted_as_the_colormap_minimum(
    tmp_path, monkeypatch
):
    """THE control. Repaint NaN as viridis's darkest purple and require a FAIL.

    This is the exact bug requirement 4 names: "never as the colormap's
    minimum". The masked cylinder would then read as a region of very low
    speed -- a physically plausible, entirely fabricated stagnation zone. If
    the check above cannot fail on this image, it is not a check.
    """
    from cfdviz.colormaps import sample_colormap

    monkeypatch.setattr(mrf, "INVALID_COLOR", tuple(sample_colormap("viridis", 0.0)))
    result = mrf.render_figure(
        mrf.velocity_magnitude_slice(CASE_ROOT, frame=mrf.DEFAULT_FRAME),
        str(tmp_path / "bad.png"),
    )

    verdict, reason = invalid_is_visibly_distinct(result, load_rgb(result.path))

    assert not verdict, (
        "NaN was painted as the colormap minimum and the check still passed, "
        "so it cannot detect requirement 4's failure mode"
    )
    assert "indistinguishable" in reason


def test_invalid_check_fails_when_nan_is_filled_with_the_range_minimum(tmp_path):
    """The other half of the same bug: NaN replaced by a number before drawing.

    `imshow` cannot mark what is no longer NaN, so the masked cylinder renders
    as a genuine low-speed region -- a fabricated stagnation zone. The figure
    still carries the legend's invalid swatch, which is exactly why this check
    counts only pixels inside the data axes.
    """
    field = mrf.velocity_magnitude_slice(CASE_ROOT, frame=mrf.DEFAULT_FRAME)
    filled = mrf.replace_values(field, np.nan_to_num(field.values, nan=field.vmin))
    result = mrf.render_figure(filled, str(tmp_path / "filled.png"))
    image = load_rgb(result.path)

    assert exact_color_count(image, result.invalid_color) > 0, (
        "the legend swatch should still be present; without it this test would "
        "not be distinguishing the axes from the whole figure"
    )

    verdict, reason = invalid_is_visibly_distinct(result, image)

    assert not verdict, (
        "NaN was filled with the range minimum and the check still passed"
    )
    assert "drawn as data or dropped" in reason


def test_invalid_color_clears_every_colormap_the_corpus_uses(figures):
    for key, result in figures.items():
        distance = perceptual_distance(
            result.invalid_color, mrf.colormap_samples(result.colormap)
        )
        assert distance >= 15.0, (
            "%s: invalid color is only %.1f OKLab units from the %s ramp"
            % (key, distance, result.colormap)
        )


# ---------------------------------------------------------------------------
# Requirement 2 -- every pseudocolored view carries a scalar legend
# ---------------------------------------------------------------------------

@pytest.mark.parametrize(
    "key", ["velocity_magnitude_slice", "vorticity_slice", "vorticity_magnitude_mip"]
)
def test_every_figure_has_a_colorbar_labelled_with_field_and_unit(key):
    field = mrf.FIELD_BUILDERS[key](CASE_ROOT, frame=mrf.DEFAULT_FRAME)
    figure, result = mrf.build_figure(field)
    try:
        colorbars = [
            axes for axes in figure.axes if getattr(axes, "_colorbar", None) is not None
        ]
        assert len(colorbars) == 1, "expected exactly one scalar legend"
        label = colorbars[0]._colorbar.ax.get_ylabel()
        assert field.field_name.lower() in label.lower(), (
            "the legend does not name the field: %r" % label
        )
        assert field.unit in label, "the legend does not state the unit: %r" % label
    finally:
        matplotlib.pyplot.close(figure)


@pytest.mark.parametrize(
    "key", ["velocity_magnitude_slice", "vorticity_slice", "vorticity_magnitude_mip"]
)
def test_the_legend_states_the_numeric_range(key):
    field = mrf.FIELD_BUILDERS[key](CASE_ROOT, frame=mrf.DEFAULT_FRAME)
    figure, result = mrf.build_figure(field)
    try:
        ticks = [
            axes._colorbar.get_ticks()
            for axes in figure.axes
            if getattr(axes, "_colorbar", None) is not None
        ][0]
        assert min(ticks) == pytest.approx(result.vmin, rel=1e-6)
        assert max(ticks) == pytest.approx(result.vmax, rel=1e-6)
    finally:
        matplotlib.pyplot.close(figure)


def test_the_legend_names_the_invalid_swatch():
    """A distinct color nobody explains is a mystery, not a legend.

    The label must sit *beside the swatch*, not merely appear somewhere on the
    figure. Searching the whole text of the figure passed even with the swatch
    label deleted, because the footnote about invalid cells being excluded from
    the color scale also contains the word -- and a footnote in the bottom-left
    does not tell a reader what the magenta block on the right means.
    """
    field = mrf.velocity_magnitude_slice(CASE_ROOT, frame=mrf.DEFAULT_FRAME)
    figure, _result = mrf.build_figure(field)
    try:
        swatch = [
            axes
            for axes in figure.axes
            if axes.get_images()
            and axes.get_images()[0].get_array().shape[:2] == (1, 1)
        ]
        assert len(swatch) == 1, "expected exactly one 1x1 invalid swatch"
        swatch_box = swatch[0].get_position()

        nearby = []
        for text in figure.findobj(matplotlib.text.Text):
            content = text.get_text().lower()
            if not content or text.get_figure() is not figure:
                continue
            try:
                x, y = text.get_position()
            except (TypeError, ValueError):
                continue
            if text.get_transform() is not figure.transFigure:
                continue
            if abs(y - swatch_box.y0) < 0.06 and 0.0 < (x - swatch_box.x1) < 0.10:
                nearby.append(content)

        joined = " ".join(nearby)
        assert "invalid" in joined, (
            "no label beside the invalid swatch; found %r" % (nearby,)
        )
        assert "nan" in joined or "masked" in joined, (
            "the swatch label does not say what invalid means: %r" % (nearby,)
        )
    finally:
        matplotlib.pyplot.close(figure)


# ---------------------------------------------------------------------------
# Requirement 3 -- the range is stable and stated
# ---------------------------------------------------------------------------

def test_default_range_is_the_manifest_global_range(figures):
    from cfdviz.manifest import field_by_id, load_manifest

    manifest = load_manifest(CASE_ROOT)
    statistics = field_by_id(manifest, "U")["statistics"]

    result = figures["velocity_magnitude_slice"]
    assert result.range_mode == "global"
    assert result.vmin == pytest.approx(statistics["globalMagnitudeMin"])
    assert result.vmax == pytest.approx(statistics["globalMagnitudeMax"])


def test_per_frame_range_differs_from_global_and_says_so(tmp_path):
    """A per-frame range must be labelled -- and must be worth labelling."""
    global_field = mrf.velocity_magnitude_slice(CASE_ROOT, frame=mrf.DEFAULT_FRAME)
    frame_field = mrf.velocity_magnitude_slice(
        CASE_ROOT, frame=mrf.DEFAULT_FRAME, range_mode="frame"
    )

    assert frame_field.range_mode == "frame"
    assert (frame_field.vmin, frame_field.vmax) != (
        global_field.vmin,
        global_field.vmax,
    ), "the two range modes agree here, so this test proves nothing"

    figure, _result = mrf.build_figure(frame_field)
    try:
        texts = " ".join(
            text.get_text().lower() for text in figure.findobj(matplotlib.text.Text)
        )
        assert "per-frame" in texts, (
            "a per-frame auto range must be visibly labelled (VISUAL_QA 1.3)"
        )
    finally:
        matplotlib.pyplot.close(figure)


def test_global_range_figures_do_not_claim_to_be_per_frame():
    figure, _result = mrf.build_figure(
        mrf.velocity_magnitude_slice(CASE_ROOT, frame=mrf.DEFAULT_FRAME)
    )
    try:
        texts = " ".join(
            text.get_text().lower() for text in figure.findobj(matplotlib.text.Text)
        )
        assert "per-frame" not in texts
        assert "global range" in texts
    finally:
        matplotlib.pyplot.close(figure)


# ---------------------------------------------------------------------------
# Requirement 5 -- interpolation is disclosed
# ---------------------------------------------------------------------------

def test_the_figure_discloses_how_cells_were_interpolated():
    figure, _result = mrf.build_figure(
        mrf.velocity_magnitude_slice(CASE_ROOT, frame=mrf.DEFAULT_FRAME)
    )
    try:
        texts = " ".join(
            text.get_text().lower() for text in figure.findobj(matplotlib.text.Text)
        )
        assert "nearest" in texts or "no interpolation" in texts
    finally:
        matplotlib.pyplot.close(figure)


def _color_transitions_along_a_row(result, image: np.ndarray) -> int:
    """Pixel columns where the color changes, on the middle row of the axes."""
    cropped = axes_pixels(result, image)
    row = cropped[cropped.shape[0] // 2]
    return int((np.abs(np.diff(row, axis=0)).sum(axis=1) > 0.004).sum())


def test_the_disclosed_nearest_interpolation_is_what_was_actually_drawn(figures, images):
    """The claim on the figure must match the pixels.

    Requirement 5 is a disclosure rule, so a figure that says "no interpolation
    between cells" while drawing a bilinear ramp is worse than one that says
    nothing -- it makes a false statement about the data. Nearest-neighbour
    over 56 cells can change color at most ~55 times along a row; bilinear
    changes at nearly every pixel.
    """
    result = figures["velocity_magnitude_slice"]
    cells_across = mrf.velocity_magnitude_slice(
        CASE_ROOT, frame=mrf.DEFAULT_FRAME
    ).values.shape[0]

    transitions = _color_transitions_along_a_row(
        result, images["velocity_magnitude_slice"]
    )

    assert transitions <= cells_across, (
        "%d color transitions across a row of %d cells: the cells are being "
        "smoothed, contradicting the figure's own interpolation disclosure"
        % (transitions, cells_across)
    )


def test_a_bilinear_figure_would_fail_the_blockiness_check(tmp_path):
    """Control: bilinear really does blow past the cell count."""
    field = mrf.velocity_magnitude_slice(CASE_ROOT, frame=mrf.DEFAULT_FRAME)
    figure, result = mrf.build_figure(field)
    try:
        for axes in figure.axes:
            for image in axes.get_images():
                image.set_interpolation("bilinear")
        path = str(tmp_path / "bilinear.png")
        figure.savefig(path, dpi=100, facecolor=figure.get_facecolor(),
                       metadata={"Software": None})
    finally:
        matplotlib.pyplot.close(figure)
    result.path = path

    transitions = _color_transitions_along_a_row(result, load_rgb(path))

    assert transitions > field.values.shape[0], (
        "bilinear produced only %d transitions, so the check above cannot tell "
        "smoothed cells from sharp ones" % transitions
    )


# ---------------------------------------------------------------------------
# Resolution -- blind.audit() refuses a mismatched pair
# ---------------------------------------------------------------------------

def test_figures_are_exactly_the_default_resolution(figures, images):
    for key, result in figures.items():
        height, width = images[key].shape[:2]
        assert (width, height) == mrf.DEFAULT_RESOLUTION, (
            "%s is %dx%d, not %s; blind.audit() reads a size mismatch as an "
            "identity leak" % (key, width, height, mrf.DEFAULT_RESOLUTION)
        )


@pytest.mark.parametrize("resolution", [(1280, 720), (1920, 1080)])
def test_resolution_is_a_parameter(tmp_path, resolution):
    result = mrf.render_figure(
        mrf.velocity_magnitude_slice(CASE_ROOT, frame=mrf.DEFAULT_FRAME),
        str(tmp_path / "shot.png"),
        resolution=resolution,
    )
    height, width = load_rgb(result.path).shape[:2]
    assert (width, height) == resolution


def test_blind_audit_accepts_a_pair_of_generated_figures(figures, tmp_path):
    """End to end: two of our figures must survive the real audit.

    matplotlib stamps `Software: Matplotlib version...` into a PNG tEXt chunk
    by default. That does not name a renderer `blind.py` knows about today, but
    it is exactly the class of metadata the audit exists to catch, so the
    generator strips it.
    """
    shown = tmp_path / "shown"
    key = tmp_path / "key.json"
    sealed = seal(
        figures["velocity_magnitude_slice"].path,
        figures["vorticity_slice"].path,
        str(shown),
        str(key),
    )
    assert audit(sealed) == []


def test_figures_carry_no_png_text_metadata(figures):
    from blind import _png_text_chunks

    for key, result in figures.items():
        assert _png_text_chunks(result.path) == [], (
            "%s carries PNG text metadata" % key
        )


# ---------------------------------------------------------------------------
# Requirement 12 -- no clipped labels
# ---------------------------------------------------------------------------

@pytest.mark.parametrize(
    "key", ["velocity_magnitude_slice", "vorticity_slice", "vorticity_magnitude_mip"]
)
def test_no_artist_is_clipped_by_the_figure_edge(key):
    field = mrf.FIELD_BUILDERS[key](CASE_ROOT, frame=mrf.DEFAULT_FRAME)
    figure, _result = mrf.build_figure(field)
    try:
        figure.canvas.draw()
        tight = figure.get_tightbbox(figure.canvas.get_renderer())
        assert tight.x0 >= -0.02 and tight.y0 >= -0.02, (
            "%s: content extends past the bottom-left figure edge" % key
        )
        assert tight.x1 <= figure.get_size_inches()[0] + 0.02, (
            "%s: content is clipped on the right" % key
        )
        assert tight.y1 <= figure.get_size_inches()[1] + 0.02, (
            "%s: content is clipped at the top" % key
        )
    finally:
        matplotlib.pyplot.close(figure)


# ---------------------------------------------------------------------------
# The figures show something
# ---------------------------------------------------------------------------

@pytest.mark.parametrize(
    "key", ["velocity_magnitude_slice", "vorticity_slice", "vorticity_magnitude_mip"]
)
def test_the_data_region_is_neither_blank_nor_uniform(figures, images, key):
    cropped = axes_pixels(figures[key], images[key])
    assert cropped.size > 0
    colors = np.unique(np.round(cropped.reshape(-1, 3) * 255).astype(np.uint8), axis=0)
    assert len(colors) > 50, "%s: only %d distinct colors in the data axes" % (
        key,
        len(colors),
    )
    assert cropped.std() > 0.05, "%s: the data axes is nearly flat" % key


def test_the_vortex_street_is_visible_in_the_vorticity_figure(figures, images):
    """The structure that makes this case interesting must be in the picture."""
    result = figures["vorticity_slice"]
    lobes = vortex_street_lobes(result, images["vorticity_slice"])

    positive = [x for x, sign in lobes if sign > 0]
    negative = [x for x, sign in lobes if sign < 0]
    assert len(positive) >= 2, "fewer than two positive cores below the wake axis"
    assert len(negative) >= 2, "fewer than two negative cores above the wake axis"
    assert alternation_count(lobes) >= 3, (
        "cores do not alternate downstream (%r); a Karman street is staggered, "
        "and a picture without the stagger is not showing one" % (lobes,)
    )


def test_vortex_street_detector_rejects_a_featureless_field(tmp_path):
    """Control: a smooth ramp through the same renderer must find no street."""
    field = mrf.velocity_magnitude_slice(CASE_ROOT, frame=mrf.DEFAULT_FRAME)
    ramp = np.linspace(-1.0, 1.0, field.values.shape[0])[:, None] * np.ones(
        (1, field.values.shape[1])
    )
    featureless = mrf.replace_values(field, ramp * 16.0, diverging=True)

    result = mrf.render_figure(featureless, str(tmp_path / "ramp.png"))
    lobes = vortex_street_lobes(result, load_rgb(result.path))

    assert alternation_count(lobes) <= 1, (
        "the detector found an alternating street in a monotone ramp (%r), so "
        "it fires on anything" % (lobes,)
    )


def test_the_projection_is_not_a_copy_of_the_mid_plane_slice(figures, images):
    """A MIP that equals one slice is not a projection through the depth.

    Degenerate-data guard: the mock case is only six cells deep, so a
    projection that silently took a single layer would still look plausible.
    """
    mip = mrf.vorticity_magnitude_mip(CASE_ROOT, frame=mrf.DEFAULT_FRAME)
    slice_ = mrf.vorticity_magnitude_slice(CASE_ROOT, frame=mrf.DEFAULT_FRAME)

    difference = np.abs(
        np.nan_to_num(mip.values) - np.nan_to_num(slice_.values)
    )
    assert difference.max() > 0.01 * mip.vmax, (
        "the projection is identical to the mid-plane slice"
    )
    assert (difference > 1e-6).mean() > 0.05, (
        "the projection differs from the mid-plane slice in only %.2f%% of "
        "cells" % (100.0 * (difference > 1e-6).mean())
    )


def test_the_projection_takes_the_maximum_through_the_depth():
    mip = mrf.vorticity_magnitude_mip(CASE_ROOT, frame=mrf.DEFAULT_FRAME)
    slice_ = mrf.vorticity_magnitude_slice(CASE_ROOT, frame=mrf.DEFAULT_FRAME)
    valid = ~np.isnan(mip.values) & ~np.isnan(slice_.values)
    assert (mip.values[valid] >= slice_.values[valid] - 1e-6).all(), (
        "a maximum-intensity projection can never fall below any single layer"
    )


# ---------------------------------------------------------------------------
# Output contract
# ---------------------------------------------------------------------------

def test_generate_writes_into_the_scientific_subdirectory(tmp_path):
    results = mrf.generate(CASE_ROOT, str(tmp_path))
    assert results
    for result in results:
        assert os.path.isfile(result.path)
        assert os.path.basename(os.path.dirname(result.path)) == "scientific"


def test_generate_writes_a_provenance_sidecar(tmp_path):
    """VISUAL_QA 4 wants a finding to be reproducible from what was recorded."""
    results = mrf.generate(CASE_ROOT, str(tmp_path))
    sidecar = os.path.join(tmp_path, "scientific", "figures.json")
    assert os.path.isfile(sidecar)

    with open(sidecar) as handle:
        recorded = json.load(handle)

    assert recorded["profile"] == "Scientific"
    assert recorded["resolution"] == list(mrf.DEFAULT_RESOLUTION)
    assert len(recorded["figures"]) == len(results)
    for entry in recorded["figures"]:
        assert entry["colormap"]
        assert entry["unit"]
        assert entry["rangeMode"] in ("global", "frame")


def test_the_corpus_covers_the_three_requested_views(figures):
    assert set(figures) == {
        "velocity_magnitude_slice",
        "vorticity_slice",
        "vorticity_magnitude_mip",
    }


def test_review_would_find_the_generated_corpus(tmp_path):
    """`review.Session.references()` is what a real loop calls; it must see these."""
    from review import Session

    mrf.generate(CASE_ROOT, str(tmp_path))
    session = Session("Scientific", os.path.join(tmp_path, "scientific"), str(tmp_path))

    found = session.references()
    assert len(found) == 3
    assert all(path.endswith(".png") for path in found)
