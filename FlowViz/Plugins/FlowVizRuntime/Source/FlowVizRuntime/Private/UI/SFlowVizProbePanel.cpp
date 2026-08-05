// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizProbePanel.h"

#include "Internationalization/Text.h"
#include "UI/FlowVizWorkspaceStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FlowVizProbePanel"

// NAMED namespace, not anonymous: this module is a unity build, so anonymous
// namespaces from sibling .cpp files merge into one and same-named helpers
// collide by ODR. See the same note on every other UI .cpp here.
namespace FlowVizProbePanelLocal
{
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
	 * REFUSES rather than substituting a default. A field that reads unparseable
	 * text as 0 places the probe at the domain's origin corner, which looks like a
	 * successful placement and is really the user's typo being executed - and a
	 * probe reports a real number from whatever cell it lands in, so no range
	 * check downstream can catch it.
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

	/** A compact icon button for a probe row. */
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

	/** A three-field XYZ row, for a position being composed. */
	TSharedRef<SWidget> MakeVectorRow(
		const FText& Label, TStaticArray<TSharedPtr<SFlowVizNumericEntry>, 3>& OutBoxes,
		TFunctionRef<TSharedRef<SFlowVizNumericEntry>(int32)> MakeBox)
	{
		const float U = FlowVizWorkspaceStyle::GetUnit();

		const TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 1.0f * U, 0.0f))
			[
				SNew(SBox)
					// FIXED WIDTH so the label column lines up with its siblings.
					.WidthOverride(5.0f * U)
				[
					SNew(STextBlock)
						.Text(Label)
						.Font(FlowVizWorkspaceStyle::GetLabelFont())
						.ColorAndOpacity(
							FSlateColor(FlowVizWorkspaceStyle::GetTextSecondaryColor()))
				]
			];

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const TSharedRef<SFlowVizNumericEntry> Box = MakeBox(Axis);
			OutBoxes[Axis] = Box;

			Row->AddSlot()
				.FillWidth(1.0f)
				.Padding(FMargin(0.0f, 0.0f, Axis < 2 ? 0.5f * U : 0.0f, 0.0f))
			[
				Box
			];
		}

		return Row;
	}
}

