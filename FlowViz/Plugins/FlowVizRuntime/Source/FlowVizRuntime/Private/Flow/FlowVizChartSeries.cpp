// Copyright FlowViz contributors. All Rights Reserved.

#include "Flow/FlowVizChartSeries.h"

namespace FlowVizChartLocal
{
	double MagnitudeOf(const TArray<double>& Components)
	{
		double SumSquares = 0.0;
		for (const double Component : Components)
		{
			SumSquares += Component * Component;
		}
		return FMath::Sqrt(SumSquares);
	}
}

void FFlowVizChartSeries::Finalize()
{
	bHasRange = false;
	MinY = 0.0;
	MaxY = 0.0;
	GapCount = 0;

	for (const FFlowVizChartPoint& Point : Points)
	{
		if (!Point.bHasValue)
		{
			++GapCount;
			continue;
		}
		if (!bHasRange)
		{
			MinY = MaxY = Point.Y;
			bHasRange = true;
		}
		else
		{
			MinY = FMath::Min(MinY, Point.Y);
			MaxY = FMath::Max(MaxY, Point.Y);
		}
	}
}

bool FlowVizChart::BuildLineSeries(
	const FFlowVizFieldSampler& Sampler,
	const FVector& LineStart,
	const FVector& LineEnd,
	int32 SampleCount,
	EFlowVizLineProbeAxis AxisMode,
	FFlowVizChartSeries& OutSeries)
{
	using namespace FlowVizChartLocal;

	OutSeries = FFlowVizChartSeries();

	if (!Sampler.IsBuilt() || SampleCount < 2)
	{
		return false;
	}
	const double Length = (LineEnd - LineStart).Size();
	if (Length < UE_DOUBLE_SMALL_NUMBER)
	{
		// A zero-length line is a point probe wearing a line's controls -- the
		// view model refuses it too (MinLineSamples' comment).
		return false;
	}

	OutSeries.Points.Reserve(SampleCount);
	TArray<double> Components;
	for (int32 Index = 0; Index < SampleCount; ++Index)
	{
		// Denominator SampleCount-1, so the last sample lands exactly on the
		// end -- same rule as GetLineSamplePositions, same reason: a plot that
		// stops just short of a wall reads as the flow stopping there.
		const double Fraction = static_cast<double>(Index) / (SampleCount - 1);
		const FVector Position = FMath::Lerp(LineStart, LineEnd, Fraction);

		FFlowVizChartPoint& Point = OutSeries.Points.AddDefaulted_GetRef();
		Point.X = AxisMode == EFlowVizLineProbeAxis::NormalizedDistance
			? Fraction
			: Fraction * Length;
		Point.bHasValue = Sampler.Sample(Position, Components);
		if (Point.bHasValue)
		{
			Point.Y = MagnitudeOf(Components);
		}
	}

	OutSeries.Finalize();
	return true;
}

bool FlowVizChart::BuildTimeSeries(
	TArrayView<const FFlowVizFieldSampler* const> Samplers,
	TArrayView<const double> Times,
	const FVector& ProbePosition,
	FFlowVizChartSeries& OutSeries)
{
	using namespace FlowVizChartLocal;

	OutSeries = FFlowVizChartSeries();

	if (Samplers.Num() == 0 || Samplers.Num() != Times.Num())
	{
		return false;
	}

	OutSeries.Points.Reserve(Samplers.Num());
	TArray<double> Components;
	for (int32 Frame = 0; Frame < Samplers.Num(); ++Frame)
	{
		FFlowVizChartPoint& Point = OutSeries.Points.AddDefaulted_GetRef();
		Point.X = Times[Frame];

		// A null or unbuilt sampler is a GAP at that frame, not a skipped
		// frame: the x axis stays the full timeline, so a decode failure shows
		// as a hole rather than silently compressing time.
		const FFlowVizFieldSampler* Sampler = Samplers[Frame];
		Point.bHasValue = Sampler != nullptr && Sampler->IsBuilt()
			&& Sampler->Sample(ProbePosition, Components);
		if (Point.bHasValue)
		{
			Point.Y = MagnitudeOf(Components);
		}
	}

	OutSeries.Finalize();
	return true;
}
