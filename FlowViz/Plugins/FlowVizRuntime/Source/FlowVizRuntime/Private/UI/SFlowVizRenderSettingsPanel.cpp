// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizRenderSettingsPanel.h"

#include "UI/FlowVizWorkspaceStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FlowVizRenderSettingsPanel"

// NAMED namespace: unity build. See the note on every other UI .cpp here.
namespace FlowVizRenderSettingsPanelLocal
{
	/** The composite modes offered, in the order they are shown. */
	struct FModeRow
	{
		EFlowVizCompositeMode Mode;
		FText Label;
		FText Tip;
	};

	TArray<FModeRow> GetModeRows()
	{
		return {
			{ EFlowVizCompositeMode::Alpha, LOCTEXT("ModeAlpha", "Alpha"),
				LOCTEXT("ModeAlphaTip", "Front-to-back alpha compositing. The default.") },
			{ EFlowVizCompositeMode::Maximum, LOCTEXT("ModeMax", "MIP"),
				LOCTEXT("ModeMaxTip", "Maximum-intensity projection.") },
			{ EFlowVizCompositeMode::Minimum, LOCTEXT("ModeMin", "MinIP"),
				LOCTEXT("ModeMinTip", "Minimum-intensity projection.") },
			{ EFlowVizCompositeMode::Average, LOCTEXT("ModeAvg", "Average"),
				LOCTEXT("ModeAvgTip",
					"Mean of the valid samples along the ray. Invalid samples are excluded "
					"from both the sum and the count.") },
			{ EFlowVizCompositeMode::IsoSurface, LOCTEXT("ModeIso", "Iso-surface"),
				LOCTEXT("ModeIsoTip",
					"Ray-marched iso-surface at the threshold below, with a linear crossing "
					"between bracketing samples.") },
			{ EFlowVizCompositeMode::Diagnostic, LOCTEXT("ModeDiag", "Diagnostic"),
				LOCTEXT("ModeDiagTip",
					"Cbuffer round-trip echo. Renders the parameters the GPU actually "
					"received; for verifying transport, not for viewing data.") },
		};
	}

	/*
	 * The no-data palette. TRANSPARENT FIRST because it is the shipped default;
	 * the others are the disclosure choices VISUAL_QA rule 4 anticipates -- a
	 * user who needs to SEE where data is absent picks one that cannot occur in
	 * their colormap. Magenta is the conventional "obviously wrong" marker; the
	 * mid-grey reads as neutral in print.
	 *
	 * A FIXED PALETTE, NOT A COLOUR PICKER, deliberately: rule 4 requires the
	 * five disclosure colours to remain distinguishable, and a free picker is
	 * how NoData quietly becomes the same magenta as NaN. Every colour here is
	 * distinct from the other four disclosure defaults.
	 */
	const FLinearColor NoDataPalette[] = {
		FLinearColor(0.0f, 0.0f, 0.0f, 0.0f),
		FLinearColor(1.0f, 0.0f, 1.0f, 1.0f),
		FLinearColor(0.5f, 0.5f, 0.5f, 1.0f),
	};

	const FText NoDataNames[] = {
		LOCTEXT("NoDataTransparent", "Transparent"),
		LOCTEXT("NoDataMagenta", "Magenta"),
		LOCTEXT("NoDataGrey", "Grey"),
	};

	/** Fixed format, same anti-jitter reasoning as the transfer function's readouts. */
	FText FormatValue(float Value)
	{
		return FText::FromString(FString::Printf(TEXT("%.4g"), Value));
	}
}

