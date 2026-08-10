// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizPipelinePanel.h"

#include "CFDViz/CFDVizManifest.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "UI/FlowVizWorkspaceStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#if FLOWVIZ_WITH_FILE_DIALOG
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#endif

#define LOCTEXT_NAMESPACE "FlowVizPipelinePanel"

void SFlowVizPipelinePanel::Construct(const FArguments& InArgs)
{
	Model = InArgs._Model;
	OnFieldChanged = InArgs._OnFieldChanged;
	OnOpenCaseRequested = InArgs._OnOpenCaseRequested;

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

		/*
		 * THE MODE TOGGLES (renderer overhaul P6): a row of independent
		 * switches, FluidX3D's interaction model. They compose freely because
		 * everything except the volume is opaque and z-writing -- the P6
		 * compositing rules are what make this row honest.
		 */
		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 1.0f * U))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0, 0, 0.5f * U, 0))
			[
				SAssignNew(ModeToggleButtons[0], SButton)
					.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
					.IsEnabled(Model != nullptr)
					.OnClicked(FOnClicked::CreateSP(this, &SFlowVizPipelinePanel::OnModeToggleClicked, 0))
					.ToolTipText(LOCTEXT("ObstacleTip", "Show or hide the obstacle surface"))
					.ContentPadding(FMargin(1.0f * U, 0.5f * U))
				[
					SNew(STextBlock).Text(LOCTEXT("ObstacleMode", "Obstacle"))
						.Font(FlowVizWorkspaceStyle::GetCaptionFont())
						.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0, 0, 0.5f * U, 0))
			[
				SAssignNew(ModeToggleButtons[1], SButton)
					.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
					.IsEnabled(Model != nullptr)
					.OnClicked(FOnClicked::CreateSP(this, &SFlowVizPipelinePanel::OnModeToggleClicked, 1))
					.ToolTipText(LOCTEXT("CutPlaneTip", "Show or hide the cut plane"))
					.ContentPadding(FMargin(1.0f * U, 0.5f * U))
				[
					SNew(STextBlock).Text(LOCTEXT("CutPlaneMode", "Cut plane"))
						.Font(FlowVizWorkspaceStyle::GetCaptionFont())
						.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0, 0, 0.5f * U, 0))
			[
				SAssignNew(ModeToggleButtons[2], SButton)
					.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
					.IsEnabled(Model != nullptr)
					.OnClicked(FOnClicked::CreateSP(this, &SFlowVizPipelinePanel::OnModeToggleClicked, 2))
					.ToolTipText(LOCTEXT("IsoTip", "Show or hide the Q iso-surface"))
					.ContentPadding(FMargin(1.0f * U, 0.5f * U))
				[
					SNew(STextBlock).Text(LOCTEXT("IsoMode", "Iso"))
						.Font(FlowVizWorkspaceStyle::GetCaptionFont())
						.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0, 0, 0.5f * U, 0))
			[
				SAssignNew(ModeToggleButtons[3], SButton)
					.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
					.IsEnabled(Model != nullptr)
					.OnClicked(FOnClicked::CreateSP(this, &SFlowVizPipelinePanel::OnModeToggleClicked, 3))
					.ToolTipText(LOCTEXT("StreamTip", "Show or hide streamlines"))
					.ContentPadding(FMargin(1.0f * U, 0.5f * U))
				[
					SNew(STextBlock).Text(LOCTEXT("StreamMode", "Streamlines"))
						.Font(FlowVizWorkspaceStyle::GetCaptionFont())
						.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(0, 0, 0.5f * U, 0))
			[
				SAssignNew(ModeToggleButtons[4], SButton)
					.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
					.IsEnabled(Model != nullptr)
					.OnClicked(FOnClicked::CreateSP(this, &SFlowVizPipelinePanel::OnModeToggleClicked, 4))
					.ToolTipText(LOCTEXT("ParticlesTip",
						"Show or hide tracer particles advected through the flow"))
					.ContentPadding(FMargin(1.0f * U, 0.5f * U))
				[
					SNew(STextBlock).Text(LOCTEXT("ParticlesMode", "Particles"))
						.Font(FlowVizWorkspaceStyle::GetCaptionFont())
						.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SAssignNew(ModeToggleButtons[5], SButton)
					.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
					.IsEnabled(Model != nullptr)
					.OnClicked(FOnClicked::CreateSP(this, &SFlowVizPipelinePanel::OnModeToggleClicked, 5))
					.ToolTipText(LOCTEXT("VolumeTip",
						"Show or hide the volume render (off by default -- surfaces are "
						"the picture; the volume is the optional fog)"))
					.ContentPadding(FMargin(1.0f * U, 0.5f * U))
				[
					SNew(STextBlock).Text(LOCTEXT("VolumeMode", "Volume"))
						.Font(FlowVizWorkspaceStyle::GetCaptionFont())
						.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		]

		+ SVerticalBox::Slot()
			.AutoHeight()
		[
			SAssignNew(RowsBox, SVerticalBox)
		]

		/*
		 * OPEN A CASE, from the UI (the gap a user found: the packaged app
		 * offered no way to load anything without the console). The panel only
		 * ANNOUNCES the path; opening stays the workspace's job.
		 */
		+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 1.5f * U, 0.0f, 0.0f))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(FMargin(0.0f, 0.0f, 0.5f * U, 0.0f))
			[
				SAssignNew(OpenPathBox, SEditableTextBox)
					.HintText(LOCTEXT("OpenPathHint", "path/to/case.cfdviz"))
					.Font(FlowVizWorkspaceStyle::GetCaptionFont())
					// Enter in the box == clicking Open: the box is where the
					// user's attention already is.
					.OnTextCommitted_Lambda(
						[this](const FText&, ETextCommit::Type Commit)
						{
							if (Commit == ETextCommit::OnEnter)
							{
								OnOpenClicked();
							}
						})
			]

#if FLOWVIZ_WITH_FILE_DIALOG
			+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(FMargin(0.0f, 0.0f, 0.5f * U, 0.0f))
			[
				SNew(SButton)
					.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
					.OnClicked(FOnClicked::CreateSP(
						this, &SFlowVizPipelinePanel::OnBrowseClicked))
					.ToolTipText(LOCTEXT("BrowseTip",
						"Pick a .cfdviz case directory"))
					.ContentPadding(FMargin(1.5f * U, 0.5f * U))
				[
					SNew(STextBlock)
						.Text(LOCTEXT("BrowseLabel", "Browse..."))
						.Font(FlowVizWorkspaceStyle::GetCaptionFont())
						.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
#endif

			+ SHorizontalBox::Slot()
				.AutoWidth()
			[
				SAssignNew(OpenButton, SButton)
					.ButtonStyle(&FlowVizWorkspaceStyle::GetToolButtonStyle())
					.OnClicked(FOnClicked::CreateSP(
						this, &SFlowVizPipelinePanel::OnOpenClicked))
					.ToolTipText(LOCTEXT("OpenTip",
						"Open the case at this path"))
					.ContentPadding(FMargin(1.5f * U, 0.5f * U))
				[
					SNew(STextBlock)
						.Text(LOCTEXT("OpenLabel", "Open"))
						.Font(FlowVizWorkspaceStyle::GetCaptionFont())
						.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		]
	];

	RefreshModeToggleStyles();
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

TSharedPtr<SButton> SFlowVizPipelinePanel::GetModeToggleButton(int32 Index) const
{
	return (Index >= 0 && Index < ModeToggleCount) ? ModeToggleButtons[Index] : nullptr;
}

bool SFlowVizPipelinePanel::IsModeToggleActive(int32 ModeIndex) const
{
	if (Model == nullptr)
	{
		return false;
	}
	switch (ModeIndex)
	{
	case 0: return Model->IsObstacleVisible();
	case 1: return Model->Slice.IsVisible();
	case 2: return Model->IsIsoSurfaceEnabled();
	case 3: return Model->AreStreamlinesEnabled();
	case 4: return Model->AreParticlesEnabled();
	case 5: return Model->IsVolumeVisible();
	default: return false;
	}
}

void SFlowVizPipelinePanel::RefreshModeToggleStyles()
{
	for (int32 Index = 0; Index < ModeToggleCount; ++Index)
	{
		if (ModeToggleButtons[Index].IsValid())
		{
			ModeToggleButtons[Index]->SetButtonStyle(IsModeToggleActive(Index)
				? &FlowVizWorkspaceStyle::GetSelectedToolButtonStyle()
				: &FlowVizWorkspaceStyle::GetToolButtonStyle());
		}
	}
}

FReply SFlowVizPipelinePanel::OnModeToggleClicked(int32 ModeIndex)
{
	if (Model == nullptr)
	{
		return FReply::Handled();
	}
	switch (ModeIndex)
	{
	case 0: Model->SetObstacleVisible(!Model->IsObstacleVisible()); break;
	case 1: Model->Slice.SetVisible(!Model->Slice.IsVisible()); break;
	case 2: Model->SetIsoSurfaceEnabled(!Model->IsIsoSurfaceEnabled()); break;
	case 3: Model->SetStreamlinesEnabled(!Model->AreStreamlinesEnabled()); break;
	case 4: Model->SetParticlesEnabled(!Model->AreParticlesEnabled()); break;
	case 5: Model->SetVolumeVisible(!Model->IsVolumeVisible()); break;
	default: return FReply::Handled();
	}
	RefreshModeToggleStyles();
	// One announcement channel: the workspace pushes, which applies component
	// visibility and (for the slice) recomposes the clip.
	OnFieldChanged.ExecuteIfBound();
	return FReply::Handled();
}

FReply SFlowVizPipelinePanel::OnOpenClicked()
{
	const FString Path =
		OpenPathBox.IsValid() ? OpenPathBox->GetText().ToString().TrimStartAndEnd() : FString();

	// An empty commit is a stray Enter, not a request to open nothing.
	if (!Path.IsEmpty())
	{
		OnOpenCaseRequested.ExecuteIfBound(Path);
	}
	return FReply::Handled();
}

FReply SFlowVizPipelinePanel::OnBrowseClicked()
{
#if FLOWVIZ_WITH_FILE_DIALOG
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (DesktopPlatform != nullptr)
	{
		const void* ParentWindow =
			FSlateApplication::Get().FindBestParentWindowHandleForDialogs(AsShared());
		FString PickedDir;
		// A case is a DIRECTORY (manifest.json + frames/), so a folder picker,
		// not a file picker that would stop at the manifest.
		if (DesktopPlatform->OpenDirectoryDialog(const_cast<void*>(ParentWindow),
				TEXT("Open a CFDViz case (.cfdviz directory)"), FString(), PickedDir)
			&& !PickedDir.IsEmpty())
		{
			if (OpenPathBox.IsValid())
			{
				OpenPathBox->SetText(FText::FromString(PickedDir));
			}
			OnOpenCaseRequested.ExecuteIfBound(PickedDir);
		}
	}
#endif
	return FReply::Handled();
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
