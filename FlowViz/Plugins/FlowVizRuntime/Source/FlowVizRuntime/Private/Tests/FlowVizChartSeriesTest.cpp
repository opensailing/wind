// Copyright FlowViz contributors. All Rights Reserved.

#include "Flow/FlowVizChartSeries.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizChartSeriesTest
{
	FString GetSampleManifest()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FlowVizRuntime"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		const FString ProjectDir = FPaths::GetPath(FPaths::GetPath(Plugin->GetBaseDir()));
		return FPaths::Combine(
			ProjectDir, TEXT("Samples"), TEXT("MockCylinderWake.cfdviz"), TEXT("manifest.json"));
	}
}

/**
 * Chart series (#82 / Milestone E): the line probe's distance plot (DoD 12)
 * and the point probe's time plot (DoD 11), as numbers -- the widget draws
 * what these produce, so what these produce is where the correctness lives.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizChartSeriesTest,
	"FlowViz.Flow.ChartSeries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizChartSeriesTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizChartSeriesTest;

	FCFDVizCase Case;
	if (!TestTrue(TEXT("CONTROL: the sample case loads"),
			FCFDVizCase::LoadFromFile(GetSampleManifest(), Case).IsOk()))
	{
		return false;
	}

	FFlowVizFieldSampler Frame0;
	if (!TestTrue(TEXT("CONTROL: frame 0's U sampler builds"),
			Frame0.Build(Case, FName(TEXT("U")), 0).IsOk()))
	{
		return false;
	}

	/* == The line series (DoD 12) =========================================== */
	{
		// A line across the wake, fully inside the domain, at y = 3.0 -- OFF the
		// cylinder axis (centre (4, 2), r = 0.3). The first draft ran along
		// y = 2 and "failed" with 5 gaps: the line crossed the cylinder, where
		// the mask makes the field NaN, and the gaps were the feature working.
		// The cylinder crossing is now its own assertion below.
		const FVector Start(1.0, 3.0, 0.5);
		const FVector End(11.0, 3.0, 0.5);

		FFlowVizChartSeries Series;
		TestTrue(TEXT("the line series builds"),
			FlowVizChart::BuildLineSeries(
				Frame0, Start, End, 48, EFlowVizLineProbeAxis::Distance, Series));

		TestEqual(TEXT("with the requested sample count"), Series.Points.Num(), 48);
		TestEqual(TEXT("and NO gaps -- the line avoids the cylinder and stays interior, so "
					   "a gap means the sampler refused a clean point"),
			Series.GapCount, 0);
		TestTrue(TEXT("the x axis runs 0..length"),
			FMath::IsNearlyEqual(Series.Points[0].X, 0.0, 1e-9)
				&& FMath::IsNearlyEqual(Series.Points.Last().X, 10.0, 1e-9));

		/*
		 * -- THE CYLINDER CROSSING: a line along y = 2 passes through the
		 * masked cylinder, and those points must be GAPS -- the solid body is
		 * exactly where "no data" is the honest answer, and a zero there would
		 * plot stagnant fluid inside the metal.
		 */
		FFlowVizChartSeries ThroughCylinder;
		TestTrue(TEXT("a line through the cylinder builds"),
			FlowVizChart::BuildLineSeries(Frame0, FVector(1.0, 2.0, 0.5),
				FVector(11.0, 2.0, 0.5), 48, EFlowVizLineProbeAxis::Distance,
				ThroughCylinder));
		TestTrue(TEXT("the cylinder shows as gaps in the plot"),
			ThroughCylinder.GapCount > 0);
		TestTrue(TEXT("and the flow on either side still has values"),
			ThroughCylinder.Points[0].bHasValue && ThroughCylinder.Points.Last().bHasValue);
		TestTrue(TEXT("the range is real: |U| >= 0.93 everywhere by the manifest's own "
					  "declaration, and below the declared maximum"),
			Series.bHasRange && Series.MinY >= 0.9 && Series.MaxY <= 13.5);
		TestTrue(TEXT("CONTROL: the series is not constant -- a wake crossing with no "
					  "variation would make every assertion above vacuous"),
			Series.MaxY - Series.MinY > 0.1);

		/* -- Normalized axis: same y values, x in 0..1. --------------------- */
		FFlowVizChartSeries Normalized;
		TestTrue(TEXT("the normalized series builds"),
			FlowVizChart::BuildLineSeries(
				Frame0, Start, End, 48, EFlowVizLineProbeAxis::NormalizedDistance, Normalized));
		TestTrue(TEXT("its x axis runs 0..1"),
			FMath::IsNearlyEqual(Normalized.Points.Last().X, 1.0, 1e-9));
		TestEqual(TEXT("and its y values are IDENTICAL -- the axis mode relabels, it must "
					   "not resample"),
			Normalized.Points[24].Y, Series.Points[24].Y);
	}

	/* == Gaps are gaps, not zeros (rule 10) ================================= */
	{
		// A line that EXITS the domain: the tail beyond x = 12 has no data.
		FFlowVizChartSeries Series;
		TestTrue(TEXT("a line leaving the domain still builds"),
			FlowVizChart::BuildLineSeries(Frame0, FVector(10.0, 2.0, 0.5),
				FVector(14.0, 2.0, 0.5), 40, EFlowVizLineProbeAxis::Distance, Series));
		TestTrue(TEXT("the outside tail is GAPS, not zeros"), Series.GapCount > 0);
		TestTrue(TEXT("and the range ignores the gaps -- |U| >= 0.93, so a zero-filled "
					  "gap would drag MinY below it"),
			Series.bHasRange && Series.MinY >= 0.9);
		TestFalse(TEXT("the last point (outside) has no value"),
			Series.Points.Last().bHasValue);
	}

	/* == Refusals ============================================================ */
	{
		FFlowVizChartSeries Series;
		TestFalse(TEXT("a zero-length line is refused"),
			FlowVizChart::BuildLineSeries(Frame0, FVector(6.0, 2.0, 0.5),
				FVector(6.0, 2.0, 0.5), 8, EFlowVizLineProbeAxis::Distance, Series));
		TestFalse(TEXT("one sample is refused -- a one-sample line is a point probe"),
			FlowVizChart::BuildLineSeries(Frame0, FVector(1.0, 2.0, 0.5),
				FVector(11.0, 2.0, 0.5), 1, EFlowVizLineProbeAxis::Distance, Series));
	}

	/* == The time series (DoD 11) =========================================== */
	{
		// Three frames, the middle one MISSING (null sampler): the series must
		// keep the full time axis and gap the hole.
		FFlowVizFieldSampler Frame2;
		if (!TestTrue(TEXT("CONTROL: frame 2's sampler builds"),
				Frame2.Build(Case, FName(TEXT("U")), 2).IsOk()))
		{
			return false;
		}

		const FFlowVizFieldSampler* Samplers[] = { &Frame0, nullptr, &Frame2 };
		const double Times[] = { 0.0, 0.1, 0.2 };
		const FVector Probe(6.0, 2.0, 0.5);

		FFlowVizChartSeries Series;
		TestTrue(TEXT("the time series builds"),
			FlowVizChart::BuildTimeSeries(Samplers, Times, Probe, Series));
		TestEqual(TEXT("one point per frame, the missing one included"),
			Series.Points.Num(), 3);
		TestEqual(TEXT("the x axis is physical time"), Series.Points[2].X, 0.2);
		TestTrue(TEXT("the sampled frames have values"),
			Series.Points[0].bHasValue && Series.Points[2].bHasValue);
		TestFalse(TEXT("the missing frame is a GAP at its own time, not a skipped or "
					   "zero-filled point"),
			Series.Points[1].bHasValue);
		TestEqual(TEXT("counted as such"), Series.GapCount, 1);

		TestTrue(TEXT("CONTROL: the two sampled frames differ -- the wake is unsteady, "
					  "which is what makes a time plot worth drawing"),
			!FMath::IsNearlyEqual(Series.Points[0].Y, Series.Points[2].Y, 1e-9));

		FFlowVizChartSeries Mismatched;
		TestFalse(TEXT("mismatched samplers/times lengths are refused"),
			FlowVizChart::BuildTimeSeries(
				Samplers, TArrayView<const double>(Times, 2), Probe, Mismatched));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
