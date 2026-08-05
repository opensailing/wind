"""The synthetic cylinder-wake generator — plan section 7.

This module writes a complete, valid CFDViz 1.0 case containing an unsteady
wake behind a circular cylinder. It exists so that the Unreal renderer has
something to display before any real solver output exists, and so that every
reader in the project can be exercised against data with realistic structure:
an obstacle, masked cells, NaN, anisotropic spacing, and partial edge bricks.

**The data is not CFD.** It is a closed-form construction, and the manifest says
so verbatim in ``case.description`` (:data:`MOCK_DISCLAIMER`). Nothing here
solves Navier–Stokes; it superposes analytic pieces that *look* like a Kármán
street and are cheap, deterministic, and exactly differentiable on paper, which
is what makes the gradient code testable against a closed form.

Construction
------------

Velocity is a sum of three analytic terms, in canonical coordinates with X
streamwise, Y cross-stream and Z spanwise:

1. **Uniform inflow plus a potential-flow doublet.** Two-dimensional inviscid
   flow past a cylinder of radius ``R`` centred at ``(cx, cy)``::

       u = U (1 - R^2 (dx^2 - dy^2) / r^4)
       v = U (-2 R^2 dx dy / r^4)

   This is what makes the flow go *around* the obstacle rather than through it.
   It is singular at the cylinder centre, which is inside the masked region;
   ``r^2`` is floored at a fraction of ``R^2`` so no division ever produces an
   infinity, even in cells that are about to be discarded.

2. **A wake deficit.** A Gaussian in ``y`` that switches on downstream of the
   cylinder and spreads as it travels, subtracting momentum from the core of
   the wake. Without it the vortex street sits in undisturbed free stream and
   reads as decoration rather than as a wake.

3. **Convecting Lamb–Oseen vortices, alternating in sign.** Vortex ``n`` is
   released at ``t_n = n T / 2`` (``T = 1 / f``) at a fixed point just behind
   the cylinder, on alternating sides, and travels downstream at
   ``convection_ratio * U``. Its induced velocity is::

       u_theta(r) = Gamma / (2 pi r) * (1 - exp(-r^2 / rc^2))

   which is finite at ``r = 0``, unlike a point vortex. The ``0/0`` limit is
   evaluated by series rather than by dividing, so no warning is ever raised
   and the core value is exact.

Spanwise variation (plan section 7: "mild spanwise variation so the volume is
not perfectly extruded") enters as a per-vortex sinusoidal modulation of both
circulation and lateral position in ``Z``, plus a small spanwise velocity
component confined to the wake. It is what gives the volume genuine 3-D
structure — and therefore non-zero X and Y vorticity — instead of an extruded
2-D slice that would hide any bug in the third texture axis.

Derived fields
--------------

``vorticity`` and ``qCriterion`` come from second-order finite differences in
**physical coordinates** (:func:`velocity_gradient`), never in index space. On
an anisotropic grid — which the default domain is, at 12/128 x 4/64 x 1/24 —
the two answers differ by a factor of the spacing per axis. On an isotropic
grid with unit spacing they are identical, which is why the tests use a grid
whose three spacings are different and none of them 1.

``pressure`` uses a documented synthetic relationship, not a solved Poisson
equation::

    p = 0.5 * density * (U_inf^2 - |u|^2)

i.e. the incompressible Bernoulli relation with the free stream as the
reference state, so a vortex core reads as a pressure minimum. It is
qualitatively right and quantitatively meaningless, which is the whole point of
the disclaimer.

``passiveScalar`` is dye released in the near wake and carried by the vortices;
it is a marker for visualisation, not a transported quantity.

Masking
-------

Cells whose **centre** lies inside the cylinder are rejected by ``validMask``
and every other field stores ``NaN`` there. Both halves matter: the mask is
what statistics and rendering honour, and the NaN is what stops a reader that
ignores the mask from drawing a plausible-looking zero inside a solid. The
centre convention is spec 3.2's — ``origin + spacing * (i + 0.5)`` — and using
node coordinates instead shifts the obstacle by half a cell.
"""

from __future__ import annotations

import json
import math
import uuid
from dataclasses import dataclass, field as dataclass_field, replace
from pathlib import Path
from typing import Any, Final, Iterator, Sequence

import numpy as np

from . import FORMAT_VERSION, __version__
from .case import KNOWN_VALUES_NAME, write_known_values
from .codecs import codec_id_from_name
from .cvf import write_cvf
from .cvm import make_box_mesh, make_cylinder_mesh, write_cvm

__all__ = [
    "MOCK_DISCLAIMER",
    "MockCaseParameters",
    "WakeField",
    "curl",
    "default_parameters",
    "dump_json",
    "generate_mock_case",
    "low_resolution_parameters",
    "obstacle_mask",
    "q_criterion",
    "velocity_gradient",
]

#: The exact label plan section 7 requires in the manifest. Copied, not
#: paraphrased: a test asserts the manifest string equals this one character for
#: character, so softening the wording here fails the build rather than quietly
#: dropping the caveat from the UI.
MOCK_DISCLAIMER: Final = (
    "Synthetic visualization demonstration; not validation-grade CFD."
)

#: ``case.quality``, which the UI surfaces next to the case name.
MOCK_QUALITY: Final = "visualization-demo"

#: Namespace for deterministic case IDs. Fixed, so regenerating a case with the
#: same parameters reproduces the same id and therefore the same manifest bytes.
_CASE_NAMESPACE: Final = uuid.UUID("6f1f1c34-4b2a-4a8e-9e0f-0a2f4d6b8c10")

#: Patch IDs. Spec 5.3 names all four; the numbers are ours, the names are not.
PATCH_INLET: Final = 1
PATCH_OUTLET: Final = 2
PATCH_SIDE_WALLS: Final = 3
PATCH_CYLINDER_WALL: Final = 4

#: Below this, ``(1 - exp(-t)) / t`` is evaluated by its series instead of by
#: division. The series is *more* accurate here, not less: the subtraction in
#: the numerator loses every significant digit as ``t`` goes to zero.
_SERIES_CUTOFF: Final = 1e-8