void SFlowVizProbePanel::Construct(const FArguments& InArgs)
{
	ViewModel = InArgs._ViewModel;

	const float U = FlowVizWorkspaceStyle::GetUnit();

	// BOUND, not assigned. The predicates are re-read every paint, so a case
	// closing under this panel - or a probe added from elsewhere - updates the
	// controls without anything having to notify them.
	const TAttribute<bool> PanelLive = TAttribute<bool>::CreateSP(this, &SFlowVizProbePanel::IsBound);
	const TAttribute<bool> ClearLive =
		TAttribute<bool>::CreateSP(this, &SFlowVizProbePanel::CanRemoveAll);

	/* --- Placement row ---------------------------------------------------- */

	const TSharedRef<SWidget> PlacementRow = FlowVizProbePanelLocal::MakeVectorRow(
		LOCTEXT("PlaceLabel", "Place at"), PlacementBoxes,
		[this, &PanelLive](int32 Axis) -> TSharedRef<SFlowVizNumericEntry>
		{
			return SNew(SFlowVizNumericEntry)
				.Text(FlowVizProbePanelLocal::FormatValue(PendingPosition[Axis]))
				.OnTextCommitted(FOnTextCommitted::CreateSP(
					this, &SFlowVizProbePanel::OnPlacementCommitted, Axis))
				.IsEnabled(PanelLive)
				.Font(FlowVizWorkspaceStyle::GetNumericFont())
				.HintText(FlowVizProbePanelLocal::GetAxisLabel(Axis))
				.ToolTipText(FText::Format(
					LOCTEXT("PlaceTip",
						"{0} of the new probe, in SOLVER units on the canonical CFDViz axes - "
						"NOT Unreal centimetres, and not mirrored in Y."),
					FlowVizProbePanelLocal::GetAxisLabel(Axis)));
		});

	/* --- Line probe rows -------------------------------------------------- */

	const TSharedRef<SWidget> LineStartRow = FlowVizProbePanelLocal::MakeVectorRow(
		LOCTEXT("LineStartLabel", "From"), LineStartBoxes,
		[this, &PanelLive](int32 Axis) -> TSharedRef<SFlowVizNumericEntry>
		{
			return SNew(SFlowVizNumericEntry)
				.Text(FlowVizProbePanelLocal::FormatValue(PendingLineStart[Axis]))
				.OnTextCommitted(FOnTextCommitted::CreateSP(
					this, &SFlowVizProbePanel::OnLineStartCommitted, Axis))
				.IsEnabled(PanelLive)
				.Font(FlowVizWorkspaceStyle::GetNumericFont())
				.HintText(FlowVizProbePanelLocal::GetAxisLabel(Axis));
		});

	const TSharedRef<SWidget> LineEndRow = FlowVizProbePanelLocal::MakeVectorRow(
		LOCTEXT("LineEndLabel", "To"), LineEndBoxes,
		[this, &PanelLive](int32 Axis) -> TSharedRef<SFlowVizNumericEntry>
		{
			return SNew(SFlowVizNumericEntry)
				.Text(FlowVizProbePanelLocal::FormatValue(PendingLineEnd[Axis]))
				.OnTextCommitted(FOnTextCommitted::CreateSP(
					this, &SFlowVizProbePanel::OnLineEndCommitted, Axis))
				.IsEnabled(PanelLive)
				.Font(FlowVizWorkspaceStyle::GetNumericFont())
				.HintText(FlowVizProbePanelLocal::GetAxisLabel(Axis));
		});

	ChildSlot
	[
		SNew(SVerticalBox)

		/* --- THE DISCLOSURE, FIRST ---------------------------------------- */

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 1.5f * U))
		[
			// AT THE TOP, not under the list. Nothing schedules a sample, so every
			// probe below reads "not sampled" forever. Placed after the controls
			// this would be read once the user had already decided the probe was
			// broken.
			SNew(SBorder)
				.BorderImage(FlowVizWorkspaceStyle::GetRaisedBrush())
				.Padding(FMargin(1.5f * U, 1.0f * U))
			[
				SNew(STextBlock)
					.Text(TAttribute<FText>::CreateSP(
						this, &SFlowVizProbePanel::GetNotSampledAdvisoryText))
					.Font(FlowVizWorkspaceStyle::GetCaptionFont())
					.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetAdvisoryColor()))
					.AutoWrapText(true)
			]
		]

		/* --- Placement ---------------------------------------------------- */

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
		[
			PlacementRow
		]

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 1.0f * U))
			.HAlign(HAlign_Right)
		[
			SAssignNew(AddProbeButton, SButton)
				.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
				.OnClicked(FOnClicked::CreateSP(this, &SFlowVizProbePanel::OnAddProbeClicked))
				// RULE 15: the same condition that makes the action possible guards
				// the control, so "disabled" and "refused" are one fact.
				.IsEnabled(PanelLive)
				.ToolTipText(LOCTEXT("AddProbeTip", "Place a probe at the position above."))
				.ContentPadding(FMargin(1.5f * U, 0.75f * U))
			[
				SNew(STextBlock)
					.Text(LOCTEXT("AddProbe", "Add probe"))
					.Font(FlowVizWorkspaceStyle::GetCaptionFont())
					.ColorAndOpacity(FSlateColor::UseForeground())
			]
		]

		/* --- The list ------------------------------------------------------ */

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
					// BOUND: the count follows the model, including changes this
					// panel did not make.
					.Text(TAttribute<FText>::CreateSP(
						this, &SFlowVizProbePanel::GetProbeCountText))
					.Font(FlowVizWorkspaceStyle::GetLabelFont())
					.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextSecondaryColor()))
			]

			+ SHorizontalBox::Slot()
				.AutoWidth()
			[
				SAssignNew(RemoveAllButton, SButton)
					.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
					.OnClicked(
						FOnClicked::CreateSP(this, &SFlowVizProbePanel::OnRemoveAllClicked))
					.IsEnabled(ClearLive)
					.ToolTipText(LOCTEXT("RemoveAllTip", "Remove every probe"))
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
			SAssignNew(ProbeListBox, SVerticalBox)
		]

		/* --- Line probe ---------------------------------------------------- */

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
		[
			SNew(STextBlock)
				.Text(LOCTEXT("LineHeading", "Line probe"))
				.Font(FlowVizWorkspaceStyle::GetLabelFont())
				.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextSecondaryColor()))
		]

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
		[
			LineStartRow
		]

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
		[
			LineEndRow
		]

		+ SVerticalBox::Slot()
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
					.WidthOverride(5.0f * U)
				[
					SNew(STextBlock)
						.Text(LOCTEXT("LineSamplesLabel", "Samples"))
						.Font(FlowVizWorkspaceStyle::GetLabelFont())
						.ColorAndOpacity(
							FSlateColor(FlowVizWorkspaceStyle::GetTextSecondaryColor()))
				]
			]

			+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(FMargin(0.0f, 0.0f, 0.5f * U, 0.0f))
			[
				SAssignNew(LineSamplesBox, SFlowVizNumericEntry)
					// BOUND text: follows a sample-count change made elsewhere.
					.Text(TAttribute<FText>::CreateSP(
						this, &SFlowVizProbePanel::GetLineSamplesText))
					.OnTextCommitted(FOnTextCommitted::CreateSP(
						this, &SFlowVizProbePanel::OnLineSamplesCommitted))
					.IsEnabled(PanelLive)
					.Font(FlowVizWorkspaceStyle::GetNumericFont())
					.ToolTipText(LOCTEXT("LineSamplesTip",
						"How many points are sampled along the line, endpoints included. Two "
						"is the minimum: a one-sample line is a point probe."))
			]

			+ SHorizontalBox::Slot()
				.AutoWidth()
			[
				SAssignNew(SetLineButton, SButton)
					.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
					.OnClicked(FOnClicked::CreateSP(this, &SFlowVizProbePanel::OnSetLineClicked))
					.IsEnabled(PanelLive)
					.ToolTipText(LOCTEXT("SetLineTip", "Define the line probe between the "
													   "endpoints above."))
					.ContentPadding(FMargin(1.0f * U, 0.5f * U))
				[
					SNew(STextBlock)
						.Text(LOCTEXT("SetLine", "Set"))
						.Font(FlowVizWorkspaceStyle::GetCaptionFont())
						.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		]

		+ SVerticalBox::Slot()
			.AutoHeight()
		[
			SNew(STextBlock)
				// BOUND summary, so the panel says whether a line EXISTS. Without
				// it, "Set" pressed on a zero-length line is refused by the view
				// model with nothing on screen to show for it.
				.Text(TAttribute<FText>::CreateSP(this, &SFlowVizProbePanel::GetLineSummaryText))
				.Font(FlowVizWorkspaceStyle::GetCaptionFont())
				.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextDisabledColor()))
				.AutoWrapText(true)
		]
	];

	RebuildProbeRows();
}

