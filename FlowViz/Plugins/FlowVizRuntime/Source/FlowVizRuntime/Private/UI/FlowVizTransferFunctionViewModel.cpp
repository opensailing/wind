// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizTransferFunctionViewModel.h"

/**
 * See FlowVizTransferFunctionViewModel.h for the design rules.
 *
 * THERE IS NO COLOUR ARITHMETIC IN THIS FILE, AND THAT IS THE POINT. Every
 * question about what colour a value takes is answered by
 * Render/FlowVizTransferFunction.* and CFDViz::ColorMaps. This file decides WHICH
 * transfer function, never what it evaluates to. The zero-centring rule in
 * particular is called, not copied: MakeDefaultDomain is the one implementation
 * of "a diverging map centres on zero", so a figure produced through this view
 * model and one produced through the render layer cannot disagree about it.
 */

// Named rather than anonymous: see the note in FlowVizClipViewModel.cpp. Under a
// unity build these helpers share a translation unit with the sibling view
// models, and an anonymous namespace would collide with their same-named ones.
namespace FlowVizTransferFunctionViewModelLocal
{
	FCFDVizResult MakeUnboundResult()
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			TEXT("No field is bound, so there is nothing to colour"));
	}

	/** Component count a choice requires. Magnitude works on anything, so it returns 1. */
	int32 RequiredComponentCount(EFlowVizComponentChoice Choice)
	{
		switch (Choice)
		{
			case EFlowVizComponentChoice::X: return 1;
			case EFlowVizComponentChoice::Y: return 2;
			case EFlowVizComponentChoice::Z: return 3;
			case EFlowVizComponentChoice::W: return 4;
			case EFlowVizComponentChoice::Magnitude: return 1;
			default: return TNumericLimits<int32>::Max();
		}
	}

	const TCHAR* ComponentChoiceName(EFlowVizComponentChoice Choice)
	{
		switch (Choice)
		{
			case EFlowVizComponentChoice::X: return TEXT("X");
			case EFlowVizComponentChoice::Y: return TEXT("Y");
			case EFlowVizComponentChoice::Z: return TEXT("Z");
			case EFlowVizComponentChoice::W: return TEXT("W");
			case EFlowVizComponentChoice::Magnitude: return TEXT("magnitude");
			default: return TEXT("<unknown>");
		}
	}
}


FFlowVizTransferFunctionViewModel::FFlowVizTransferFunctionViewModel()
{
	// An unbound view model still produces a VALID transfer function. Returning a
	// degenerate one - a zero-width domain, say - would make every caller add a
	// "did you bind first" branch, and the ones that forgot would divide by zero
	// somewhere far from here.
	TransferFunction = FFlowVizTransferFunction::MakeDefault(CFDViz::ColorMaps::Default, 0.0f, 1.0f);
}

/* ========================================================================== */
/* Field binding                                                               */
/* ========================================================================== */

