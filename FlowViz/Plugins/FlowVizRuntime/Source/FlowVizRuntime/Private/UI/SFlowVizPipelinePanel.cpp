// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizPipelinePanel.h"

#include "CFDViz/CFDVizManifest.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "UI/FlowVizWorkspaceStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FlowVizPipelinePanel"

void SFlowVizPipelinePanel::Construct(const FArguments& InArgs)
{
	Model = InArgs._Model;
	OnFieldChanged = InArgs._OnFieldChanged;

	const float U = FlowVizWorkspaceStyle::GetUnit();

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 1.0f * U))
		[
			SNew(STextBlock)
				// BOUND: the label follows the case without a refresh call --
				// only the ROWS need explicit rebuilding.
				.Text(TAttribute<FText>::CreateSP(this, &SFlowVizPipelinePanel::GetCaseLabel))
				.Font(FlowVizWorkspaceStyle::GetLabelFont())
				.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextPrimaryColor()))
		]

		+ SVerticalBox::Slot()
			.AutoHeight()
		[
			SAssignNew(RowsBox, SVerticalBox)
		]
	];

	RefreshFields();
}

void SFlowVizPipelinePanel::RefreshFields()
{
	FieldButtons.Reset();
	FieldIds.Reset();
	if (!RowsBox.IsValid())
	{
		return;
	}
	RowsBox->ClearChildren();

	if (Model == nullptr || !Model->IsCaseOpen())
	{
		return;
	}

	const float U = FlowVizWorkspaceStyle::GetUnit();
	FieldIds = Model->GetVolumeFieldIds();

	for (const FName FieldId : FieldIds)
	{
		TSharedPtr<SButton> Row;
		SAssignNew(Row, SButton)
			.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
			.OnClicked(
				FOnClicked::CreateSP(this, &SFlowVizPipelinePanel::OnFieldClicked, FieldId))
			.ToolTipText(LOCTEXT("FieldRowTip",
				"Display this field. Switching re-opens the case: playback resets and the "
				"frame cache is dropped."))
			.HAlign(HAlign_Left)
			.ContentPadding(FMargin(1.5f * U, 0.5f * U))
			[
				SNew(STextBlock)
					.Text(FText::FromName(FieldId))
					.Font(FlowVizWorkspaceStyle::GetCaptionFont())
					.ColorAndOpacity(FSlateColor::UseForeground())
			];

		FieldButtons.Add(Row);
		RowsBox->AddSlot()
			.AutoHeight()
			.Padding(FMargin(1.0f * U, 0.0f, 0.0f, 0.25f * U))
		[
			Row.ToSharedRef()
		];
	}
}

TSharedPtr<SButton> SFlowVizPipelinePanel::GetFieldButton(int32 Index) const
{
	return FieldButtons.IsValidIndex(Index) ? FieldButtons[Index] : nullptr;
}

FReply SFlowVizPipelinePanel::OnFieldClicked(FName FieldId)
{
	if (Model == nullptr || !Model->IsCaseOpen())
	{
		return FReply::Handled();
	}

	// RE-CLICKING THE DISPLAYED FIELD IS A NO-OP. SetField re-opens the case --
	// playback resets, the cache drops -- and nobody asked for that by clicking
	// the row that is already showing.
	if (Model->TransferFunction.GetFieldId() == FieldId)
	{
		return FReply::Handled();
	}

	if (Model->SetField(FieldId).IsOk())
	{
		OnFieldChanged.ExecuteIfBound();
	}
	return FReply::Handled();
}

FText SFlowVizPipelinePanel::GetCaseLabel() const
{
	if (Model == nullptr || !Model->IsCaseOpen())
	{
		return LOCTEXT("NoCase", "No case open");
	}
	const FCFDVizCase* Case = Model->GetCase();
	return Case != nullptr ? FText::FromString(Case->Metadata.Name)
						   : LOCTEXT("NoCase", "No case open");
}

#undef LOCTEXT_NAMESPACE