void SFlowVizRenderSettingsPanel::Construct(const FArguments& InArgs)
{
	using namespace FlowVizRenderSettingsPanelLocal;

	ViewModel = InArgs._ViewModel;
	OnRenderSettingsChanged = InArgs._OnRenderSettingsChanged;

	const float U = FlowVizWorkspaceStyle::GetUnit();

	// One predicate for the whole panel: bound means live (rule 15). Controls
	// with a narrower condition (lighting terms, jitter terms, the iso value)
	// AND their own predicate onto this one below.
	const TAttribute<bool> BoundEnabled =
		TAttribute<bool>::CreateSP(this, &SFlowVizRenderSettingsPanel::IsBound);

	/* --- Composite mode buttons -------------------------------------------- */

	TSharedRef<SWrapBox> ModeBox = SNew(SWrapBox).UseAllottedSize(true);

	for (const FModeRow& Row : GetModeRows())
	{
		TSharedPtr<SButton> ModeButton;
		SAssignNew(ModeButton, SButton)
			.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
			.OnClicked(FOnClicked::CreateSP(
				this, &SFlowVizRenderSettingsPanel::OnCompositeModeClicked, Row.Mode))
			.IsEnabled(BoundEnabled)
			.ToolTipText(Row.Tip)
			.ContentPadding(FMargin(1.5f * U, 1.0f * U))
			[
				SNew(STextBlock)
					.Text(Row.Label)
					.Font(FlowVizWorkspaceStyle::GetLabelFont())
					.ColorAndOpacity(FSlateColor::UseForeground())
			];

		CompositeModeButtons.Add(Row.Mode, ModeButton);

		ModeBox->AddSlot()
			.Padding(FMargin(0.0f, 0.0f, 0.5f * U, 0.5f * U))
			[
				ModeButton.ToSharedRef()
			];
	}

	/* --- Iso value ---------------------------------------------------------- */

	// Enabled ONLY in IsoSurface mode: in every other mode the shader never
	// reads IsoValue, and an editable box whose edits change nothing is rule
	// 15's nonfunctional control in text-box form.
	const TAttribute<bool> IsoEnabled = TAttribute<bool>::CreateLambda(
		[this]()
		{
			return IsBound()
				&& ViewModel->GetCompositeMode() == EFlowVizCompositeMode::IsoSurface;
		});

	SAssignNew(IsoValueBox, SFlowVizNumericEntry)
		.Text(TAttribute<FText>::CreateSP(this, &SFlowVizRenderSettingsPanel::GetIsoValueText))
		.Font(FlowVizWorkspaceStyle::GetNumericFont())
		.OnTextCommitted(FOnTextCommitted::CreateSP(
			this, &SFlowVizRenderSettingsPanel::OnIsoValueCommitted))
		.IsEnabled(IsoEnabled)
		.ToolTipText(LOCTEXT("IsoTip",
			"Iso-surface threshold, in field units. Editable in Iso-surface mode only - "
			"no other mode reads it."));

	/* --- Lighting ----------------------------------------------------------- */

	SAssignNew(LightingButton, SButton)
		.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
		.OnClicked(FOnClicked::CreateSP(this, &SFlowVizRenderSettingsPanel::OnLightingClicked))
		.IsEnabled(BoundEnabled)
		.ToolTipText(LOCTEXT("LightingTip",
			"Gradient lighting. Off in the Scientific profile because shading modulates "
			"apparent scalar value; turning it on is the Presentation profile's trade, "
			"made knowingly."))
		.ContentPadding(FMargin(1.5f * U, 0.75f * U))
		[
			SNew(STextBlock)
				.Text(TAttribute<FText>::CreateSP(
					this, &SFlowVizRenderSettingsPanel::GetLightingLabel))
				.Font(FlowVizWorkspaceStyle::GetLabelFont())
				.ColorAndOpacity(FSlateColor::UseForeground())
		];

	// Live only while lighting is on: with it off the shader never reads these,
	// and rule 15 forbids offering a control that silently does nothing.
	const TAttribute<bool> LightingTermsEnabled = TAttribute<bool>::CreateLambda(
		[this]() { return IsBound() && ViewModel->IsLightingEnabled(); });

	SAssignNew(AmbientBox, SFlowVizNumericEntry)
		.Text(TAttribute<FText>::CreateSP(this, &SFlowVizRenderSettingsPanel::GetAmbientText))
		.Font(FlowVizWorkspaceStyle::GetNumericFont())
		.OnTextCommitted(FOnTextCommitted::CreateSP(
			this, &SFlowVizRenderSettingsPanel::OnAmbientCommitted))
		.IsEnabled(LightingTermsEnabled)
		.ToolTipText(LOCTEXT("AmbientTip", "Ambient term, clamped to [0, 1]."));

	SAssignNew(DiffuseBox, SFlowVizNumericEntry)
		.Text(TAttribute<FText>::CreateSP(this, &SFlowVizRenderSettingsPanel::GetDiffuseText))
		.Font(FlowVizWorkspaceStyle::GetNumericFont())
		.OnTextCommitted(FOnTextCommitted::CreateSP(
			this, &SFlowVizRenderSettingsPanel::OnDiffuseCommitted))
		.IsEnabled(LightingTermsEnabled)
		.ToolTipText(LOCTEXT("DiffuseTip", "Diffuse term, clamped to [0, 1]."));

	TSharedRef<SHorizontalBox> DirectionBox = SNew(SHorizontalBox);
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		SAssignNew(LightDirectionBoxes[Axis], SFlowVizNumericEntry)
			.Text(TAttribute<FText>::CreateSP(
				this, &SFlowVizRenderSettingsPanel::GetLightDirectionText, Axis))
			.Font(FlowVizWorkspaceStyle::GetNumericFont())
			.OnTextCommitted(FOnTextCommitted::CreateSP(
				this, &SFlowVizRenderSettingsPanel::OnLightDirectionCommitted, Axis))
			.IsEnabled(LightingTermsEnabled)
			.ToolTipText(LOCTEXT("LightDirTip",
				"Light direction in local volume space, solver axes. Normalised on the "
				"way in; an all-zero direction is refused."));

		DirectionBox->AddSlot()
			.FillWidth(1.0f)
			.Padding(FMargin(Axis == 0 ? 0.0f : 0.5f * U, 0.0f, 0.0f, 0.0f))
			[
				LightDirectionBoxes[Axis].ToSharedRef()
			];
	}

	/* --- Marching ----------------------------------------------------------- */

	SAssignNew(StepVoxelsBox, SFlowVizNumericEntry)
		.Text(TAttribute<FText>::CreateSP(this, &SFlowVizRenderSettingsPanel::GetStepVoxelsText))
		.Font(FlowVizWorkspaceStyle::GetNumericFont())
		.OnTextCommitted(FOnTextCommitted::CreateSP(
			this, &SFlowVizRenderSettingsPanel::OnStepVoxelsCommitted))
		.IsEnabled(BoundEnabled)
		.ToolTipText(LOCTEXT("StepTip",
			"Sample step in voxels. Smaller is more accurate and costs fill rate. "
			"Zero is refused: the ray would never advance."));

	SAssignNew(ReferenceStepBox, SFlowVizNumericEntry)
		.Text(TAttribute<FText>::CreateSP(
			this, &SFlowVizRenderSettingsPanel::GetReferenceStepText))
		.Font(FlowVizWorkspaceStyle::GetNumericFont())
		.OnTextCommitted(FOnTextCommitted::CreateSP(
			this, &SFlowVizRenderSettingsPanel::OnReferenceStepCommitted))
		.IsEnabled(BoundEnabled)
		.ToolTipText(LOCTEXT("ReferenceStepTip",
			"Opacity-correction reference step. Alpha is compensated by "
			"step/reference so changing the step changes the noise, not the "
			"apparent density. It is a divisor; zero is refused."));

	SAssignNew(MaxStepsBox, SFlowVizNumericEntry)
		.Text(TAttribute<FText>::CreateSP(this, &SFlowVizRenderSettingsPanel::GetMaxStepsText))
		.Font(FlowVizWorkspaceStyle::GetNumericFont())
		.OnTextCommitted(FOnTextCommitted::CreateSP(
			this, &SFlowVizRenderSettingsPanel::OnMaxStepsCommitted))
		.IsEnabled(BoundEnabled)
		.ToolTipText(LOCTEXT("MaxStepsTip",
			"Step ceiling, clamped to the shader's limit. The bound on the GPU loop."));

	SAssignNew(EarlyOutBox, SFlowVizNumericEntry)
		.Text(TAttribute<FText>::CreateSP(this, &SFlowVizRenderSettingsPanel::GetEarlyOutText))
		.Font(FlowVizWorkspaceStyle::GetNumericFont())
		.OnTextCommitted(FOnTextCommitted::CreateSP(
			this, &SFlowVizRenderSettingsPanel::OnEarlyOutCommitted))
		.IsEnabled(BoundEnabled)
		.ToolTipText(LOCTEXT("EarlyOutTip",
			"Alpha at which the march stops early. 1.0 never triggers, which is "
			"reference quality."));

	/* --- Jitter ------------------------------------------------------------- */

	SAssignNew(JitterButton, SButton)
		.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
		.OnClicked(FOnClicked::CreateSP(this, &SFlowVizRenderSettingsPanel::OnJitterClicked))
		.IsEnabled(BoundEnabled)
		.ToolTipText(LOCTEXT("JitterTip",
			"Sub-step jitter. Trades banding for temporal shimmer; off by default "
			"(ADR 002)."))
		.ContentPadding(FMargin(1.5f * U, 0.75f * U))
		[
			SNew(STextBlock)
				.Text(TAttribute<FText>::CreateSP(
					this, &SFlowVizRenderSettingsPanel::GetJitterLabel))
				.Font(FlowVizWorkspaceStyle::GetLabelFont())
				.ColorAndOpacity(FSlateColor::UseForeground())
		];

	const TAttribute<bool> JitterTermsEnabled = TAttribute<bool>::CreateLambda(
		[this]() { return IsBound() && ViewModel->IsJitterEnabled(); });

	SAssignNew(JitterAmountBox, SFlowVizNumericEntry)
		.Text(TAttribute<FText>::CreateSP(
			this, &SFlowVizRenderSettingsPanel::GetJitterAmountText))
		.Font(FlowVizWorkspaceStyle::GetNumericFont())
		.OnTextCommitted(FOnTextCommitted::CreateSP(
			this, &SFlowVizRenderSettingsPanel::OnJitterAmountCommitted))
		.IsEnabled(JitterTermsEnabled)
		.ToolTipText(LOCTEXT("JitterAmountTip",
			"Jitter magnitude as a fraction of one step, clamped to [0, 1]. Never "
			"changes the step count."));

	SAssignNew(JitterSeedBox, SFlowVizNumericEntry)
		.Text(TAttribute<FText>::CreateSP(this, &SFlowVizRenderSettingsPanel::GetJitterSeedText))
		.Font(FlowVizWorkspaceStyle::GetNumericFont())
		.OnTextCommitted(FOnTextCommitted::CreateSP(
			this, &SFlowVizRenderSettingsPanel::OnJitterSeedCommitted))
		.IsEnabled(JitterTermsEnabled)
		.ToolTipText(LOCTEXT("JitterSeedTip",
			"Jitter seed. A fixed seed is what makes a capture reproducible."));

	/* --- Sampling ----------------------------------------------------------- */

	SAssignNew(FilteringButton, SButton)
		.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
		.OnClicked(FOnClicked::CreateSP(this, &SFlowVizRenderSettingsPanel::OnFilteringClicked))
		.IsEnabled(BoundEnabled)
		.ToolTipText(LOCTEXT("FilteringTip",
			"Trilinear smooths between cell centres; nearest shows the stored cells. "
			"Smoothing invents values the solver never computed."))
		.ContentPadding(FMargin(1.5f * U, 0.75f * U))
		[
			SNew(STextBlock)
				.Text(TAttribute<FText>::CreateSP(
					this, &SFlowVizRenderSettingsPanel::GetFilteringLabel))
				.Font(FlowVizWorkspaceStyle::GetLabelFont())
				.ColorAndOpacity(FSlateColor::UseForeground())
		];

	SAssignNew(StrictFilterButton, SButton)
		.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
		.OnClicked(
			FOnClicked::CreateSP(this, &SFlowVizRenderSettingsPanel::OnStrictFilterClicked))
		.IsEnabled(BoundEnabled)
		.ToolTipText(LOCTEXT("StrictTip",
			"Reject a filtered sample whose footprint touches an invalid voxel. The "
			"difference between a correct edge and a plausible one that has bled "
			"invalid data. Thins the volume at every boundary, which is why it is "
			"the user's call."))
		.ContentPadding(FMargin(1.5f * U, 0.75f * U))
		[
			SNew(STextBlock)
				.Text(TAttribute<FText>::CreateSP(
					this, &SFlowVizRenderSettingsPanel::GetStrictFilterLabel))
				.Font(FlowVizWorkspaceStyle::GetLabelFont())
				.ColorAndOpacity(FSlateColor::UseForeground())
		];

	/* --- No-data swatches --------------------------------------------------- */

	TSharedRef<SHorizontalBox> SwatchBox = SNew(SHorizontalBox);
	for (int32 Index = 0; Index < GetNoDataSwatchCount(); ++Index)
	{
		TSharedPtr<SButton> Swatch;
		SAssignNew(Swatch, SButton)
			.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
			.OnClicked(FOnClicked::CreateSP(
				this, &SFlowVizRenderSettingsPanel::OnNoDataSwatchClicked, Index))
			.IsEnabled(BoundEnabled)
			.ToolTipText(LOCTEXT("NoDataTip",
				"The colour for a voxel that carries no data at all. Part of the "
				"disclosure palette: it must stay distinguishable from the NaN, "
				"masked and range colours, which is why this is a fixed set rather "
				"than a picker."))
			.ContentPadding(FMargin(1.5f * U, 0.75f * U))
			[
				SNew(STextBlock)
					.Text(NoDataNames[Index])
					.Font(FlowVizWorkspaceStyle::GetLabelFont())
					.ColorAndOpacity(FSlateColor::UseForeground())
			];

		NoDataSwatchButtons.Add(Swatch);

		SwatchBox->AddSlot()
			.AutoWidth()
			.Padding(FMargin(Index == 0 ? 0.0f : 0.5f * U, 0.0f, 0.0f, 0.0f))
			[
				Swatch.ToSharedRef()
			];
	}

	/* --- Layout ------------------------------------------------------------- */

	const auto MakeRowLabel = [U](const FText& Label) -> TSharedRef<SWidget>
	{
		return SNew(SBox)
			.Padding(FMargin(0.0f, 1.0f * U, 0.0f, 0.5f * U))
			[
				SNew(STextBlock)
					.Text(Label)
					.Font(FlowVizWorkspaceStyle::GetLabelFont())
					.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextSecondaryColor()))
			];
	};

	const auto MakePairRow = [U](TSharedRef<SWidget> Left,
							   TSharedRef<SWidget> Right) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f) [ Left ]
			+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(FMargin(0.5f * U, 0.0f, 0.0f, 0.0f)) [ Right ];
	};

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeRowLabel(LOCTEXT("ModeLabel", "Compositing"))
		]
		+ SVerticalBox::Slot().AutoHeight() [ ModeBox ]

		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeRowLabel(LOCTEXT("IsoLabel", "Iso value"))
		]
		+ SVerticalBox::Slot().AutoHeight() [ IsoValueBox.ToSharedRef() ]

		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeRowLabel(LOCTEXT("LightingLabel", "Lighting"))
		]
		+ SVerticalBox::Slot().AutoHeight() [ LightingButton.ToSharedRef() ]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 0.5f * U, 0.0f, 0.0f))
		[
			MakePairRow(AmbientBox.ToSharedRef(), DiffuseBox.ToSharedRef())
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 0.5f * U, 0.0f, 0.0f))
		[
			DirectionBox
		]

		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeRowLabel(LOCTEXT("MarchLabel", "Marching"))
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakePairRow(StepVoxelsBox.ToSharedRef(), ReferenceStepBox.ToSharedRef())
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 0.5f * U, 0.0f, 0.0f))
		[
			MakePairRow(MaxStepsBox.ToSharedRef(), EarlyOutBox.ToSharedRef())
		]

		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeRowLabel(LOCTEXT("JitterLabel", "Jitter"))
		]
		+ SVerticalBox::Slot().AutoHeight() [ JitterButton.ToSharedRef() ]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 0.5f * U, 0.0f, 0.0f))
		[
			MakePairRow(JitterAmountBox.ToSharedRef(), JitterSeedBox.ToSharedRef())
		]

		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeRowLabel(LOCTEXT("SamplingLabel", "Sampling"))
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			MakePairRow(FilteringButton.ToSharedRef(), StrictFilterButton.ToSharedRef())
		]

		+ SVerticalBox::Slot().AutoHeight()
		[
			MakeRowLabel(LOCTEXT("NoDataLabel", "No-data color"))
		]
		+ SVerticalBox::Slot().AutoHeight() [ SwatchBox ]
	];
}

