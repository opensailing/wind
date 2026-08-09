// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Scene/FlowVizMeshPayload.h"

/**
 * Marching-cubes iso-surface extraction (renderer overhaul P4; research doc
 * section 1 ingredient 2 -- THE hero shot at full resolution).
 *
 * EDGE-INTERPOLATING, NOT MIDPOINT. The vertex sits where the field actually
 * crosses the iso value along each cube edge, which is what makes tubes read
 * as smooth surfaces instead of voxel staircases (FluidX3D's kernel does the
 * same; its blocky obstacle variant is the halfway version we deliberately
 * do not use for data).
 *
 * OPERATES ON A PLAIN GRID, not on a sampler: the topology logic is the part
 * that rots silently, so it takes arrays a test can author analytically
 * (spheres, linear ramps, NaN pockets). The workspace adapter fills the grid
 * from a sampler; NaN values ARE the mask (the P1 policy -- any cube with a
 * NaN corner is skipped, and the obstacle's surface comes from the patch
 * mesh, not from field MC).
 *
 * OUTPUT IS UNREAL SPACE with the winding crossing the Y-mirror swapped,
 * like every builder. Normals are field gradients -- one-sided at mask and
 * domain boundaries -- interpolated to the vertex and negated toward
 * decreasing field (outward for a "high inside" quantity like Q).
 */
namespace FlowVizIsoSurface
{
	/** A dense scalar grid, X-fastest -- the CVF order. NaN = masked. */
	struct FIsoGrid
	{
		FIntVector Counts = FIntVector::ZeroValue;
		FVector Origin = FVector::ZeroVector;
		FVector Spacing = FVector::OneVector;
		TArray<double> Values;

		bool IsValid() const
		{
			// int64 for the product: a large grid overflows int32 at the second
			// multiply, and a wrapped product can equal Values.Num() by accident.
			// The comparison against Num() (int32) then also bounds a valid
			// grid's value count to MAX_int32, which is what lets ValueIndex
			// return int32 safely.
			const int64 ValueCount = static_cast<int64>(Counts.X)
				* static_cast<int64>(Counts.Y) * static_cast<int64>(Counts.Z);
			return Counts.X >= 2 && Counts.Y >= 2 && Counts.Z >= 2
				&& static_cast<int64>(Values.Num()) == ValueCount
				&& Spacing.GetMin() > 0.0;
		}
	};

	/**
	 * Extract the iso surface at IsoValue.
	 *
	 * Vertices are SHARED across triangles (edge-key welding), which is what
	 * makes the mesh watertight and the topology testable; ScalarUVs is left
	 * empty -- color-by is the caller's pass, over the vertex positions this
	 * returns. Returns false for an invalid grid or an empty surface.
	 */
	FLOWVIZRUNTIME_API bool ExtractIsoSurface(
		const FIsoGrid& Grid,
		double IsoValue,
		FFlowVizMeshSection& OutSection);

	/**
	 * The default iso value: the given percentile of the POSITIVE values
	 * (research doc: neither FluidX3D's lattice-unit 0.0001 nor a tutorial's
	 * SI 1000 transfers between cases -- a percentile of this case's own Q
	 * does). Non-finite values are ignored. Returns false when nothing is
	 * positive -- a wake with no vortices has no honest default.
	 */
	FLOWVIZRUNTIME_API bool PercentilePositiveIsoValue(
		TArrayView<const double> Values,
		double Percentile,
		double& OutIsoValue);
}
