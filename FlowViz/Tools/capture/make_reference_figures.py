#!/usr/bin/env python3
"""Generate the Scientific-profile reference corpus for blind comparison.

`references/README.md` states the problem plainly: a clean checkout cannot run
a real blind comparison until someone puts images in `references/`, and a
review run against an empty corpus has not compared anything. For the
Presentation profile the references are film and AAA stills that this project
cannot redistribute. For the **Scientific** profile they are ParaView/Tecplot/
JFM-quality figures -- and those we can make ourselves, from our own data, with
our own tools. That is what this script does.

WHAT MAKES THIS A LEGITIMATE REFERENCE AND NOT A STRAW MAN
----------------------------------------------------------
`references/README.md` warns against picking a reference the render can beat:
it produces a PASS that means nothing. So these figures are held to the same
`Docs/VISUAL_QA.md` section 1 bar the render is:

  1.2  Every pseudocolored view carries a scalar legend naming the field, its
       unit, and the numeric range.
  1.3  The range is the manifest's *global* range by default. A per-frame range
       is available and is stamped "per-frame auto range" on the figure.
  1.4  NaN and masked cells are drawn in a color no colormap here can produce,
       and the legend says what it means. Never zero, never the ramp minimum --
       painting the masked cylinder as viridis's darkest purple would invent a
       stagnation zone that is not in the data.
  1.5  Interpolation is disclosed. These use `interpolation="nearest"`: one
       cell is one block, so nothing on screen is smoother than the data.
  1.6  viridis by default. Never turbo or jet.
  1.7  Signed data (vorticity) uses coolwarm centered on zero, so the neutral
       color means zero everywhere and the sign is readable at a glance.
  1.12 Text is rendered at the figure's true pixel size -- no upscaling.

WHY THE COLORMAP COMES FROM cfdviz.colormaps
--------------------------------------------
`cfdviz/colormaps.py` is the single source of truth shared with the C++ side.
Its viridis is a nine-point table, and it differs from matplotlib's 256-entry
one by as much as 103/255 in a channel. Sampling matplotlib's here would make
the reference and the Unreal render disagree about what "viridis" means, and
that disagreement would show up in a blind review as a color-fidelity finding
against whichever image happened to be the candidate. The reference must use
the render's own table or the comparison is measuring the wrong thing.

RESOLUTION
----------
`blind.audit()` reports a size mismatch between the two presented images as an
identity leak and refuses the comparison -- if one is 3840 wide and the other
1920, the critic knows which is the film still without looking at a pixel. So
these are emitted at exactly 1920x1080 by default, matching a standard Unreal
capture, and `--resolution` moves both together.

USAGE
-----
    ./make_reference_figures.py                     # default case -> references/scientific/
    ./make_reference_figures.py --frame 7 --resolution 2560 1440
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import dataclass, field as dataclass_field
from typing import Callable

import numpy as np

import matplotlib

matplotlib.use("Agg")

import matplotlib.colors as mcolors
import matplotlib.pyplot as plt

_TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_FLOWVIZ = os.path.dirname(_TOOLS)
_CFDVIZ_SRC = os.path.join(_TOOLS, "cfdviz", "src")
if _CFDVIZ_SRC not in sys.path:
    sys.path.insert(0, _CFDVIZ_SRC)

from cfdviz.colormaps import build_lut  # noqa: E402
from cfdviz.cvf import read_cvf  # noqa: E402
from cfdviz.manifest import field_by_id, frame_path, grid_by_id, load_manifest  # noqa: E402

__all__ = [
    "DEFAULT_RESOLUTION",
    "DEFAULT_FRAME",
    "INVALID_COLOR",
    "FIELD_BUILDERS",
    "ScalarField",
    "FigureResult",
    "centered_norm",
    "colormap_samples",
    "project_colormap",
    "velocity_magnitude_slice",
    "vorticity_slice",
    "vorticity_magnitude_slice",
    "vorticity_magnitude_mip",
    "replace_values",
    "build_figure",
    "render_figure",
    "generate",
    "main",
]

#: 1920x1080 pairs with a standard Unreal capture without either side being
#: upscaled. Upscaling hides aliasing, which is one of the specific things the
#: review is looking for (VISUAL_QA section 4).
DEFAULT_RESOLUTION: tuple[int, int] = (1920, 1080)

#: Frame 10 of 20. Mid-sequence, so the shed street is fully developed and
#: several vortex pairs are in the domain at once; frame 0 has the flow still
#: organising itself near the cylinder.
DEFAULT_FRAME = 10

#: Color for NaN and masked cells. Magenta sits at least 22 OKLab units from
#: every color in viridis, coolwarm, and inferno -- comfortably past the 15
#: normal-vision floor -- so an invalid cell can never be mistaken for a valid
#: one. ParaView's dark-red default is only ~14 units from inferno and would
#: read as a hot region on that map.
INVALID_COLOR: tuple[float, float, float] = (1.0, 0.0, 1.0)

#: Ink colors. Recessive axes, primary ink for labels: the data is the only
#: thing on the page allowed to be saturated.
_INK = "#1a1a1a"
_MUTED_INK = "#5c5c5c"
_AXES_LINE = "#b0b0b0"

_DPI = 100.0


# ---------------------------------------------------------------------------
# Colormaps -- from the project's table, never matplotlib's
# ---------------------------------------------------------------------------

def colormap_samples(name: str, size: int = 256) -> np.ndarray:
    """``(size, 3)`` RGB samples of a project colormap, pixel-center sampled.

    Delegates to :func:`cfdviz.colormaps.build_lut`, which is the same table
    and the same sampling convention Unreal uploads to its 1D LUT texture.
    """
    return np.asarray(build_lut(name, size), dtype=np.float64)


def project_colormap(name: str, size: int = 256) -> mcolors.ListedColormap:
    """A matplotlib colormap backed by the project's table.

    The ``bad`` color is set here rather than at draw time so that every path
    that renders through this colormap -- image, colorbar, legend swatch --
    agrees about what an invalid cell looks like.
    """
    colormap = mcolors.ListedColormap(colormap_samples(name, size), name="cfdviz-" + name)
    colormap.set_bad(color=INVALID_COLOR)
    return colormap


def centered_norm(vmin: float, vmax: float) -> mcolors.Normalize:
    """A symmetric norm about zero, spanning the larger of ``|vmin|, |vmax|``.

    VISUAL_QA 1.7: diverging data is centered on the *meaningful* zero. A plain
    ``Normalize(vmin, vmax)`` over asymmetric signed data puts the neutral
    color at the data midpoint, so zero paints as "somewhat negative" and the
    sign of the field is no longer readable from the color. That is not a
    styling preference; it fabricates a bias the data does not have.
    """
    extent = max(abs(float(vmin)), abs(float(vmax)))
    if extent == 0.0:
        extent = 1.0
    return mcolors.Normalize(vmin=-extent, vmax=extent)


# ---------------------------------------------------------------------------
# Field extraction
# ---------------------------------------------------------------------------

@dataclass
class ScalarField:
    """A 2D scalar plane ready to draw, plus everything the legend must state.

    ``values`` is indexed ``[x, y]`` in the case's own axis order and carries
    NaN for invalid cells. Nothing downstream is allowed to fill those in.
    """

    key: str
    title: str
    subtitle: str
    field_name: str
    unit: str
    values: np.ndarray
    extent: tuple[float, float, float, float]
    colormap: str
    diverging: bool
    vmin: float
    vmax: float
    range_mode: str
    frame: int
    simulation_time: float
    case_name: str
    projection_note: str
    axis_labels: tuple[str, str] = ("x (m)", "y (m)")
    obstacle: tuple[float, float, float] | None = None
    metadata: dict = dataclass_field(default_factory=dict)


@dataclass
class FigureResult:
    """What was written, and the mapping a test needs to read it back."""

    key: str
    path: str
    colormap: str
    diverging: bool
    vmin: float
    vmax: float
    range_mode: str
    unit: str
    field_name: str
    frame: int
    resolution: tuple[int, int]
    extent: tuple[float, float, float, float]
    axes_bbox_px: tuple[float, float, float, float]
    invalid_color: tuple[float, float, float]
    norm_of_zero: float

    def as_dict(self) -> dict:
        return {
            "key": self.key,
            "file": os.path.basename(self.path),
            "field": self.field_name,
            "unit": self.unit,
            "colormap": self.colormap,
            "diverging": self.diverging,
            "range": [self.vmin, self.vmax],
            "rangeMode": self.range_mode,
            "frame": self.frame,
            "resolution": list(self.resolution),
            "invalidColor": list(self.invalid_color),
        }


def _grid_extent(manifest: dict) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    grid = grid_by_id(manifest, "main") or (manifest.get("grids") or [{}])[0]
    dimensions = np.asarray(grid["dimensions"], dtype=np.int64)
    origin = np.asarray(grid["origin"], dtype=np.float64)
    spacing = np.asarray(grid["spacing"], dtype=np.float64)
    return dimensions, origin, spacing


def _plane_extent(origin, spacing, dimensions) -> tuple[float, float, float, float]:
    """Cell-edge extent in metres, so a cell is drawn where it physically is.

    Cell-*centre* extents would shift the image by half a cell against the
    obstacle outline, which at this resolution is a visible half-cell offset
    between the mask and the cylinder it represents.
    """
    return (
        float(origin[0]),
        float(origin[0] + dimensions[0] * spacing[0]),
        float(origin[1]),
        float(origin[1] + dimensions[1] * spacing[1]),
    )


def _read_field(case_root: str, manifest: dict, field_id: str, frame: int) -> np.ndarray:
    entry = field_by_id(manifest, field_id)
    if entry is None:
        raise KeyError("the case declares no field %r" % field_id)
    pattern = entry["storage"]["pathPattern"]
    return read_cvf(frame_path(case_root, pattern, frame)).values.astype(np.float64)


def _apply_mask(case_root: str, manifest: dict, values: np.ndarray, frame: int,
                *, plane: int | None = None) -> np.ndarray:
    """Force masked cells to NaN.

    The sample case already stores NaN inside the cylinder, but a case whose
    mask rejects cells that still hold finite numbers would otherwise render
    those numbers as if they were physical. Spec 1.7 and 4.4.7 exclude masked
    cells from every statistic; excluding them from the picture is the same
    rule applied to pixels.

    ``plane`` selects a single z layer of the mask, for ``values`` that are
    already a 2D slice. Leave it None for a full volume.
    """
    grid = grid_by_id(manifest, "main") or (manifest.get("grids") or [{}])[0]
    mask_id = grid.get("maskField")
    if not mask_id:
        return values
    mask = _read_field(case_root, manifest, mask_id, frame)[..., 0]
    if plane is not None:
        mask = mask[:, :, plane]
    return np.where(mask != 0, values, np.nan)


def _mid_plane(dimensions: np.ndarray) -> int:
    """The z index of the cylinder mid-plane."""
    return int(dimensions[2]) // 2


def _global_range(manifest: dict, field_id: str, *, magnitude: bool
                  ) -> tuple[float, float]:
    statistics = field_by_id(manifest, field_id)["statistics"]
    if magnitude:
        return (
            float(statistics["globalMagnitudeMin"]),
            float(statistics["globalMagnitudeMax"]),
        )
    return (
        float(min(statistics["globalComponentMin"])),
        float(max(statistics["globalComponentMax"])),
    )


def _resolve_range(values: np.ndarray, range_mode: str,
                   global_range: tuple[float, float]) -> tuple[float, float]:
    if range_mode == "global":
        return global_range
    if range_mode != "frame":
        raise ValueError("range_mode must be 'global' or 'frame', got %r" % range_mode)
    if not np.isfinite(values).any():
        return global_range
    return float(np.nanmin(values)), float(np.nanmax(values))


def _case_context(case_root: str, frame: int):
    manifest = load_manifest(case_root)
    dimensions, origin, spacing = _grid_extent(manifest)
    times = (manifest.get("timeline") or {}).get("times") or [0.0]
    time = float(times[frame]) if frame < len(times) else 0.0
    name = (manifest.get("case") or {}).get("name", os.path.basename(case_root))
    return manifest, dimensions, origin, spacing, time, name


def _obstacle_circle(case_root: str, manifest: dict):
    """Centre and radius of the obstacle mesh in the xy plane, if there is one.

    Read from the mesh rather than assumed, so the outline cannot drift away
    from the geometry the render will draw.
    """
    from cfdviz.cvm import read_cvm

    for entry in manifest.get("meshes") or []:
        if entry.get("role") != "obstacle":
            continue
        mesh = read_cvm(os.path.join(case_root, entry["path"]))
        positions = np.asarray(mesh.positions, dtype=np.float64)
        center_x = 0.5 * (positions[:, 0].min() + positions[:, 0].max())
        center_y = 0.5 * (positions[:, 1].min() + positions[:, 1].max())
        radius = float(
            np.max(np.hypot(positions[:, 0] - center_x, positions[:, 1] - center_y))
        )
        return float(center_x), float(center_y), radius
    return None


def velocity_magnitude_slice(case_root: str, frame: int = DEFAULT_FRAME,
                             range_mode: str = "global") -> ScalarField:
    """|U| on the cylinder mid-plane.

    Computed from the stored vector rather than read from the `speed` field so
    the figure exercises the same magnitude arithmetic a renderer does, and so
    the manifest's `globalMagnitude*` statistics are the right range to state.
    """
    manifest, dimensions, origin, spacing, time, case_name = _case_context(
        case_root, frame
    )
    vectors = _read_field(case_root, manifest, "U", frame)
    plane = _mid_plane(dimensions)
    with np.errstate(invalid="ignore"):
        magnitude = np.sqrt((vectors[:, :, plane, :] ** 2).sum(axis=-1))
    # NaN in any component already poisons the magnitude -- it propagates
    # through the sum -- but the mask is applied as well, for the case where a
    # rejected cell still holds finite numbers.
    magnitude = _apply_mask(case_root, manifest, magnitude, frame, plane=plane)

    vmin, vmax = _resolve_range(
        magnitude, range_mode, _global_range(manifest, "U", magnitude=True)
    )
    return ScalarField(
        key="velocity_magnitude_slice",
        title="Velocity magnitude, cylinder mid-plane",
        subtitle="|U| on the z = %.3f m plane" % float(
            origin[2] + (plane + 0.5) * spacing[2]
        ),
        field_name="Velocity magnitude",
        unit="m/s",
        values=magnitude,
        extent=_plane_extent(origin, spacing, dimensions),
        colormap="viridis",
        diverging=False,
        vmin=vmin,
        vmax=vmax,
        range_mode=range_mode,
        frame=frame,
        simulation_time=time,
        case_name=case_name,
        projection_note="Slice: single cell layer, k = %d of %d" % (
            plane, int(dimensions[2])
        ),
        obstacle=_obstacle_circle(case_root, manifest),
    )


def vorticity_slice(case_root: str, frame: int = DEFAULT_FRAME,
                    range_mode: str = "global") -> ScalarField:
    """Spanwise vorticity omega_z on the cylinder mid-plane.

    The z component, not the magnitude: the magnitude of a signed field throws
    away the sign, and the sign is the whole story of a Karman street. The
    alternating red/blue stagger downstream of the cylinder *is* the shed
    vortex street, and it is only visible because the map is diverging and
    centered on zero.
    """
    manifest, dimensions, origin, spacing, time, case_name = _case_context(
        case_root, frame
    )
    vorticity = _read_field(case_root, manifest, "vorticity", frame)
    plane = _mid_plane(dimensions)
    omega_z = _apply_mask(
        case_root, manifest, vorticity[:, :, plane, 2], frame, plane=plane
    )

    statistics = field_by_id(manifest, "vorticity")["statistics"]
    global_range = (
        float(statistics["globalComponentMin"][2]),
        float(statistics["globalComponentMax"][2]),
    )
    vmin, vmax = _resolve_range(omega_z, range_mode, global_range)
    norm = centered_norm(vmin, vmax)

    return ScalarField(
        key="vorticity_slice",
        title="Spanwise vorticity, cylinder mid-plane",
        subtitle="$\\omega_z$ on the z = %.3f m plane; shed vortex street" % float(
            origin[2] + (plane + 0.5) * spacing[2]
        ),
        field_name="Vorticity $\\omega_z$",
        unit="1/s",
        values=omega_z,
        extent=_plane_extent(origin, spacing, dimensions),
        colormap="coolwarm",
        diverging=True,
        vmin=float(norm.vmin),
        vmax=float(norm.vmax),
        range_mode=range_mode,
        frame=frame,
        simulation_time=time,
        case_name=case_name,
        projection_note="Slice: single cell layer, k = %d of %d" % (
            plane, int(dimensions[2])
        ),
        obstacle=_obstacle_circle(case_root, manifest),
    )


def vorticity_magnitude_slice(case_root: str, frame: int = DEFAULT_FRAME,
                              range_mode: str = "global") -> ScalarField:
    """|omega| on the mid-plane. Not part of the corpus; the MIP's control."""
    manifest, dimensions, origin, spacing, time, case_name = _case_context(
        case_root, frame
    )
    values = _read_field(case_root, manifest, "vorticityMagnitude", frame)[..., 0]
    plane = _mid_plane(dimensions)
    layer = _apply_mask(case_root, manifest, values[:, :, plane], frame, plane=plane)

    vmin, vmax = _resolve_range(
        layer, range_mode, _global_range(manifest, "vorticityMagnitude", magnitude=True)
    )
    return ScalarField(
        key="vorticity_magnitude_slice",
        title="Vorticity magnitude, cylinder mid-plane",
        subtitle="$|\\omega|$ on the mid-plane",
        field_name="Vorticity magnitude",
        unit="1/s",
        values=layer,
        extent=_plane_extent(origin, spacing, dimensions),
        colormap="inferno",
        diverging=False,
        vmin=vmin,
        vmax=vmax,
        range_mode=range_mode,
        frame=frame,
        simulation_time=time,
        case_name=case_name,
        projection_note="Slice: single cell layer, k = %d of %d" % (
            plane, int(dimensions[2])
        ),
        obstacle=_obstacle_circle(case_root, manifest),
    )