/* ========================================================================== */
/* Test seams                                                                  */
/* ========================================================================== */

TSharedPtr<SButton> SFlowVizRenderSettingsPanel::GetCompositeModeButton(
	EFlowVizCompositeMode Mode) const
{
	const TSharedPtr<SButton>* Found = CompositeModeButtons.Find(Mode);
	return Found != nullptr ? *Found : nullptr;
}

TSharedPtr<SFlowVizNumericEntry> SFlowVizRenderSettingsPanel::GetLightDirectionBox(
	int32 Axis) const
{
	if (Axis < 0 || Axis > 2)
	{
		return nullptr;
	}
	return LightDirectionBoxes[Axis];
}

TSharedPtr<SButton> SFlowVizRenderSettingsPanel::GetNoDataSwatchButton(int32 Index) const
{
	return NoDataSwatchButtons.IsValidIndex(Index) ? NoDataSwatchButtons[Index] : nullptr;
}

int32 SFlowVizRenderSettingsPanel::GetNoDataSwatchCount()
{
	return UE_ARRAY_COUNT(FlowVizRenderSettingsPanelLocal::NoDataPalette);
}

FLinearColor SFlowVizRenderSettingsPanel::GetNoDataSwatchColor(int32 Index)
{
	using namespace FlowVizRenderSettingsPanelLocal;
	if (Index < 0 || Index >= GetNoDataSwatchCount())
	{
		return FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
	}
	return NoDataPalette[Index];
}

