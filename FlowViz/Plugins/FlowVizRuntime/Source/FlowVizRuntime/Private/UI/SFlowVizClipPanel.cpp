// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizClipPanel.h"

#include "Internationalization/Text.h"
#include "UI/FlowVizWorkspaceStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FlowVizClipPanel"

// NAMED namespace, not anonymous: this module is a unity build, so anonymous
// namespaces from sibling .cpp files merge into one and same-named helpers
// collide by ODR. See the same note on every other UI .cpp here.
namespace FlowVizClipPanelLocal
{
	/** The presets offered, in the order they are shown: the three axis pairs. */
	const EFlowVizClipPreset OfferedPresets[] = {
		EFlowVizClipPreset::KeepPlusX,
		EFlowVizClipPreset::KeepMinusX,
		EFlowVizClipPreset::KeepPlusY,
		EFlowVizClipPreset::KeepMinusY,
		EFlowVizClipPreset::KeepPlusZ,
		EFlowVizClipPreset::KeepMinusZ,
	};

	FText GetPresetLabel(EFlowVizClipPreset Preset)
	{
		switch (Preset)
		{
		case EFlowVizClipPreset::KeepPlusX:
			return LOCTEXT("PresetPlusX", "+X");
		case EFlowVizClipPreset::KeepMinusX:
			return LOCTEXT("PresetMinusX", "−X");
		case EFlowVizClipPreset::KeepPlusY:
			return LOCTEXT("PresetPlusY", "+Y");
		case EFlowVizClipPreset::KeepMinusY:
			return LOCTEXT("PresetMinusY", "−Y");
		case EFlowVizClipPreset::KeepPlusZ:
			return LOCTEXT("PresetPlusZ", "+Z");
		case EFlowVizClipPreset::KeepMinusZ:
			return LOCTEXT("PresetMinusZ", "−Z");
		default:
			return LOCTEXT("PresetUnknown", "?");
		}
	}

	FText GetPresetTooltip(EFlowVizClipPreset Preset)
	{
		return FText::Format(
			LOCTEXT("PresetTip",
				"Add a plane through the domain centre that KEEPS the {0} half. Solver axes, "
				"solver units - not Unreal's."),
			GetPresetLabel(Preset));
	}

	/** Axis names for the crop rows. Solver axes, so X/Y/Z is unambiguous. */
	FText GetAxisLabel(int32 Axis)
	{
		switch (Axis)
		{
		case 0:
			return LOCTEXT("AxisX", "X");
		case 1:
			return LOCTEXT("AxisY", "Y");
		case 2:
			return LOCTEXT("AxisZ", "Z");
		default:
			return LOCTEXT("AxisUnknown", "?");
		}
	}

	/**
	 * Fixed significant figures, for the same anti-jitter reason as the transport
	 * readout: a format that drops trailing zeros changes the field's width as the
	 * value changes, so the row shifts under the cursor while it is being edited.
	 */
	FText FormatValue(double Value)
	{
		return FText::FromString(FString::Printf(TEXT("%.6g"), Value));
	}

	/**
	 * Parse a typed number.
	 *
	 * REFUSES rather than substituting a default. A box that silently reads
	 * unparseable text as 0 moves the crop face to the domain edge, which looks
	 * like the crop working and is the user's typo being executed.
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

	/** A compact square-ish icon button for a plane row. */
	TSharedRef<SButton> MakeRowButton(
		const TAttribute<FText>& Glyph, const FText& Tooltip, FOnClicked OnClicked)
	{
		const float U = FlowVizWorkspaceStyle::GetUnit();
		return SNew(SButton)
			.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
			.OnClicked(OnClicked)
			.ToolTipText(Tooltip)
			.ContentPadding(FMargin(1.0f * U, 0.5f * U))
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
					.Text(Glyph)
					.Font(FlowVizWorkspaceStyle::GetLabelFont())
					.ColorAndOpacity(FSlateColor::UseForeground())
			];
	}
}