/* ========================================================================== */
/* Rows                                                                        */
/* ========================================================================== */

void SFlowVizProbePanel::RebuildProbeRows()
{
	ProbeRows.Reset();

	if (!ProbeListBox.IsValid())
	{
		return;
	}
	ProbeListBox->ClearChildren();

	if (!IsBound())
	{
		return;
	}

	const float U = FlowVizWorkspaceStyle::GetUnit();
	const TArray<FFlowVizProbe>& Probes = ViewModel->GetProbes();

	for (const FFlowVizProbe& Probe : Probes)
	{
		FProbeRow Row;
		// CAPTURED BY ID, not by index. Removing a probe renumbers every probe
		// after it, so a row holding an index would keep working - on a different
		// probe. See the note on FProbeRow::Id.
		Row.Id = Probe.Id;

		const FGuid Id = Probe.Id;

		ProbeListBox->AddSlot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 0.5f * U))
		[
			SNew(SBorder)
				.BorderImage(FlowVizWorkspaceStyle::GetRaisedBrush())
				.Padding(FMargin(1.0f * U, 0.75f * U))
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
					.AutoHeight()
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						.Padding(FMargin(0.0f, 0.0f, 0.5f * U, 0.0f))
					[
						SAssignNew(Row.NameBox, SFlowVizNumericEntry)
							.Text(TAttribute<FText>::CreateSP(
								this, &SFlowVizProbePanel::GetProbeNameText, Id))
							.OnTextCommitted(FOnTextCommitted::CreateSP(
								this, &SFlowVizProbePanel::OnProbeNameCommitted, Id))
							.Font(FlowVizWorkspaceStyle::GetLabelFont())
							.ToolTipText(LOCTEXT("ProbeNameTip", "Rename this probe."))
					]

					+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(FMargin(0.0f, 0.0f, 0.5f * U, 0.0f))
					[
						SAssignNew(Row.VisibleButton, SButton)
							.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
							.OnClicked(FOnClicked::CreateSP(
								this, &SFlowVizProbePanel::OnProbeVisibleClicked, Id))
							.ToolTipText(LOCTEXT("ProbeVisibleTip",
								"Show or hide this probe. It keeps its position and its "
								"reading either way - this is not a delete."))
							.ContentPadding(FMargin(1.0f * U, 0.5f * U))
						[
							SNew(STextBlock)
								// BOUND GLYPH, so the button says which state the
								// probe is IN rather than which state it would move to.
								.Text(TAttribute<FText>::CreateSP(
									this, &SFlowVizProbePanel::GetProbeVisibleGlyph, Id))
								.Font(FlowVizWorkspaceStyle::GetLabelFont())
								.ColorAndOpacity(FSlateColor::UseForeground())
						]
					]

					+ SHorizontalBox::Slot()
						.AutoWidth()
					[
						SAssignNew(Row.RemoveButton, SButton)
							.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
							.OnClicked(FOnClicked::CreateSP(
								this, &SFlowVizProbePanel::OnProbeRemoveClicked, Id))
							.ToolTipText(LOCTEXT("ProbeRemoveTip", "Delete this probe."))
							.ContentPadding(FMargin(1.0f * U, 0.5f * U))
						[
							SNew(STextBlock)
								.Text(LOCTEXT("ProbeRemoveGlyph", "✕"))
								.Font(FlowVizWorkspaceStyle::GetLabelFont())
								.ColorAndOpacity(FSlateColor::UseForeground())
						]
					]
				]

				+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.0f, 0.5f * U, 0.0f, 0.0f))
				[
					SNew(STextBlock)
						.Text(TAttribute<FText>::CreateSP(
							this, &SFlowVizProbePanel::GetProbePositionText, Id))
						.Font(FlowVizWorkspaceStyle::GetCaptionFont())
						.ColorAndOpacity(
							FSlateColor(FlowVizWorkspaceStyle::GetTextDisabledColor()))
				]

				+ SVerticalBox::Slot()
					.AutoHeight()
				[
					SNew(STextBlock)
						// THE VALUE. Bound through GetProbeValueText, which checks
						// bHasValue before it formats anything - see rule 10 there.
						.Text(TAttribute<FText>::CreateLambda(
							[this, Id]() -> FText
							{
								if (!IsBound())
								{
									return FText::GetEmpty();
								}
								const TArray<FFlowVizProbe>& All = ViewModel->GetProbes();
								for (int32 I = 0; I < All.Num(); ++I)
								{
									if (All[I].Id == Id)
									{
										return GetProbeValueText(I);
									}
								}
								return FText::GetEmpty();
							}))
						.Font(FlowVizWorkspaceStyle::GetNumericFont())
						.ColorAndOpacity(
							FSlateColor(FlowVizWorkspaceStyle::GetTextPrimaryColor()))
				]
			]
		];

		ProbeRows.Add(Row);
	}
}

