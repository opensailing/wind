// Copyright FlowViz contributors. All Rights Reserved.

#include "Flow/FlowVizFlowInspection.h"

namespace FlowVizFlowLocal
{
	/**
	 * Two in-plane basis vectors for a plane normal.
	 *
	 * The engine's own FindBestAxisVectors, so the glyph grid's "axis 1" and
	 * a gizmo drawn from the same call agree. Both unit length and mutually
	 * perpendicular by construction.
	 */
	void PlaneBasis(const FVector& Normal, FVector& OutU, FVector& OutV)
	{
		Normal.FindBestAxisVectors(OutU, OutV);
	}

	/** The field's domain box in solver units: [Origin, Origin + Spacing * Dimensions]. */
	void DomainBox(const FCFDVizGrid& Grid, FVector& OutMin, FVector& OutMax)
	{
		OutMin = Grid.Origin;
		OutMax = Grid.Origin + Grid.Spacing * FVector(Grid.Dimensions);
	}
}

bool FlowVizFlow::BuildSliceGlyphs(
	const FFlowVizFieldSampler& Sampler,
	const FVector& PlaneOrigin,
	const FVector& PlaneNormal,
	const FFlowVizGlyphSettings& Settings,
	TArray<FFlowVizGlyph>& OutGlyphs)
{
	using namespace FlowVizFlowLocal;

	OutGlyphs.Reset();

	if (!Sampler.IsBuilt() || Sampler.GetComponentCount() < 3)
	{
		return false;
	}
	if (PlaneNormal.IsNearlyZero())
	{
		return false;
	}

	const FVector Normal = PlaneNormal.GetSafeNormal();
	FVector U, V;
	PlaneBasis(Normal, U, V);

	FVector DomainMin, DomainMax;
	DomainBox(Sampler.GetGrid(), DomainMin, DomainMax);

	/*
	 * THE GRID SPANS THE DOMAIN'S PROJECTION onto the plane axes, centred on
	 * the plane origin. Half the domain diagonal each way is guaranteed to
	 * cover the plane-box intersection whatever the orientation; positions
	 * that land outside the box are simply refused by the sampler and yield
	 * no glyph -- over-coverage costs skipped samples, under-coverage costs
	 * missing glyphs, so the bound errs wide.
	 */
	const double HalfDiagonal = (DomainMax - DomainMin).Size() * 0.5;
	const FVector Center = PlaneOrigin;

	const int32 PerAxis = FMath::Clamp(Settings.SamplesPerAxis, 2, 64);
	const double Step = (HalfDiagonal * 2.0) / (PerAxis - 1);

	struct FRawSample
	{
		FVector Position;
		FVector Velocity;
	};
	TArray<FRawSample> Raw;
	Raw.Reserve(PerAxis * PerAxis);

	double MaxMagnitude = 0.0;
	for (int32 IU = 0; IU < PerAxis; ++IU)
	{
		for (int32 IV = 0; IV < PerAxis; ++IV)
		{
			const FVector Position = Center + U * (-HalfDiagonal + IU * Step)
				+ V * (-HalfDiagonal + IV * Step);

			FVector Velocity;
			if (!Sampler.SampleVector(Position, Velocity))
			{
				// Outside the domain or a NaN footprint: NO glyph. A zero-length
				// arrow here would read as stagnation inside the data.
				continue;
			}
			Raw.Add({ Position, Velocity });
			MaxMagnitude = FMath::Max(MaxMagnitude, Velocity.Size());
		}
	}

	// The magnitude floor is relative to THIS slice's own maximum, so the same
	// fraction means the same visual pruning whether the slice cuts the free
	// stream or the dead water behind the cylinder.
	const double Floor = MaxMagnitude * FMath::Max(0.0, Settings.MinMagnitudeFraction);

	for (const FRawSample& Sample : Raw)
	{
		const double Magnitude = Sample.Velocity.Size();
		if (Magnitude <= Floor || Magnitude <= 0.0)
		{
			continue;
		}
		FFlowVizGlyph& Glyph = OutGlyphs.AddDefaulted_GetRef();
		Glyph.Position = Sample.Position;
		Glyph.Direction = Sample.Velocity / Magnitude;
		Glyph.Magnitude = Magnitude;
	}
	return true;
}