def vorticity_magnitude_mip(case_root: str, frame: int = DEFAULT_FRAME,
                            range_mode: str = "global") -> ScalarField:
    """Maximum-intensity projection of |omega| through the spanwise depth.

    This is the closest 2D analogue of what the ray-marcher produces: every
    cell along the view ray contributes, and the brightest one wins. It is the
    figure to compare a volume render against, because a slice cannot show
    whether the march is integrating through the depth at all.

    A column that is invalid at every depth stays invalid -- collapsing it to
    zero would paint the cylinder as a quiet region rather than as a hole.
    """
    manifest, dimensions, origin, spacing, time, case_name = _case_context(
        case_root, frame
    )
    values = _apply_mask(
        case_root,
        manifest,
        _read_field(case_root, manifest, "vorticityMagnitude", frame)[..., 0],
        frame,
    )

    valid = ~np.isnan(values)
    filled = np.where(valid, values, -np.inf)
    projected = filled.max(axis=2)
    projected = np.where(valid.any(axis=2), projected, np.nan)

    vmin, vmax = _resolve_range(
        projected, range_mode,
        _global_range(manifest, "vorticityMagnitude", magnitude=True),
    )
    return ScalarField(
        key="vorticity_magnitude_mip",
        title="Vorticity magnitude, maximum-intensity projection",
        subtitle="max $|\\omega|$ along z through the full %d-cell depth" % int(
            dimensions[2]
        ),
        field_name="Vorticity magnitude",
        unit="1/s",
        values=projected,
        extent=_plane_extent(origin, spacing, dimensions),
        colormap="inferno",
        diverging=False,
        vmin=vmin,
        vmax=vmax,
        range_mode=range_mode,
        frame=frame,
        simulation_time=time,
        case_name=case_name,
        projection_note="Projection: maximum along z over %d cell layers" % int(
            dimensions[2]
        ),
        obstacle=_obstacle_circle(case_root, manifest),
    )