/* ========================================================================== */
/* Handlers                                                                    */
/* ========================================================================== */

void SFlowVizRenderSettingsPanel::NotifyRenderSettingsChanged()
{
	OnRenderSettingsChanged.ExecuteIfBound();
}

FReply SFlowVizRenderSettingsPanel::OnCompositeModeClicked(EFlowVizCompositeMode Mode)
{
	if (ViewModel != nullptr && ViewModel->GetCompositeMode() != Mode)
	{
		// GATED ON CHANGE: SetCompositeMode cannot refuse a typed enum, so the
		// no-op case is re-clicking the selected mode, and announcing that would
		// republish an identical frame.
		ViewModel->SetCompositeMode(Mode);
		NotifyRenderSettingsChanged();
	}
	return FReply::Handled();
}

FReply SFlowVizRenderSettingsPanel::OnLightingClicked()
{
	if (ViewModel != nullptr)
	{
		// A toggle is always a change; no gate needed.
		ViewModel->SetLightingEnabled(!ViewModel->IsLightingEnabled());
		NotifyRenderSettingsChanged();
	}
	return FReply::Handled();
}

FReply SFlowVizRenderSettingsPanel::OnJitterClicked()
{
	if (ViewModel != nullptr)
	{
		ViewModel->SetJitterEnabled(!ViewModel->IsJitterEnabled());
		NotifyRenderSettingsChanged();
	}
	return FReply::Handled();
}

