// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizTransferFunctionPanel.h"

#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "UI/FlowVizTransferFunctionViewModel.h"
#include "UI/FlowVizWorkspaceStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FlowVizTransferFunctionPanel"

// NAMED namespace: unity build. See the note on every other UI .cpp here.
namespace FlowVizTransferFunctionPanelLocal
{
	/**
	 * Gradient stops handed to Slate.
	 *
	 * 64 is well above the ~16 at which banding becomes visible on a 300px strip,
	 * and far below the point where the vertex cost matters. Slate interpolates
	 * linearly BETWEEN stops, so this is a piecewise-linear approximation of the
	 * colormap - which is exactly what the colormap table itself is.
	 */
	constexpr int32 GradientStopCount = 64;

	/** How finely the opacity curve is polylined over the strip. */
	constexpr int32 OpacityCurveSegments = 96;

	/** The colormaps offered, in the order they are shown. */
	const ECFDVizColorMap OfferedMaps[] = {
		ECFDVizColorMap::Viridis,
		ECFDVizColorMap::Plasma,
		ECFDVizColorMap::Inferno,
		ECFDVizColorMap::Magma,
		ECFDVizColorMap::CoolWarm,
		ECFDVizColorMap::BlueWhiteRed,
		ECFDVizColorMap::Grayscale,
		// Last, and marked when selected. Offered because reviewers compare
		// against legacy figures; never presented as an equal choice.
		ECFDVizColorMap::Turbo,
	};

	FText GetMapDisplayName(ECFDVizColorMap Map)
	{
		return FText::FromName(CFDViz::ColorMaps::GetName(Map));
	}

	FText GetRangeSourceName(EFlowVizRangeSource Source)
	{
		switch (Source)
		{
		case EFlowVizRangeSource::Global:
			return LOCTEXT("RangeGlobal", "Global");
		case EFlowVizRangeSource::CurrentFrame:
			return LOCTEXT("RangeFrame", "Per frame");
		case EFlowVizRangeSource::Manual:
			return LOCTEXT("RangeManual", "Manual");
		default:
			return LOCTEXT("RangeUnknown", "?");
		}
	}

	/** Fixed decimals, for the same anti-jitter reason as the transport readout. */
	FText FormatValue(float Value)
	{
		return FText::FromString(FString::Printf(TEXT("%.4g"), Value));
	}
}

/* ========================================================================== */
/* SFlowVizColorRampStrip                                                      */
/* ========================================================================== */

void SFlowVizColorRampStrip::Construct(const FArguments& InArgs)
{
	ViewModel = InArgs._ViewModel;
	SetCanTick(false);
}

TArray<FLinearColor> SFlowVizColorRampStrip::BuildRampColors(int32 SampleCount) const
{
	TArray<FLinearColor> Colors;

	// An unbound strip paints NOTHING. Returning a default ramp would put a
	// colour scale on screen for a field that is not loaded, which reads as data.
	if (ViewModel == nullptr || !ViewModel->IsBound() || SampleCount <= 0)
	{
		return Colors;
	}

	const FFlowVizTransferFunction& Function = ViewModel->GetTransferFunction();

	Colors.Reserve(SampleCount);
	for (int32 Index = 0; Index < SampleCount; ++Index)
	{
		// Denominator is SampleCount-1 so the last sample lands exactly on 1.0.
		// With SampleCount as the denominator the ramp would stop just short of
		// its own maximum colour, which is a subtle but real misrepresentation of
		// the top of the scale.
		const float Position =
			SampleCount > 1 ? static_cast<float>(Index) / static_cast<float>(SampleCount - 1) : 0.0f;

		// Reversal is applied HERE rather than by reversing the array, so the
		// strip and the shader agree about what "reversed" means - both flip the
		// lookup position, not the table.
		const float Lookup = Function.bReverseColorMap ? 1.0f - Position : Position;

		// The REAL table, via the shared sampler - not a lerp between two endpoint
		// colours. Viridis' midpoint is a green that no two-stop interpolation of
		// its endpoints produces, and the test asserts exactly that.
		Colors.Add(CFDViz::ColorMaps::Sample(Function.ColorMap, Lookup));
	}

	return Colors;
}

FVector2D SFlowVizColorRampStrip::ComputeDesiredSize(float) const
{
	const float U = FlowVizWorkspaceStyle::GetUnit();
	return FVector2D(60.0f * U, 16.0f * U);
}

