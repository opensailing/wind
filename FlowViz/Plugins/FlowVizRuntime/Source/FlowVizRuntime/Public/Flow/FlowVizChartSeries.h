// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Flow/FlowVizFieldSampler.h"
#include "UI/FlowVizProbeViewModel.h"

/**
 * Chart series builders (#82 / Milestone E, DoD 11 and 12).
 *
 * A series is x/y pairs plus the disclosure a chart must carry: which points
 * are MISSING and why. Both builders are pure functions of a sampler and the
 * probe geometry -- no widget, no world -- so the numbers on the chart are
 * testable against the mock case without rendering anything.
 *
 * GAPS ARE FIRST-CLASS, NOT ZEROS (rule 10). A line probe that leaves the
 * domain, or crosses a NaN pocket, produces points with bHasValue false. A
 * chart that plotted zero there would draw flow structure that is not in the
 * data; the chart widget draws a gap instead.
 */
struct FFlowVizChartPoint
{
	/** X coordinate: distance (or 0..1) for a line series, physical time for a time series. */
	double X = 0.0;

	/** Y value. MEANINGLESS when bHasValue is false -- never read it as zero. */
	double Y = 0.0;

	bool bHasValue = false;
};

struct FFlowVizChartSeries
{
	TArray<FFlowVizChartPoint> Points;

	/** Bounds over the points WITH values. False when no point has one. */
	bool bHasRange = false;
	double MinY = 0.0;
	double MaxY = 0.0;

	/** How many points carry no value -- surfaced so the chart can say "N gaps". */
	int32 GapCount = 0;

	void Finalize();
};

namespace FlowVizChart
{
	/**
	 * The line probe's distance plot (DoD 12): the field's magnitude at
	 * SampleCount points along [LineStart, LineEnd], x per AxisMode.
	 *
	 * @param Sampler A built sampler of the CHART'S field -- scalar or vector;
	 *                vector fields plot the magnitude over the components.
	 * @return false for an unbuilt sampler, a degenerate line, or a sample
	 *         count below 2.
	 */
	FLOWVIZRUNTIME_API bool BuildLineSeries(
		const FFlowVizFieldSampler& Sampler,
		const FVector& LineStart,
		const FVector& LineEnd,
		int32 SampleCount,
		EFlowVizLineProbeAxis AxisMode,
		FFlowVizChartSeries& OutSeries);

	/**
	 * A point probe's time plot (DoD 11): the probe's value across every frame.
	 *
	 * ONE SAMPLER PER FRAME, BUILT BY THE CALLER on a worker: this function
	 * only assembles, so its cost and its thread contract stay visible at the
	 * call site. Samplers[i] is frame i's; a null entry yields a gap at that
	 * frame rather than skipping it, so the x axis stays the full timeline.
	 *
	 * @param Times Physical time per frame, parallel to Samplers.
	 */
	FLOWVIZRUNTIME_API bool BuildTimeSeries(
		TArrayView<const FFlowVizFieldSampler* const> Samplers,
		TArrayView<const double> Times,
		const FVector& ProbePosition,
		FFlowVizChartSeries& OutSeries);
}