#: The corpus. `vorticity_magnitude_slice` is deliberately absent -- it exists
#: as the MIP's control, not as a reference.
FIELD_BUILDERS: dict[str, Callable[..., ScalarField]] = {
    "velocity_magnitude_slice": velocity_magnitude_slice,
    "vorticity_slice": vorticity_slice,
    "vorticity_magnitude_mip": vorticity_magnitude_mip,
}


def replace_values(source: ScalarField, values: np.ndarray, *,
                   diverging: bool | None = None) -> ScalarField:
    """A copy of ``source`` carrying different values. For tests and controls."""
    changed = dict(source.__dict__)
    changed["values"] = np.asarray(values, dtype=np.float64)
    if diverging is not None:
        changed["diverging"] = diverging
        changed["colormap"] = "coolwarm" if diverging else source.colormap
    return ScalarField(**changed)


# ---------------------------------------------------------------------------
# Drawing
# ---------------------------------------------------------------------------

def _norm_for(field: ScalarField) -> mcolors.Normalize:
    if field.diverging:
        return centered_norm(field.vmin, field.vmax)
    return mcolors.Normalize(vmin=field.vmin, vmax=field.vmax)


#: Layout, in figure fractions. The data axes is fitted to the domain's own
#: aspect ratio inside this box rather than given a fixed rectangle: the mock
#: domain is 12 m x 4 m, and a fixed rectangle either distorts it (forbidden --
#: VISUAL_QA 1.13 requires anisotropic spacing to render with correct
#: proportions) or leaves two thirds of a 16:9 canvas empty.
_LEFT = 0.055
_RIGHT_RESERVE = 0.150      # colorbar, its tick labels, and its axis label
_TOP = 0.860                # below the subtitle
_BOTTOM = 0.135             # above the provenance footer
_CBAR_GAP = 0.020
_CBAR_WIDTH = 0.013