FReply SFlowVizRenderSettingsPanel::OnFilteringClicked()
{
	if (ViewModel != nullptr)
	{
		ViewModel->SetFieldFilteringEnabled(!ViewModel->IsFieldFilteringEnabled());
		NotifyRenderSettingsChanged();
	}
	return FReply::Handled();
}

FReply SFlowVizRenderSettingsPanel::OnStrictFilterClicked()
{
	if (ViewModel != nullptr)
	{
		ViewModel->SetStrictStatusFilter(!ViewModel->IsStrictStatusFilter());
		NotifyRenderSettingsChanged();
	}
	return FReply::Handled();
}

FReply SFlowVizRenderSettingsPanel::OnNoDataSwatchClicked(int32 Index)
{
	if (ViewModel != nullptr)
	{
		const FLinearColor Chosen = GetNoDataSwatchColor(Index);
		if (!ViewModel->GetNoDataColor().Equals(Chosen))
		{
			ViewModel->SetNoDataColor(Chosen);
			NotifyRenderSettingsChanged();
		}
	}
	return FReply::Handled();
}

namespace FlowVizRenderSettingsPanelLocal
{
	/**
	 * Parse-or-ignore, shared by every numeric commit handler.
	 *
	 * UNPARSEABLE TEXT IS IGNORED, NOT COERCED TO ZERO. A typo silently becoming
	 * a zero would render plausibly wrong (an iso-surface at 0, an unlit
	 * ambient); the bound attribute redisplays the real value on the next paint,
	 * so the box visibly snaps back instead.
	 */
	bool ParseFloat(const FText& Text, float& OutValue)
	{
		double Parsed = 0.0;
		if (!LexTryParseString(Parsed, *Text.ToString()))
		{
			return false;
		}
		OutValue = static_cast<float>(Parsed);
		return true;
	}
}