void SFlowVizClipPanel::Construct(const FArguments& InArgs)
{
	ViewModel = InArgs._ViewModel;

	const float U = FlowVizWorkspaceStyle::GetUnit();

	// BOUND, not assigned. The predicate is re-read every paint, so a case
	// closing under this panel - or a sixth plane being added from elsewhere -
	// disables the add row without anything having to notify it.
	const TAttribute<bool> AddEnabled =
		TAttribute<bool>::CreateSP(this, &SFlowVizClipPanel::CanAddPlane);

	const TSharedRef<SWrapBox> PresetBox = SNew(SWrapBox)
		.UseAllottedSize(true)
		.InnerSlotPadding(FVector2D(0.5f * U, 0.5f * U));

	for (const EFlowVizClipPreset Preset : FlowVizClipPanelLocal::OfferedPresets)
	{
		TSharedPtr<SButton> Button;

		PresetBox->AddSlot()
		[
			SAssignNew(Button, SButton)
				.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
				.OnClicked(FOnClicked::CreateSP(this, &SFlowVizClipPanel::OnPresetClicked, Preset))
				// RULE 15: the same predicate that guards the action guards the
				// control, so "disabled" and "refused" are one fact rather than two
				// that can drift.
				.IsEnabled(AddEnabled)
				.ToolTipText(FlowVizClipPanelLocal::GetPresetTooltip(Preset))
				.ContentPadding(FMargin(1.5f * U, 0.75f * U))
				.HAlign(HAlign_Center)
			[
				SNew(STextBlock)
					.Text(FlowVizClipPanelLocal::GetPresetLabel(Preset))
					.Font(FlowVizWorkspaceStyle::GetNumericFont())
					.ColorAndOpacity(FSlateColor::UseForeground())
			]
		];

		PresetButtons.Add(Preset, Button);
	}

	/* --- Crop rows: one per solver axis ---------------------------------- */

	const TSharedRef<SVerticalBox> CropBox = SNew(SVerticalBox);

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		CropBox->AddSlot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 1.0f * U, 0.0f))
			[
				SNew(SBox)
					.WidthOverride(2.0f * U)
				[
					SNew(STextBlock)
						.Text(FlowVizClipPanelLocal::GetAxisLabel(Axis))
						.Font(FlowVizWorkspaceStyle::GetNumericFont())
						.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextSecondaryColor()))
				]
			]

			+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(FMargin(0.0f, 0.0f, 0.5f * U, 0.0f))
			[
				SAssignNew(CropMinBoxes[Axis], SFlowVizNumericEntry)
					// BOUND text: the crop follows a domain change and a session
					// load without this box being touched.
					.Text(TAttribute<FText>::CreateSP(
						this, &SFlowVizClipPanel::GetCropMinText, Axis))
					.OnTextCommitted(FOnTextCommitted::CreateSP(
						this, &SFlowVizClipPanel::OnCropMinCommitted, Axis))
					.IsEnabled(TAttribute<bool>::CreateSP(this, &SFlowVizClipPanel::IsBound))
					.Font(FlowVizWorkspaceStyle::GetNumericFont())
					.ToolTipText(LOCTEXT("CropMinTip",
						"Lower crop bound on this axis, in SOLVER units measured from the "
						"domain's minimum corner."))
			]

			+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
			[
				SAssignNew(CropMaxBoxes[Axis], SFlowVizNumericEntry)
					.Text(TAttribute<FText>::CreateSP(
						this, &SFlowVizClipPanel::GetCropMaxText, Axis))
					.OnTextCommitted(FOnTextCommitted::CreateSP(
						this, &SFlowVizClipPanel::OnCropMaxCommitted, Axis))
					.IsEnabled(TAttribute<bool>::CreateSP(this, &SFlowVizClipPanel::IsBound))
					.Font(FlowVizWorkspaceStyle::GetNumericFont())
					.ToolTipText(LOCTEXT("CropMaxTip",
						"Upper crop bound on this axis, in SOLVER units measured from the "
						"domain's minimum corner."))
			]
		];
	}

	ChildSlot
	[
		SNew(SVerticalBox)

		/* --- THE DISCLOSURE, FIRST ---------------------------------------- */

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 1.5f * U))
		[
			// AT THE TOP, not tucked under the controls. This says the planes do
			// not reach the renderer yet. Placed below the controls it would be
			// read after the user had already formed the belief it corrects.
			SNew(SBorder)
				.BorderImage(FlowVizWorkspaceStyle::GetRaisedBrush())
				.Padding(FMargin(1.5f * U, 1.0f * U))
			[
				SNew(STextBlock)
					.Text(TAttribute<FText>::CreateSP(
						this, &SFlowVizClipPanel::GetNotWiredAdvisoryText))
					.Font(FlowVizWorkspaceStyle::GetCaptionFont())
					.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetAdvisoryColor()))
					.AutoWrapText(true)
			]
		]

		/* --- Presets ------------------------------------------------------ */

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
		[
			SNew(STextBlock)
				.Text(LOCTEXT("AddPlaneHeading", "Add a plane"))
				.Font(FlowVizWorkspaceStyle::GetLabelFont())
				.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextSecondaryColor()))
		]

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 1.0f * U))
		[
			PresetBox
		]

		/* --- Plane list --------------------------------------------------- */

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
					// BOUND: "3 / 6 planes" follows the model, including changes
					// this panel did not make.
					.Text(TAttribute<FText>::CreateSP(
						this, &SFlowVizClipPanel::GetPlaneCountText))
					.Font(FlowVizWorkspaceStyle::GetLabelFont())
					.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextSecondaryColor()))
			]

			+ SHorizontalBox::Slot()
				.AutoWidth()
			[
				SAssignNew(RemoveAllButton, SButton)
					.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
					.OnClicked(FOnClicked::CreateSP(this, &SFlowVizClipPanel::OnRemoveAllClicked))
					.IsEnabled(TAttribute<bool>::CreateSP(this, &SFlowVizClipPanel::IsBound))
					.ToolTipText(LOCTEXT("RemoveAllTip", "Remove every clip plane"))
					.ContentPadding(FMargin(1.0f * U, 0.5f * U))
				[
					SNew(STextBlock)
						.Text(LOCTEXT("RemoveAll", "Clear"))
						.Font(FlowVizWorkspaceStyle::GetCaptionFont())
						.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		]

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 1.5f * U))
		[
			SAssignNew(PlaneListBox, SVerticalBox)
		]

		/* --- Crop box ----------------------------------------------------- */

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
					.Text(LOCTEXT("CropHeading", "Crop box (min / max, solver units)"))
					.Font(FlowVizWorkspaceStyle::GetLabelFont())
					.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextSecondaryColor()))
			]

			+ SHorizontalBox::Slot()
				.AutoWidth()
			[
				SAssignNew(ResetCropButton, SButton)
					.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
					.OnClicked(FOnClicked::CreateSP(this, &SFlowVizClipPanel::OnResetCropClicked))
					.IsEnabled(TAttribute<bool>::CreateSP(this, &SFlowVizClipPanel::IsBound))
					.ToolTipText(LOCTEXT("ResetCropTip", "Crop the whole domain again"))
					.ContentPadding(FMargin(1.0f * U, 0.5f * U))
				[
					SNew(STextBlock)
						.Text(LOCTEXT("ResetCrop", "Reset"))
						.Font(FlowVizWorkspaceStyle::GetCaptionFont())
						.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		]

		+ SVerticalBox::Slot()
			.AutoHeight()
		[
			CropBox
		]
	];

	// Build whatever the model already holds. A workspace restoring a saved
	// session hands over a populated model, and a list that only grew from this
	// panel's own clicks would show it as empty.
	RebuildPlaneRows();
}