def _layout(extent, resolution):
    """Fit the data axes to the domain aspect; return every panel rectangle.

    Returns ``(axes_rect, colorbar_rect, swatch_rect)`` as
    ``(x0, y0, width, height)`` in figure fractions.
    """
    width_px, height_px = resolution
    data_width = float(extent[1] - extent[0])
    data_height = float(extent[3] - extent[2])
    aspect = data_height / data_width if data_width else 1.0

    available_width = 1.0 - _LEFT - _RIGHT_RESERVE
    available_height = _TOP - _BOTTOM

    # Height the axes would need if it used the full available width.
    needed_height = available_width * width_px * aspect / height_px
    if needed_height <= available_height:
        width, height = available_width, needed_height
    else:
        height = available_height
        width = height * height_px / aspect / width_px

    x0 = _LEFT
    y0 = _BOTTOM + 0.5 * (available_height - height)

    colorbar = (x0 + width + _CBAR_GAP, y0, _CBAR_WIDTH, height)
    # The invalid swatch sits directly under the ramp it is not part of, at the
    # same width, so the two read as one legend.
    swatch = (colorbar[0], max(0.045, y0 - 0.075), _CBAR_WIDTH, 0.030)
    return (x0, y0, width, height), colorbar, swatch


def build_figure(field: ScalarField, resolution: tuple[int, int] = DEFAULT_RESOLUTION):
    """Draw ``field`` and return ``(figure, FigureResult)``.

    The figure is sized in inches at a fixed DPI so the requested pixel size is
    exact, and text is laid out at that true size -- never rendered small and
    upscaled, which is how labels end up blurry (VISUAL_QA 1.12).
    """
    width, height = resolution
    figure = plt.figure(figsize=(width / _DPI, height / _DPI), dpi=_DPI,
                        facecolor="white")

    axes_rect, colorbar_rect, swatch_rect = _layout(field.extent, resolution)
    axes = figure.add_axes(axes_rect)
    axes.set_facecolor("white")

    masked = np.ma.masked_invalid(np.asarray(field.values, dtype=np.float64))
    colormap = project_colormap(field.colormap)
    norm = _norm_for(field)

    image = axes.imshow(
        masked.T,                    # values are [x, y]; imshow wants [row, col]
        origin="lower",
        extent=field.extent,
        cmap=colormap,
        norm=norm,
        interpolation="nearest",     # one cell, one block. Disclosed below.
        aspect="equal",
        rasterized=False,
    )

    if field.obstacle is not None:
        center_x, center_y, radius = field.obstacle
        axes.add_patch(
            plt.Circle(
                (center_x, center_y), radius, fill=False, linewidth=1.4,
                edgecolor=_INK, zorder=5,
            )
        )

    axes.set_xlabel(field.axis_labels[0], color=_INK, fontsize=11)
    axes.set_ylabel(field.axis_labels[1], color=_INK, fontsize=11)
    axes.tick_params(colors=_MUTED_INK, labelsize=10, width=0.8)
    for spine in axes.spines.values():
        spine.set_edgecolor(_AXES_LINE)
        spine.set_linewidth(0.8)

    # --- Scalar legend (VISUAL_QA 1.2) -------------------------------------
    # Placed on its own axes rather than stolen from the data axes, so the
    # ramp is exactly as tall as the image it explains.
    colorbar = figure.colorbar(image, cax=figure.add_axes(colorbar_rect),
                               extend="neither")
    # The end ticks are the stated range. A colorbar whose top tick is 12 on a
    # range that reaches 13.5 invites the reader to misjudge the maximum.
    ticks = list(np.linspace(field.vmin, field.vmax, 7))
    colorbar.set_ticks(ticks)
    colorbar.ax.set_yticklabels(["%.4g" % tick for tick in ticks])
    colorbar.set_label(
        "%s  [%s]" % (field.field_name, field.unit), color=_INK, fontsize=11
    )
    colorbar.ax.tick_params(colors=_MUTED_INK, labelsize=9, width=0.8)
    colorbar.outline.set_edgecolor(_AXES_LINE)
    colorbar.outline.set_linewidth(0.8)

    range_note = (
        "global range" if field.range_mode == "global" else "PER-FRAME AUTO RANGE"
    )
    figure.text(
        colorbar_rect[0], colorbar_rect[1] + colorbar_rect[3] + 0.018,
        "range %.4g to %.4g\n%s" % (field.vmin, field.vmax, range_note),
        fontsize=9, color=_MUTED_INK, va="bottom", ha="left",
    )

    # --- Invalid swatch (VISUAL_QA 1.4) ------------------------------------
    # A distinct color nobody explains is a mystery. The swatch says what it
    # means and sits directly below the ramp it is not part of.
    invalid_axes = figure.add_axes(swatch_rect)
    invalid_axes.imshow(
        np.ones((1, 1, 3)) * np.asarray(INVALID_COLOR)[None, None, :],
        aspect="auto",
    )
    invalid_axes.set_xticks([])
    invalid_axes.set_yticks([])
    for spine in invalid_axes.spines.values():
        spine.set_edgecolor(_AXES_LINE)
        spine.set_linewidth(0.8)

    label_x = swatch_rect[0] + swatch_rect[2] + 0.006
    figure.text(
        label_x, swatch_rect[1] + 0.019, "invalid", fontsize=9, color=_INK,
        va="center", ha="left",
    )
    figure.text(
        label_x, swatch_rect[1] + 0.004, "(NaN / masked)", fontsize=8,
        color=_MUTED_INK, va="center", ha="left",
    )

    # --- Titles and provenance ---------------------------------------------
    figure.text(_LEFT, 0.960, field.title, fontsize=19, color=_INK, va="top")
    figure.text(_LEFT, 0.912, field.subtitle, fontsize=11.5, color=_MUTED_INK,
                va="top")

    provenance = "%s  |  frame %d, t = %.4g s  |  %s  |  colormap %s%s" % (
        field.case_name,
        field.frame,
        field.simulation_time,
        field.projection_note,
        field.colormap,
        " (diverging, centered on 0)" if field.diverging else "",
    )
    figure.text(_LEFT, 0.048, provenance, fontsize=9, color=_MUTED_INK, va="center")
    figure.text(
        _LEFT, 0.024,
        "Cell-wise display, nearest-neighbour (no interpolation between cells); "
        "no temporal interpolation. Invalid cells excluded from the color scale.",
        fontsize=8.5, color=_MUTED_INK, va="center",
    )

    figure.canvas.draw()
    bbox = axes.get_window_extent()
    # Figure pixel rows count up from the bottom; PNG rows count down.
    axes_bbox_px = (
        float(bbox.x0),
        float(height - bbox.y1),
        float(bbox.x1),
        float(height - bbox.y0),
    )

    result = FigureResult(
        key=field.key,
        path="",
        colormap=field.colormap,
        diverging=field.diverging,
        vmin=float(norm.vmin),
        vmax=float(norm.vmax),
        range_mode=field.range_mode,
        unit=field.unit,
        field_name=field.field_name,
        frame=field.frame,
        resolution=resolution,
        extent=field.extent,
        axes_bbox_px=axes_bbox_px,
        invalid_color=tuple(INVALID_COLOR),
        norm_of_zero=float(norm(0.0)),
    )
    return figure, result