/* ========================================================================== */
/* Seams                                                                       */
/* ========================================================================== */

TSharedPtr<SFlowVizNumericEntry> SFlowVizProbePanel::GetPlacementBox(int32 Axis) const
{
	if (Axis < 0 || Axis >= PlacementBoxes.Num())
	{
		return nullptr;
	}
	return PlacementBoxes[Axis];
}

TSharedPtr<SFlowVizNumericEntry> SFlowVizProbePanel::GetLineStartBox(int32 Axis) const
{
	if (Axis < 0 || Axis >= LineStartBoxes.Num())
	{
		return nullptr;
	}
	return LineStartBoxes[Axis];
}

TSharedPtr<SFlowVizNumericEntry> SFlowVizProbePanel::GetLineEndBox(int32 Axis) const
{
	if (Axis < 0 || Axis >= LineEndBoxes.Num())
	{
		return nullptr;
	}
	return LineEndBoxes[Axis];
}

TSharedPtr<SFlowVizNumericEntry> SFlowVizProbePanel::GetProbeNameBox(int32 Index) const
{
	if (!ProbeRows.IsValidIndex(Index))
	{
		return nullptr;
	}
	return ProbeRows[Index].NameBox;
}

TSharedPtr<SButton> SFlowVizProbePanel::GetProbeVisibleButton(int32 Index) const
{
	if (!ProbeRows.IsValidIndex(Index))
	{
		return nullptr;
	}
	return ProbeRows[Index].VisibleButton;
}