# ---------------------------------------------------------------------------
# JSON that a strict parser will accept
# ---------------------------------------------------------------------------

def dump_json(payload: Any, *, indent: int = 2) -> str:
    """Serialize ``payload``, refusing to emit ``NaN`` or ``Infinity``.

    ``json.dumps`` emits bare ``NaN``, ``Infinity`` and ``-Infinity`` tokens by
    default. None of the three is JSON — RFC 8259 has no such literals — and
    Unreal's parser rejects the whole document when it meets one. A manifest
    that Python round-trips happily and Unreal refuses to load is exactly the
    cross-implementation trap this project has already been bitten by, so the
    failure is forced here, at write time, where the offending statistic can
    still be identified.

    Raises:
        ValueError: If any value in ``payload`` is NaN or infinite.
    """
    return json.dumps(payload, indent=indent, allow_nan=False)


# ---------------------------------------------------------------------------
# Parameters
# ---------------------------------------------------------------------------

def _as_triple(value: Sequence[float]) -> tuple[float, float, float]:
    values = tuple(float(v) for v in value)
    if len(values) != 3:
        raise ValueError(f"expected 3 components, got {len(values)}: {value!r}")
    return values  # type: ignore[return-value]


@dataclass(frozen=True)
class MockCaseParameters:
    """Everything the generator can be steered by.

    The defaults are the ones plan section 7 specifies and they are
    deterministic: two runs with the same parameters produce byte-identical
    files, including ``seed``, which is present even though nothing about the
    construction *needs* randomness. It perturbs vortex strength, lateral
    position and spanwise phase, which keeps the street from looking stamped
    out by a machine, and it gives the determinism test something that can
    actually fail — a generator that ignored its seed would satisfy
    "same seed, same bytes" trivially.

    Attributes:
        dimensions: Cell counts ``(nx, ny, nz)``. Each must be at least 3, the
            minimum for a second-order one-sided difference at a boundary.
        frame_count: Number of stored frames.
        frame_interval: Physical seconds between stored frames.
        domain: Physical extent ``(Lx, Ly, Lz)`` in metres. Deliberately
            anisotropic by default: 12 x 4 x 1 over 128 x 64 x 24 gives three
            different cell sizes, so an index-space gradient cannot hide.
        origin: Physical position of the grid corner.
        inlet_velocity: Uniform streamwise inflow speed, m/s.
        cylinder_radius: Obstacle radius, m.
        cylinder_center_fraction: Obstacle centre as a fraction of the domain
            in X and Y. The default puts it in the upstream third.
        shedding_frequency: Full shedding cycles per second. One vortex leaves
            each side per cycle, so vortices are released every ``1/(2f)``.
        circulation: Peak vortex circulation, m^2/s. Sign alternates.
        core_radius: Lamb–Oseen core radius, m.
        spanwise_perturbation: Relative amplitude of the spanwise modulation.
            Zero produces a perfectly extruded volume, which is exactly what
            plan section 7 forbids.
        convection_ratio: Vortex convection speed as a fraction of the inflow.
        lateral_offset_ratio: Half-width of the vortex street, in radii.
        wake_deficit_ratio: Peak momentum deficit, as a fraction of the inflow.
        density: Fluid density used by the synthetic pressure relation.
        seed: Reproducible jitter seed.
        codec: ``'none'``, ``'zlib'`` or ``'lz4'``.
        level: Codec level, or ``None`` for the codec's documented default.
        float_type: ``'float32'`` or ``'float16'`` for every non-mask field.
        brick_size: CVF brick extent in voxels.
        name: Case name, and the stem of the deterministic case id.
    """

    dimensions: tuple[int, int, int] = (128, 64, 24)
    frame_count: int = 90
    frame_interval: float = 0.02
    domain: tuple[float, float, float] = (12.0, 4.0, 1.0)
    origin: tuple[float, float, float] = (0.0, 0.0, 0.0)
    inlet_velocity: float = 7.5
    cylinder_radius: float = 0.3
    cylinder_center_fraction: tuple[float, float] = (1.0 / 3.0, 0.5)
    shedding_frequency: float = 2.5
    circulation: float = 2.25
    core_radius: float = 0.15
    spanwise_perturbation: float = 0.15
    convection_ratio: float = 0.8
    lateral_offset_ratio: float = 1.1
    wake_deficit_ratio: float = 0.35
    density: float = 1.0
    seed: int = 20260804
    codec: str = "zlib"
    level: int | None = None
    float_type: str = "float32"
    brick_size: tuple[int, int, int] = (32, 32, 32)
    name: str = "Mock Cylinder Wake"

    def __post_init__(self) -> None:
        object.__setattr__(self, "dimensions", tuple(int(n) for n in self.dimensions))
        object.__setattr__(self, "domain", _as_triple(self.domain))
        object.__setattr__(self, "origin", _as_triple(self.origin))
        object.__setattr__(self, "brick_size", tuple(int(n) for n in self.brick_size))
        object.__setattr__(
            self,
            "cylinder_center_fraction",
            tuple(float(v) for v in self.cylinder_center_fraction),
        )

        if len(self.dimensions) != 3:
            raise ValueError(f"dimensions must have 3 entries: {self.dimensions!r}")
        if min(self.dimensions) < 3:
            # np.gradient's second-order edge formula needs three samples, and
            # a two-cell axis cannot describe a gradient at all. Rejecting it
            # here beats an obscure numpy error thirty frames in.
            raise ValueError(
                f"every dimension must be at least 3 for second-order finite "
                f"differences; got {self.dimensions}"
            )
        if self.frame_count < 1:
            raise ValueError(f"frame_count must be >= 1; got {self.frame_count}")
        if self.frame_interval <= 0:
            raise ValueError(
                f"frame_interval must be > 0 so times are strictly monotonic; got "
                f"{self.frame_interval}"
            )
        if min(self.domain) <= 0:
            raise ValueError(f"every domain extent must be > 0; got {self.domain}")
        if self.cylinder_radius <= 0:
            raise ValueError(
                f"cylinder_radius must be > 0; got {self.cylinder_radius}"
            )
        if self.core_radius <= 0:
            raise ValueError(f"core_radius must be > 0; got {self.core_radius}")
        if self.shedding_frequency <= 0:
            raise ValueError(
                f"shedding_frequency must be > 0; got {self.shedding_frequency}"
            )
        if self.float_type not in ("float16", "float32"):
            raise ValueError(
                f"float_type must be 'float16' or 'float32'; got "
                f"{self.float_type!r}"
            )
        codec_id_from_name(self.codec)  # raises UnsupportedCodecError if unknown

    # -- derived geometry ---------------------------------------------------

    @property
    def spacing(self) -> tuple[float, float, float]:
        """Cell size per axis. Anisotropic by default, and that is deliberate."""
        return tuple(  # type: ignore[return-value]
            length / count for length, count in zip(self.domain, self.dimensions)
        )

    @property
    def cylinder_center(self) -> tuple[float, float]:
        """``(cx, cy)`` of the obstacle axis, in physical coordinates."""
        return (
            self.origin[0] + self.domain[0] * self.cylinder_center_fraction[0],
            self.origin[1] + self.domain[1] * self.cylinder_center_fraction[1],
        )

    @property
    def times(self) -> list[float]:
        """Physical time of each stored frame, strictly increasing."""
        return [round(index * self.frame_interval, 12)
                for index in range(self.frame_count)]

    @property
    def case_id(self) -> str:
        """A UUID derived from every parameter, so it is stable but not shared.

        Two cases that differ in resolution or seed are different cases and get
        different ids; regenerating the same case reproduces its id exactly,
        which is what keeps the whole output byte-identical between runs.
        """
        signature = "|".join(
            f"{key}={value!r}" for key, value in sorted(self.as_dict().items())
        )
        return str(uuid.uuid5(_CASE_NAMESPACE, signature))

    def as_dict(self) -> dict[str, Any]:
        """Every parameter as plain JSON-serializable data, for provenance."""
        return {
            "dimensions": list(self.dimensions),
            "frameCount": self.frame_count,
            "frameInterval": self.frame_interval,
            "domain": list(self.domain),
            "origin": list(self.origin),
            "inletVelocity": self.inlet_velocity,
            "cylinderRadius": self.cylinder_radius,
            "cylinderCenterFraction": list(self.cylinder_center_fraction),
            "sheddingFrequency": self.shedding_frequency,
            "circulation": self.circulation,
            "coreRadius": self.core_radius,
            "spanwisePerturbation": self.spanwise_perturbation,
            "convectionRatio": self.convection_ratio,
            "lateralOffsetRatio": self.lateral_offset_ratio,
            "wakeDeficitRatio": self.wake_deficit_ratio,
            "density": self.density,
            "seed": self.seed,
            "codec": self.codec,
            "level": self.level,
            "floatType": self.float_type,
            "brickSize": list(self.brick_size),
            "name": self.name,
        }