def render_figure(field: ScalarField, path: str,
                  resolution: tuple[int, int] = DEFAULT_RESOLUTION) -> FigureResult:
    """Draw ``field`` and write it to ``path``. Returns the result record.

    PNG text metadata is stripped. matplotlib stamps ``Software: Matplotlib
    version...`` by default, and `blind.audit()` treats a tEXt chunk naming a
    renderer as an identity leak -- the reference must not announce which tool
    made it.
    """
    figure, result = build_figure(field, resolution=resolution)
    try:
        directory = os.path.dirname(os.path.abspath(path))
        if directory and not os.path.isdir(directory):
            os.makedirs(directory)
        figure.savefig(
            path,
            dpi=_DPI,
            facecolor=figure.get_facecolor(),
            metadata={"Software": None, "Creation Time": None},
        )
    finally:
        plt.close(figure)
    result.path = os.path.abspath(path)
    return result


def generate(case_root: str, output_root: str, *,
             frame: int = DEFAULT_FRAME,
             resolution: tuple[int, int] = DEFAULT_RESOLUTION,
             range_mode: str = "global") -> list[FigureResult]:
    """Write the whole Scientific corpus under ``output_root/scientific/``.

    `references/README.md` requires the profile directories to be kept apart:
    a reference from the wrong one produces a verdict that sounds authoritative
    and means nothing. Nothing here ever writes to ``presentation/``.
    """
    destination = os.path.join(output_root, "scientific")
    if not os.path.isdir(destination):
        os.makedirs(destination)

    results = []
    for key, builder in FIELD_BUILDERS.items():
        field = builder(case_root, frame=frame, range_mode=range_mode)
        results.append(
            render_figure(
                field,
                os.path.join(destination, "%s_f%04d.png" % (key, frame)),
                resolution=resolution,
            )
        )

    sidecar = {
        "profile": "Scientific",
        "case": os.path.abspath(case_root),
        "frame": frame,
        "resolution": list(resolution),
        "generator": os.path.basename(__file__),
        "note": (
            "Generated reference corpus for blind comparison. Regenerate with "
            "make_reference_figures.py; these PNGs are not committed."
        ),
        "figures": [result.as_dict() for result in results],
    }
    with open(os.path.join(destination, "figures.json"), "w") as handle:
        json.dump(sidecar, handle, indent=2)
        handle.write("\n")

    return results


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument(
        "--case",
        default=os.path.join(_FLOWVIZ, "Samples", "MockCylinderWake.cfdviz"),
        help="Case root to render (default: the mock cylinder wake sample).",
    )
    parser.add_argument(
        "--output",
        default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "references"),
        help="References root; figures land in <output>/scientific/.",
    )
    parser.add_argument("--frame", type=int, default=DEFAULT_FRAME)
    parser.add_argument(
        "--resolution", type=int, nargs=2, metavar=("WIDTH", "HEIGHT"),
        default=list(DEFAULT_RESOLUTION),
        help="Emit at this size. Must match the capture it will be paired "
             "against: blind.audit() refuses a mismatched pair.",
    )
    parser.add_argument(
        "--range", dest="range_mode", choices=("global", "frame"), default="global",
        help="Color range. 'global' uses the manifest statistics; 'frame' is "
             "auto-ranged per frame and is stamped on the figure as such.",
    )
    arguments = parser.parse_args(argv)

    results = generate(
        os.path.abspath(arguments.case),
        os.path.abspath(arguments.output),
        frame=arguments.frame,
        resolution=tuple(arguments.resolution),
        range_mode=arguments.range_mode,
    )
    for result in results:
        print(
            "%-32s %s  %s [%.4g, %.4g] %s"
            % (
                result.key,
                result.path,
                result.colormap,
                result.vmin,
                result.vmax,
                result.range_mode,
            )
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