TSharedPtr<SButton> SFlowVizProbePanel::GetProbeRemoveButton(int32 Index) const
{
	if (!ProbeRows.IsValidIndex(Index))
	{
		return nullptr;
	}
	return ProbeRows[Index].RemoveButton;
}

FText SFlowVizProbePanel::GetNotSampledAdvisoryText() const
{
	// UNCONDITIONAL. Nothing in production calls FlowVizProbe::SampleStoredField,
	// so there is no state in which this is untrue.
	return LOCTEXT("NotSampledAdvisory",
		"Not sampled yet: nothing schedules a field read, so every probe below reports "
		"\"no reading\" regardless of where it is placed. Positions are saved and are in "
		"SOLVER units.");
}

FText SFlowVizProbePanel::GetProbeValueText(int32 Index) const
{
	if (!IsBound())
	{
		return LOCTEXT("ValueNoModel", "no reading");
	}

	const TArray<FFlowVizProbe>& Probes = ViewModel->GetProbes();
	if (!Probes.IsValidIndex(Index))
	{
		return LOCTEXT("ValueNoProbe", "no reading");
	}

	const FFlowVizProbeReading& Reading = Probes[Index].LastReading;

	// RULE 10, AND THE ORDER MATTERS. bHasValue is checked BEFORE anything is
	// formatted, because FFlowVizProbeReading::Magnitude defaults to 0.0 - and
	// zero is a real, interesting velocity. A readout that formatted it
	// unconditionally would turn "never sampled" into a measurement, and no
	// downstream check could tell them apart.
	if (!Reading.bHasValue)
	{
		// The string carries NO DIGITS at all, deliberately: a user scanning the
		// column must not find a number here under any reading of it.
		return Reading.bInsideDomain ? LOCTEXT("ValueNotSampled", "no reading")
									 : LOCTEXT("ValueOutside", "outside the domain");
	}

	// The VOXEL is named, not just the value. SampleStoredField reads the nearest
	// voxel, so the number is the value AT A CELL rather than at the requested
	// point; a readout that showed only the number would imply a point measurement.
	return FText::Format(
		LOCTEXT("ValueReadout", "|v| {0}  at cell ({1}, {2}, {3})"),
		FlowVizProbePanelLocal::FormatValue(Reading.Magnitude),
		FText::AsNumber(Reading.Voxel.X), FText::AsNumber(Reading.Voxel.Y),
		FText::AsNumber(Reading.Voxel.Z));
}