def default_parameters(**overrides: Any) -> MockCaseParameters:
    """The plan's default case: 128 x 64 x 24, 90 frames, dt = 0.02 s."""
    return MockCaseParameters(**overrides)


def low_resolution_parameters(**overrides: Any) -> MockCaseParameters:
    """The small preset that is committed to source control.

    Plan section 7 allows either a committed low-resolution sample or one
    generated at project setup. A committed one is strictly better for the demo
    map, which must auto-load a case on first launch and therefore cannot
    depend on anyone having run a Python script first.

    The numbers are the largest that stay comfortably inside a few megabytes:
    the grid is coarse but still resolves the obstacle across four cells,
    ``float16`` halves every payload, ``level = 9`` squeezes the rest, and 20
    frames at 0.05 s cover two and a half shedding cycles so the wake is
    visibly unsteady rather than a still frame that jitters.

    The cylinder is *larger* here than in the default case, not smaller. Keeping
    the default 0.3 m radius would leave the obstacle 2.4 cells across on this
    grid — too coarse to read as a circle, and too coarse for the masking to be
    worth checking.
    """
    settings: dict[str, Any] = {
        "name": "Mock Cylinder Wake (low resolution)",
        "dimensions": (56, 28, 6),
        "frame_count": 20,
        "frame_interval": 0.05,
        "cylinder_radius": 0.45,
        "core_radius": 0.22,
        "float_type": "float16",
        "level": 9,
        "brick_size": (32, 32, 8),
    }
    settings.update(overrides)
    return MockCaseParameters(**settings)


# ---------------------------------------------------------------------------
# Finite differences in physical coordinates
# ---------------------------------------------------------------------------

def _edge_order(length: int) -> int:
    """Second order where there is room for it, first order otherwise."""
    return 2 if length >= 3 else 1


def velocity_gradient(velocity: np.ndarray,
                      spacing: Sequence[float]) -> np.ndarray:
    """The velocity Jacobian ``J[..., i, j] = d u_i / d x_j``.

    Differentiation is with respect to **physical** coordinates: each axis is
    divided by its own cell size. Doing it in index space instead is the classic
    error, and it is undetectable on an isotropic grid with unit spacing because
    the two are then the same computation. Hence the three separate spacings.

    Args:
        velocity: ``(X, Y, Z, 3)``.
        spacing: Physical cell size along X, Y, Z. Every entry must be > 0.

    Returns:
        ``(X, Y, Z, 3, 3)`` float64.

    Raises:
        ValueError: On a wrong shape or a non-positive spacing.
    """
    values = np.asarray(velocity, dtype=np.float64)
    if values.ndim != 4 or values.shape[3] != 3:
        raise ValueError(
            f"velocity must be (X, Y, Z, 3); got shape {values.shape}"
        )
    steps = _as_triple(spacing)
    if min(steps) <= 0:
        raise ValueError(f"every spacing component must be > 0; got {steps}")

    jacobian = np.empty(values.shape[:3] + (3, 3), dtype=np.float64)
    for axis, step in enumerate(steps):
        derivative = np.gradient(
            values, step, axis=axis, edge_order=_edge_order(values.shape[axis])
        )
        jacobian[..., :, axis] = derivative
    return jacobian


