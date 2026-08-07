// Copyright FlowViz contributors. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "UI/SFlowVizChartStrip.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizChartStripTest
{
	/** A series with a gap in the middle: values 1, 2, GAP, 4, 5 at x 0..4. */
	FFlowVizChartSeries MakeGappedSeries()
	{
		FFlowVizChartSeries Series;
		const double Values[] = { 1.0, 2.0, 0.0, 4.0, 5.0 };
		for (int32 Index = 0; Index < 5; ++Index)
		{
			FFlowVizChartPoint& Point = Series.Points.AddDefaulted_GetRef();
			Point.X = static_cast<double>(Index);
			Point.bHasValue = Index != 2;
			Point.Y = Values[Index];
		}
		Series.Finalize();
		return Series;
	}
}

/**
 * The chart strip's polyline builder (#85, DoD 11/12's "displays").
 *
 * The DRAWING is a Slate call; the correctness is in BuildPolylines: series
 * points to screen segments, gaps SPLITTING the line rather than being
 * bridged or zero-plotted. That is the function under test -- the OnPaint
 * body is a thin consumer of its output, the same split the colour ramp
 * strip uses (BuildRampColors tested, MakeGradient trusted).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizChartStripTest,
	"FlowViz.UI.ChartStrip.Polylines",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizChartStripTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizChartStripTest;

	const FVector2D Size(200.0, 100.0);

	/* == A gap splits the polyline =========================================== */
	{
		const FFlowVizChartSeries Series = MakeGappedSeries();
		TArray<TArray<FVector2D>> Polylines;
		TestTrue(TEXT("the gapped series builds"),
			SFlowVizChartStrip::BuildPolylines(Series, Size, Polylines));

		if (!TestEqual(
				TEXT("one gap yields TWO polylines -- a bridged gap would draw flow through "
					 "the solid body the gap represents, a zero-plotted one would draw "
					 "stagnation that is not in the data (rule 10)"),
				Polylines.Num(), 2))
		{
			return false;
		}
		TestEqual(TEXT("the first run has its two points"), Polylines[0].Num(), 2);
		TestEqual(TEXT("the second run has its two points"), Polylines[1].Num(), 2);
	}

	/* == The mapping: x spans the width, y INVERTS (screen y grows down) ===== */
	{
		const FFlowVizChartSeries Series = MakeGappedSeries();
		TArray<TArray<FVector2D>> Polylines;
		SFlowVizChartStrip::BuildPolylines(Series, Size, Polylines);

		const FVector2D& First = Polylines[0][0];    // x=0, y=1 (the minimum)
		const FVector2D& Last = Polylines[1].Last(); // x=4, y=5 (the maximum)

		TestTrue(TEXT("the first point sits at the left edge"),
			FMath::IsNearlyEqual(First.X, 0.0, 1e-6));
		TestTrue(TEXT("the last point sits at the right edge"),
			FMath::IsNearlyEqual(Last.X, Size.X, 1e-6));

		// Screen y grows DOWNWARD: the series MINIMUM must be at the BOTTOM
		// (large y) and the maximum at the top. An un-inverted map draws every
		// chart upside down -- plausible at a glance, wrong at first reading.
		TestTrue(TEXT("the minimum value maps to the bottom of the strip"),
			FMath::IsNearlyEqual(First.Y, Size.Y, 1e-6));
		TestTrue(TEXT("and the maximum to the top"),
			FMath::IsNearlyEqual(Last.Y, 0.0, 1e-6));
	}

	/* == Degenerate series =================================================== */
	{
		// All gaps: nothing to draw, and that is an ANSWER (false), not a crash.
		FFlowVizChartSeries AllGaps;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			FFlowVizChartPoint& Point = AllGaps.Points.AddDefaulted_GetRef();
			Point.X = Index;
			Point.bHasValue = false;
		}
		AllGaps.Finalize();

		TArray<TArray<FVector2D>> Polylines;
		TestFalse(TEXT("an all-gap series declines to build"),
			SFlowVizChartStrip::BuildPolylines(AllGaps, Size, Polylines));
		TestEqual(TEXT("with no polylines"), Polylines.Num(), 0);

		// A CONSTANT series has zero y-range; it must draw a mid-strip line
		// rather than dividing by zero.
		FFlowVizChartSeries Constant;
		for (int32 Index = 0; Index < 3; ++Index)
		{
			FFlowVizChartPoint& Point = Constant.Points.AddDefaulted_GetRef();
			Point.X = Index;
			Point.bHasValue = true;
			Point.Y = 7.0;
		}
		Constant.Finalize();

		TestTrue(TEXT("a constant series builds"),
			SFlowVizChartStrip::BuildPolylines(Constant, Size, Polylines));
		TestEqual(TEXT("as one run"), Polylines.Num(), 1);
		TestTrue(TEXT("drawn mid-strip rather than NaN from the zero range"),
			FMath::IsNearlyEqual(Polylines[0][0].Y, Size.Y * 0.5, 1e-6)
				&& FMath::IsFinite(Polylines[0][0].Y));
	}

	/* == The widget itself builds and tolerates an empty series ============== */
	{
		const TSharedRef<SFlowVizChartStrip> Strip = SNew(SFlowVizChartStrip);
		Strip->SetSeries(FFlowVizChartSeries());
		// No crash on paint is covered by construction here; the paint itself
		// needs a Slate frame. The seam a test CAN check: the strip reports its
		// series state so the panel can show "no data" beside it.
		TestFalse(TEXT("an empty strip reports no drawable series"),
			Strip->HasDrawableSeries());

		Strip->SetSeries(MakeGappedSeries());
		TestTrue(TEXT("a real series is drawable"), Strip->HasDrawableSeries());
		TestEqual(TEXT("and its gap count is surfaced for the caption"),
			Strip->GetSeries().GapCount, 1);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