void SFlowVizRenderSettingsPanel::OnIsoValueCommitted(
	const FText& NewText, ETextCommit::Type CommitType)
{
	using namespace FlowVizRenderSettingsPanelLocal;
	float Parsed = 0.0f;
	if (ViewModel == nullptr || !ParseFloat(NewText, Parsed))
	{
		return;
	}
	// ANNOUNCED ONLY WHEN ACCEPTED AND CHANGED. SetIsoValue refuses non-finite;
	// re-committing the held value is accepted but changes nothing.
	if (ViewModel->GetIsoValue() != Parsed && ViewModel->SetIsoValue(Parsed))
	{
		NotifyRenderSettingsChanged();
	}
}

void SFlowVizRenderSettingsPanel::OnAmbientCommitted(
	const FText& NewText, ETextCommit::Type CommitType)
{
	using namespace FlowVizRenderSettingsPanelLocal;
	float Parsed = 0.0f;
	if (ViewModel == nullptr || !ParseFloat(NewText, Parsed))
	{
		return;
	}
	// THE CHANGE GATE COMPARES THE CLAMPED RESULT, not the input. SetAmbientStrength
	// clamps, so committing 9 while holding 1 stores 1 again -- announcing that
	// would push a byte-identical model.
	const float Before = ViewModel->GetAmbientStrength();
	ViewModel->SetAmbientStrength(Parsed);
	if (ViewModel->GetAmbientStrength() != Before)
	{
		NotifyRenderSettingsChanged();
	}
}

void SFlowVizRenderSettingsPanel::OnDiffuseCommitted(
	const FText& NewText, ETextCommit::Type CommitType)
{
	using namespace FlowVizRenderSettingsPanelLocal;
	float Parsed = 0.0f;
	if (ViewModel == nullptr || !ParseFloat(NewText, Parsed))
	{
		return;
	}
	const float Before = ViewModel->GetDiffuseStrength();
	ViewModel->SetDiffuseStrength(Parsed);
	if (ViewModel->GetDiffuseStrength() != Before)
	{
		NotifyRenderSettingsChanged();
	}
}