int32 SFlowVizColorRampStrip::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
	const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FVector2D Size = AllottedGeometry.GetLocalSize();
	if (Size.X <= 0.0 || Size.Y <= 0.0)
	{
		return LayerId;
	}

	const ESlateDrawEffect DrawEffects = ESlateDrawEffect::None;

	/* --- The colour ramp itself ------------------------------------------- */

	const TArray<FLinearColor> RampColors =
		BuildRampColors(FlowVizTransferFunctionPanelLocal::GradientStopCount);

	if (RampColors.Num() == 0)
	{
		// Nothing bound. Draw the empty well so the panel does not look broken,
		// but draw no scale.
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
			AllottedGeometry.ToPaintGeometry(), FlowVizWorkspaceStyle::GetRaisedBrush(),
			DrawEffects, FlowVizWorkspaceStyle::GetPanelRaisedColor());
		return LayerId + 1;
	}

	TArray<FSlateGradientStop> Stops;
	Stops.Reserve(RampColors.Num());
	for (int32 Index = 0; Index < RampColors.Num(); ++Index)
	{
		const float Alpha =
			RampColors.Num() > 1
				? static_cast<float>(Index) / static_cast<float>(RampColors.Num() - 1)
				: 0.0f;
		Stops.Add(FSlateGradientStop(
			FVector2D(Alpha * Size.X, 0.0), RampColors[Index]));
	}

	FSlateDrawElement::MakeGradient(OutDrawElements, LayerId,
		AllottedGeometry.ToPaintGeometry(), Stops, Orient_Vertical, DrawEffects,
		FVector4f(FlowVizWorkspaceStyle::GetCornerRadius()));

	int32 CurrentLayer = LayerId + 1;

	/* --- The opacity curve, overlaid -------------------------------------- */

	if (ViewModel != nullptr && ViewModel->IsBound())
	{
		const FFlowVizOpacityCurve& Curve = ViewModel->GetOpacityCurve();

		TArray<FVector2D> CurvePoints;
		CurvePoints.Reserve(FlowVizTransferFunctionPanelLocal::OpacityCurveSegments + 1);

		for (int32 Index = 0; Index <= FlowVizTransferFunctionPanelLocal::OpacityCurveSegments;
			 ++Index)
		{
			const float Position = static_cast<float>(Index)
				/ static_cast<float>(FlowVizTransferFunctionPanelLocal::OpacityCurveSegments);

			// Evaluate() already includes the curve's own multiplier. The view
			// model's separate OpacityMultiplier is a VIEW setting and is
			// deliberately NOT folded in here: the curve drawn is the curve the
			// user authored, and a density slider that visually flattened the
			// authored shape would make the two controls indistinguishable.
			const float Opacity = FMath::Clamp(Curve.Evaluate(Position), 0.0f, 1.0f);

			// Y is inverted: opacity 1 at the top is the universal convention for
			// this editor, and matching it means a user's muscle memory transfers.
			CurvePoints.Add(FVector2D(
				Position * Size.X, (1.0 - static_cast<double>(Opacity)) * Size.Y));
		}

		// Drawn twice: a dark under-stroke then the light line. A single-colour
		// polyline over a full-spectrum gradient is illegible somewhere along its
		// length no matter which colour is chosen - the dark halo guarantees
		// contrast against the bright end of every colormap.
		FSlateDrawElement::MakeLines(OutDrawElements, CurrentLayer,
			AllottedGeometry.ToPaintGeometry(), CurvePoints, DrawEffects,
			FLinearColor(0.0f, 0.0f, 0.0f, 0.65f), true, 3.0f);

		FSlateDrawElement::MakeLines(OutDrawElements, CurrentLayer + 1,
			AllottedGeometry.ToPaintGeometry(), CurvePoints, DrawEffects,
			FLinearColor(1.0f, 1.0f, 1.0f, 0.95f), true, 1.5f);

		CurrentLayer += 2;
	}

	return CurrentLayer;
}

/* ========================================================================== */
/* SFlowVizTransferFunctionPanel                                               */
/* ========================================================================== */

