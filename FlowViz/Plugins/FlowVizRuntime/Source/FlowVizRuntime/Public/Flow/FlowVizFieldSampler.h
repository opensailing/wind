// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CFDViz/CFDVizManifest.h"
#include "CFDViz/CFDVizTypes.h"
#include "CoreMinimal.h"

/**
 * A dense, decoded frame of one field, samplable on the CPU (#80/#81,
 * Milestone E).
 *
 * WHY THIS EXISTS BESIDE FlowVizProbe::SampleStoredField. The probe sampler
 * opens the file, reads ONE voxel, and closes -- right for a probe, ruinous
 * for glyphs and streamlines, which take thousands of samples per frame. This
 * type decodes the frame ONCE (worker thread: disk I/O and zlib, engineering
 * rule 1) and then samples from memory, trilinearly.
 *
 * TRILINEAR, AND DISCLOSED. A streamline integrator cannot use
 * nearest-neighbour: the velocity field would be piecewise constant, RK4
 * would gain nothing over Euler, and streamlines would kink at every cell
 * face. Interpolated values are values the solver never computed -- rule 7 --
 * so every consumer's UI carries the same interpolation advisory the slice
 * panel does.
 *
 * SAMPLES OUTSIDE THE DOMAIN return false, never zero. A zero velocity at the
 * domain edge would curve every streamline INTO the boundary and park it
 * there, which reads as recirculation that is not in the data (rule 10).
 *
 * THREADING. Build on a worker; Sample from any thread thereafter (the state
 * is immutable after Build).
 */
class FLOWVIZRUNTIME_API FFlowVizFieldSampler
{
public:
	/**
	 * Decode one frame of one field into memory.
	 *
	 * @param Case       An open, validated case.
	 * @param FieldId    The field. Vector (3-component) or scalar.
	 * @param FrameIndex Bounds-checked against the timeline.
	 * @return Ok, or the reader's failure. On failure the sampler stays empty
	 *         and every Sample returns false.
	 */
	FCFDVizResult Build(const FCFDVizCase& Case, FName FieldId, int32 FrameIndex);

	bool IsBuilt() const { return bBuilt; }
	int32 GetComponentCount() const { return ComponentCount; }
	const FIntVector& GetValueCounts() const { return ValueCounts; }

	/** The field's grid geometry: origin, spacing, counts -- solver units. */
	const FCFDVizGrid& GetGrid() const { return Grid; }

	/**
	 * Trilinear sample at a SOLVER-space position.
	 *
	 * @param OutValue Per-component result; sized to the component count.
	 * @return false outside the domain, before Build, or when any voxel of the
	 *         interpolation footprint is non-finite -- never a zero-filled
	 *         value (rule 10).
	 */
	bool Sample(const FVector& SolverPosition, TArray<double>& OutValue) const;

	/** Vector convenience for 3-component fields. False for scalar fields. */
	bool SampleVector(const FVector& SolverPosition, FVector& OutVector) const;

private:
	bool bBuilt = false;
	int32 ComponentCount = 0;
	FIntVector ValueCounts = FIntVector::ZeroValue;
	ECFDVizAssociation Association = ECFDVizAssociation::Cell;
	FCFDVizGrid Grid;

	/** Every value as double, component-major per voxel, X-fastest -- the CVF order. */
	TArray<double> Values;
};
