// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizSlicePanel.h"

#include "Internationalization/Text.h"
#include "UI/FlowVizWorkspaceStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FlowVizSlicePanel"

// NAMED namespace, not anonymous: this module is a unity build, so anonymous
// namespaces from sibling .cpp files merge into one and same-named helpers
// collide by ODR. See the same note on every other UI .cpp here.
namespace FlowVizSlicePanelLocal
{
	/** The axis presets offered, in the order shown. */
	const EFlowVizSliceAxis OfferedAxes[] = {
		EFlowVizSliceAxis::X,
		EFlowVizSliceAxis::Y,
		EFlowVizSliceAxis::Z,
	};

	/** The aggregations offered. None is deliberately absent: it is a STATE, not a choice. */
	const EFlowVizSlabOp OfferedSlabOps[] = {
		EFlowVizSlabOp::Average,
		EFlowVizSlabOp::Minimum,
		EFlowVizSlabOp::Maximum,
	};

	FText GetAxisLabel(EFlowVizSliceAxis Axis)
	{
		switch (Axis)
		{
		case EFlowVizSliceAxis::X:
			return LOCTEXT("AxisX", "X");
		case EFlowVizSliceAxis::Y:
			return LOCTEXT("AxisY", "Y");
		case EFlowVizSliceAxis::Z:
			return LOCTEXT("AxisZ", "Z");
		default:
			return LOCTEXT("AxisUnknown", "?");
		}
	}

	FText GetAxisTooltip(EFlowVizSliceAxis Axis)
	{
		return FText::Format(
			LOCTEXT("AxisTip",
				"Face the slice along the {0} axis. SOLVER axes, not Unreal's - a slice "
				"authored in Unreal's frame would be mirrored in Y."),
			GetAxisLabel(Axis));
	}

	FText GetSlabOpLabel(EFlowVizSlabOp Op)
	{
		switch (Op)
		{
		case EFlowVizSlabOp::Average:
			return LOCTEXT("SlabAverage", "Mean");
		case EFlowVizSlabOp::Minimum:
			return LOCTEXT("SlabMinimum", "Min");
		case EFlowVizSlabOp::Maximum:
			return LOCTEXT("SlabMaximum", "Max");
		case EFlowVizSlabOp::None:
		default:
			return LOCTEXT("SlabNone", "—");
		}
	}

	FText GetSlabOpTooltip(EFlowVizSlabOp Op)
	{
		switch (Op)
		{
		case EFlowVizSlabOp::Average:
			return LOCTEXT("SlabAverageTip",
				"Collapse the slab by averaging its samples. The neutral choice: it is the "
				"only aggregation that does not bias toward an extreme.");
		case EFlowVizSlabOp::Minimum:
			return LOCTEXT("SlabMinimumTip",
				"Collapse the slab to its smallest sample. Note this is a MINIMUM THROUGH "
				"THE SLAB, so the value shown exists somewhere in the thickness - not "
				"necessarily on the plane.");
		case EFlowVizSlabOp::Maximum:
			return LOCTEXT("SlabMaximumTip",
				"Collapse the slab to its largest sample. Note this is a MAXIMUM THROUGH "
				"THE SLAB, so the value shown exists somewhere in the thickness - not "
				"necessarily on the plane.");
		default:
			return FText::GetEmpty();
		}
	}

	/**
	 * Fixed significant figures, for the same anti-jitter reason as the transport
	 * readout and the crop rows: a format that drops trailing zeros changes the
	 * field's width as the value changes, so the row shifts under the cursor while
	 * it is being edited.
	 */
	FText FormatValue(double Value)
	{
		return FText::FromString(FString::Printf(TEXT("%.6g"), Value));
	}

	/**
	 * Parse a typed number.
	 *
	 * REFUSES rather than substituting a default, for the reason spelled out on
	 * the clip panel: a box that reads unparseable text as 0 collapses the slab to
	 * a plane, which looks like the thickness control working and is really the
	 * user's typo being executed.
	 */
	bool TryParse(const FText& Text, double& OutValue)
	{
		const FString Trimmed = Text.ToString().TrimStartAndEnd();
		if (Trimmed.IsEmpty() || !Trimmed.IsNumeric())
		{
			return false;
		}
		OutValue = FCString::Atod(*Trimmed);
		return FMath::IsFinite(OutValue);
	}