/* ========================================================================== */
/* Plane rows                                                                  */
/* ========================================================================== */

void SFlowVizClipPanel::RebuildPlaneRows()
{
	PlaneRows.Reset();

	if (!PlaneListBox.IsValid())
	{
		return;
	}
	PlaneListBox->ClearChildren();

	const int32 Count = ViewModel != nullptr ? ViewModel->GetPlaneCount() : 0;
	const float U = FlowVizWorkspaceStyle::GetUnit();

	for (int32 Index = 0; Index < Count; ++Index)
	{
		FPlaneRow Row;

		Row.EnableButton = FlowVizClipPanelLocal::MakeRowButton(
			TAttribute<FText>::CreateSP(this, &SFlowVizClipPanel::GetPlaneEnableGlyph, Index),
			LOCTEXT("PlaneEnableTip",
				"Show or hide this plane. Hiding RETAINS it - it stops being sent to the "
				"renderer but keeps its position."),
			FOnClicked::CreateSP(this, &SFlowVizClipPanel::OnPlaneEnableClicked, Index));

		Row.InvertButton = FlowVizClipPanelLocal::MakeRowButton(
			FText::FromString(TEXT("⇄")),
			LOCTEXT("PlaneInvertTip",
				"Keep the other half. Flips the normal AND the distance, so the plane stays "
				"where it is and only which side survives changes."),
			FOnClicked::CreateSP(this, &SFlowVizClipPanel::OnPlaneInvertClicked, Index));

		Row.RemoveButton = FlowVizClipPanelLocal::MakeRowButton(
			FText::FromString(TEXT("✕")),
			LOCTEXT("PlaneRemoveTip", "Delete this plane"),
			FOnClicked::CreateSP(this, &SFlowVizClipPanel::OnPlaneRemoveClicked, Index));

		PlaneListBox->AddSlot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
		[
			SNew(SBorder)
				.BorderImage(FlowVizWorkspaceStyle::GetRaisedBrush())
				.Padding(FMargin(1.0f * U, 0.5f * U))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(FMargin(0.0f, 0.0f, 1.0f * U, 0.0f))
				[
					Row.EnableButton.ToSharedRef()
				]

				+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
						// BOUND to the model's own label and offset, so an inverted
						// or moved plane relabels itself.
						.Text(TAttribute<FText>::CreateSP(
							this, &SFlowVizClipPanel::GetPlaneLabelText, Index))
						.Font(FlowVizWorkspaceStyle::GetNumericFont())
						.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextPrimaryColor()))
				]

				+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(FMargin(0.5f * U, 0.0f, 0.0f, 0.0f))
				[
					Row.InvertButton.ToSharedRef()
				]

				+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(FMargin(0.5f * U, 0.0f, 0.0f, 0.0f))
				[
					Row.RemoveButton.ToSharedRef()
				]
			]
		];

		PlaneRows.Add(Row);
	}
}

