// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FFlowVizFieldSampler;

/**
 * The mask policy, formalized once (renderer overhaul P1; research doc
 * section 5 item 0).
 *
 * NaN IS THE SOLID. CVF masks obstacle voxels with quiet NaN, so "is this
 * voxel fluid?" is a fact about the CASE, not about any one field -- and
 * every consumer that asks it ad hoc invents its own slightly different
 * answer. This type is the single answer: marching cubes asks it to skip
 * NaN-corner cubes, cut planes ask it to hard-edge mask cells, streamline
 * and particle integrators ask it to terminate, and the gradient stencils
 * below ask it to go one-sided at boundaries instead of reading NaN.
 *
 * OUT OF BOUNDS IS MASKED. "Not fluid" is one answer whether the reason is
 * an obstacle or the end of the domain; collapsing them is what every
 * consumer wants (an MC cube straddling the boundary is skipped the same way
 * a cube straddling the cylinder is).
 *
 * THREADING. Build on a worker beside the sampler it derives from; immutable
 * and readable from any thread thereafter.
 */
class FLOWVIZRUNTIME_API FFlowVizFieldMask
{
public:
	/**
	 * Derive the mask from a built sampler: a voxel is masked when ANY
	 * component of its value is non-finite. An unbuilt sampler yields an
	 * unbuilt mask (every query answers masked -- fail closed).
	 */
	void Build(const FFlowVizFieldSampler& Sampler);

	bool IsBuilt() const { return bBuilt; }
	const FIntVector& GetValueCounts() const { return ValueCounts; }

	/** Masked voxels in the grid. The identity control for tests. */
	int32 CountMasked() const;

	/** True for a masked voxel, an out-of-bounds voxel, or an unbuilt mask. */
	bool IsVoxelMasked(const FIntVector& Voxel) const;

	/** IsVoxelMasked for the voxel containing a SOLVER-space position. */
	bool IsMaskedAt(const FVector& SolverPosition) const;

private:
	bool bBuilt = false;
	FIntVector ValueCounts = FIntVector::ZeroValue;
	FVector Origin = FVector::ZeroVector;
	FVector Spacing = FVector::OneVector;

	/** One bit per voxel, X-fastest -- the CVF order. */
	TBitArray<> Masked;
};

namespace FlowVizMaskStencil
{
	/**
	 * The spatial gradient of one component at a voxel, mask-aware.
	 *
	 * Per axis: central difference when both neighbours are fluid, one-sided
	 * (forward or backward) when exactly one is, and REFUSED -- the function
	 * returns false -- when the voxel itself is masked or an axis has no
	 * fluid neighbour at all. Never reads a masked voxel's value, which is
	 * the whole point: a central difference across a NaN neighbour poisons Q
	 * and every MC normal at exactly the surface the eye looks at.
	 *
	 * @param Component Which component of the sampler's value to differentiate.
	 * @param OutGradient d(value)/d(solver position), physical units.
	 * @return false at a masked voxel, out of bounds, or when any axis is
	 *         starved of fluid neighbours -- never a zero-filled gradient.
	 */
	FLOWVIZRUNTIME_API bool OneSidedGradient(
		const FFlowVizFieldSampler& Sampler,
		const FFlowVizFieldMask& Mask,
		const FIntVector& Voxel,
		int32 Component,
		FVector& OutGradient);
}