namespace FlowVizFlowLocal
{
	/**
	 * One RK4 step of dP/dt = v(P), signed direction.
	 *
	 * @return false when any stage samples outside the domain -- the caller
	 *         records LeftDomain rather than integrating with a truncated
	 *         stencil, which would curve the line along the boundary.
	 */
	bool RK4Step(
		const FFlowVizFieldSampler& Sampler,
		const FVector& Position,
		double StepSize,
		double DirectionSign,
		FVector& OutNext)
	{
		FVector K1, K2, K3, K4;
		if (!Sampler.SampleVector(Position, K1))
		{
			return false;
		}
		K1 *= DirectionSign;
		if (!Sampler.SampleVector(Position + K1.GetSafeNormal() * (StepSize * 0.5), K2))
		{
			return false;
		}
		K2 *= DirectionSign;
		if (!Sampler.SampleVector(Position + K2.GetSafeNormal() * (StepSize * 0.5), K3))
		{
			return false;
		}
		K3 *= DirectionSign;
		if (!Sampler.SampleVector(Position + K3.GetSafeNormal() * StepSize, K4))
		{
			return false;
		}
		K4 *= DirectionSign;

		/*
		 * NORMALISED VELOCITY, FIXED ARC LENGTH. Integrating v directly makes
		 * the step size proportional to speed: streamlines race through the
		 * free stream and crawl in the wake, and the step-count ceiling
		 * truncates exactly the interesting slow regions. Normalising each
		 * stage advances a constant arc length per step, so the polyline is
		 * evenly sampled everywhere. The direction field v/|v| has the same
		 * integral curves as v -- speed reparametrises a streamline, it does
		 * not reroute it.
		 */
		const FVector Combined =
			(K1.GetSafeNormal() + K2.GetSafeNormal() * 2.0 + K3.GetSafeNormal() * 2.0
				+ K4.GetSafeNormal())
			/ 6.0;
		if (Combined.IsNearlyZero())
		{
			return false;
		}
		OutNext = Position + Combined.GetSafeNormal() * StepSize;
		return true;
	}

	void IntegrateDirection(
		const FFlowVizFieldSampler& Sampler,
		const FVector& Seed,
		double StepSize,
		double DirectionSign,
		int32 MaxSteps,
		double StagnationSpeed,
		TArray<FVector>& OutPoints,
		TArray<double>& OutMagnitudes,
		FFlowVizStreamline::EEndReason& OutReason)
	{
		OutReason = FFlowVizStreamline::EEndReason::MaxSteps;
		FVector Position = Seed;

		for (int32 Step = 0; Step < MaxSteps; ++Step)
		{
			FVector Velocity;
			if (!Sampler.SampleVector(Position, Velocity))
			{
				OutReason = FFlowVizStreamline::EEndReason::LeftDomain;
				return;
			}
			if (Velocity.Size() < StagnationSpeed)
			{
				OutReason = FFlowVizStreamline::EEndReason::Stagnant;
				return;
			}

			FVector Next;
			if (!RK4Step(Sampler, Position, StepSize, DirectionSign, Next))
			{
				OutReason = FFlowVizStreamline::EEndReason::LeftDomain;
				return;
			}

			OutPoints.Add(Next);
			OutMagnitudes.Add(Velocity.Size());
			Position = Next;
		}
	}
}

