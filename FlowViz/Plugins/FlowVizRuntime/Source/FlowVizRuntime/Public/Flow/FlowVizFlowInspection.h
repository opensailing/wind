// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Flow/FlowVizFieldSampler.h"

/**
 * Glyphs and streamlines over a sampled vector field (#80/#81, Milestone E).
 *
 * PURE GEOMETRY BUILDERS. Both functions turn a sampler plus settings into
 * plain arrays a scene component instances or draws; neither touches the
 * RHI, a world, or a widget, which is what makes the maths testable headless
 * against the mock case's analytic wake. The rendering consumer is a thin
 * instanced-mesh / line-batch binding.
 *
 * STEADY-STATE, AND SAID SO. Both operate on ONE frame's field -- the frame
 * on display. plan.md section 20 defers "fully accurate time-dependent
 * pathlines"; a streamline here is the integral curve of the displayed
 * instant, which for an unsteady wake is NOT a particle path. The UI's
 * advisory carries that sentence.
 */

/** One velocity glyph: an arrow anchored on the slice. */
struct FFlowVizGlyph
{
	/** Anchor, SOLVER units. */
	FVector Position = FVector::ZeroVector;

	/** Unit direction of the sampled velocity. */
	FVector Direction = FVector::XAxisVector;

	/** The sampled |velocity|, solver units -- the consumer maps it to length/colour. */
	double Magnitude = 0.0;
};

/** Glyph placement settings (plan.md 10.7: density and scale are user controls). */
struct FFlowVizGlyphSettings
{
	/** Samples along the slice's two in-plane axes. Clamped to [2, 64] per axis: 64^2 glyphs is already a hedgehog. */
	int32 SamplesPerAxis = 16;

	/**
	 * Glyphs with |v| below this fraction of the slice's own maximum are
	 * dropped. Zero keeps everything. A stagnant cell's glyph is a dot that
	 * occludes its neighbours and says only "slow", which the colour already
	 * says.
	 */
	double MinMagnitudeFraction = 0.02;
};

/** A streamline: the integral curve through one seed. */
struct FFlowVizStreamline
{
	/** Polyline vertices, SOLVER units, seed included. */
	TArray<FVector> Points;

	/** |velocity| at each point, parallel to Points -- the consumer colours by it. */
	TArray<double> Magnitudes;

	/**
	 * Why integration stopped: left the domain, hit a stagnant region, or ran
	 * out of steps. Reported, not guessed -- a streamline that stops mid-domain
	 * for an undisclosed reason reads as flow structure.
	 */
	enum class EEndReason : uint8
	{
		LeftDomain,
		Stagnant,
		MaxSteps,
	};
	EEndReason EndReason = EEndReason::MaxSteps;
};

/** Streamline seeding and integration settings (plan.md 10.8). */
struct FFlowVizStreamlineSettings
{
	/** Seeds along the rake. Clamped to [1, 256]. */
	int32 SeedCount = 16;

	/** Integration step, as a fraction of the minimum cell edge. Clamped to [0.05, 2]. */
	double StepCellFraction = 0.5;

	/** Ceiling on steps per line, both directions combined. Clamped to [16, 8192]. */
	int32 MaxSteps = 1024;

	/** |v| below which the flow counts as stagnant and integration stops, solver units. */
	double StagnationSpeed = 1.0e-9;

	/** Integrate upstream from the seed too, so a rake in the wake shows where flow CAME from. */
	bool bBothDirections = true;
};

namespace FlowVizFlow
{
	/**
	 * Sample a glyph grid over a slice plane.
	 *
	 * The grid spans the intersection of the slice plane with the field's
	 * domain box, SamplesPerAxis each way along the plane's two in-plane basis
	 * vectors. Positions where the sampler refuses (outside, NaN footprint)
	 * produce NO glyph rather than a zero-length one.
	 *
	 * @param Sampler     A built VECTOR sampler (3 components).
	 * @param PlaneOrigin A point on the slice plane, solver units.
	 * @param PlaneNormal The slice normal. Degenerate normals are refused.
	 * @param OutGlyphs   Overwritten.
	 * @return false for an unbuilt/scalar sampler or a degenerate normal.
	 */
	FLOWVIZRUNTIME_API bool BuildSliceGlyphs(
		const FFlowVizFieldSampler& Sampler,
		const FVector& PlaneOrigin,
		const FVector& PlaneNormal,
		const FFlowVizGlyphSettings& Settings,
		TArray<FFlowVizGlyph>& OutGlyphs);

	/**
	 * Integrate streamlines from a seed rake.
	 *
	 * RK4 with a fixed step of StepCellFraction times the minimum cell edge.
	 * Fixed rather than adaptive: the mock wake's velocity varies over cells,
	 * not within them, and a fixed step's error is uniform and statable where
	 * an adaptive controller's tolerance would be a second thing to verify.
	 *
	 * @param RakeStart/RakeEnd The seed segment, solver units. Seeds are
	 *        evenly spaced, endpoints included (SeedCount 1 seeds the start).
	 * @param OutStreamlines One entry PER SEED, in rake order -- a seed outside
	 *        the domain yields an empty streamline rather than shifting its
	 *        neighbours' indices.
	 * @return false for an unbuilt/scalar sampler.
	 */
	FLOWVIZRUNTIME_API bool BuildStreamlines(
		const FFlowVizFieldSampler& Sampler,
		const FVector& RakeStart,
		const FVector& RakeEnd,
		const FFlowVizStreamlineSettings& Settings,
		TArray<FFlowVizStreamline>& OutStreamlines);
}