	/** A labelled row: caption on the left at a fixed width, content filling the rest. */
	TSharedRef<SWidget> MakeFieldRow(const FText& Label, TSharedRef<SWidget> Content)
	{
		const float U = FlowVizWorkspaceStyle::GetUnit();

		return SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 1.0f * U, 0.0f))
			[
				SNew(SBox)
					// FIXED WIDTH so the fields line up into a column. Ragged left
					// edges make a panel read as a pile of controls rather than a form.
					.WidthOverride(7.0f * U)
				[
					SNew(STextBlock)
						.Text(Label)
						.Font(FlowVizWorkspaceStyle::GetLabelFont())
						.ColorAndOpacity(
							FSlateColor(FlowVizWorkspaceStyle::GetTextSecondaryColor()))
				]
			]

			+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
			[
				Content
			];
	}
}

void SFlowVizSlicePanel::Construct(const FArguments& InArgs)
{
	ViewModel = InArgs._ViewModel;
	OnSliceChanged = InArgs._OnSliceChanged;

	const float U = FlowVizWorkspaceStyle::GetUnit();

	// BOUND, not assigned. Both predicates are re-read every paint, so a case
	// closing under this panel - or the thickness being changed from elsewhere -
	// updates the controls without anything having to notify them.
	const TAttribute<bool> PanelLive =
		TAttribute<bool>::CreateSP(this, &SFlowVizSlicePanel::IsPanelLive);
	const TAttribute<bool> SlabLive =
		TAttribute<bool>::CreateSP(this, &SFlowVizSlicePanel::IsSlabActive);

	/* --- Axis presets ----------------------------------------------------- */

	const TSharedRef<SWrapBox> AxisBox = SNew(SWrapBox)
		.UseAllottedSize(true)
		.InnerSlotPadding(FVector2D(0.5f * U, 0.5f * U));

	for (const EFlowVizSliceAxis Axis : FlowVizSlicePanelLocal::OfferedAxes)
	{
		TSharedPtr<SButton> Button;

		AxisBox->AddSlot()
		[
			SAssignNew(Button, SButton)
				.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
				.OnClicked(FOnClicked::CreateSP(this, &SFlowVizSlicePanel::OnAxisClicked, Axis))
				// RULE 15: the same condition that makes the action possible guards
				// the control, so "disabled" and "refused" are one fact rather than
				// two that can drift apart.
				.IsEnabled(PanelLive)
				.ToolTipText(FlowVizSlicePanelLocal::GetAxisTooltip(Axis))
				.ContentPadding(FMargin(1.5f * U, 0.75f * U))
				.HAlign(HAlign_Center)
			[
				SNew(STextBlock)
					.Text(FlowVizSlicePanelLocal::GetAxisLabel(Axis))
					.Font(FlowVizWorkspaceStyle::GetNumericFont())
					.ColorAndOpacity(FSlateColor::UseForeground())
			]
		];

		AxisButtons[static_cast<int32>(Axis)] = Button;
	}

	/* --- Slab aggregation selector ---------------------------------------- */

	const TSharedRef<SWrapBox> SlabOpBox = SNew(SWrapBox)
		.UseAllottedSize(true)
		.InnerSlotPadding(FVector2D(0.5f * U, 0.5f * U));

	for (const EFlowVizSlabOp Op : FlowVizSlicePanelLocal::OfferedSlabOps)
	{
		TSharedPtr<SButton> Button;

		SlabOpBox->AddSlot()
		[
			SAssignNew(Button, SButton)
				.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
				.OnClicked(FOnClicked::CreateSP(this, &SFlowVizSlicePanel::OnSlabOpClicked, Op))
				// THE SLAB CONTROLS FOLLOW THE THICKNESS. The view model refuses an
				// aggregation on a zero-thickness slice; a live button here would be
				// pressed, refused, and nothing would happen - with nothing on screen
				// to say why.
				.IsEnabled(SlabLive)
				.ToolTipText(FlowVizSlicePanelLocal::GetSlabOpTooltip(Op))
				.ContentPadding(FMargin(1.5f * U, 0.75f * U))
				.HAlign(HAlign_Center)
			[
				SNew(STextBlock)
					.Text(FlowVizSlicePanelLocal::GetSlabOpLabel(Op))
					.Font(FlowVizWorkspaceStyle::GetLabelFont())
					.ColorAndOpacity(TAttribute<FSlateColor>::CreateLambda(
						[this, Op]() -> FSlateColor
						{
							// The SELECTED aggregation is picked out in the accent
							// colour. Without it the three buttons look identical and
							// the panel does not say which one is in force.
							return IsSlabOpSelected(Op)
								? FSlateColor(FlowVizWorkspaceStyle::GetAccentColor())
								: FSlateColor::UseForeground();
						}))
			]
		];

		SlabOpButtons[static_cast<int32>(Op)] = Button;
	}

	ChildSlot
	[
		SNew(SVerticalBox)

		/* --- THE DISCLOSURE, FIRST ---------------------------------------- */

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 1.5f * U))
		[
			// AT THE TOP. Nothing draws a slice yet, and unlike an unclipped volume
			// there is no visual symptom at all - the only evidence a slice exists
			// would be the slice itself. Placed under the controls this would be
			// read after the user had already spent time on the belief it corrects.
			SNew(SBorder)
				.BorderImage(FlowVizWorkspaceStyle::GetRaisedBrush())
				.Padding(FMargin(1.5f * U, 1.0f * U))
			[
				SNew(STextBlock)
					.Text(TAttribute<FText>::CreateSP(
						this, &SFlowVizSlicePanel::GetNotDrawnAdvisoryText))
					.Font(FlowVizWorkspaceStyle::GetCaptionFont())
					.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetAdvisoryColor()))
					.AutoWrapText(true)
			]
		]

		/* --- Orientation --------------------------------------------------- */

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
		[
			SNew(STextBlock)
				.Text(LOCTEXT("FacingHeading", "Facing"))
				.Font(FlowVizWorkspaceStyle::GetLabelFont())
				.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextSecondaryColor()))
		]

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.75f * U))
		[
			AxisBox
		]

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 1.0f * U))
		[
			// THE NUMERIC TRUTH, beside the presets. An oblique normal set from
			// elsewhere is not any of the three presets, and a panel that showed
			// only buttons would render that state as "none selected" - which is
			// indistinguishable from "no slice configured".
			SNew(STextBlock)
				.Text(TAttribute<FText>::CreateSP(this, &SFlowVizSlicePanel::GetNormalText))
				.Font(FlowVizWorkspaceStyle::GetCaptionFont())
				.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextDisabledColor()))
		]

		/* --- Position ------------------------------------------------------ */

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
		[
			FlowVizSlicePanelLocal::MakeFieldRow(
				LOCTEXT("PositionLabel", "Position"),
				SAssignNew(PositionSlider, SFlowVizScrubSlider)
					.Value(TAttribute<float>::CreateSP(
						this, &SFlowVizSlicePanel::GetPositionValue))
					.OnValueChanged(FOnFloatValueChanged::CreateSP(
						this, &SFlowVizSlicePanel::OnPositionChanged))
					.IsEnabled(PanelLive)
					.SliderBarColor(FSlateColor(FlowVizWorkspaceStyle::GetPanelColor()))
					.SliderHandleColor(FSlateColor(FlowVizWorkspaceStyle::GetAccentColor()))
					.ToolTipText(LOCTEXT("PositionTip",
						"Where the plane sits along its own normal, as a fraction of the "
						"domain's extent IN THAT DIRECTION. An oblique slice therefore "
						"still runs 0 to 1 across the whole box rather than running out "
						"of travel partway.")))
		]

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 1.0f * U))
		[
			// The ORIGIN in solver units, because the slider's 0..1 is not a
			// quantity anyone can check against a mesh or a probe reading.
			SNew(STextBlock)
				.Text(TAttribute<FText>::CreateSP(this, &SFlowVizSlicePanel::GetOriginText))
				.Font(FlowVizWorkspaceStyle::GetCaptionFont())
				.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextDisabledColor()))
		]

		/* --- Slab ---------------------------------------------------------- */

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
		[
			SNew(STextBlock)
				.Text(LOCTEXT("SlabHeading", "Slab"))
				.Font(FlowVizWorkspaceStyle::GetLabelFont())
				.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextSecondaryColor()))
		]

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
		[
			FlowVizSlicePanelLocal::MakeFieldRow(
				LOCTEXT("ThicknessLabel", "Thickness"),
				SAssignNew(ThicknessBox, SFlowVizNumericEntry)
					// BOUND text: the field follows a thickness change made
					// elsewhere without this box being touched.
					.Text(TAttribute<FText>::CreateSP(
						this, &SFlowVizSlicePanel::GetThicknessText))
					.OnTextCommitted(FOnTextCommitted::CreateSP(
						this, &SFlowVizSlicePanel::OnThicknessCommitted))
					.IsEnabled(PanelLive)
					.Font(FlowVizWorkspaceStyle::GetNumericFont())
					.ToolTipText(LOCTEXT("ThicknessTip",
						"Slab thickness in SOLVER units. Zero means a single sample on the "
						"plane, which is what disables the aggregation controls below.")))
		]

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
		[
			FlowVizSlicePanelLocal::MakeFieldRow(
				LOCTEXT("SamplesLabel", "Samples"),
				SAssignNew(SlabSamplesBox, SFlowVizNumericEntry)
					.Text(TAttribute<FText>::CreateSP(
						this, &SFlowVizSlicePanel::GetSlabSamplesText))
					.OnTextCommitted(FOnTextCommitted::CreateSP(
						this, &SFlowVizSlicePanel::OnSlabSamplesCommitted))
					.IsEnabled(SlabLive)
					.Font(FlowVizWorkspaceStyle::GetNumericFont())
					.ToolTipText(LOCTEXT("SamplesTip",
						"How many samples are taken through the slab before it is "
						"collapsed. Too few and the aggregation misses features thinner "
						"than the spacing.")))
		]

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 1.0f * U))
		[
			FlowVizSlicePanelLocal::MakeFieldRow(LOCTEXT("AggregationLabel", "Collapse by"), SlabOpBox)
		]

		/* --- Appearance ----------------------------------------------------- */

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
		[
			SNew(STextBlock)
				.Text(LOCTEXT("AppearanceHeading", "Appearance"))
				.Font(FlowVizWorkspaceStyle::GetLabelFont())
				.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextSecondaryColor()))
		]

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
		[
			FlowVizSlicePanelLocal::MakeFieldRow(
				LOCTEXT("OpacityLabel", "Opacity"),
				SAssignNew(OpacitySlider, SFlowVizScrubSlider)
					.Value(TAttribute<float>::CreateSP(
						this, &SFlowVizSlicePanel::GetOpacityValue))
					.OnValueChanged(FOnFloatValueChanged::CreateSP(
						this, &SFlowVizSlicePanel::OnOpacityChanged))
					.IsEnabled(PanelLive)
					.SliderBarColor(FSlateColor(FlowVizWorkspaceStyle::GetPanelColor()))
					.SliderHandleColor(FSlateColor(FlowVizWorkspaceStyle::GetAccentColor()))
					.ToolTipText(LOCTEXT("OpacityTip", "How opaque the slice surface is.")))
		]

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
		[
			SNew(SWrapBox)
				.UseAllottedSize(true)
				.InnerSlotPadding(FVector2D(0.5f * U, 0.5f * U))

			+ SWrapBox::Slot()
			[
				SAssignNew(VisibleButton, SButton)
					.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
					.OnClicked(FOnClicked::CreateSP(this, &SFlowVizSlicePanel::OnVisibleClicked))
					.IsEnabled(PanelLive)
					.ToolTipText(LOCTEXT("VisibleTip", "Show or hide the slice."))
					.ContentPadding(FMargin(1.5f * U, 0.75f * U))
				[
					SNew(STextBlock)
						// BOUND LABEL, so the button says which state it is IN rather
						// than which state it would move to - the ambiguity that makes
						// a toggle unreadable in a screenshot.
						.Text(TAttribute<FText>::CreateSP(
							this, &SFlowVizSlicePanel::GetVisibleLabel))
						.Font(FlowVizWorkspaceStyle::GetCaptionFont())
						.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]

			+ SWrapBox::Slot()
			[
				SAssignNew(ShowWidgetButton, SButton)
					.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
					.OnClicked(
						FOnClicked::CreateSP(this, &SFlowVizSlicePanel::OnShowWidgetClicked))
					.IsEnabled(PanelLive)
					.ToolTipText(LOCTEXT("WidgetTip",
						"Show the manipulation gizmo. Separate from visibility: a hidden "
						"gizmo over a visible slice is a legitimate presentation state."))
					.ContentPadding(FMargin(1.5f * U, 0.75f * U))
				[
					SNew(STextBlock)
						.Text(TAttribute<FText>::CreateSP(
							this, &SFlowVizSlicePanel::GetShowWidgetLabel))
						.Font(FlowVizWorkspaceStyle::GetCaptionFont())
						.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]

			+ SWrapBox::Slot()
			[
				SAssignNew(TrilinearButton, SButton)
					.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
					.OnClicked(
						FOnClicked::CreateSP(this, &SFlowVizSlicePanel::OnTrilinearClicked))
					.IsEnabled(PanelLive)
					.ToolTipText(LOCTEXT("TrilinearTip",
						"Trilinear sampling smooths between cell centres; nearest shows the "
						"stored cells. Smoothing invents values the solver never computed."))
					.ContentPadding(FMargin(1.5f * U, 0.75f * U))
				[
					SNew(STextBlock)
						.Text(TAttribute<FText>::CreateSP(
							this, &SFlowVizSlicePanel::GetTrilinearLabel))
						.Font(FlowVizWorkspaceStyle::GetCaptionFont())
						.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		]

		/* --- Rule 7: interpolation is VISIBLY identified -------------------- */

		+ SVerticalBox::Slot()
			.AutoHeight()
		[
			SNew(SBorder)
				.BorderImage(FlowVizWorkspaceStyle::GetRaisedBrush())
				.Padding(FMargin(1.5f * U, 1.0f * U))
				// CONDITIONAL, and that is the point. An advisory that is always on
				// screen carries no information and is read as decoration; this one
				// appears exactly when the slice is showing interpolated values.
				.Visibility(TAttribute<EVisibility>::CreateSP(
					this, &SFlowVizSlicePanel::GetInterpolationAdvisoryVisibility))
			[
				SNew(STextBlock)
					.Text(LOCTEXT("InterpolationAdvisory",
						"Interpolated: values between cell centres are smoothed and were "
						"not computed by the solver. Switch to nearest to see the stored "
						"cells."))
					.Font(FlowVizWorkspaceStyle::GetCaptionFont())
					.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetAdvisoryColor()))
					.AutoWrapText(true)
			]
		]
	];
}

