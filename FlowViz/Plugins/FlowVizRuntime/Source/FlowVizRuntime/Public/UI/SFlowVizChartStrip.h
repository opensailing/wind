// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Flow/FlowVizChartSeries.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SLeafWidget.h"

/**
 * A line chart as a custom-painted leaf (#85 / DoD 11 and 12's "displays").
 *
 * SAME SPLIT AS THE COLOUR RAMP STRIP: the correctness lives in a testable
 * builder (BuildPolylines -- series to screen segments, with the gap and
 * inversion rules), and OnPaint is a thin consumer calling MakeLines on its
 * output. A chart widget whose mapping is inside the paint call is a chart
 * nobody can test headless.
 *
 * GAPS SPLIT THE LINE. A gap bridged draws flow through the solid body the
 * gap represents; a gap plotted at zero draws stagnation that is not in the
 * data (rule 10). Two shorter polylines with visible daylight between them
 * is the honest picture, and the caption beside the strip says "N gaps".
 *
 * THE SERIES IS SET, NOT BOUND. Chart data comes from the sampling service's
 * worker at its own cadence; a per-paint pull would re-run the mapping every
 * frame for a series that changes at most once per displayed frame.
 */
class FLOWVIZRUNTIME_API SFlowVizChartStrip : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SFlowVizChartStrip)
	{
	}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Replace the drawn series. Repaints on the next frame. */
	void SetSeries(const FFlowVizChartSeries& InSeries);

	const FFlowVizChartSeries& GetSeries() const { return Series; }

	/** True when the series has at least two valued points -- something to draw a line through. */
	bool HasDrawableSeries() const;

	/**
	 * Map a series into screen-space polylines, one per gap-free run.
	 *
	 * X maps [first.X, last.X] to [0, Size.X]; Y maps [MinY, MaxY] to
	 * [Size.Y, 0] -- INVERTED, because screen y grows downward. A constant
	 * series (zero y-range) draws mid-strip rather than dividing by zero.
	 *
	 * @return false when fewer than two valued points exist; OutPolylines is
	 *         emptied then.
	 */
	static bool BuildPolylines(
		const FFlowVizChartSeries& InSeries,
		const FVector2D& Size,
		TArray<TArray<FVector2D>>& OutPolylines);

	/* --- SLeafWidget ------------------------------------------------------- */

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float) const override;

private:
	FFlowVizChartSeries Series;
};