void SFlowVizRenderSettingsPanel::OnLightDirectionCommitted(
	const FText& NewText, ETextCommit::Type CommitType, int32 Axis)
{
	using namespace FlowVizRenderSettingsPanelLocal;
	float Parsed = 0.0f;
	if (ViewModel == nullptr || Axis < 0 || Axis > 2 || !ParseFloat(NewText, Parsed))
	{
		return;
	}

	// PER-AXIS EDIT OF A VECTOR SETTER. The other two components are the CURRENT
	// ones, so typing X does not reset Y and Z. The setter refuses zero and
	// non-finite vectors and normalises the rest; the gate below compares the
	// NORMALISED result, so retyping a component of an already-normalised
	// direction does not announce.
	FVector3f Direction = ViewModel->GetLightDirection();
	Direction[Axis] = Parsed;

	const FVector3f Before = ViewModel->GetLightDirection();
	if (ViewModel->SetLightDirection(Direction)
		&& !ViewModel->GetLightDirection().Equals(Before))
	{
		NotifyRenderSettingsChanged();
	}
}

void SFlowVizRenderSettingsPanel::OnStepVoxelsCommitted(
	const FText& NewText, ETextCommit::Type CommitType)
{
	using namespace FlowVizRenderSettingsPanelLocal;
	float Parsed = 0.0f;
	if (ViewModel == nullptr || !ParseFloat(NewText, Parsed))
	{
		return;
	}
	if (ViewModel->GetStepVoxels() != Parsed && ViewModel->SetStepVoxels(Parsed))
	{
		NotifyRenderSettingsChanged();
	}
}

void SFlowVizRenderSettingsPanel::OnReferenceStepCommitted(
	const FText& NewText, ETextCommit::Type CommitType)
{
	using namespace FlowVizRenderSettingsPanelLocal;
	float Parsed = 0.0f;
	if (ViewModel == nullptr || !ParseFloat(NewText, Parsed))
	{
		return;
	}
	if (ViewModel->GetReferenceStepVoxels() != Parsed
		&& ViewModel->SetReferenceStepVoxels(Parsed))
	{
		NotifyRenderSettingsChanged();
	}
}

void SFlowVizRenderSettingsPanel::OnMaxStepsCommitted(
	const FText& NewText, ETextCommit::Type CommitType)
{
	using namespace FlowVizRenderSettingsPanelLocal;
	float Parsed = 0.0f;
	if (ViewModel == nullptr || !ParseFloat(NewText, Parsed) || Parsed < 0.0f)
	{
		return;
	}
	// SetMaxSteps clamps; the gate compares the clamped result, as with ambient.
	const uint32 Before = ViewModel->GetMaxSteps();
	ViewModel->SetMaxSteps(static_cast<uint32>(Parsed));
	if (ViewModel->GetMaxSteps() != Before)
	{
		NotifyRenderSettingsChanged();
	}
}

void SFlowVizRenderSettingsPanel::OnEarlyOutCommitted(
	const FText& NewText, ETextCommit::Type CommitType)
{
	using namespace FlowVizRenderSettingsPanelLocal;
	float Parsed = 0.0f;
	if (ViewModel == nullptr || !ParseFloat(NewText, Parsed))
	{
		return;
	}
	const float Before = ViewModel->GetEarlyTerminationAlpha();
	ViewModel->SetEarlyTerminationAlpha(Parsed);
	if (ViewModel->GetEarlyTerminationAlpha() != Before)
	{
		NotifyRenderSettingsChanged();
	}
}

void SFlowVizRenderSettingsPanel::OnJitterAmountCommitted(
	const FText& NewText, ETextCommit::Type CommitType)
{
	using namespace FlowVizRenderSettingsPanelLocal;
	float Parsed = 0.0f;
	if (ViewModel == nullptr || !ParseFloat(NewText, Parsed))
	{
		return;
	}
	const float Before = ViewModel->GetJitterAmount();
	ViewModel->SetJitterAmount(Parsed);
	if (ViewModel->GetJitterAmount() != Before)
	{
		NotifyRenderSettingsChanged();
	}
}