/* ========================================================================== */
/* Seams                                                                       */
/* ========================================================================== */

TSharedPtr<SButton> SFlowVizSlicePanel::GetAxisButton(EFlowVizSliceAxis Axis) const
{
	const int32 Index = static_cast<int32>(Axis);
	if (Index < 0 || Index >= AxisButtons.Num())
	{
		return nullptr;
	}
	return AxisButtons[Index];
}

TSharedPtr<SButton> SFlowVizSlicePanel::GetSlabOpButton(EFlowVizSlabOp Op) const
{
	const int32 Index = static_cast<int32>(Op);
	if (Index < 0 || Index >= SlabOpButtons.Num())
	{
		return nullptr;
	}
	// Slot 0 (None) is null by construction: "no aggregation" is a consequence of
	// zero thickness, not something a user selects.
	return SlabOpButtons[Index];
}

bool SFlowVizSlicePanel::IsInterpolationAdvisoryVisible() const
{
	return GetInterpolationAdvisoryVisibility() == EVisibility::Visible;
}

FText SFlowVizSlicePanel::GetNotDrawnAdvisoryText() const
{
	/*
	 * REWRITTEN WHEN THE SLICE GAINED A RENDERER (#77). It renders as a SLAB of
	 * the volume -- the visible slice composes two opposed clip planes into the
	 * pushed clip -- so the old "nothing consumes this" text became the stale
	 * advisory #54 exists to prevent. What remains worth disclosing is the
	 * MECHANISM's two edges: opacity and trilinear affect the volume render as
	 * a whole rather than a dedicated slice surface, and the slab shares the
	 * clip budget (six planes).
	 */
	return LOCTEXT("SlabAdvisory",
		"Drawn as a slab of the volume: showing the slice clips the render to the "
		"slab. It shares the six-plane clip budget with the Clipping panel - with "
		"five or more user planes the slab does not fit and the volume stays whole.");
}