FCFDVizResult FFlowVizTransferFunctionViewModel::BindField(const FCFDVizCase& Case, FName InFieldId)
{
	const FCFDVizField* Field = Case.FindField(InFieldId);
	if (Field == nullptr)
	{
		// NOTHING IS CHANGED ON FAILURE - not even partially. A view model that
		// half-adopted a missing field would colour the previous field's data
		// against the new field's name.
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(TEXT("The case declares no field '%s'"), *InFieldId.ToString()));
	}

	bBound = true;
	FieldId = InFieldId;
	ComponentCount = FMath::Max(1, Field->ComponentCount);

	/* -- The colormap the manifest asked for, when we know it ---------------- */
	ECFDVizColorMap Map = CFDViz::ColorMaps::Default;
	if (!Field->Display.DefaultColorMap.IsEmpty())
	{
		ECFDVizColorMap Parsed = CFDViz::ColorMaps::Default;
		if (CFDViz::ColorMaps::TryParse(FName(*Field->Display.DefaultColorMap), Parsed))
		{
			Map = Parsed;
		}
		// An unrecognised name falls back to viridis rather than failing the bind.
		// Format rule 1.6 makes display hints advisory: refusing to show a field
		// because a future colormap name was added is a worse outcome than showing
		// it on the default map.
	}
	TransferFunction.ColorMap = Map;

	/* -- The component the manifest asked for -------------------------------- */
	// Default when nothing is declared: magnitude for a vector, component 0 for a
	// scalar. Magnitude of a 1-component field is |x|, which is NOT the field -
	// it loses the sign, and for a signed scalar like pressure that is a silent
	// data corruption in the display.
	Component = ComponentCount > 1 ? EFlowVizComponentChoice::Magnitude : EFlowVizComponentChoice::X;
	if (!Field->Display.DefaultComponent.IsEmpty())
	{
		if (Field->Display.DefaultComponent.Equals(
				CFDViz::MagnitudeComponentName, ESearchCase::CaseSensitive))
		{
			Component = EFlowVizComponentChoice::Magnitude;
		}
		else
		{
			const int32 Index = Field->FindComponentIndex(Field->Display.DefaultComponent);
			if (Index >= 0 && Index <= 3)
			{
				Component = static_cast<EFlowVizComponentChoice>(Index);
			}
		}
	}

	/* -- The declared global range, if there is one -------------------------- */
	// WHICH declared range depends on the component: a vector's magnitude range
	// and its per-component ranges are different numbers, and colouring a
	// magnitude against a component's range washes out the top of the map.
	bRangeKnown = false;
	GlobalMin = 0.0f;
	GlobalMax = 1.0f;

	const FCFDVizFieldStatistics& Stats = Field->Statistics;
	if (Component == EFlowVizComponentChoice::Magnitude && Stats.bHasMagnitudeRange)
	{
		GlobalMin = static_cast<float>(Stats.GlobalMagnitudeMin);
		GlobalMax = static_cast<float>(Stats.GlobalMagnitudeMax);
		bRangeKnown = true;
	}
	else if (Stats.bHasComponentRange)
	{
		const int32 Index = static_cast<int32>(Component);
		if (Stats.GlobalComponentMin.IsValidIndex(Index) && Stats.GlobalComponentMax.IsValidIndex(Index))
		{
			GlobalMin = static_cast<float>(Stats.GlobalComponentMin[Index]);
			GlobalMax = static_cast<float>(Stats.GlobalComponentMax[Index]);
			bRangeKnown = true;
		}
	}

	if (!bRangeKnown && Field->Display.RecommendedRange.IsSet())
	{
		// A recommended range is a declared range, so it counts as known: the
		// author measured something to write it. It ranks below statistics because
		// statistics describe the data and a recommendation describes a preference.
		const FVector2D& Recommended = Field->Display.RecommendedRange.GetValue();
		GlobalMin = static_cast<float>(Recommended.X);
		GlobalMax = static_cast<float>(Recommended.Y);
		bRangeKnown = true;
	}

	/* -- The opacity ramp the manifest asked for ----------------------------- */
	FFlowVizOpacityCurve Curve = FFlowVizOpacityCurve::MakeLinearRamp();
	if (Field->Display.OpacityPoints.Num() > 0 && bRangeKnown && GlobalMax > GlobalMin)
	{
		// The manifest's points are in the FIELD'S OWN UNIT; FFlowVizOpacityCurve
		// positions are NORMALISED over the domain. Storing one as the other is a
		// unit error that renders as an opacity ramp compressed against the left
		// edge - plausible, and wrong.
		FFlowVizOpacityCurve FromManifest;
		const float Span = GlobalMax - GlobalMin;
		for (const FVector2D& Point : Field->Display.OpacityPoints)
		{
			const float Normalized = (static_cast<float>(Point.X) - GlobalMin) / Span;
			FromManifest.Points.Add(FFlowVizOpacityPoint(
				FMath::Clamp(Normalized, 0.0f, 1.0f),
				FMath::Clamp(static_cast<float>(Point.Y), 0.0f, 1.0f)));
		}
		FromManifest.SortPoints();
		if (FromManifest.Validate().IsOk())
		{
			Curve = MoveTemp(FromManifest);
		}
		// An invalid declared curve falls back to the linear ramp rather than
		// failing the bind, for the same reason as the colormap name.
	}
	TransferFunction.Opacity = MoveTemp(Curve);

	/* -- Rule 8: global by default ------------------------------------------- */
	bHasFrameRange = false;
	RangeSource = EFlowVizRangeSource::Global;
	return ApplyRangeFromSource();
}