/* ========================================================================== */
/* Predicates                                                                  */
/* ========================================================================== */

bool SFlowVizProbePanel::CanRemoveAll() const
{
	// BOTH conditions. Bound-ness alone would leave Clear live with an empty
	// list, where pressing it does nothing at all.
	return IsBound() && ViewModel->GetProbeCount() > 0;
}

/* ========================================================================== */
/* Handlers                                                                    */
/* ========================================================================== */

FReply SFlowVizProbePanel::OnAddProbeClicked()
{
	if (IsBound())
	{
		// SOLVER position, straight through. AddProbeAtUnrealPosition is for a
		// viewport pick; routing a typed field through it would apply the Y-mirror
		// and the unit scale to a number the user typed in solver coordinates.
		ViewModel->AddProbeAtSolverPosition(PendingPosition);
		RebuildProbeRows();
	}
	return FReply::Handled();
}

FReply SFlowVizProbePanel::OnRemoveAllClicked()
{
	if (IsBound())
	{
		ViewModel->RemoveAllProbes();
		RebuildProbeRows();
	}
	return FReply::Handled();
}

FReply SFlowVizProbePanel::OnProbeVisibleClicked(FGuid Id)
{
	if (IsBound())
	{
		if (const FFlowVizProbe* Probe = ViewModel->FindProbe(Id))
		{
			// A TOGGLE, not a delete. The probe keeps its position and its reading.
			ViewModel->SetProbeVisible(Id, !Probe->bVisible);
		}
	}
	// NO REBUILD: the count did not change, and the glyph is bound.
	return FReply::Handled();
}

FReply SFlowVizProbePanel::OnProbeRemoveClicked(FGuid Id)
{
	if (IsBound())
	{
		ViewModel->RemoveProbe(Id);
		// MANDATORY here, not an optimisation: the row for the removed probe must
		// go, and the rows are what GetProbeRemoveButton(Index) indexes.
		RebuildProbeRows();
	}
	return FReply::Handled();
}

FReply SFlowVizProbePanel::OnSetLineClicked()
{
	if (IsBound())
	{
		// The view model refuses a zero-length line. Not swallowed here: the bound
		// summary text below the row reports whether a line exists, so a refusal is
		// visible rather than silent.
		ViewModel->SetLineProbe(PendingLineStart, PendingLineEnd);
	}
	return FReply::Handled();
}

void SFlowVizProbePanel::OnPlacementCommitted(
	const FText& NewText, ETextCommit::Type CommitType, int32 Axis)
{
	double Value = 0.0;
	if (!FlowVizProbePanelLocal::TryParse(NewText, Value))
	{
		// Refused rather than defaulted - see TryParse.
		return;
	}
	if (Axis < 0 || Axis >= 3)
	{
		return;
	}
	// ONE COMPONENT. Building a fresh FVector from this field would silently zero
	// the other two axes, placing the probe somewhere the user never typed.
	PendingPosition[Axis] = Value;
}

void SFlowVizProbePanel::OnLineStartCommitted(
	const FText& NewText, ETextCommit::Type CommitType, int32 Axis)
{
	double Value = 0.0;
	if (!FlowVizProbePanelLocal::TryParse(NewText, Value) || Axis < 0 || Axis >= 3)
	{
		return;
	}
	PendingLineStart[Axis] = Value;
}

void SFlowVizProbePanel::OnLineEndCommitted(
	const FText& NewText, ETextCommit::Type CommitType, int32 Axis)
{
	double Value = 0.0;
	if (!FlowVizProbePanelLocal::TryParse(NewText, Value) || Axis < 0 || Axis >= 3)
	{
		return;
	}
	PendingLineEnd[Axis] = Value;
}