/* ========================================================================== */
/* Predicates                                                                  */
/* ========================================================================== */

bool SFlowVizSlicePanel::IsSlabActive() const
{
	// BOTH conditions. Bound-ness alone would leave the slab controls live on a
	// zero-thickness slice, where the view model refuses them.
	return IsBound() && ViewModel->IsSlab();
}

EVisibility SFlowVizSlicePanel::GetInterpolationAdvisoryVisibility() const
{
	return (IsBound() && ViewModel->IsTrilinear()) ? EVisibility::Visible : EVisibility::Collapsed;
}

bool SFlowVizSlicePanel::IsSlabOpSelected(EFlowVizSlabOp Op) const
{
	return IsBound() && ViewModel->GetSlabOp() == Op;
}

/* ========================================================================== */
/* Handlers                                                                    */
/* ========================================================================== */

void SFlowVizSlicePanel::NotifySliceChanged() const
{
	OnSliceChanged.ExecuteIfBound();
}

FReply SFlowVizSlicePanel::OnAxisClicked(EFlowVizSliceAxis Axis)
{
	if (IsBound())
	{
		ViewModel->SetAxisPreset(Axis);
		NotifySliceChanged();
	}
	return FReply::Handled();
}

FReply SFlowVizSlicePanel::OnSlabOpClicked(EFlowVizSlabOp Op)
{
	if (IsBound())
	{
		// The refusal on a zero-thickness slice is not handled here: the button is
		// disabled in that state, so reaching this line already means the model
		// will accept it. Swallowing an error here would let those two drift.
		if (ViewModel->SetSlabOp(Op).IsOk())
		{
			NotifySliceChanged();
		}
	}
	return FReply::Handled();
}