void FFlowVizTransferFunctionViewModel::Unbind()
{
	bBound = false;
	FieldId = FName();
	ComponentCount = 1;
	Component = EFlowVizComponentChoice::Magnitude;
	// THE RANGE IS DROPPED, NOT KEPT. A stale domain from the previous field is
	// the ingredient of a mislabelled figure: the legend would read the old
	// field's numbers under the new field's name.
	bRangeKnown = false;
	GlobalMin = 0.0f;
	GlobalMax = 1.0f;
	bHasFrameRange = false;
	RangeSource = EFlowVizRangeSource::Global;
	TransferFunction = FFlowVizTransferFunction::MakeDefault(CFDViz::ColorMaps::Default, 0.0f, 1.0f);
}

/* ========================================================================== */
/* Component                                                                   */
/* ========================================================================== */

FCFDVizResult FFlowVizTransferFunctionViewModel::SetComponent(EFlowVizComponentChoice InComponent)
{
	if (!bBound)
	{
		return FlowVizTransferFunctionViewModelLocal::MakeUnboundResult();
	}
	const int32 Required = FlowVizTransferFunctionViewModelLocal::RequiredComponentCount(InComponent);
	if (Required > ComponentCount)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("Component %s needs at least %d components; field '%s' has %d"),
				FlowVizTransferFunctionViewModelLocal::ComponentChoiceName(InComponent), Required, *FieldId.ToString(), ComponentCount));
	}
	Component = InComponent;
	// The domain follows the component when it came from the manifest, because
	// the two are different numbers per component. A MANUAL domain is left alone:
	// the user typed it.
	return ApplyRangeFromSource();
}

EFlowVizComponentMode FFlowVizTransferFunctionViewModel::GetShaderComponentMode() const
{
	// A switch rather than a cast. The two enums agree today, and a cast would
	// keep compiling - silently mapping to the wrong component - if either were
	// reordered.
	switch (Component)
	{
		case EFlowVizComponentChoice::X: return EFlowVizComponentMode::X;
		case EFlowVizComponentChoice::Y: return EFlowVizComponentMode::Y;
		case EFlowVizComponentChoice::Z: return EFlowVizComponentMode::Z;
		case EFlowVizComponentChoice::W: return EFlowVizComponentMode::W;
		case EFlowVizComponentChoice::Magnitude: return EFlowVizComponentMode::Magnitude;
		default: return EFlowVizComponentMode::Magnitude;
	}
}

/* ========================================================================== */
/* Colormap                                                                    */
/* ========================================================================== */

FCFDVizResult FFlowVizTransferFunctionViewModel::SetColorMap(ECFDVizColorMap Map)
{
	if (Map >= ECFDVizColorMap::Count)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange, TEXT("That colormap is not one this build ships"));
	}
	// THE RANGE IS NOT RE-DERIVED. Switching to coolwarm does not silently
	// re-centre a domain the user typed; ResetRange is how they ask for that.
	// Losing a typed domain on a colormap change reads as a rendering bug.
	TransferFunction.ColorMap = Map;
	return FCFDVizResult::Ok();
}

void FFlowVizTransferFunctionViewModel::SetReverseColorMap(bool bReverse)
{
	TransferFunction.bReverseColorMap = bReverse;
}

FCFDVizResult FFlowVizTransferFunctionViewModel::SetColorBands(int32 Bands)
{
	if (Bands < 0)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange, TEXT("A band count cannot be negative"));
	}
	TransferFunction.ColorBands = Bands;
	return FCFDVizResult::Ok();
}

bool FFlowVizTransferFunctionViewModel::IsColorMapPerceptuallyUniform() const
{
	return CFDViz::ColorMaps::IsPerceptuallyUniform(TransferFunction.ColorMap);
}

/* ========================================================================== */
/* Range                                                                       */
/* ========================================================================== */