/* ========================================================================== */
/* Test seams                                                                  */
/* ========================================================================== */

TSharedPtr<SButton> SFlowVizClipPanel::GetPresetButton(EFlowVizClipPreset Preset) const
{
	const TSharedPtr<SButton>* Found = PresetButtons.Find(Preset);
	return Found != nullptr ? *Found : TSharedPtr<SButton>();
}

TSharedPtr<SButton> SFlowVizClipPanel::GetPlaneEnableButton(int32 Index) const
{
	return PlaneRows.IsValidIndex(Index) ? PlaneRows[Index].EnableButton : TSharedPtr<SButton>();
}

TSharedPtr<SButton> SFlowVizClipPanel::GetPlaneInvertButton(int32 Index) const
{
	return PlaneRows.IsValidIndex(Index) ? PlaneRows[Index].InvertButton : TSharedPtr<SButton>();
}

TSharedPtr<SButton> SFlowVizClipPanel::GetPlaneRemoveButton(int32 Index) const
{
	return PlaneRows.IsValidIndex(Index) ? PlaneRows[Index].RemoveButton : TSharedPtr<SButton>();
}

TSharedPtr<SFlowVizNumericEntry> SFlowVizClipPanel::GetCropMinBox(int32 Axis) const
{
	return (Axis >= 0 && Axis < 3) ? CropMinBoxes[Axis] : TSharedPtr<SFlowVizNumericEntry>();
}

