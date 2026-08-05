// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CFDViz/CFDVizManifest.h"
#include "CFDViz/CFDVizTypes.h"

/**
 * Numeric probes (plan.md sections 10.11, 5F; engineering rule 9; ADR 004 s8).
 *
 * ADR 004 SECTION 8, RESTATED BECAUSE IT IS THE WHOLE DESIGN: "Probes read the
 * field, never the render. A probe converts the picked Unreal location BACK
 * through MakeUnrealToSolverTransform and samples the stored field. It never
 * reads back a pixel and never reports a centimetre value to the user."
 *
 * That has three consequences this file is built around.
 *
 *  1. A PROBE'S POSITION IS STORED IN SOLVER UNITS. Unreal centimetres are an
 *     input and an output, never the state. Storing centimetres would make every
 *     readout depend on the case's units.length, and a case authored in
 *     millimetres would report positions 1000x wrong while still looking like a
 *     position.
 *
 *  2. THE Y-MIRROR IS APPLIED BY ::MakeUnrealToSolverTransform AND BY
 *     NOTHING ELSE - a GLOBAL-scope function, not a member of namespace CFDViz.
 *     Only the MetersToUnrealCentimeters constant lives in that namespace; the
 *     adapters sit beside it at global scope (CFDVizTypes.h). A hand-written
 *     (x, -y, z) here would be a second
 *     implementation of the mirror, and the failure mode of getting it wrong is
 *     a probe that reports a value from the mirrored location - a real number
 *     from the wrong cell, which no range check can catch.
 *
 *  3. SAMPLING IS A FREE FUNCTION, NOT A METHOD. Reading a value means decoding
 *     a CVF brick, which is disk I/O and decompression - forbidden on the game
 *     thread by engineering rule 1. FlowVizProbe::SampleStoredField is therefore
 *     callable from a worker, takes everything it needs by argument, and touches
 *     no view-model state. The view model holds positions and the last reading;
 *     it never reads a file itself.
 *
 * WHAT IS NOT HERE. Plotting over time, CSV export and interactive dragging are
 * plan.md 10.11 controls whose consumers (a chart widget, an export path, a
 * gizmo) do not exist in this plugin. They are not modelled here, because a
 * "plot over time" toggle with nothing to plot is the nonfunctional control
 * rule 15 forbids.
 *
 * THREADING. The view model is game thread. SampleStoredField is worker-safe and
 * shares no state.
 */

/** One probe's reading of one field at one frame. All values in SOLVER units (engineering rule 4). */
struct FFlowVizProbeReading
{
	/** False when the probe is outside the domain, or the read failed. Components is empty in that case - never zero-filled (rule 10). */
	bool bHasValue = false;

	/** True when the position lies inside the field's value extent. False with bHasValue false means "outside", which is not an error. */
	bool bInsideDomain = false;

	/** One entry per field component, in the field's own storage order and unit. NaN is preserved, never coerced. */
	TArray<double> Components;

	/** sqrt of the sum of squares over the components. NaN when any component is NaN, so an invalid sample cannot masquerade as a finite magnitude. */
	double Magnitude = 0.0;

	/** The voxel actually sampled. Reported so a readout can state WHICH cell it came from rather than implying a point measurement. */
	FIntVector Voxel = FIntVector(INDEX_NONE, INDEX_NONE, INDEX_NONE);

	/** Centre of that voxel in solver coordinates, which is generally NOT the requested position. */
	FVector SampledSolverPosition = FVector::ZeroVector;

	/** Frame this reading came from, and its physical time in the case's time unit. */
	int32 FrameIndex = INDEX_NONE;
	double Time = 0.0;

	/** Set when the read failed for a reason other than being outside the domain. */
	FCFDVizResult Status;
};

/** One placed probe. */
struct FFlowVizProbe
{
	/** Stable across a session save and reload; what a chart series is keyed by. */
	FGuid Id;

	/** User-editable ("rename probes", plan.md 10.11). */
	FString Name;