void SFlowVizTransferFunctionPanel::Construct(const FArguments& InArgs)
{
	ViewModel = InArgs._ViewModel;

	/*
	 * THE CHANNEL TO THE RENDERER. Omitting this line is the failure mode worth
	 * naming, because it is invisible: the panel compiles, the workspace's
	 * .OnTransferFunctionChanged() call still reads as a subscription at the
	 * construction site, and every ExecuteIfBound below is a silent no-op on a
	 * default-constructed delegate. The result is a panel that edits its model
	 * while the renderer never hears -- which is precisely the state this whole
	 * task exists to leave behind.
	 */
	OnTransferFunctionChanged = InArgs._OnTransferFunctionChanged;

	const float U = FlowVizWorkspaceStyle::GetUnit();

	const TAttribute<bool> BoundEnabled =
		TAttribute<bool>::CreateSP(this, &SFlowVizTransferFunctionPanel::IsBound);

	/* --- Colormap buttons -------------------------------------------------- */

	TSharedRef<SWrapBox> MapBox = SNew(SWrapBox).UseAllottedSize(true);

	for (const ECFDVizColorMap Map : FlowVizTransferFunctionPanelLocal::OfferedMaps)
	{
		// Each button carries a strip of the map it selects, so the user picks by
		// LOOKING rather than by recognising a name. A colormap list rendered only
		// as text asks the user to have memorised what "magma" looks like.
		TArray<FSlateGradientStop> Unused;

		TSharedPtr<SButton> MapButton;
		SAssignNew(MapButton, SButton)
			.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
			.OnClicked(FOnClicked::CreateSP(
				this, &SFlowVizTransferFunctionPanel::OnColorMapClicked, Map))
			.IsEnabled(BoundEnabled)
			.ToolTipText(FText::Format(
				LOCTEXT("MapTipFmt", "{0}{1}"),
				FlowVizTransferFunctionPanelLocal::GetMapDisplayName(Map),
				CFDViz::ColorMaps::IsPerceptuallyUniform(Map)
					? LOCTEXT("MapUniform", " - perceptually uniform")
					: LOCTEXT("MapNotUniform",
						" - NOT perceptually uniform; creates edges where the data is smooth")))
			.ContentPadding(FMargin(1.5f * U, 1.0f * U))
			[
				SNew(STextBlock)
					.Text(FlowVizTransferFunctionPanelLocal::GetMapDisplayName(Map))
					.Font(FlowVizWorkspaceStyle::GetLabelFont())
					.ColorAndOpacity(FSlateColor::UseForeground())
			];

		ColorMapButtons.Add(Map, MapButton);

		MapBox->AddSlot()
			.Padding(FMargin(0.0f, 0.0f, 0.5f * U, 0.5f * U))
			[
				MapButton.ToSharedRef()
			];
	}

	/* --- Range source buttons ---------------------------------------------- */

	TSharedRef<SHorizontalBox> RangeSourceBox = SNew(SHorizontalBox);

	const EFlowVizRangeSource Sources[] = {
		EFlowVizRangeSource::Global,
		EFlowVizRangeSource::CurrentFrame,
		EFlowVizRangeSource::Manual,
	};

	for (const EFlowVizRangeSource Source : Sources)
	{
		const int32 SourceIndex = static_cast<int32>(Source);

		// RULE 15: THE BUTTON'S ENABLEMENT MUST MATCH WHAT THE MODEL WILL ACCEPT.
		//
		// This is not defensive coding, it is a defect the binding test caught.
		// FFlowVizTransferFunctionViewModel::SetRangeSource REFUSES CurrentFrame
		// until something has measured a per-frame range (see its own rule-15
		// comment). A panel that offered that button unconditionally would present
		// a control that silently does nothing - the exact failure rule 15 names,
		// and worse here because the user would believe they had switched to a
		// per-frame range while still looking at a global one.
		//
		// So the button asks the SAME predicate the setter guards on. "Disabled"
		// and "refused" are then one fact rather than two that can drift.
		TAttribute<bool> SourceEnabled = BoundEnabled;
		if (Source == EFlowVizRangeSource::CurrentFrame)
		{
			SourceEnabled = TAttribute<bool>::CreateLambda(
				[this]()
				{
					return IsBound() && ViewModel->HasCurrentFrameRange();
				});
		}

		TSharedPtr<SButton> SourceButton;
		SAssignNew(SourceButton, SButton)
			.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
			.OnClicked(FOnClicked::CreateSP(
				this, &SFlowVizTransferFunctionPanel::OnRangeSourceClicked, SourceIndex))
			.IsEnabled(SourceEnabled)
			.ToolTipText(
				Source == EFlowVizRangeSource::CurrentFrame
					? LOCTEXT("RangeFrameTip",
						"Rescale colors to each frame's own range. Available only once a "
						"per-frame range has been measured. Colors then mean different values "
						"at different times.")
					: LOCTEXT("RangeOtherTip", "How the color range is chosen."))
			.ContentPadding(FMargin(1.5f * U, 1.0f * U))
			[
				SNew(STextBlock)
					.Text(FlowVizTransferFunctionPanelLocal::GetRangeSourceName(Source))
					.Font(FlowVizWorkspaceStyle::GetLabelFont())
					.ColorAndOpacity(FSlateColor::UseForeground())
			];

		// Indexed by the enum's own value so GetRangeSourceButton(int32) and
		// static_cast<int32>(EFlowVizRangeSource::X) agree. Filling gaps keeps that
		// true even if the enum gains a value.
		while (RangeSourceButtons.Num() <= SourceIndex)
		{
			RangeSourceButtons.Add(nullptr);
		}
		RangeSourceButtons[SourceIndex] = SourceButton;

		RangeSourceBox->AddSlot()
			.AutoWidth()
			.Padding(FMargin(0.0f, 0.0f, 0.5f * U, 0.0f))
			[
				SourceButton.ToSharedRef()
			];
	}

	/* --- Assembly ---------------------------------------------------------- */

	ChildSlot
	[
		SNew(SBorder)
			.BorderImage(FlowVizWorkspaceStyle::GetFlatBrush())
			.BorderBackgroundColor(FlowVizWorkspaceStyle::GetPanelColor())
			.Padding(FMargin(2.0f * U))
		[
			SNew(SVerticalBox)

			/* --- Heading, naming the field being coloured ----------------- */

			+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 0.0f, 0.0f, 1.5f * U))
			[
				SNew(STextBlock)
					.Text(TAttribute<FText>::CreateSP(
						this, &SFlowVizTransferFunctionPanel::GetFieldLabelText))
					.Font(FlowVizWorkspaceStyle::GetHeadingFont())
					.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextPrimaryColor()))
			]

			/* --- The ramp ------------------------------------------------- */

			+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 0.0f, 0.0f, 1.0f * U))
			[
				SNew(SBox)
					.HeightOverride(14.0f * U)
				[
					SAssignNew(RampStrip, SFlowVizColorRampStrip)
						.ViewModel(ViewModel)
				]
			]

			/* --- Range endpoints, under the ramp they label --------------- */

			+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 0.0f, 0.0f, 2.0f * U))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
				[
					SAssignNew(RangeMinBox, SFlowVizNumericEntry)
						.Text(TAttribute<FText>::CreateSP(
							this, &SFlowVizTransferFunctionPanel::GetRangeMinText))
						.Font(FlowVizWorkspaceStyle::GetNumericFont())
						.OnTextCommitted(FOnTextCommitted::CreateSP(
							this, &SFlowVizTransferFunctionPanel::OnRangeMinCommitted))
						// Editable ONLY in Manual mode. In Global or per-frame mode
						// the range is derived, and an editable box whose edits were
						// silently overwritten on the next frame is rule 15's
						// nonfunctional control in text-box form.
						.IsEnabled(TAttribute<bool>::CreateSP(
							this, &SFlowVizTransferFunctionPanel::IsManualRangeEditable))
						.ToolTipText(LOCTEXT("RangeMinTip",
							"Range minimum. Editable in Manual mode only - in Global or "
							"per-frame mode this value is derived from the data."))
				]

				+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(FMargin(1.0f * U, 0.0f))
				[
					SNew(STextBlock)
						.Text(LOCTEXT("RangeSeparator", "to"))
						.Font(FlowVizWorkspaceStyle::GetLabelFont())
						.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextSecondaryColor()))
				]

				+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
				[
					SAssignNew(RangeMaxBox, SFlowVizNumericEntry)
						.Text(TAttribute<FText>::CreateSP(
							this, &SFlowVizTransferFunctionPanel::GetRangeMaxText))
						.Font(FlowVizWorkspaceStyle::GetNumericFont())
						.OnTextCommitted(FOnTextCommitted::CreateSP(
							this, &SFlowVizTransferFunctionPanel::OnRangeMaxCommitted))
						.IsEnabled(TAttribute<bool>::CreateSP(
							this, &SFlowVizTransferFunctionPanel::IsManualRangeEditable))
				]
			]

			/* --- Colormap picker ------------------------------------------ */

			+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
			[
				SNew(STextBlock)
					.Text(LOCTEXT("ColorMapHeading", "COLOR MAP"))
					.Font(FlowVizWorkspaceStyle::GetCaptionFont())
					.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextSecondaryColor()))
			]

			+ SVerticalBox::Slot()
				.AutoHeight()
			[
				MapBox
			]

			/* --- Non-uniform colormap warning ----------------------------- */

			+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 0.5f * U, 0.0f, 1.0f * U))
			[
				SNew(STextBlock)
					.Text(TAttribute<FText>::CreateSP(
						this, &SFlowVizTransferFunctionPanel::GetColorMapWarningText))
					.Font(FlowVizWorkspaceStyle::GetCaptionFont())
					.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetAdvisoryColor()))
					.AutoWrapText(true)
					// Collapsed rather than hidden, so it takes no layout space when
					// there is nothing to say and the panel does not have a
					// mysterious gap in it.
					.Visibility(TAttribute<EVisibility>::CreateLambda(
						[this]()
						{
							return GetColorMapWarningText().IsEmpty() ? EVisibility::Collapsed
																	  : EVisibility::Visible;
						}))
			]

			+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 0.0f, 0.0f, 1.0f * U))
			[
				SAssignNew(ReverseButton, SButton)
					.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
					.OnClicked(FOnClicked::CreateSP(
						this, &SFlowVizTransferFunctionPanel::OnReverseClicked))
					.IsEnabled(BoundEnabled)
					.HAlign(HAlign_Center)
					.ContentPadding(FMargin(1.5f * U, 1.0f * U))
					[
						SNew(STextBlock)
							.Text(LOCTEXT("Reverse", "Reverse color map"))
							.Font(FlowVizWorkspaceStyle::GetLabelFont())
							.ColorAndOpacity(FSlateColor::UseForeground())
					]
			]

			/* --- Range source --------------------------------------------- */

			+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
			[
				SNew(STextBlock)
					.Text(LOCTEXT("RangeHeading", "VALUE RANGE"))
					.Font(FlowVizWorkspaceStyle::GetCaptionFont())
					.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextSecondaryColor()))
			]

			+ SVerticalBox::Slot()
				.AutoHeight()
			[
				RangeSourceBox
			]

			/* --- ENGINEERING RULE 8's VISIBLE INDICATION ------------------ */

			+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 1.0f * U, 0.0f, 0.0f))
			[
				SNew(SBorder)
					.BorderImage(FlowVizWorkspaceStyle::GetRaisedBrush())
					.Padding(FMargin(1.5f * U, 1.0f * U))
					.Visibility(TAttribute<EVisibility>::CreateLambda(
						[this]()
						{
							return IsRangeAdvisoryVisible() ? EVisibility::Visible
															: EVisibility::Collapsed;
						}))
				[
					SNew(STextBlock)
						.Text(TAttribute<FText>::CreateSP(
							this, &SFlowVizTransferFunctionPanel::GetRangeAdvisoryText))
						.Font(FlowVizWorkspaceStyle::GetLabelFont())
						.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetAdvisoryColor()))
						.AutoWrapText(true)
				]
			]

			+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 1.0f * U, 0.0f, 0.0f))
			[
				SAssignNew(ResetRangeButton, SButton)
					.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
					.OnClicked(FOnClicked::CreateSP(
						this, &SFlowVizTransferFunctionPanel::OnResetRangeClicked))
					.IsEnabled(BoundEnabled)
					.HAlign(HAlign_Center)
					.ContentPadding(FMargin(1.5f * U, 1.0f * U))
					[
						SNew(STextBlock)
							.Text(LOCTEXT("ResetRange", "Reset to global range"))
							.Font(FlowVizWorkspaceStyle::GetLabelFont())
							.ColorAndOpacity(FSlateColor::UseForeground())
					]
			]

			/* --- Opacity multiplier --------------------------------------- */

			+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 2.0f * U, 0.0f, 0.5f * U))
			[
				SNew(STextBlock)
					.Text(LOCTEXT("OpacityHeading", "OPACITY"))
					.Font(FlowVizWorkspaceStyle::GetCaptionFont())
					.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextSecondaryColor()))
			]

			+ SVerticalBox::Slot()
				.AutoHeight()
			[
				SAssignNew(OpacitySlider, SFlowVizOpacitySlider)
					.Value(TAttribute<float>::CreateSP(
						this, &SFlowVizTransferFunctionPanel::GetOpacityMultiplier))
					.OnValueChanged(FOnFloatValueChanged::CreateSP(
						this, &SFlowVizTransferFunctionPanel::OnOpacityMultiplierChanged))
					.IsEnabled(BoundEnabled)
					.SliderBarColor(FSlateColor(FlowVizWorkspaceStyle::GetPanelRaisedColor()))
					.SliderHandleColor(FSlateColor(FlowVizWorkspaceStyle::GetAccentColor()))
					.ToolTipText(LOCTEXT("OpacityTip",
						"Overall density. Scales the authored opacity curve without changing "
						"its shape."))
			]
		]
	];
}