void SFlowVizRenderSettingsPanel::OnJitterSeedCommitted(
	const FText& NewText, ETextCommit::Type CommitType)
{
	using namespace FlowVizRenderSettingsPanelLocal;
	float Parsed = 0.0f;
	if (ViewModel == nullptr || !ParseFloat(NewText, Parsed) || Parsed < 0.0f)
	{
		return;
	}
	const uint32 Seed = static_cast<uint32>(Parsed);
	if (ViewModel->GetJitterSeed() != Seed)
	{
		ViewModel->SetJitterSeed(Seed);
		NotifyRenderSettingsChanged();
	}
}

/* ========================================================================== */
/* Text bindings                                                               */
/* ========================================================================== */

FText SFlowVizRenderSettingsPanel::GetIsoValueText() const
{
	using namespace FlowVizRenderSettingsPanelLocal;
	return IsBound() ? FormatValue(ViewModel->GetIsoValue()) : FText::GetEmpty();
}

FText SFlowVizRenderSettingsPanel::GetAmbientText() const
{
	using namespace FlowVizRenderSettingsPanelLocal;
	return IsBound() ? FormatValue(ViewModel->GetAmbientStrength()) : FText::GetEmpty();
}

FText SFlowVizRenderSettingsPanel::GetDiffuseText() const
{
	using namespace FlowVizRenderSettingsPanelLocal;
	return IsBound() ? FormatValue(ViewModel->GetDiffuseStrength()) : FText::GetEmpty();
}

FText SFlowVizRenderSettingsPanel::GetLightDirectionText(int32 Axis) const
{
	using namespace FlowVizRenderSettingsPanelLocal;
	if (!IsBound() || Axis < 0 || Axis > 2)
	{
		return FText::GetEmpty();
	}
	return FormatValue(ViewModel->GetLightDirection()[Axis]);
}

FText SFlowVizRenderSettingsPanel::GetStepVoxelsText() const
{
	using namespace FlowVizRenderSettingsPanelLocal;
	return IsBound() ? FormatValue(ViewModel->GetStepVoxels()) : FText::GetEmpty();
}

FText SFlowVizRenderSettingsPanel::GetReferenceStepText() const
{
	using namespace FlowVizRenderSettingsPanelLocal;
	return IsBound() ? FormatValue(ViewModel->GetReferenceStepVoxels()) : FText::GetEmpty();
}

FText SFlowVizRenderSettingsPanel::GetMaxStepsText() const
{
	return IsBound() ? FText::AsNumber(ViewModel->GetMaxSteps()) : FText::GetEmpty();
}

FText SFlowVizRenderSettingsPanel::GetEarlyOutText() const
{
	using namespace FlowVizRenderSettingsPanelLocal;
	return IsBound() ? FormatValue(ViewModel->GetEarlyTerminationAlpha()) : FText::GetEmpty();
}

FText SFlowVizRenderSettingsPanel::GetJitterAmountText() const
{
	using namespace FlowVizRenderSettingsPanelLocal;
	return IsBound() ? FormatValue(ViewModel->GetJitterAmount()) : FText::GetEmpty();
}

FText SFlowVizRenderSettingsPanel::GetJitterSeedText() const
{
	return IsBound() ? FText::AsNumber(ViewModel->GetJitterSeed()) : FText::GetEmpty();
}

FText SFlowVizRenderSettingsPanel::GetLightingLabel() const
{
	if (!IsBound())
	{
		return LOCTEXT("LightingUnavailable", "Lighting");
	}
	// The label names the STATE IN FORCE, matching the slice panel's toggles.
	return ViewModel->IsLightingEnabled() ? LOCTEXT("LightingOn", "Lit")
										  : LOCTEXT("LightingOff", "Unlit");
}

FText SFlowVizRenderSettingsPanel::GetJitterLabel() const
{
	if (!IsBound())
	{
		return LOCTEXT("JitterUnavailable", "Jitter");
	}
	return ViewModel->IsJitterEnabled() ? LOCTEXT("JitterOn", "Jitter on")
										: LOCTEXT("JitterOff", "Jitter off");
}

FText SFlowVizRenderSettingsPanel::GetFilteringLabel() const
{
	if (!IsBound())
	{
		return LOCTEXT("FilteringUnavailable", "Sampling");
	}
	return ViewModel->IsFieldFilteringEnabled() ? LOCTEXT("FilteringTrilinear", "Interpolated")
												: LOCTEXT("FilteringNearest", "Nearest");
}

FText SFlowVizRenderSettingsPanel::GetStrictFilterLabel() const
{
	if (!IsBound())
	{
		return LOCTEXT("StrictUnavailable", "Strict filter");
	}
	return ViewModel->IsStrictStatusFilter() ? LOCTEXT("StrictOn", "Strict edges")
											 : LOCTEXT("StrictOff", "Permissive edges");
}

#undef LOCTEXT_NAMESPACE