	/** SOLVER units, canonical CFDViz axes. Never centimetres - see the file comment. */
	FVector SolverPosition = FVector::ZeroVector;

	/** A hidden probe keeps its position and its history; this is a visibility toggle, not a delete. */
	bool bVisible = true;

	/** The most recent reading, or a default-constructed one before any sample. */
	FFlowVizProbeReading LastReading;
};

/** How a line probe's X axis is labelled (plan.md 10.11). */
enum class EFlowVizLineProbeAxis : uint8
{
	/** Solver-unit distance from the start point. */
	Distance = 0,
	/** 0..1 along the line, which is what makes two lines of different lengths comparable. */
	NormalizedDistance = 1,
};

class FLOWVIZRUNTIME_API FFlowVizProbeViewModel
{
public:
	FFlowVizProbeViewModel() = default;

	/* --- Unit scale ------------------------------------------------------- */

	/**
	 * The case's length scale, from `units.length` - NOT assumed to be metres
	 * (ADR 004 section 5). Every Unreal-space conversion below uses it.
	 *
	 * @return A failure for a non-positive or non-finite scale, which would make
	 *         MakeUnrealToSolverTransform fall back to the identity and silently
	 *         drop the mirror.
	 */
	FCFDVizResult SetMetersToUnrealUnits(double Scale);
	double GetMetersToUnrealUnits() const { return MetersToUnrealUnits; }

	/* --- Placement -------------------------------------------------------- */

	/** Place by numeric XYZ in solver units (plan.md 10.11 "place by numeric XYZ"). @return the new probe's Id, invalid on refusal. */
	FGuid AddProbeAtSolverPosition(const FVector& SolverPosition, const FString& Name = FString());

	/**
	 * Place from a picked Unreal world location - a click on a slice or on
	 * boundary geometry.
	 *
	 * Converts through ::MakeUnrealToSolverTransform - global scope, NOT
	 * CFDViz:: - so the Y-mirror and the unit scale are applied once, by the
	 * shared adapter.
	 */
	FGuid AddProbeAtUnrealPosition(const FVector& UnrealPosition, const FString& Name = FString());

	/**
	 * Place a probe under an id it ALREADY HAS - restoring a saved session.
	 *
	 * WHY THIS EXISTS RATHER THAN A REID-AFTER-ADD. A probe's id is what a chart
	 * series is keyed by (see FFlowVizProbe::Id), so a reload that minted fresh
	 * ids would silently orphan every saved reference: the probes would be there,
	 * in the right places, with no data attached. Restoring under the saved id has
	 * to be a single operation, because the alternative - add, then assign over
	 * the minted id - leaves a window in which the caller holds an id that is
	 * about to stop being valid.
	 *
	 * @return A failure for a non-finite position, for an INVALID id, or for an id
	 *         a probe already has. Duplicate ids would make every id-keyed lookup
	 *         in this class ambiguous, and the one that "wins" would be whichever
	 *         happened to be earlier in the array.
	 */
	FCFDVizResult RestoreProbe(const FGuid& Id, const FVector& SolverPosition, const FString& Name);

	/** Drag. @return false when no probe has that Id. */
	bool MoveProbeToSolverPosition(const FGuid& Id, const FVector& SolverPosition);
	bool MoveProbeToUnrealPosition(const FGuid& Id, const FVector& UnrealPosition);

	bool RemoveProbe(const FGuid& Id);
	void RemoveAllProbes();
	bool RenameProbe(const FGuid& Id, const FString& NewName);
	bool SetProbeVisible(const FGuid& Id, bool bVisible);

	int32 GetProbeCount() const { return Probes.Num(); }
	const TArray<FFlowVizProbe>& GetProbes() const { return Probes; }
	const FFlowVizProbe* FindProbe(const FGuid& Id) const;