FReply SFlowVizSlicePanel::OnVisibleClicked()
{
	if (IsBound())
	{
		ViewModel->SetVisible(!ViewModel->IsVisible());
		// ALWAYS announced: visibility is precisely the control that adds or
		// retracts the slab planes from the pushed clip (#77).
		NotifySliceChanged();
	}
	return FReply::Handled();
}

FReply SFlowVizSlicePanel::OnShowWidgetClicked()
{
	if (IsBound())
	{
		ViewModel->SetShowWidget(!ViewModel->IsWidgetShown());
	}
	return FReply::Handled();
}

FReply SFlowVizSlicePanel::OnTrilinearClicked()
{
	if (IsBound())
	{
		ViewModel->SetTrilinear(!ViewModel->IsTrilinear());
		NotifySliceChanged();
	}
	return FReply::Handled();
}

void SFlowVizSlicePanel::OnPositionChanged(float NewValue)
{
	if (IsBound())
	{
		if (ViewModel->SetNormalizedPosition(static_cast<double>(NewValue)).IsOk())
		{
			NotifySliceChanged();
		}
	}
}

void SFlowVizSlicePanel::OnOpacityChanged(float NewValue)
{
	if (IsBound())
	{
		if (ViewModel->SetOpacity(NewValue).IsOk())
		{
			NotifySliceChanged();
		}
	}
}

