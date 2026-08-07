// Copyright FlowViz contributors. All Rights Reserved.

#include "Flow/FlowVizFieldMask.h"

#include "Flow/FlowVizFieldSampler.h"

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizFieldMaskLocal
{
	int32 VoxelIndex(const FIntVector& Counts, const FIntVector& Voxel)
	{
		return (Voxel.Z * Counts.Y + Voxel.Y) * Counts.X + Voxel.X;
	}

	bool InBounds(const FIntVector& Counts, const FIntVector& Voxel)
	{
		return Voxel.X >= 0 && Voxel.X < Counts.X
			&& Voxel.Y >= 0 && Voxel.Y < Counts.Y
			&& Voxel.Z >= 0 && Voxel.Z < Counts.Z;
	}
}

void FFlowVizFieldMask::Build(const FFlowVizFieldSampler& Sampler)
{
	using namespace FlowVizFieldMaskLocal;

	bBuilt = false;
	Masked.Reset();

	if (!Sampler.IsBuilt())
	{
		// Fail closed: every query on an unbuilt mask answers masked.
		return;
	}

	ValueCounts = Sampler.GetValueCounts();
	Origin = Sampler.GetGrid().Origin;
	Spacing = Sampler.GetGrid().Spacing;

	const int32 VoxelCount = ValueCounts.X * ValueCounts.Y * ValueCounts.Z;
	Masked.Init(false, VoxelCount);

	TArray<double> Value;
	for (int32 Z = 0; Z < ValueCounts.Z; ++Z)
	{
		for (int32 Y = 0; Y < ValueCounts.Y; ++Y)
		{
			for (int32 X = 0; X < ValueCounts.X; ++X)
			{
				const FIntVector Voxel(X, Y, Z);
				// GetVoxelValue refuses only out-of-bounds here; a NON-FINITE
				// value still returns true with the raw number, which is
				// exactly what this loop needs to see.
				if (!Sampler.GetVoxelValue(Voxel, Value))
				{
					continue;
				}
				for (const double Component : Value)
				{
					// ANY non-finite component masks the voxel: the solid is a
					// property of the geometry, and a producer that masked U
					// but left a stale pressure value would otherwise yield a
					// solid that exists for one field and not another.
					if (!FMath::IsFinite(Component))
					{
						Masked[VoxelIndex(ValueCounts, Voxel)] = true;
						break;
					}
				}
			}
		}
	}

	bBuilt = true;
}

int32 FFlowVizFieldMask::CountMasked() const
{
	return bBuilt ? Masked.CountSetBits() : 0;
}

bool FFlowVizFieldMask::IsVoxelMasked(const FIntVector& Voxel) const
{
	using namespace FlowVizFieldMaskLocal;

	if (!bBuilt || !InBounds(ValueCounts, Voxel))
	{
		// Out of bounds and unbuilt both collapse to masked -- "not fluid" is
		// one answer regardless of the reason. See the header.
		return true;
	}
	return Masked[VoxelIndex(ValueCounts, Voxel)];
}

bool FFlowVizFieldMask::IsMaskedAt(const FVector& SolverPosition) const
{
	const FVector Local = (SolverPosition - Origin) / Spacing;
	return IsVoxelMasked(FIntVector(
		FMath::FloorToInt32(Local.X),
		FMath::FloorToInt32(Local.Y),
		FMath::FloorToInt32(Local.Z)));
}

bool FlowVizMaskStencil::OneSidedGradient(
	const FFlowVizFieldSampler& Sampler,
	const FFlowVizFieldMask& Mask,
	const FIntVector& Voxel,
	int32 Component,
	FVector& OutGradient)
{
	// A masked voxel has no derivative -- refuse, never zero-fill (rule 10 in
	// stencil form; a zero gradient normalizes to a plausible normal).
	if (Mask.IsVoxelMasked(Voxel) || !Sampler.IsBuilt()
		|| Component < 0 || Component >= Sampler.GetComponentCount())
	{
		return false;
	}

	TArray<double> Here;
	if (!Sampler.GetVoxelValue(Voxel, Here))
	{
		return false;
	}

	const FVector Spacing = Sampler.GetGrid().Spacing;
	TArray<double> Forward, Backward;

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		FIntVector Step = FIntVector::ZeroValue;
		Step[Axis] = 1;

		const bool bForwardFluid = !Mask.IsVoxelMasked(Voxel + Step);
		const bool bBackwardFluid = !Mask.IsVoxelMasked(Voxel - Step);
		const double H = Spacing[Axis];

		if (bForwardFluid && bBackwardFluid)
		{
			// Both neighbours fluid: the plain central difference. One-sided
			// stencils are a BOUNDARY behaviour, not a global downgrade.
			Sampler.GetVoxelValue(Voxel + Step, Forward);
			Sampler.GetVoxelValue(Voxel - Step, Backward);
			OutGradient[Axis] = (Forward[Component] - Backward[Component]) / (2.0 * H);
		}
		else if (bForwardFluid)
		{
			Sampler.GetVoxelValue(Voxel + Step, Forward);
			OutGradient[Axis] = (Forward[Component] - Here[Component]) / H;
		}
		else if (bBackwardFluid)
		{
			Sampler.GetVoxelValue(Voxel - Step, Backward);
			OutGradient[Axis] = (Here[Component] - Backward[Component]) / H;
		}
		else
		{
			// An axis with no fluid neighbour at all (a one-cell-thick fluid
			// sliver): there is no honest derivative on this axis.
			return false;
		}
	}

	return true;
}