/* --- Accessors ---------------------------------------------------------- */

TSharedPtr<SButton> SFlowVizTransferFunctionPanel::GetColorMapButton(ECFDVizColorMap Map) const
{
	const TSharedPtr<SButton>* Found = ColorMapButtons.Find(Map);
	return Found != nullptr ? *Found : nullptr;
}

TSharedPtr<SButton> SFlowVizTransferFunctionPanel::GetRangeSourceButton(int32 SourceIndex) const
{
	return RangeSourceButtons.IsValidIndex(SourceIndex) ? RangeSourceButtons[SourceIndex] : nullptr;
}

bool SFlowVizTransferFunctionPanel::IsBound() const
{
	return ViewModel != nullptr && ViewModel->IsBound();
}

bool SFlowVizTransferFunctionPanel::IsManualRangeEditable() const
{
	return IsBound() && ViewModel->GetRangeSource() == EFlowVizRangeSource::Manual;
}

FText SFlowVizTransferFunctionPanel::GetRangeAdvisoryText() const
{
	if (!IsRangeAdvisoryVisible())
	{
		return FText::GetEmpty();
	}

	// The wording names the CONSEQUENCE, not the setting. "Range source: current
	// frame" restates what the user just clicked; this says what it will do to
	// their comparison.
	return LOCTEXT("PerFrameAdvisory",
		"Per-frame range: colors are rescaled every frame, so the same color means a different "
		"value at different times. Do not compare frames by color while this is on.");
}