def curl(velocity: np.ndarray, spacing: Sequence[float]) -> np.ndarray:
    """Vorticity ``omega = curl(u)``, in physical coordinates.

    Args:
        velocity: ``(X, Y, Z, 3)``.
        spacing: Physical cell size along X, Y, Z.

    Returns:
        ``(X, Y, Z, 3)`` float64.
    """
    jacobian = velocity_gradient(velocity, spacing)
    return np.stack(
        [
            jacobian[..., 2, 1] - jacobian[..., 1, 2],
            jacobian[..., 0, 2] - jacobian[..., 2, 0],
            jacobian[..., 1, 0] - jacobian[..., 0, 1],
        ],
        axis=-1,
    )


def q_criterion(jacobian: np.ndarray) -> np.ndarray:
    """``Q = 0.5 * (|Omega|^2 - |S|^2)`` from a velocity Jacobian.

    ``S`` and ``Omega`` are the symmetric and antisymmetric parts of ``J`` and
    ``|A|^2 = A_ij A_ij``. Positive ``Q`` marks regions where rotation dominates
    strain, i.e. vortex cores.

    Note that pure shear gives ``Q = 0`` for *any* spacing, so a shear-only test
    cannot tell a physical-coordinate gradient from an index-space one; the
    tests use rotation plus strain, where the answer depends on the spacing.

    Args:
        jacobian: ``(..., 3, 3)`` as returned by :func:`velocity_gradient`.

    Returns:
        ``(...)`` float64.
    """
    values = np.asarray(jacobian, dtype=np.float64)
    if values.shape[-2:] != (3, 3):
        raise ValueError(f"jacobian must be (..., 3, 3); got {values.shape}")
    transposed = np.swapaxes(values, -1, -2)
    strain = 0.5 * (values + transposed)
    rotation = 0.5 * (values - transposed)
    return 0.5 * (
        (rotation * rotation).sum(axis=(-2, -1))
        - (strain * strain).sum(axis=(-2, -1))
    )


# ---------------------------------------------------------------------------
# The analytic wake
# ---------------------------------------------------------------------------

def _one_minus_exp_over(t: np.ndarray) -> np.ndarray:
    """``(1 - exp(-t)) / t``, finite and accurate at ``t = 0`` where it is 1.

    Two things are being avoided. Dividing by ``t`` raises a divide-by-zero
    warning, and this package promotes ``RuntimeWarning`` to an error. And the
    numerator suffers catastrophic cancellation for small ``t``, so even a
    guarded division loses precision exactly where the vortex core is. Both
    branches are evaluated unconditionally on clamped input, then selected —
    ``np.where`` evaluates both arms, so the clamp is what keeps the unused one
    quiet.
    """
    safe = np.maximum(t, _SERIES_CUTOFF)
    exact = -np.expm1(-safe) / safe
    series = 1.0 - 0.5 * t + t * t / 6.0
    return np.where(t > _SERIES_CUTOFF, exact, series)


