// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizChartStrip.h"

#include "Rendering/DrawElements.h"
#include "UI/FlowVizWorkspaceStyle.h"

void SFlowVizChartStrip::Construct(const FArguments& InArgs)
{
	SetCanTick(false);
}

void SFlowVizChartStrip::SetSeries(const FFlowVizChartSeries& InSeries)
{
	Series = InSeries;
	Invalidate(EInvalidateWidgetReason::Paint);
}

bool SFlowVizChartStrip::HasDrawableSeries() const
{
	int32 Valued = 0;
	for (const FFlowVizChartPoint& Point : Series.Points)
	{
		if (Point.bHasValue && ++Valued >= 2)
		{
			return true;
		}
	}
	return false;
}

bool SFlowVizChartStrip::BuildPolylines(
	const FFlowVizChartSeries& InSeries,
	const FVector2D& Size,
	TArray<TArray<FVector2D>>& OutPolylines)
{
	OutPolylines.Reset();

	if (!InSeries.bHasRange || InSeries.Points.Num() < 2 || Size.X <= 0.0 || Size.Y <= 0.0)
	{
		return false;
	}

	int32 Valued = 0;
	for (const FFlowVizChartPoint& Point : InSeries.Points)
	{
		if (Point.bHasValue)
		{
			++Valued;
		}
	}
	if (Valued < 2)
	{
		return false;
	}

	const double MinX = InSeries.Points[0].X;
	const double MaxX = InSeries.Points.Last().X;
	const double SpanX = MaxX - MinX;
	const double SpanY = InSeries.MaxY - InSeries.MinY;

	TArray<FVector2D>* Run = nullptr;
	for (const FFlowVizChartPoint& Point : InSeries.Points)
	{
		if (!Point.bHasValue)
		{
			// A GAP ENDS THE RUN. The next valued point starts a new polyline,
			// so the gap draws as daylight rather than a bridge or a zero.
			Run = nullptr;
			continue;
		}

		if (Run == nullptr)
		{
			Run = &OutPolylines.AddDefaulted_GetRef();
		}

		const double AlphaX = SpanX > 0.0 ? (Point.X - MinX) / SpanX : 0.0;
		// INVERTED: screen y grows downward, so MinY sits at the bottom. A
		// constant series (SpanY == 0) draws mid-strip rather than dividing by
		// zero into NaN geometry.
		const double AlphaY = SpanY > 0.0 ? (Point.Y - InSeries.MinY) / SpanY : 0.5;
		Run->Add(FVector2D(AlphaX * Size.X, (1.0 - AlphaY) * Size.Y));
	}

	// Single-point runs (a valued point between two gaps) have no line to draw;
	// they are kept -- OnPaint renders them as a dot -- so an isolated reading
	// does not silently vanish from the plot.
	return OutPolylines.Num() > 0;
}

FVector2D SFlowVizChartStrip::ComputeDesiredSize(float) const
{
	const float U = FlowVizWorkspaceStyle::GetUnit();
	return FVector2D(60.0f * U, 24.0f * U);
}

int32 SFlowVizChartStrip::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
	const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FVector2D Size = AllottedGeometry.GetLocalSize();
	if (Size.X <= 0.0 || Size.Y <= 0.0)
	{
		return LayerId;
	}

	// The well, always: an empty chart looks like an empty chart, not a hole.
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
		AllottedGeometry.ToPaintGeometry(), FlowVizWorkspaceStyle::GetRaisedBrush(),
		ESlateDrawEffect::None, FlowVizWorkspaceStyle::GetPanelRaisedColor());
	int32 CurrentLayer = LayerId + 1;

	TArray<TArray<FVector2D>> Polylines;
	if (!BuildPolylines(Series, Size, Polylines))
	{
		return CurrentLayer;
	}

	const FLinearColor LineColor = FlowVizWorkspaceStyle::GetAccentColor();
	for (const TArray<FVector2D>& Run : Polylines)
	{
		if (Run.Num() >= 2)
		{
			FSlateDrawElement::MakeLines(OutDrawElements, CurrentLayer,
				AllottedGeometry.ToPaintGeometry(), Run, ESlateDrawEffect::None,
				LineColor, /*bAntialias*/ true, /*Thickness*/ 1.5f);
		}
		else if (Run.Num() == 1)
		{
			// An isolated reading between gaps: a dot, not nothing.
			TArray<FVector2D> Dot;
			Dot.Add(Run[0] + FVector2D(-1.0, 0.0));
			Dot.Add(Run[0] + FVector2D(1.0, 0.0));
			FSlateDrawElement::MakeLines(OutDrawElements, CurrentLayer,
				AllottedGeometry.ToPaintGeometry(), Dot, ESlateDrawEffect::None,
				LineColor, /*bAntialias*/ true, /*Thickness*/ 3.0f);
		}
	}

	return CurrentLayer + 1;
}