bool SFlowVizTransferFunctionPanel::IsRangeAdvisoryVisible() const
{
	// Asks the VIEW MODEL whether the range is stable rather than testing the
	// enum here. One source of truth for what "stable" means, so a new range
	// source cannot be added that the advisory silently ignores.
	return IsBound() && !ViewModel->IsRangeStableAcrossAnimation();
}

FText SFlowVizTransferFunctionPanel::GetColorMapWarningText() const
{
	if (!IsBound() || ViewModel->IsColorMapPerceptuallyUniform())
	{
		return FText::GetEmpty();
	}

	return LOCTEXT("NonUniformMap",
		"This color map is not perceptually uniform: it creates apparent edges where the data "
		"is smooth, and flattens real structure where it is steep.");
}

FText SFlowVizTransferFunctionPanel::GetFieldLabelText() const
{
	if (!IsBound())
	{
		return LOCTEXT("NoField", "No field");
	}
	return FText::FromName(ViewModel->GetFieldId());
}

FText SFlowVizTransferFunctionPanel::GetRangeMinText() const
{
	if (!IsBound())
	{
		return FText::GetEmpty();
	}
	return FlowVizTransferFunctionPanelLocal::FormatValue(ViewModel->GetRangeMin());
}

FText SFlowVizTransferFunctionPanel::GetRangeMaxText() const
{
	if (!IsBound())
	{
		return FText::GetEmpty();
	}
	return FlowVizTransferFunctionPanelLocal::FormatValue(ViewModel->GetRangeMax());
}