def _smoothstep(edge0: float, edge1: float, values: np.ndarray) -> np.ndarray:
    """Hermite smoothstep, clamped outside ``[edge0, edge1]``."""
    t = np.clip((values - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


class WakeField:
    """The closed-form velocity and scalar fields of the mock case.

    One instance owns the grid coordinates and the per-vortex jitter, so
    evaluating many frames costs one allocation each rather than rebuilding the
    coordinate arrays 90 times.
    """

    def __init__(self, parameters: MockCaseParameters) -> None:
        self.parameters = parameters
        nx, ny, nz = parameters.dimensions
        sx, sy, sz = parameters.spacing
        ox, oy, oz = parameters.origin

        # Spec 3.2: a cell value lives at origin + spacing * (i + 0.5). Using
        # origin + spacing * i instead shifts every field, and the obstacle
        # with it, by half a cell.
        self.x = (ox + sx * (np.arange(nx) + 0.5)).astype(np.float64)
        self.y = (oy + sy * (np.arange(ny) + 0.5)).astype(np.float64)
        self.z = (oz + sz * (np.arange(nz) + 0.5)).astype(np.float64)

        self._x3 = self.x[:, None, None]
        self._y3 = self.y[None, :, None]
        self._z3 = self.z[None, None, :]

        cx, cy = parameters.cylinder_center
        self._dx = self._x3 - cx
        self._dy = self._y3 - cy
        # Floored well inside the obstacle, so the doublet stays finite in cells
        # that are about to be masked. Outside the cylinder the floor never
        # binds and the expression is exact.
        floor = (0.25 * parameters.cylinder_radius) ** 2
        self._r2 = np.maximum(self._dx**2 + self._dy**2, floor)

        # Vortices are released from just behind the shoulder of the cylinder,
        # alternating sides, and convect downstream.
        self.shed_x = cx + 1.2 * parameters.cylinder_radius
        self.shed_y = cy
        self.lateral = parameters.lateral_offset_ratio * parameters.cylinder_radius
        self.convection = parameters.convection_ratio * parameters.inlet_velocity
        self.release_interval = 0.5 / parameters.shedding_frequency

        # The clock starts with a street already in place: a first frame of
        # undisturbed free stream would make the case look broken for the first
        # second of playback.
        self.warmup = _WARMUP_RELEASES * self.release_interval

        span = float(parameters.domain[2])
        self.spanwise_wavenumber = 2.0 * math.pi / span

        # Pre-draw jitter for every vortex the run will ever release, so the
        # values a vortex carries do not depend on which frame first asked for
        # it. Drawing lazily would make the output depend on evaluation order.
        total = self._release_index(
            parameters.times[-1] if parameters.times else 0.0
        ) + 2
        rng = np.random.default_rng(parameters.seed)
        self.jitter_strength = 1.0 + 0.15 * rng.uniform(-1.0, 1.0, size=total)
        self.jitter_lateral = 1.0 + 0.10 * rng.uniform(-1.0, 1.0, size=total)
        self.jitter_phase = rng.uniform(0.0, 2.0 * math.pi, size=total)

    # -- vortex bookkeeping -------------------------------------------------

    def _release_index(self, time: float) -> int:
        """Index of the most recently released vortex at ``time``."""
        return int(math.floor((time + self.warmup) / self.release_interval))

    def _release_time(self, index: int) -> float:
        return index * self.release_interval - self.warmup

    def active_vortices(self, time: float) -> list[int]:
        """Indices of vortices currently inside the domain, upstream first.

        A vortex leaves the list once it has convected past the outlet; keeping
        it would cost time and change nothing, since its induced velocity has
        already decayed below the free stream by then.
        """
        newest = self._release_index(time)
        limit = self.parameters.origin[0] + self.parameters.domain[0]
        indices: list[int] = []
        for index in range(newest, -1, -1):
            age = time - self._release_time(index)
            if age < 0.0:
                continue
            if self.shed_x + self.convection * age > limit + 2.0 * self.lateral:
                break
            indices.append(index)
        return sorted(indices)

    def vortex_circulation_at(self, index: int, time: float) -> float:
        """Signed circulation of vortex ``index``, including its birth ramp.

        The sign alternates with ``index``: that is what makes the wake a Kármán
        street rather than a jet. Every other factor is strictly positive, so
        the alternation survives the jitter and the ramp.
        """
        age = max(time - self._release_time(index), 0.0)
        sign = -1.0 if index % 2 == 0 else 1.0
        ramp = -math.expm1(-age / (0.35 * self.release_interval))
        decay = math.exp(-age / (12.0 * self.release_interval))
        strength = float(self.jitter_strength[index % len(self.jitter_strength)])
        return sign * self.parameters.circulation * strength * ramp * decay

    def _vortex_state(self, index: int, time: float):
        """``(xc, yc(z), gamma(z))`` for one vortex, as broadcastable arrays."""
        parameters = self.parameters
        age = max(time - self._release_time(index), 0.0)
        gamma = self.vortex_circulation_at(index, time)
        side = 1.0 if index % 2 == 0 else -1.0
        offset = side * self.lateral * float(
            self.jitter_lateral[index % len(self.jitter_lateral)]
        )
        phase = float(self.jitter_phase[index % len(self.jitter_phase)])

        wave = np.sin(self.spanwise_wavenumber * self._z3 + phase)
        xc = self.shed_x + self.convection * age
        # Both the lateral position and the strength breathe along the span, so
        # no two spanwise slices are the same picture.
        yc = self.shed_y + offset + (
            parameters.spanwise_perturbation * self.lateral * wave
        )
        strength = gamma * (1.0 + parameters.spanwise_perturbation * wave)
        return xc, yc, strength

    # -- fields -------------------------------------------------------------

    def velocity(self, time: float) -> np.ndarray:
        """Velocity at ``time`` as ``(X, Y, Z, 3)`` float64, before masking."""
        parameters = self.parameters
        inflow = parameters.inlet_velocity
        radius2 = parameters.cylinder_radius**2

        shape = parameters.dimensions
        u = np.empty(shape, dtype=np.float64)
        v = np.empty(shape, dtype=np.float64)
        w = np.zeros(shape, dtype=np.float64)

        # 1. Uniform inflow plus a potential-flow doublet: the flow goes around
        #    the obstacle instead of through it.
        r4 = self._r2 * self._r2
        u[...] = inflow * (1.0 - radius2 * (self._dx**2 - self._dy**2) / r4)
        v[...] = inflow * (-2.0 * radius2 * self._dx * self._dy / r4)

        # 2. Wake deficit: a Gaussian in y that switches on behind the cylinder
        #    and spreads downstream.
        downstream = np.maximum(self._dx, 0.0)
        gate = _smoothstep(
            0.0, 1.5 * parameters.cylinder_radius, self._dx * np.ones_like(u)
        )
        half_width = parameters.cylinder_radius * (
            1.0 + 0.35 * downstream / max(parameters.cylinder_radius, 1e-12)
        )
        deficit = (
            parameters.wake_deficit_ratio
            * inflow
            * gate
            * np.exp(-((self._dy / half_width) ** 2))
            * np.exp(-downstream / (6.0 * parameters.domain[0]))
        )
        u -= deficit

        # 3. Convecting Lamb-Oseen vortices, alternating in sign.
        core2 = parameters.core_radius**2
        for index in self.active_vortices(time):
            xc, yc, strength = self._vortex_state(index, time)
            px = self._x3 - xc
            py = self._y3 - yc
            r2 = px * px + py * py
            # u_theta = G/(2 pi r) (1 - exp(-r^2/rc^2)), written so the r -> 0
            # limit is evaluated rather than divided into.
            factor = (strength / (2.0 * math.pi * core2)) * _one_minus_exp_over(
                r2 / core2
            )
            u += -factor * py
            v += factor * px

        # Spanwise velocity: small, confined to the wake, and enough to give the
        # volume genuine three-dimensional structure. Without it the X and Y
        # vorticity components would come only from the vortex modulation.
        if parameters.spanwise_perturbation:
            swirl = (
                0.25
                * parameters.spanwise_perturbation
                * inflow
                * gate
                * np.exp(-((self._dy / (2.0 * parameters.cylinder_radius)) ** 2))
                * np.sin(
                    self.spanwise_wavenumber * self._z3
                    + 2.0 * math.pi * parameters.shedding_frequency * time
                )
            )
            w += np.broadcast_to(swirl, shape)

        return np.stack([u, v, w], axis=-1)

    def passive_scalar(self, time: float) -> np.ndarray:
        """Dye released in the near wake and carried by the vortices, in [0, 1].

        A visualisation marker, not a transported quantity: nothing here solves
        an advection equation.
        """
        parameters = self.parameters
        shape = parameters.dimensions
        values = np.zeros(shape, dtype=np.float64)

        gate = _smoothstep(
            0.0, 1.5 * parameters.cylinder_radius,
            self._dx * np.ones(shape, dtype=np.float64),
        )
        band = (
            0.45
            * gate
            * np.exp(-((self._dy / (1.6 * parameters.cylinder_radius)) ** 2))
            * np.exp(-np.maximum(self._dx, 0.0) / (0.5 * parameters.domain[0]))
        )
        values += np.broadcast_to(band, shape)

        core2 = 2.0 * parameters.core_radius**2
        for index in self.active_vortices(time):
            xc, yc, strength = self._vortex_state(index, time)
            px = self._x3 - xc
            py = self._y3 - yc
            bump = np.exp(-(px * px + py * py) / core2)
            values += np.broadcast_to(
                0.85 * np.abs(strength) / max(parameters.circulation, 1e-12) * bump,
                shape,
            )
        return np.clip(values, 0.0, 1.0)


#: How many vortices are already in the wake at t = 0. Enough to fill the
#: domain, so playback opens on a developed street rather than on free stream.
_WARMUP_RELEASES: Final = 10


def obstacle_mask(parameters: MockCaseParameters) -> np.ndarray:
    """``True`` where the cell is fluid, ``False`` inside the cylinder.

    The test is on the cell **centre** (spec 3.2). This is the same array the
    generator writes as ``validMask``, exposed so a caller can reason about the
    obstacle without generating a case.
    """
    nx, ny, nz = parameters.dimensions
    sx, sy, _ = parameters.spacing
    ox, oy, _ = parameters.origin
    cx, cy = parameters.cylinder_center

    dx = (ox + sx * (np.arange(nx) + 0.5) - cx)[:, None]
    dy = (oy + sy * (np.arange(ny) + 0.5) - cy)[None, :]
    inside = (dx * dx + dy * dy) < parameters.cylinder_radius**2
    return np.repeat((~inside)[:, :, None], nz, axis=2)


# ---------------------------------------------------------------------------
# Field table
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class _FieldSpec:
    numeric_id: int
    field_id: str
    name: str
    components: tuple[str, ...]
    unit: str
    semantic: str
    colormap: str
    description: str
    data_type: str = ""          # "" means "the case's float type"
    codec: str = ""              # "" means "the case's codec"
    range_mode: str = "global"


_FIELDS: Final[tuple[_FieldSpec, ...]] = (
    _FieldSpec(
        1, "validMask", "Valid cell mask", ("valid",), "1", "mask", "grayscale",
        "0 inside the cylinder, 1 in the fluid. Cells rejected here are "
        "excluded from every statistic and should not be rendered.",
        data_type="uint8", codec="none", range_mode="manual",
    ),
    _FieldSpec(
        2, "U", "Velocity", ("x", "y", "z"), "m/s", "velocity", "viridis",
        "Uniform inflow, a potential-flow doublet around the cylinder, a wake "
        "deficit, and convecting alternating Lamb-Oseen vortices.",
    ),
    _FieldSpec(
        3, "pressure", "Pressure", ("p",), "Pa", "pressure", "coolwarm",
        "Synthetic: p = 0.5 * density * (U_inf^2 - |u|^2), the incompressible "
        "Bernoulli relation referenced to the free stream. Not a solved "
        "pressure field.",
    ),
    _FieldSpec(
        4, "speed", "Speed", ("magnitude",), "m/s", "velocity-magnitude", "viridis",
        "|u|, the magnitude of the stored velocity.",
    ),
    _FieldSpec(
        5, "vorticity", "Vorticity", ("x", "y", "z"), "1/s", "vorticity", "coolwarm",
        "curl(u), by second-order finite differences in physical coordinates.",
    ),
    _FieldSpec(
        6, "vorticityMagnitude", "Vorticity magnitude", ("magnitude",), "1/s",
        "vorticity-magnitude", "inferno", "|curl(u)|.",
    ),
    _FieldSpec(
        7, "qCriterion", "Q criterion", ("q",), "1/s2", "q-criterion", "plasma",
        "Q = 0.5 * (|Omega|^2 - |S|^2) from the velocity Jacobian, in physical "
        "coordinates. Positive values mark vortex cores.",
    ),
    _FieldSpec(
        8, "passiveScalar", "Passive scalar", ("s",), "1", "scalar", "magma",
        "Dye released in the near wake and carried by the vortices, clamped to "
        "[0, 1]. A visualisation marker, not a transported quantity.",
    ),
)


def _storage_dtype(spec: _FieldSpec, parameters: MockCaseParameters) -> str:
    return spec.data_type or parameters.float_type


def _storage_codec(spec: _FieldSpec, parameters: MockCaseParameters) -> str:
    return spec.codec or parameters.codec


# ---------------------------------------------------------------------------
# Statistics, accumulated over the exact bytes that were written
# ---------------------------------------------------------------------------

class _StatisticsAccumulator:
    """Per-component min/max and magnitude range, ignoring NaN and masked cells.

    The arrays folded in here are the *stored* ones — already cast to the
    field's storage dtype — so the declared statistics are exactly what a reader
    recomputes from the file, not what the generator happened to hold in
    float64 before rounding. Declaring the pre-cast numbers is a real bug: it
    puts a min in the manifest that no value in the case actually attains.
    """

    def __init__(self, components: int) -> None:
        self.minimum = np.full(components, np.inf, dtype=np.float64)
        self.maximum = np.full(components, -np.inf, dtype=np.float64)
        self.magnitude_min = math.inf
        self.magnitude_max = -math.inf
        self.components = components

    def add(self, values: np.ndarray, mask: np.ndarray | None) -> None:
        data = np.asarray(values).astype(np.float64).reshape(-1, self.components)
        valid = ~np.isnan(data)
        if mask is not None:
            valid = valid & np.asarray(mask).reshape(-1, 1)
        if not valid.any():
            return
        self.minimum = np.minimum(
            self.minimum, np.where(valid, data, np.inf).min(axis=0)
        )
        self.maximum = np.maximum(
            self.maximum, np.where(valid, data, -np.inf).max(axis=0)
        )
        rows = valid.all(axis=1)
        if rows.any():
            magnitude = np.sqrt((data[rows] ** 2).sum(axis=1))
            self.magnitude_min = min(self.magnitude_min, float(magnitude.min()))
            self.magnitude_max = max(self.magnitude_max, float(magnitude.max()))

    def to_manifest(self) -> dict[str, Any] | None:
        """The ``statistics`` object, or ``None`` when nothing valid was seen.

        A field with no valid cell anywhere has ``+inf``/``-inf`` statistics
        (spec 4.4.7), and JSON cannot express either. Omitting the block is
        honest; writing ``Infinity`` would produce a manifest that Unreal's
        parser rejects outright.
        """
        if not np.all(np.isfinite(self.minimum)) or not np.all(
            np.isfinite(self.maximum)
        ):
            return None
        statistics: dict[str, Any] = {
            "globalComponentMin": [float(v) for v in self.minimum],
            "globalComponentMax": [float(v) for v in self.maximum],
        }
        if math.isfinite(self.magnitude_min) and math.isfinite(self.magnitude_max):
            statistics["globalMagnitudeMin"] = self.magnitude_min
            statistics["globalMagnitudeMax"] = self.magnitude_max
        return statistics


# ---------------------------------------------------------------------------
# Generation
# ---------------------------------------------------------------------------

def _frame_fields(field: WakeField, time: float,
                  parameters: MockCaseParameters) -> dict[str, np.ndarray]:
    """Every field for one frame, in float64, before masking or casting."""
    velocity = field.velocity(time)
    jacobian = velocity_gradient(velocity, parameters.spacing)
    vorticity = np.stack(
        [
            jacobian[..., 2, 1] - jacobian[..., 1, 2],
            jacobian[..., 0, 2] - jacobian[..., 2, 0],
            jacobian[..., 1, 0] - jacobian[..., 0, 1],
        ],
        axis=-1,
    )
    speed = np.sqrt((velocity**2).sum(axis=-1))
    pressure = 0.5 * parameters.density * (
        parameters.inlet_velocity**2 - speed**2
    )
    return {
        "U": velocity,
        "pressure": pressure[..., None],
        "speed": speed[..., None],
        "vorticity": vorticity,
        "vorticityMagnitude": np.sqrt((vorticity**2).sum(axis=-1))[..., None],
        "qCriterion": q_criterion(jacobian)[..., None],
        "passiveScalar": field.passive_scalar(time)[..., None],
    }


def _write_meshes(root: Path, parameters: MockCaseParameters) -> list[dict[str, Any]]:
    """Write the obstacle and boundary meshes; return their manifest entries."""
    ox, oy, oz = parameters.origin
    lx, ly, lz = parameters.domain
    cx, cy = parameters.cylinder_center

    positions, indices, normals, patch_ids = make_cylinder_mesh(
        center=(cx, cy, oz + 0.5 * lz),
        radius=parameters.cylinder_radius,
        height=lz,
        axis="z",
        segments=48,
        patch_id=PATCH_CYLINDER_WALL,
        capped=False,  # the ends are buried in the spanwise walls
    )
    write_cvm(
        root / "meshes" / "obstacle.cvm",
        positions=positions, indices=indices, normals=normals,
        patch_ids=patch_ids,
    )

    # The domain box is viewed from the fluid inside it, so its faces wind
    # inward; an outward box would be invisible from every useful camera.
    positions, indices, normals, patch_ids = make_box_mesh(
        (ox, oy, oz), (ox + lx, oy + ly, oz + lz),
        {
            "sideWalls": PATCH_SIDE_WALLS,
            "inlet": PATCH_INLET,
            "outlet": PATCH_OUTLET,
        },
        inward=True,
    )
    write_cvm(
        root / "meshes" / "boundaries.cvm",
        positions=positions, indices=indices, normals=normals,
        patch_ids=patch_ids,
    )

    return [
        {
            "id": "obstacle",
            "name": "Cylinder",
            "path": "meshes/obstacle.cvm",
            "role": "obstacle",
            "static": True,
            "patches": [
                {
                    "id": PATCH_CYLINDER_WALL,
                    "name": "cylinderWall",
                    "type": "wall",
                    "color": [0.72, 0.72, 0.75],
                    "defaultVisible": True,
                    "opacity": 1.0,
                }
            ],
        },
        {
            "id": "boundaries",
            "name": "Domain boundaries",
            "path": "meshes/boundaries.cvm",
            "role": "boundary",
            "static": True,
            "patches": [
                {
                    "id": PATCH_INLET, "name": "inlet", "type": "inlet",
                    "color": [0.25, 0.55, 0.95], "defaultVisible": True,
                    "opacity": 0.25,
                },
                {
                    "id": PATCH_OUTLET, "name": "outlet", "type": "outlet",
                    "color": [0.95, 0.45, 0.25], "defaultVisible": True,
                    "opacity": 0.25,
                },
                {
                    "id": PATCH_SIDE_WALLS, "name": "sideWalls", "type": "wall",
                    "color": [0.55, 0.55, 0.55], "defaultVisible": False,
                    "opacity": 0.15,
                },
            ],
        },
    ]


def generate_mock_case(output: Path | str,
                       parameters: MockCaseParameters | None = None) -> Path:
    """Write the synthetic cylinder-wake case to ``output``.

    The order is forced by the data: field files first, because the manifest's
    statistics must describe the bytes that were actually stored; then meshes;
    then the manifest; then ``known_values.json``, which reads the finished case
    back through the Python reader so the Unreal reader has something exact to
    be checked against.

    Args:
        output: Case root directory. Created if absent; existing field files
            for the same frames are overwritten.
        parameters: Generation settings. Defaults to :func:`default_parameters`.

    Returns:
        The case root.
    """
    parameters = default_parameters() if parameters is None else parameters
    root = Path(output)
    root.mkdir(parents=True, exist_ok=True)

    field = WakeField(parameters)
    mask = obstacle_mask(parameters)
    stored_mask = mask.astype(np.uint8)[..., None]
    codec_ids = {
        spec.field_id: codec_id_from_name(_storage_codec(spec, parameters))
        for spec in _FIELDS
    }
    accumulators = {
        spec.field_id: _StatisticsAccumulator(len(spec.components))
        for spec in _FIELDS
    }

    times = parameters.times
    for frame, time in enumerate(times):
        values_by_id: dict[str, np.ndarray] = {"validMask": stored_mask}
        for name, values in _frame_fields(field, time, parameters).items():
            # NaN, not zero, inside the obstacle: a zero renders as "the flow
            # stopped here", which is a claim about the physics rather than an
            # admission that there is no data.
            masked = np.where(mask[..., None], values, np.nan)
            values_by_id[name] = masked

        for spec in _FIELDS:
            dtype = _storage_dtype(spec, parameters)
            stored = values_by_id[spec.field_id].astype(dtype)
            accumulators[spec.field_id].add(
                stored, None if spec.field_id == "validMask" else mask
            )
            write_cvf(
                root / _path_pattern(spec).format(frame=frame),
                values=stored,
                dtype=dtype,
                association="cell",
                codec=codec_ids[spec.field_id],
                level=parameters.level,
                brick_size=parameters.brick_size,
                frame_index=frame,
                field_numeric_id=spec.numeric_id,
                simulation_time=time,
                dimensions=parameters.dimensions,
                mask=None if spec.field_id == "validMask" else mask,
            )

    meshes = _write_meshes(root, parameters)
    manifest = _build_manifest(parameters, meshes, accumulators)
    (root / "manifest.json").write_text(dump_json(manifest) + "\n", encoding="utf-8")
    write_known_values(root)
    return root


def _path_pattern(spec: _FieldSpec) -> str:
    return f"frames/{{frame:06d}}/{spec.field_id}.cvf"


def _build_manifest(parameters: MockCaseParameters,
                    meshes: list[dict[str, Any]],
                    accumulators: dict[str, _StatisticsAccumulator]
                    ) -> dict[str, Any]:
    """Assemble manifest.json for the generated case."""
    fields: list[dict[str, Any]] = []
    for spec in _FIELDS:
        entry: dict[str, Any] = {
            "numericId": spec.numeric_id,
            "id": spec.field_id,
            "name": spec.name,
            "description": spec.description,
            "semantic": spec.semantic,
            "kind": "vector" if len(spec.components) == 3 else "scalar",
            "components": list(spec.components),
            "componentCount": len(spec.components),
            "dataType": _storage_dtype(spec, parameters),
            "association": "cell",
            "grid": "main",
            "unit": spec.unit,
            "solverPrecision": "float64",
            "temporalInterpolation": (
                "nearest" if spec.field_id == "validMask" else "linear"
            ),
            "storage": {
                "type": "bricked-volume",
                "codec": _storage_codec(spec, parameters),
                "brickSize": list(parameters.brick_size),
                "pathPattern": _path_pattern(spec),
            },
            "display": {
                "defaultComponent": (
                    "magnitude" if len(spec.components) == 3
                    else spec.components[0]
                ),
                "defaultColorMap": spec.colormap,
                "defaultRangeMode": spec.range_mode,
            },
        }
        if parameters.level is not None and spec.codec != "none":
            entry["storage"]["level"] = parameters.level
        statistics = accumulators[spec.field_id].to_manifest()
        if statistics is not None:
            entry["statistics"] = statistics
        fields.append(entry)

    times = parameters.times
    return {
        "format": "CFDViz",
        "version": FORMAT_VERSION,
        "case": {
            "id": parameters.case_id,
            "name": parameters.name,
            # Plan section 7 fixes this string exactly. It is the only place a
            # user learns that the pretty vortices are not a solved flow.
            "description": MOCK_DISCLAIMER,
            "quality": MOCK_QUALITY,
            "solver": {
                "name": "cfdviz.mock",
                "version": __version__,
                "method": "analytic superposition (uniform flow + doublet + "
                          "convecting Lamb-Oseen vortices)",
            },
            "tags": ["synthetic", "cylinder-wake", "demo", "not-validation-grade"],
        },
        "units": {"length": "m", "time": "s", "mass": "kg"},
        "coordinates": {
            "handedness": "right",
            "upAxis": "Z",
            "forwardAxis": "X",
            "origin": list(parameters.origin),
        },
        "timeline": {
            "frameCount": len(times),
            "times": times,
            "steps": list(range(len(times))),
            "defaultInterpolation": "linear",
        },
        "grids": [
            {
                "id": "main",
                "name": "Cartesian domain",
                "type": "uniform-cartesian",
                "dimensions": list(parameters.dimensions),
                "origin": list(parameters.origin),
                "spacing": list(parameters.spacing),
                "maskField": "validMask",
            }
        ],
        "fields": fields,
        "meshes": meshes,
        "derivedFields": [
            {
                "id": "velocityMagnitude",
                "name": "Velocity magnitude (derived)",
                "expression": "mag(U)",
                "unit": "m/s",
                "components": ["magnitude"],
                "componentCount": 1,
            }
        ],
        "provenance": {
            "generatorCommand": "python -m cfdviz generate-mock",
            "generatorVersion": __version__,
            "notes": [
                MOCK_DISCLAIMER,
                "Velocity is a closed-form superposition, not a solved flow: "
                "uniform inflow, a potential-flow doublet around the cylinder, "
                "a Gaussian wake deficit, and convecting alternating "
                "Lamb-Oseen vortices with a mild spanwise modulation.",
                "pressure = 0.5 * density * (inletVelocity^2 - |u|^2), the "
                "incompressible Bernoulli relation referenced to the free "
                "stream.",
                "vorticity and qCriterion use second-order finite differences "
                "in physical coordinates, not index space.",
                "Cells whose centre lies inside the cylinder are rejected by "
                "validMask and store NaN in every other field.",
                f"parameters = {json.dumps(parameters.as_dict(), sort_keys=True)}",
            ],
        },
    }