void SFlowVizSlicePanel::OnThicknessCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
	if (!IsBound())
	{
		return;
	}

	double Value = 0.0;
	if (!FlowVizSlicePanelLocal::TryParse(NewText, Value))
	{
		// The bound Text attribute repaints the previous value, so the field
		// visibly rejects the entry rather than keeping unparseable text on screen.
		return;
	}

	if (ViewModel->SetThickness(Value).IsOk())
	{
		NotifySliceChanged();
	}
}

void SFlowVizSlicePanel::OnSlabSamplesCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
	if (!IsBound())
	{
		return;
	}

	double Value = 0.0;
	if (!FlowVizSlicePanelLocal::TryParse(NewText, Value))
	{
		return;
	}

	// TRUNCATED, not rounded: a sample count is a count, and FMath::RoundToInt on
	// 1.5 would give 2 samples from a field that reads "1.5".
	if (ViewModel->SetSlabSamples(static_cast<int32>(Value)).IsOk())
	{
		NotifySliceChanged();
	}
}

/* ========================================================================== */
/* Bound readers. Every one tolerates a null view model.                       */
/* ========================================================================== */

float SFlowVizSlicePanel::GetPositionValue() const
{
	if (!IsBound())
	{
		return 0.0f;
	}
	// CLAMPED FOR THE SLIDER ONLY. GetNormalizedPosition deliberately reports
	// outside-the-domain positions as outside; a slider cannot draw that, so it
	// pins to the end. The numeric origin readout below it still tells the truth.
	return FMath::Clamp(static_cast<float>(ViewModel->GetNormalizedPosition()), 0.0f, 1.0f);
}