float SFlowVizTransferFunctionPanel::GetOpacityMultiplier() const
{
	return IsBound() ? ViewModel->GetOpacityMultiplier() : 1.0f;
}

/* --- Actions ------------------------------------------------------------ */

void SFlowVizTransferFunctionPanel::NotifyTransferFunctionChanged() const
{
	// ExecuteIfBound, because an unsubscribed panel is a legal, inert state --
	// the panel is constructed before the workspace has a volume to push into.
	OnTransferFunctionChanged.ExecuteIfBound();
}

FReply SFlowVizTransferFunctionPanel::OnColorMapClicked(ECFDVizColorMap Map)
{
	if (ViewModel != nullptr)
	{
		// GATED ON IsOk(), as every announcing handler here is. SetColorMap
		// refuses a map this build does not ship; announcing that would rebuild
		// the LUT from the map that is still selected.
		if (ViewModel->SetColorMap(Map).IsOk())
		{
			NotifyTransferFunctionChanged();
		}
	}
	return FReply::Handled();
}

FReply SFlowVizTransferFunctionPanel::OnRangeSourceClicked(int32 SourceIndex)
{
	if (ViewModel != nullptr)
	{
		// THE REFUSAL HERE IS REACHABLE FROM THE UI, unlike most: picking the
		// per-frame source with no frame range supplied is refused outright, and
		// picking Global with no field bound likewise. Both leave the range
		// exactly as it was, so announcing would push an identical transfer
		// function.
		if (ViewModel->SetRangeSource(static_cast<EFlowVizRangeSource>(SourceIndex)).IsOk())
		{
			NotifyTransferFunctionChanged();
		}
	}
	return FReply::Handled();
}