bool FlowVizFlow::BuildStreamlines(
	const FFlowVizFieldSampler& Sampler,
	const FVector& RakeStart,
	const FVector& RakeEnd,
	const FFlowVizStreamlineSettings& Settings,
	TArray<FFlowVizStreamline>& OutStreamlines)
{
	using namespace FlowVizFlowLocal;

	OutStreamlines.Reset();

	if (!Sampler.IsBuilt() || Sampler.GetComponentCount() < 3)
	{
		return false;
	}

	const int32 SeedCount = FMath::Clamp(Settings.SeedCount, 1, 256);
	const double StepFraction = FMath::Clamp(Settings.StepCellFraction, 0.05, 2.0);
	const int32 MaxSteps = FMath::Clamp(Settings.MaxSteps, 16, 8192);

	const FVector Spacing = Sampler.GetGrid().Spacing;
	const double StepSize = StepFraction * FMath::Min3(Spacing.X, Spacing.Y, Spacing.Z);

	OutStreamlines.SetNum(SeedCount);
	for (int32 SeedIndex = 0; SeedIndex < SeedCount; ++SeedIndex)
	{
		const double Alpha =
			SeedCount > 1 ? static_cast<double>(SeedIndex) / (SeedCount - 1) : 0.0;
		const FVector Seed = FMath::Lerp(RakeStart, RakeEnd, Alpha);

		FFlowVizStreamline& Line = OutStreamlines[SeedIndex];

		FVector SeedVelocity;
		if (!Sampler.SampleVector(Seed, SeedVelocity))
		{
			// A seed outside the domain yields an EMPTY line in place, keeping
			// rake order -- shifting neighbours' indices would re-colour every
			// line to its right when one seed leaves the box.
			Line.EndReason = FFlowVizStreamline::EEndReason::LeftDomain;
			continue;
		}

		/*
		 * DOWNSTREAM HALF, THEN THE SEED, THEN UPSTREAM REVERSED -- assembled
		 * so Points runs continuously from the upstream end to the downstream
		 * end with the seed inside it, which is what a ribbon renderer needs.
		 * The steps budget is split between the directions.
		 */
		TArray<FVector> Downstream, Upstream;
		TArray<double> DownstreamMagnitudes, UpstreamMagnitudes;
		FFlowVizStreamline::EEndReason DownstreamReason, UpstreamReason;

		const int32 PerDirection = Settings.bBothDirections ? MaxSteps / 2 : MaxSteps;

		IntegrateDirection(Sampler, Seed, StepSize, 1.0, PerDirection,
			Settings.StagnationSpeed, Downstream, DownstreamMagnitudes, DownstreamReason);

		if (Settings.bBothDirections)
		{
			IntegrateDirection(Sampler, Seed, StepSize, -1.0, PerDirection,
				Settings.StagnationSpeed, Upstream, UpstreamMagnitudes, UpstreamReason);
		}

		Line.Points.Reserve(Upstream.Num() + 1 + Downstream.Num());
		Line.Magnitudes.Reserve(Upstream.Num() + 1 + Downstream.Num());
		for (int32 Index = Upstream.Num() - 1; Index >= 0; --Index)
		{
			Line.Points.Add(Upstream[Index]);
			Line.Magnitudes.Add(UpstreamMagnitudes[Index]);
		}
		Line.Points.Add(Seed);
		Line.Magnitudes.Add(SeedVelocity.Size());
		for (int32 Index = 0; Index < Downstream.Num(); ++Index)
		{
			Line.Points.Add(Downstream[Index]);
			Line.Magnitudes.Add(DownstreamMagnitudes[Index]);
		}

		// The DOWNSTREAM reason is the line's: it is the direction a particle
		// released at the seed would actually travel.
		Line.EndReason = DownstreamReason;
	}
	return true;
}

bool FlowVizFlow::MakeDefaultRake(
	const FVector& DomainSize, FVector& OutRakeStart, FVector& OutRakeEnd)
{
	if (DomainSize.GetMin() <= 0.0)
	{
		return false;
	}

	/*
	 * JUST INSIDE THE INLET (5% of X), z-mid, middle 80% of Y: upstream of any
	 * obstacle a case is likely to hold, on the plane the default cut plane
	 * shows, clear of the wall boundary layers at both Y ends. One rule, every
	 * domain shape -- the point is that it cannot produce an empty seeding the
	 * way a fixed-stride grid can on a shallow grid.
	 */
	const double X = DomainSize.X * 0.05;
	const double ZMid = DomainSize.Z * 0.5;
	OutRakeStart = FVector(X, DomainSize.Y * 0.1, ZMid);
	OutRakeEnd = FVector(X, DomainSize.Y * 0.9, ZMid);
	return true;
}