void SFlowVizProbePanel::OnLineSamplesCommitted(
	const FText& NewText, ETextCommit::Type CommitType)
{
	if (!IsBound())
	{
		return;
	}
	double Value = 0.0;
	if (!FlowVizProbePanelLocal::TryParse(NewText, Value))
	{
		return;
	}
	// TRUNCATED, not rounded: a sample count is a count.
	ViewModel->SetLineSampleCount(static_cast<int32>(Value));
}

void SFlowVizProbePanel::OnProbeNameCommitted(
	const FText& NewText, ETextCommit::Type CommitType, FGuid Id)
{
	if (IsBound())
	{
		// BY ID. A name committed on one row must not land on another, which is
		// exactly what an index captured in a rebuilt loop would do.
		ViewModel->RenameProbe(Id, NewText.ToString());
	}
}

/* ========================================================================== */
/* Bound readers. Every one tolerates a null view model.                       */
/* ========================================================================== */

FText SFlowVizProbePanel::GetProbeCountText() const
{
	if (!IsBound())
	{
		return LOCTEXT("NoCase", "No case open");
	}
	const int32 Count = ViewModel->GetProbeCount();
	if (Count == 0)
	{
		return LOCTEXT("NoProbes", "No probes");
	}
	return FText::Format(LOCTEXT("ProbeCount", "{0} probes"), FText::AsNumber(Count));
}

FText SFlowVizProbePanel::GetProbePositionText(FGuid Id) const
{
	if (!IsBound())
	{
		return FText::GetEmpty();
	}
	const FFlowVizProbe* Probe = ViewModel->FindProbe(Id);
	if (Probe == nullptr)
	{
		return FText::GetEmpty();
	}
	const FVector& P = Probe->SolverPosition;
	// THE UNITS ARE NAMED. A bare triple in a tool that also deals in Unreal
	// centimetres is ambiguous, and the two differ by the case's length scale.
	return FText::Format(
		LOCTEXT("ProbePosition", "{0}, {1}, {2} (solver units)"),
		FlowVizProbePanelLocal::FormatValue(P.X), FlowVizProbePanelLocal::FormatValue(P.Y),
		FlowVizProbePanelLocal::FormatValue(P.Z));
}

FText SFlowVizProbePanel::GetProbeNameText(FGuid Id) const
{
	if (!IsBound())
	{
		return FText::GetEmpty();
	}
	const FFlowVizProbe* Probe = ViewModel->FindProbe(Id);
	return Probe != nullptr ? FText::FromString(Probe->Name) : FText::GetEmpty();
}

FText SFlowVizProbePanel::GetProbeVisibleGlyph(FGuid Id) const
{
	if (!IsBound())
	{
		return LOCTEXT("VisibleUnknownGlyph", "—");
	}
	const FFlowVizProbe* Probe = ViewModel->FindProbe(Id);
	if (Probe == nullptr)
	{
		return LOCTEXT("VisibleUnknownGlyph2", "—");
	}
	return Probe->bVisible ? LOCTEXT("VisibleOnGlyph", "◉") : LOCTEXT("VisibleOffGlyph", "○");
}

FText SFlowVizProbePanel::GetLineSamplesText() const
{
	if (!IsBound())
	{
		return FText::GetEmpty();
	}
	return FText::AsNumber(ViewModel->GetLineSampleCount());
}

FText SFlowVizProbePanel::GetLineSummaryText() const
{
	if (!IsBound())
	{
		return FText::GetEmpty();
	}
	if (!ViewModel->HasLineProbe())
	{
		// SAYS SO EXPLICITLY. "Set" pressed on a zero-length line is refused by the
		// view model; without this the refusal would leave no trace on screen.
		return LOCTEXT("NoLine", "No line defined. The endpoints must differ.");
	}
	const double Length = (ViewModel->GetLineEnd() - ViewModel->GetLineStart()).Length();
	return FText::Format(
		LOCTEXT("LineSummary", "Line of {0} solver units, {1} samples. Nothing plots it yet."),
		FlowVizProbePanelLocal::FormatValue(Length),
		FText::AsNumber(ViewModel->GetLineSampleCount()));
}

#undef LOCTEXT_NAMESPACE
