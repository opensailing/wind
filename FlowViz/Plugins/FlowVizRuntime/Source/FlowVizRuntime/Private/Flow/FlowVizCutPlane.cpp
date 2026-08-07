// Copyright FlowViz contributors. All Rights Reserved.

#include "Flow/FlowVizCutPlane.h"

#include "CFDViz/CFDVizTypes.h"
#include "Flow/FlowVizFieldMask.h"
#include "Flow/FlowVizFieldSampler.h"

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizCutPlaneLocal
{
	/** Magnitude for vectors, the value itself for scalars. */
	bool SampleScalar(
		const FFlowVizFieldSampler& Sampler, const FVector& Position, double& OutScalar)
	{
		if (Sampler.GetComponentCount() == 3)
		{
			FVector Vector;
			if (!Sampler.SampleVector(Position, Vector))
			{
				return false;
			}
			OutScalar = Vector.Size();
			return true;
		}
		TArray<double> Value;
		if (!Sampler.Sample(Position, Value) || Value.Num() == 0)
		{
			return false;
		}
		OutScalar = Value[0];
		return true;
	}
}

bool FlowVizCutPlane::BuildCutPlaneMesh(
	const FFlowVizFieldSampler& Sampler,
	const FFlowVizFieldMask& Mask,
	const FCutPlaneRequest& Request,
	FFlowVizMeshSection& OutSection,
	double& OutRangeMin,
	double& OutRangeMax)
{
	using namespace FlowVizCutPlaneLocal;

	OutSection = FFlowVizMeshSection();
	OutRangeMin = 0.0;
	OutRangeMax = 0.0;

	if (!Sampler.IsBuilt() || Request.Resolution < 2
		|| Request.DomainSize.GetMin() <= 0.0
		|| !Request.Normal.IsNormalized())
	{
		return false;
	}

	/*
	 * THE PLANE'S 2D FRAME. FindBestAxisVectors gives two solver-space
	 * tangents; the plane is walked as a U x V grid clipped to the domain
	 * box. The grid spans the domain's diagonal (the largest any plane
	 * section can be) and out-of-domain grid points simply fail to sample --
	 * clipping by refusal, the same policy the sampler already enforces.
	 */
	FVector TangentU, TangentV;
	Request.Normal.FindBestAxisVectors(TangentU, TangentV);

	const double Diagonal = Request.DomainSize.Size();
	const int32 CountU = Request.Resolution;
	const int32 CountV = Request.Resolution;
	const double StepU = Diagonal / (CountU - 1);
	const double StepV = Diagonal / (CountV - 1);
	const FVector GridOrigin =
		Request.Origin - TangentU * (Diagonal * 0.5) - TangentV * (Diagonal * 0.5);

	/*
	 * PASS 1: sample every grid point. A point is DEAD when it leaves the
	 * domain, fails to sample, or its position is masked -- and dead points
	 * take their quads with them, which is what "hard-edged hole" means.
	 */
	struct FGridPoint
	{
		FVector SolverPosition = FVector::ZeroVector;
		double Scalar = 0.0;
		bool bAlive = false;
	};
	TArray<FGridPoint> Grid;
	Grid.SetNum(CountU * CountV);

	double SeenMin = TNumericLimits<double>::Max();
	double SeenMax = -TNumericLimits<double>::Max();

	for (int32 V = 0; V < CountV; ++V)
	{
		for (int32 U = 0; U < CountU; ++U)
		{
			FGridPoint& Point = Grid[V * CountU + U];
			Point.SolverPosition = GridOrigin + TangentU * (U * StepU) + TangentV * (V * StepV);

			// The mask test is POSITIONAL, not footprint-wide: the sampler's
			// own refusal already covers a footprint touching NaN, and the
			// position test catches the sub-voxel sliver where a footprint of
			// fluid corners straddles solid space.
			if (Mask.IsMaskedAt(Point.SolverPosition))
			{
				continue;
			}
			if (SampleScalar(Sampler, Point.SolverPosition, Point.Scalar))
			{
				Point.bAlive = true;
				SeenMin = FMath::Min(SeenMin, Point.Scalar);
				SeenMax = FMath::Max(SeenMax, Point.Scalar);
			}
		}
	}

	if (SeenMax < SeenMin)
	{
		// Nothing sampled: the plane misses the domain, or sits inside the solid.
		return false;
	}

	// Manual range when given, the plane's own spread otherwise -- ECHOED
	// either way, so the legend and the mesh share one truth.
	const bool bManualRange = Request.RangeMax > Request.RangeMin;
	OutRangeMin = bManualRange ? Request.RangeMin : SeenMin;
	OutRangeMax = bManualRange ? Request.RangeMax : SeenMax;
	const double RangeWidth = FMath::Max(OutRangeMax - OutRangeMin, 1e-12);

	/*
	 * PASS 2: emit vertices for live points, quads where all four corners
	 * live. Unreal space through the same adapter every other builder uses.
	 */
	const FMatrix SolverToUnreal = MakeSolverToUnrealTransform();
	const FVector UnrealNormal =
		FVector(SolverToUnrealDirection(FVector3f(Request.Normal)));

	TArray<int32> VertexIndex;
	VertexIndex.Init(INDEX_NONE, Grid.Num());

	for (int32 Index = 0; Index < Grid.Num(); ++Index)
	{
		if (!Grid[Index].bAlive)
		{
			continue;
		}
		VertexIndex[Index] = OutSection.Vertices.Num();
		OutSection.Vertices.Add(SolverToUnreal.TransformPosition(Grid[Index].SolverPosition));
		OutSection.Normals.Add(UnrealNormal);
		OutSection.ScalarUVs.Add(static_cast<float>(
			FMath::Clamp((Grid[Index].Scalar - OutRangeMin) / RangeWidth, 0.0, 1.0)));
	}

	for (int32 V = 0; V + 1 < CountV; ++V)
	{
		for (int32 U = 0; U + 1 < CountU; ++U)
		{
			const int32 I00 = VertexIndex[V * CountU + U];
			const int32 I10 = VertexIndex[V * CountU + U + 1];
			const int32 I01 = VertexIndex[(V + 1) * CountU + U];
			const int32 I11 = VertexIndex[(V + 1) * CountU + U + 1];
			if (I00 == INDEX_NONE || I10 == INDEX_NONE
				|| I01 == INDEX_NONE || I11 == INDEX_NONE)
			{
				// A dead corner kills the quad whole: the hole is jagged at
				// grid resolution, never smeared across.
				continue;
			}

			/*
			 * WINDING: the solver-space quad (U x V right-handed about N)
			 * crosses the Y-mirroring adapter, which flips handedness -- the
			 * same swap every mesh builder in this plugin performs. A,C,B
			 * order restores an outward-facing plane in Unreal space.
			 */
			OutSection.Indices.Append({ I00, I01, I10 });
			OutSection.Indices.Append({ I10, I01, I11 });
		}
	}

	if (OutSection.Indices.Num() == 0)
	{
		// Vertices without a single whole quad -- a sliver too thin to draw.
		return false;
	}

	OutSection.Name = TEXT("cutPlane");
	OutSection.bDefaultVisible = true;
	return true;
}