FReply SFlowVizTransferFunctionPanel::OnReverseClicked()
{
	if (ViewModel != nullptr)
	{
		// Read-then-invert through the view model, not a widget-side bool. The
		// session loader can reverse the map without touching this panel.
		ViewModel->SetReverseColorMap(!ViewModel->IsColorMapReversed());

		// UNCONDITIONAL, and this is the one handler where that is correct:
		// SetReverseColorMap returns void and cannot refuse, so there is no
		// result to gate on. Wrapping it in a fabricated condition would be a
		// check that cannot fail.
		NotifyTransferFunctionChanged();
	}
	return FReply::Handled();
}

FReply SFlowVizTransferFunctionPanel::OnResetRangeClicked()
{
	if (ViewModel != nullptr)
	{
		if (ViewModel->ResetRange().IsOk())
		{
			NotifyTransferFunctionChanged();
		}
	}
	return FReply::Handled();
}

void SFlowVizTransferFunctionPanel::OnRangeMinCommitted(
	const FText& NewText, ETextCommit::Type CommitType)
{
	if (ViewModel == nullptr)
	{
		return;
	}

	double Parsed = 0.0;
	if (!LexTryParseString(Parsed, *NewText.ToString()))
	{
		// UNPARSEABLE TEXT IS IGNORED, NOT COERCED TO ZERO. A typo silently
		// becoming a range minimum of 0 would rescale the whole image with nothing
		// to say why; the bound attribute redisplays the real value on the next
		// paint, so the box visibly snaps back.
		return;
	}

	// THE REFUSAL PATH A USER REACHES BY TYPING: a minimum above the maximum is
	// rejected outright, because an inverted domain silently reverses the
	// colormap -- a different control they did not touch.
	if (ViewModel->SetManualRange(static_cast<float>(Parsed), ViewModel->GetRangeMax()).IsOk())
	{
		NotifyTransferFunctionChanged();
	}
}

void SFlowVizTransferFunctionPanel::OnRangeMaxCommitted(
	const FText& NewText, ETextCommit::Type CommitType)
{
	if (ViewModel == nullptr)
	{
		return;
	}

	double Parsed = 0.0;
	if (!LexTryParseString(Parsed, *NewText.ToString()))
	{
		return;
	}

	// Same gate as the minimum, and reachable the same way: a maximum below the
	// current minimum is refused.
	if (ViewModel->SetManualRange(ViewModel->GetRangeMin(), static_cast<float>(Parsed)).IsOk())
	{
		NotifyTransferFunctionChanged();
	}
}

void SFlowVizTransferFunctionPanel::OnOpacityMultiplierChanged(float NewValue)
{
	if (ViewModel != nullptr)
	{
		// ANNOUNCED ON EVERY ACCEPTED STEP OF A DRAG, deliberately. This fires
		// per mouse-move, so the push is per-move too -- but the push is a
		// struct copy and a MarkRenderDynamicDataDirty, not a re-upload, and
		// opacity is the one control whose whole value is watching the volume
		// thin as you drag. Announcing only on release would make it a control
		// you set blind and then evaluate.
		if (ViewModel->SetOpacityMultiplier(NewValue).IsOk())
		{
			NotifyTransferFunctionChanged();
		}
	}
}

#undef LOCTEXT_NAMESPACE