TSharedPtr<SFlowVizNumericEntry> SFlowVizClipPanel::GetCropMaxBox(int32 Axis) const
{
	return (Axis >= 0 && Axis < 3) ? CropMaxBoxes[Axis] : TSharedPtr<SFlowVizNumericEntry>();
}

/* ========================================================================== */
/* Bound reads                                                                 */
/* ========================================================================== */

FText SFlowVizClipPanel::GetNotWiredAdvisoryText() const
{
	// NOT CONDITIONAL ON ANYTHING. This is true whatever the model holds, and a
	// disclosure that could be empty is one a reader has to notice the absence
	// of. It stops being correct the day someone calls
	// FFlowVizClipViewModel::ApplyToRayMarchParameters from the render path -
	// and the test that names that function is what should fail then.
	return LOCTEXT("NotWired",
		"These planes are authored and saved, but NOT yet applied to the render. Nothing in "
		"the render path reads them, so the image will not change. Shown rather than hidden "
		"because a plane that does nothing is indistinguishable from one facing the wrong way.");
}

bool SFlowVizClipPanel::CanAddPlane() const
{
	// TWO CONDITIONS, and both matter. Without a model there is nothing to add
	// to; without room the view model would refuse, and a button that is pressed
	// and refused is the nonfunctional control rule 15 forbids.
	return ViewModel != nullptr && ViewModel->CanAddPlane();
}

bool SFlowVizClipPanel::IsPlaneEnabled(int32 Index) const
{
	if (ViewModel == nullptr)
	{
		return false;
	}
	const FFlowVizClipPlane* Plane = ViewModel->FindPlane(Index);
	return Plane != nullptr && Plane->bEnabled;
}

FText SFlowVizClipPanel::GetPlaneEnableGlyph(int32 Index) const
{
	// An EYE and a STRUCK-THROUGH EYE, so the state is legible without reading a
	// label, and distinct from the delete glyph beside it.
	return IsPlaneEnabled(Index) ? FText::FromString(TEXT("◉"))
								 : FText::FromString(TEXT("○"));
}

FText SFlowVizClipPanel::GetPlaneLabelText(int32 Index) const
{
	if (ViewModel == nullptr)
	{
		return FText::GetEmpty();
	}
	const FFlowVizClipPlane* Plane = ViewModel->FindPlane(Index);
	if (Plane == nullptr)
	{
		return FText::GetEmpty();
	}

	// The NORMAL AND THE OFFSET, not just the label. A preset's label says "+X"
	// forever, including after the plane has been inverted or moved; the numbers
	// are what the shader will actually use.
	return FText::FromString(FString::Printf(TEXT("n (%.3g, %.3g, %.3g)   d %.4g"),
		Plane->Normal.X, Plane->Normal.Y, Plane->Normal.Z, Plane->Distance));
}

FText SFlowVizClipPanel::GetPlaneCountText() const
{
	const int32 Count = ViewModel != nullptr ? ViewModel->GetPlaneCount() : 0;
	const int32 Enabled = ViewModel != nullptr ? ViewModel->GetEnabledPlaneCount() : 0;

	// BOTH NUMBERS. "3 planes" while one is hidden would misdescribe what is
	// being rendered, which is the count that matters.
	return FText::Format(
		LOCTEXT("PlaneCount", "Planes: {0} of {1} shown, limit {2}"),
		FText::AsNumber(Enabled), FText::AsNumber(Count),
		FText::AsNumber(FlowVizRayMarch::MaxClipPlanes));
}

FText SFlowVizClipPanel::GetCropMinText(int32 Axis) const
{
	if (ViewModel == nullptr || Axis < 0 || Axis > 2)
	{
		return FText::GetEmpty();
	}
	return FlowVizClipPanelLocal::FormatValue(ViewModel->GetCropMin()[Axis]);
}