float SFlowVizSlicePanel::GetOpacityValue() const
{
	return IsBound() ? ViewModel->GetOpacity() : 0.0f;
}

FText SFlowVizSlicePanel::GetThicknessText() const
{
	if (!IsBound())
	{
		return FText::GetEmpty();
	}
	return FlowVizSlicePanelLocal::FormatValue(ViewModel->GetThickness());
}

FText SFlowVizSlicePanel::GetSlabSamplesText() const
{
	if (!IsBound())
	{
		return FText::GetEmpty();
	}
	return FText::AsNumber(ViewModel->GetSlabSamples());
}

FText SFlowVizSlicePanel::GetVisibleLabel() const
{
	if (!IsBound())
	{
		return LOCTEXT("VisibleUnavailable", "Visible");
	}
	return ViewModel->IsVisible() ? LOCTEXT("VisibleOn", "Visible")
								  : LOCTEXT("VisibleOff", "Hidden");
}

FText SFlowVizSlicePanel::GetShowWidgetLabel() const
{
	if (!IsBound())
	{
		return LOCTEXT("WidgetUnavailable", "Gizmo");
	}
	return ViewModel->IsWidgetShown() ? LOCTEXT("WidgetOn", "Gizmo on")
									  : LOCTEXT("WidgetOff", "Gizmo off");
}

FText SFlowVizSlicePanel::GetTrilinearLabel() const
{
	if (!IsBound())
	{
		return LOCTEXT("SamplingUnavailable", "Sampling");
	}
	// The label names the MODE IN FORCE, and names it in the vocabulary rule 7
	// cares about, so the button and the advisory agree on screen.
	return ViewModel->IsTrilinear() ? LOCTEXT("SamplingTrilinear", "Interpolated")
									: LOCTEXT("SamplingNearest", "Nearest");
}

FText SFlowVizSlicePanel::GetOriginText() const
{
	if (!IsBound())
	{
		return FText::GetEmpty();
	}
	const FVector& O = ViewModel->GetOrigin();
	return FText::Format(
		LOCTEXT("OriginReadout", "Origin {0}, {1}, {2} (solver units)"),
		FlowVizSlicePanelLocal::FormatValue(O.X),
		FlowVizSlicePanelLocal::FormatValue(O.Y),
		FlowVizSlicePanelLocal::FormatValue(O.Z));
}

FText SFlowVizSlicePanel::GetNormalText() const
{
	if (!IsBound())
	{
		return FText::GetEmpty();
	}
	const FVector& N = ViewModel->GetNormal();
	return FText::Format(
		LOCTEXT("NormalReadout", "Normal {0}, {1}, {2}"),
		FlowVizSlicePanelLocal::FormatValue(N.X),
		FlowVizSlicePanelLocal::FormatValue(N.Y),
		FlowVizSlicePanelLocal::FormatValue(N.Z));
}

#undef LOCTEXT_NAMESPACE