FCFDVizResult FFlowVizTransferFunctionViewModel::ApplyRangeFromSource()
{
	float SourceMin = 0.0f;
	float SourceMax = 1.0f;

	switch (RangeSource)
	{
		case EFlowVizRangeSource::Global:
			SourceMin = GlobalMin;
			SourceMax = GlobalMax;
			break;
		case EFlowVizRangeSource::CurrentFrame:
			SourceMin = FrameMin;
			SourceMax = FrameMax;
			break;
		case EFlowVizRangeSource::Manual:
			// A MANUAL RANGE IS TAKEN EXACTLY AS TYPED and returns early: it does
			// NOT go through MakeDefaultDomain. Zero-centring a typed range would
			// silently rewrite the user's input - the numeric-entry equivalent of
			// altering a stored value.
			TransferFunction.ValueRangeMin = ManualMin;
			TransferFunction.ValueRangeMax = ManualMax;
			return FCFDVizResult::Ok();
		default:
			break;
	}

	// THE ONE IMPLEMENTATION OF VISUAL_QA RULE 7. A diverging map is centred on
	// zero here and nowhere else, so this view model and the render layer cannot
	// drift apart on it. It also handles the degenerate and non-finite cases by
	// yielding [0,1] rather than a zero-width domain.
	FFlowVizTransferFunction::MakeDefaultDomain(
		TransferFunction.ColorMap,
		SourceMin,
		SourceMax,
		TransferFunction.ValueRangeMin,
		TransferFunction.ValueRangeMax);

	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizTransferFunctionViewModel::SetRangeSource(EFlowVizRangeSource Source)
{
	if (Source == EFlowVizRangeSource::CurrentFrame && !bHasFrameRange)
	{
		// RULE 15 IN THE MODEL. Nothing measures a per-frame range yet. Accepting
		// the mode and behaving like Global would ship a radio button that appears
		// to work, and would then LABEL the figure "per-frame range" while showing
		// a global one - a rule 8 mislabelling on top of a rule 15 dead control.
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			TEXT("No per-frame range has been supplied, so the per-frame range mode is "
				 "unavailable. Call SetCurrentFrameRange first."));
	}
	if (Source == EFlowVizRangeSource::Global && !bBound)
	{
		return FlowVizTransferFunctionViewModelLocal::MakeUnboundResult();
	}

	const EFlowVizRangeSource Previous = RangeSource;
	RangeSource = Source;
	const FCFDVizResult Applied = ApplyRangeFromSource();
	if (!Applied.IsOk())
	{
		RangeSource = Previous;
	}
	return Applied;
}

FCFDVizResult FFlowVizTransferFunctionViewModel::SetManualRange(float Min, float Max)
{
	if (!FMath::IsFinite(Min) || !FMath::IsFinite(Max))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange, TEXT("A colour range bound must be finite"));
	}
	if (!(Max > Min))
	{
		// Inverted AND zero-width both land here. A zero-width domain divides by
		// zero in every mapping; an inverted one silently reverses the colormap,
		// which is a different control the user did not touch.
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(TEXT("A colour range needs max > min; got [%g, %g]"), Min, Max));
	}

	ManualMin = Min;
	ManualMax = Max;
	RangeSource = EFlowVizRangeSource::Manual;
	return ApplyRangeFromSource();
}

FCFDVizResult FFlowVizTransferFunctionViewModel::SetCurrentFrameRange(float Min, float Max)
{
	if (!FMath::IsFinite(Min) || !FMath::IsFinite(Max))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange, TEXT("A per-frame range bound must be finite"));
	}
	if (!(Max > Min))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(TEXT("A per-frame range needs max > min; got [%g, %g]"), Min, Max));
	}

	FrameMin = Min;
	FrameMax = Max;
	bHasFrameRange = true;

	// Supplying a range does NOT switch to it. A frame advance that silently
	// re-ranged the colours would be exactly the pulsing rule 8 forbids; the
	// switch stays an explicit act.
	if (RangeSource == EFlowVizRangeSource::CurrentFrame)
	{
		return ApplyRangeFromSource();
	}
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizTransferFunctionViewModel::ResetRange()
{
	if (!bBound)
	{
		return FlowVizTransferFunctionViewModelLocal::MakeUnboundResult();
	}
	// Reset means "back to the field's declared range", which is Global - and it
	// re-derives through MakeDefaultDomain, so a diverging map re-centres.
	RangeSource = EFlowVizRangeSource::Global;
	return ApplyRangeFromSource();
}