	/** Where this probe would be drawn, in Unreal world space. The inverse of the placement conversion, through the same shared adapter. */
	bool TryGetProbeUnrealPosition(const FGuid& Id, FVector& OutUnrealPosition) const;

	/** Store a reading taken by a worker. @return false when no probe has that Id. */
	bool SetProbeReading(const FGuid& Id, const FFlowVizProbeReading& Reading);

	/* --- Line probe ------------------------------------------------------- */

	/** Endpoints in solver units. @return A failure for non-finite endpoints or a zero-length line. */
	FCFDVizResult SetLineProbe(const FVector& SolverStart, const FVector& SolverEnd);
	bool HasLineProbe() const { return bHasLine; }
	const FVector& GetLineStart() const { return LineStart; }
	const FVector& GetLineEnd() const { return LineEnd; }

	/** @param Samples >= 2 - a one-sample line is a point probe wearing a line's controls. */
	FCFDVizResult SetLineSampleCount(int32 Samples);
	int32 GetLineSampleCount() const { return LineSamples; }

	void SetLineAxisMode(EFlowVizLineProbeAxis Mode);
	EFlowVizLineProbeAxis GetLineAxisMode() const { return LineAxis; }

	/**
	 * The sample positions along the line, in solver units, endpoints INCLUDED.
	 *
	 * N samples over N-1 intervals, so sample 0 is exactly the start and sample
	 * N-1 is exactly the end. Dividing by N instead would leave the far endpoint
	 * unsampled, which shows up as a line plot that stops just short of a wall.
	 */
	FCFDVizResult GetLineSamplePositions(TArray<FVector>& OutPositions) const;

	/** X-axis coordinate of each sample under the current axis mode. Parallel to GetLineSamplePositions. */
	FCFDVizResult GetLineSampleAxisValues(TArray<double>& OutValues) const;

private:
	FFlowVizProbe* FindProbeMutable(const FGuid& Id);

	TArray<FFlowVizProbe> Probes;

	double MetersToUnrealUnits = CFDViz::MetersToUnrealCentimeters;

	bool bHasLine = false;
	FVector LineStart = FVector::ZeroVector;
	FVector LineEnd = FVector::ZeroVector;
	int32 LineSamples = 32;
	EFlowVizLineProbeAxis LineAxis = EFlowVizLineProbeAxis::Distance;
};

namespace FlowVizProbe
{
	/** Smallest legal line-probe sample count: two, so both endpoints are sampled. */
	inline constexpr int32 MinLineSamples = 2;

	/** Ceiling, so a typed sample count cannot ask for an unbounded number of brick decodes. */
	inline constexpr int32 MaxLineSamples = 65536;

	/**
	 * Read one voxel of one field at one frame, at a SOLVER-space position.
	 *
	 * THIS DOES DISK I/O AND DECOMPRESSION. Engineering rule 1 forbids that on
	 * the game thread, so this is a free function a worker can call rather than a
	 * method on a game-thread object.
	 *
	 * NEAREST VOXEL, DELIBERATELY, AND IT SAYS SO. The returned reading names the
	 * voxel and its centre, because a probe that reports a trilinearly
	 * interpolated value at a requested point reports a number the solver never
	 * produced - which rule 5 forbids without saying so. Interpolated sampling is
	 * a separate feature and would need its own disclosure.
	 *
	 * A position outside the field's value extent yields bHasValue = false and
	 * bInsideDomain = false with an Ok status: outside is an answer, not an error.
	 *
	 * @param Case          An open, validated case.
	 * @param FieldId       Field to read. Unknown ids fail with IndexOutOfRange.
	 * @param FrameIndex    Bounds-checked against the timeline.
	 * @param SolverPosition Canonical CFDViz coordinates, in the case's own length unit.
	 * @param OutReading    Overwritten. Never partially filled on failure.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult SampleStoredField(
		const FCFDVizCase& Case,
		FName FieldId,
		int32 FrameIndex,
		const FVector& SolverPosition,
		FFlowVizProbeReading& OutReading);
}
