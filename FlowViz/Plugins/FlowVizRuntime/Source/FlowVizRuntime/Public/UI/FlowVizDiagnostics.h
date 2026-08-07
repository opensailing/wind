// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Playback/FlowVizFrameCache.h"

struct FFlowVizWorkspaceModel;
class UCFDVizVolumeComponent;

/**
 * The diagnostics overlay's data, separated from its presentation.
 *
 * WHY THIS IS A STRUCT AND A FREE FUNCTION RATHER THAN A WIDGET. plan section 17
 * asks for an on-screen overlay of twenty-one numbers. If those numbers were
 * computed inside a Slate paint path, the only test that could reach them would
 * be one that builds a widget and scrapes its text -- and the failure mode this
 * project keeps rediscovering is a row that displays a plausible constant while
 * measuring nothing. A zero for "cache hit rate" is not a neutral placeholder;
 * it is a measurement claim, and a wrong one.
 *
 * So collection is a named function over a workspace model, returning plain
 * values a test can assert exactly. The widget's whole job is formatting.
 *
 * EVERY FIELD HERE HAS A PRODUCTION SOURCE. Three of plan section 17's rows are
 * deliberately ABSENT rather than present-and-zero, each for a stated reason:
 *
 *   streamline computation time -- there is no streamline system in this plugin.
 *       The word appears in exactly one file, in prose. A row would time nothing.
 *
 *   volume sample count -- the ray-march shader marches, but no counter is read
 *       back from the GPU. Nothing produces this number.
 *
 *   GPU time -- UE 5.8 exposes no GGPUFrameTime global. RHIGetGPUFrameCycles
 *       exists but reports zero without a real device and cannot be driven to a
 *       known value from a test, so no test could tell a working row from a
 *       broken one. That is precisely the un-falsifiable row this file refuses.
 *
 * Adding any of the three later means adding its source first. A row whose value
 * cannot be driven to a known number by a test does not belong in this struct.
 */
struct FFlowVizDiagnosticsSnapshot
{
	/* --- Engine timing ------------------------------------------------------- */

	/**
	 * Frames per second, derived from FApp::GetDeltaTime().
	 *
	 * DERIVED, NOT SAMPLED, so a test can drive it: FApp::SetDeltaTime(1/60)
	 * makes this exactly 60. Zero only when the delta is non-positive, which is
	 * the "no frame has been timed yet" state rather than a measurement of zero.
	 */
	double FramesPerSecond = 0.0;

	/** Seconds the engine reported for the last frame. The raw input to FramesPerSecond. */
	double FrameDeltaSeconds = 0.0;

	/** Game thread milliseconds, converted from the engine's GGameThreadTime cycle counter. */
	double GameThreadMs = 0.0;

	/** Render thread milliseconds, converted from GRenderThreadTime. */
	double RenderThreadMs = 0.0;

	/* --- Playback ------------------------------------------------------------ */

	/** True when a case is open. Every field below is meaningless when this is false. */
	bool bCaseOpen = false;

	/** Frames in the open case's timeline. */
	int32 FrameCount = 0;

	/** The frame pair the playhead selects, and the blend between them. */
	int32 FrameA = INDEX_NONE;
	int32 FrameB = INDEX_NONE;
	double InterpolationAlpha = 0.0;

	/** Playhead position in solver units -- the authoritative physical-time readout. */
	double PhysicalTime = 0.0;

	/** What the renderer may actually sample. Differs from FrameA/FrameB while frames load. */
	int32 DisplayFrameA = INDEX_NONE;
	int32 DisplayFrameB = INDEX_NONE;

	/** Volume field ids the open case declares, in manifest order. */
	TArray<FName> LoadedFields;

	/* --- Loading ------------------------------------------------------------- */

	/**
	 * Decodes in flight.
	 *
	 * ONE COUNTER, NOT TWO. plan section 17 lists "pending I/O requests" and
	 * "pending decompression" as separate rows, but this player does file I/O and
	 * zlib in a single task body (FlowVizCasePlayer.cpp, "FlowVizFrameDecode"),
	 * so the two numbers would be the same number under two labels -- which reads
	 * as corroboration between independent measurements when it is one value
	 * printed twice. Split it when the stages split.
	 */
	int32 LoadsInFlight = 0;

	int64 LoadsStarted = 0;
	int64 LoadsCompleted = 0;
	int64 LoadsFailed = 0;

	/** Selections that found their frames resident, and those that did not. */
	int64 CacheHits = 0;
	int64 CacheMisses = 0;

	/**
	 * Hits / (hits + misses), in [0,1]. Negative when no selection has been made.
	 *
	 * NEGATIVE RATHER THAN ZERO for "not yet measured", because a rate of zero is
	 * a real and different state -- every selection missed -- and an overlay that
	 * spells both the same way reports a cold start as a total cache failure.
	 */
	double CacheHitRate = -1.0;

	/** Cache occupancy and budgets, straight from the cache's own counters. */
	FFlowVizFrameCacheStats Cache;

	/* --- Render settings ----------------------------------------------------- */

	/** Ray step in voxels, and the step ceiling the shader is clamped to. */
	float StepVoxels = 0.0f;
	uint32 MaxSteps = 0;

	/** Voxel dimensions of the volume currently uploaded to the GPU. Zero before any upload. */
	FIntVector VolumeDimensions = FIntVector(0, 0, 0);

	/** Clip planes defined, and how many of those are switched on. */
	int32 ClipPlaneCount = 0;
	int32 EnabledClipPlaneCount = 0;
};

namespace FlowVizDiagnostics
{
	/**
	 * Read every diagnostic from a live workspace model.
	 *
	 * Game thread only -- it touches view models and the player's diagnostics.
	 * Allocates nothing beyond the field-id array.
	 *
	 * @param Model  The workspace to measure.
	 * @param Volume Optional. Supplies VolumeDimensions, which lives on the
	 *               component's texture set rather than the model. Null leaves
	 *               that field zeroed rather than guessing at a resolution.
	 */
	FLOWVIZRUNTIME_API FFlowVizDiagnosticsSnapshot Collect(
		const FFlowVizWorkspaceModel& Model, const UCFDVizVolumeComponent* Volume = nullptr);

	/** Multi-line human-readable form, one row per line. What the overlay and FlowViz.ShowDiagnostics both print. */
	FLOWVIZRUNTIME_API FString Format(const FFlowVizDiagnosticsSnapshot& Snapshot);
}