/* ========================================================================== */
/* Opacity                                                                     */
/* ========================================================================== */

FCFDVizResult FFlowVizTransferFunctionViewModel::SetOpacityCurve(const FFlowVizOpacityCurve& Curve)
{
	// VALIDATE BEFORE ASSIGNING. Assigning first and rolling back on failure
	// leaves a window in which a caller reading through GetTransferFunction sees
	// the invalid curve, and there is no reason to open it.
	const FCFDVizResult Valid = Curve.Validate();
	if (!Valid.IsOk())
	{
		return Valid;
	}
	TransferFunction.Opacity = Curve;
	TransferFunction.Opacity.SortPoints();
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizTransferFunctionViewModel::SetOpacityMultiplier(float Multiplier)
{
	if (!FMath::IsFinite(Multiplier) || Multiplier < 0.0f)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			TEXT("An opacity multiplier must be finite and non-negative"));
	}
	OpacityMultiplier = Multiplier;
	return FCFDVizResult::Ok();
}

/* ========================================================================== */
/* Invalid-value colours                                                       */
/* ========================================================================== */

void FFlowVizTransferFunctionViewModel::SetUnderRangeColor(const FLinearColor& Color)
{
	TransferFunction.UnderRangeColor = Color;
}

void FFlowVizTransferFunctionViewModel::SetOverRangeColor(const FLinearColor& Color)
{
	TransferFunction.OverRangeColor = Color;
}

void FFlowVizTransferFunctionViewModel::SetNaNColor(const FLinearColor& Color)
{
	TransferFunction.NaNColor = Color;
}

void FFlowVizTransferFunctionViewModel::SetMaskedColor(const FLinearColor& Color)
{
	TransferFunction.MaskedColor = Color;
}

void FFlowVizTransferFunctionViewModel::SetClampToRange(bool bClamp)
{
	TransferFunction.bClampToRange = bClamp;
}

/* ========================================================================== */
/* What the render layer consumes                                              */
/* ========================================================================== */

FCFDVizResult FFlowVizTransferFunctionViewModel::ApplyToRayMarchParameters(
	FFlowVizVolumeRayMarchParameters& OutParameters) const
{
	// VALIDATE FIRST AND WRITE NOTHING ON FAILURE. A half-populated constant
	// buffer renders a plausible wrong image, which is worse than rendering
	// nothing: nobody investigates an image that looks fine.
	const FCFDVizResult Valid = TransferFunction.Validate();
	if (!Valid.IsOk())
	{
		return Valid;
	}

	OutParameters.ValueRangeMin = TransferFunction.ValueRangeMin;
	OutParameters.ValueRangeMax = TransferFunction.ValueRangeMax;
	OutParameters.ComponentMode = static_cast<uint32>(GetShaderComponentMode());
	OutParameters.OpacityMultiplier = OpacityMultiplier;

	OutParameters.UnderRangeColor = TransferFunction.UnderRangeColor;
	OutParameters.OverRangeColor = TransferFunction.OverRangeColor;
	OutParameters.NaNColor = TransferFunction.NaNColor;
	OutParameters.MaskedColor = TransferFunction.MaskedColor;

	// THIS LINE IS THE ONE THAT ROTS QUIETLY IF IT GOES MISSING. The ray-march
	// block grew bClampToRange recently; before it did, this function had nowhere
	// to put the flag. If this write disappears, the user's clamp choice stops
	// here - one layer above the gap that used to exist - while a green
	// shader-level test next door attests that the plumbing works.
	OutParameters.bClampToRange = TransferFunction.bClampToRange ? 1u : 0u;

	// NoDataColor is deliberately not written: it has no counterpart in
	// FFlowVizTransferFunction, so it belongs to whoever fills the defaults.
	return FCFDVizResult::Ok();
}