FText SFlowVizClipPanel::GetCropMaxText(int32 Axis) const
{
	if (ViewModel == nullptr || Axis < 0 || Axis > 2)
	{
		return FText::GetEmpty();
	}
	return FlowVizClipPanelLocal::FormatValue(ViewModel->GetCropMax()[Axis]);
}

/* ========================================================================== */
/* Actions                                                                     */
/* ========================================================================== */

FReply SFlowVizClipPanel::OnPresetClicked(EFlowVizClipPreset Preset)
{
	if (ViewModel != nullptr)
	{
		ViewModel->AddPresetPlane(Preset);

		// The COUNT changed, so the rows are stale. Rebuilt rather than appended
		// to because AddPresetPlane can refuse - at the limit, or with no domain -
		// and appending unconditionally would add a row for a plane that does not
		// exist.
		RebuildPlaneRows();
	}
	return FReply::Handled();
}

FReply SFlowVizClipPanel::OnPlaneEnableClicked(int32 Index)
{
	if (ViewModel != nullptr)
	{
		// TOGGLE, not remove. The distinction is the whole point of the control:
		// hiding retains the plane's position so it can be brought back.
		ViewModel->SetPlaneEnabled(Index, !IsPlaneEnabled(Index));
	}
	// No rebuild: the count is unchanged and the glyph is bound.
	return FReply::Handled();
}

FReply SFlowVizClipPanel::OnPlaneInvertClicked(int32 Index)
{
	if (ViewModel != nullptr)
	{
		// The view model negates N AND D. Doing it here instead would be a second
		// implementation of the convention, and getting it wrong moves the plane
		// rather than flipping it.
		ViewModel->InvertPlane(Index);
	}
	return FReply::Handled();
}

FReply SFlowVizClipPanel::OnPlaneRemoveClicked(int32 Index)
{
	if (ViewModel != nullptr)
	{
		ViewModel->RemovePlane(Index);

		// MANDATORY here, not an optimisation. Removing plane 0 renumbers every
		// plane after it, so every surviving row's captured index now points at a
		// different plane - row 1 would delete what the user sees as row 2.
		RebuildPlaneRows();
	}
	return FReply::Handled();
}

FReply SFlowVizClipPanel::OnRemoveAllClicked()
{
	if (ViewModel != nullptr)
	{
		ViewModel->RemoveAllPlanes();
		RebuildPlaneRows();
	}
	return FReply::Handled();
}

FReply SFlowVizClipPanel::OnResetCropClicked()
{
	if (ViewModel != nullptr)
	{
		ViewModel->ResetCropBox();
	}
	return FReply::Handled();
}

void SFlowVizClipPanel::OnCropMinCommitted(
	const FText& NewText, ETextCommit::Type CommitType, int32 Axis)
{
	if (ViewModel == nullptr || Axis < 0 || Axis > 2)
	{
		return;
	}

	double Value = 0.0;
	if (!FlowVizClipPanelLocal::TryParse(NewText, Value))
	{
		// REFUSED, and the bound Text attribute puts the model's value back on the
		// next paint - so the box visibly reverts rather than keeping a number
		// that was never accepted.
		return;
	}

	// ONE AXIS. Read the current box, change one component, write it back: a
	// handler that built a fresh FVector from this one field would silently zero
	// the other two axes, which reads as the crop collapsing.
	FVector Min = ViewModel->GetCropMin();
	Min[Axis] = Value;
	ViewModel->SetCropBox(Min, ViewModel->GetCropMax());
}

void SFlowVizClipPanel::OnCropMaxCommitted(
	const FText& NewText, ETextCommit::Type CommitType, int32 Axis)
{
	if (ViewModel == nullptr || Axis < 0 || Axis > 2)
	{
		return;
	}

	double Value = 0.0;
	if (!FlowVizClipPanelLocal::TryParse(NewText, Value))
	{
		return;
	}

	FVector Max = ViewModel->GetCropMax();
	Max[Axis] = Value;
	ViewModel->SetCropBox(ViewModel->GetCropMin(), Max);
}

#undef LOCTEXT_NAMESPACE
