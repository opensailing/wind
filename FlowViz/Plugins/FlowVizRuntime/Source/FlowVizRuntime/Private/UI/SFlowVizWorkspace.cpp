// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizWorkspace.h"

#include "Scene/FlowVizVolumeComponent.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "UI/FlowVizWorkspaceStyle.h"
#include "UI/SFlowVizClipPanel.h"
#include "UI/SFlowVizProbePanel.h"
#include "UI/SFlowVizSlicePanel.h"
#include "UI/SFlowVizTransferFunctionPanel.h"
#include "UI/SFlowVizTransportBar.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FlowVizWorkspace"

// NAMED namespace: unity build. See the note on every other UI .cpp here.
namespace FlowVizWorkspaceLocal
{
	/** Width of the docked side panel as a fraction of the workspace. */
	constexpr float SidePanelFraction = 0.26f;

	/**
	 * A panel with a heading.
	 *
	 * The heading is part of the panel rather than a separate slot because a
	 * heading that can drift away from the content it names is worse than none.
	 */
	TSharedRef<SWidget> MakeSection(const FText& Heading, TSharedRef<SWidget> Content)
	{
		const float U = FlowVizWorkspaceStyle::GetUnit();

		return SNew(SVerticalBox)

			+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 0.0f, 0.0f, 1.0f * U))
			[
				SNew(STextBlock)
					.Text(Heading)
					.Font(FlowVizWorkspaceStyle::GetHeadingFont())
					.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextPrimaryColor()))
			]

			+ SVerticalBox::Slot()
				.AutoHeight()
			[
				Content
			];
	}

	/**
	 * A region that is honest about not being built yet.
	 *
	 * WHY A LABELLED PLACEHOLDER RATHER THAN NOTHING, AND RATHER THAN A MOCK. An
	 * empty area reads as a panel that failed to load. A mock - sliders that move
	 * and do nothing - is a rule 15 violation and worse, because a control that
	 * responds is more convincing than one that is merely present. Saying what is
	 * missing is the only option that cannot mislead.
	 */
	TSharedRef<SWidget> MakePendingRegion(const FText& What)
	{
		const float U = FlowVizWorkspaceStyle::GetUnit();

		return SNew(SBorder)
			.BorderImage(FlowVizWorkspaceStyle::GetRaisedBrush())
			.Padding(FMargin(2.0f * U))
			.HAlign(HAlign_Center)
		[
			SNew(STextBlock)
				.Text(What)
				.Font(FlowVizWorkspaceStyle::GetCaptionFont())
				.ColorAndOpacity(FSlateColor(FlowVizWorkspaceStyle::GetTextDisabledColor()))
				.AutoWrapText(true)
				.Justification(ETextJustify::Center)
		];
	}
}

SFlowVizWorkspace::SFlowVizWorkspace() = default;

/*
 * Out-of-line, and it must stay that way: TUniquePtr<FFlowVizWorkspaceModel>
 * needs the complete type to destroy, and the header only forward-declares it.
 */
SFlowVizWorkspace::~SFlowVizWorkspace() = default;

FFlowVizWorkspaceModel& SFlowVizWorkspace::GetModel() const
{
	check(Model.IsValid());
	return *Model;
}

void SFlowVizWorkspace::Construct(const FArguments& InArgs)
{
	// ONE MODEL, SHARED BY EVERY PANEL. Each panel taking its own would give the
	// transport bar and the transfer-function editor different ideas of which
	// case is open - and they would each look correct in isolation.
	Model = MakeUnique<FFlowVizWorkspaceModel>();

	const float U = FlowVizWorkspaceStyle::GetUnit();

	ChildSlot
	[
		SNew(SBorder)
			.BorderImage(FlowVizWorkspaceStyle::GetFlatBrush())
			.BorderBackgroundColor(FlowVizWorkspaceStyle::GetBackgroundColor())
			.Padding(FMargin(0.0f))
		[
			SNew(SVerticalBox)

			/* --- Viewport row: the volume, and the docked side panel ------- */

			+ SVerticalBox::Slot()
				.FillHeight(1.0f)
			[
				// A SPLITTER so the side panel is resizable, per plan.md 11.
				SNew(SSplitter)
					.Orientation(Orient_Horizontal)
					.PhysicalSplitterHandleSize(2.0f)

				+ SSplitter::Slot()
					.Value(1.0f - FlowVizWorkspaceLocal::SidePanelFraction)
				[
					FlowVizWorkspaceLocal::MakePendingRegion(
						LOCTEXT("ViewportPending",
							"The volume renders in the level viewport.\n"
							"An embedded view is not wired into this panel yet."))
				]

				+ SSplitter::Slot()
					.Value(FlowVizWorkspaceLocal::SidePanelFraction)
				[
					SNew(SBorder)
						.BorderImage(FlowVizWorkspaceStyle::GetFlatBrush())
						.BorderBackgroundColor(FlowVizWorkspaceStyle::GetPanelColor())
						.Padding(FMargin(2.0f * U))
					[
						// SCROLLED, because the side panel is taller than a laptop
						// display once every section is open. Controls that fall off
						// the bottom of a fixed column are controls that do not exist.
						SNew(SScrollBox)

						+ SScrollBox::Slot()
							.Padding(FMargin(0.0f, 0.0f, 0.0f, 2.0f * U))
						[
							FlowVizWorkspaceLocal::MakeSection(
								LOCTEXT("TransferFunctionHeading", "Color & Opacity"),
								SAssignNew(TransferFunctionPanel, SFlowVizTransferFunctionPanel)
									.ViewModel(&Model->TransferFunction))
						]

						+ SScrollBox::Slot()
							.Padding(FMargin(0.0f, 0.0f, 0.0f, 2.0f * U))
						[
							FlowVizWorkspaceLocal::MakeSection(
								LOCTEXT("ClipHeading", "Clipping"),
								SAssignNew(ClipPanel, SFlowVizClipPanel)
									.ViewModel(&Model->Clip)
									// THE CHANNEL TO THE RENDERER, subscribed here
									// and nowhere else. Without this line the panel
									// edits a model nothing reads - which is the
									// state FlowViz.UI.Workspace.VolumeBinding
									// exists to fail on.
									.OnClipChanged(FSimpleDelegate::CreateSP(
										this, &SFlowVizWorkspace::HandleClipChanged)))
						]

						+ SScrollBox::Slot()
							.Padding(FMargin(0.0f, 0.0f, 0.0f, 2.0f * U))
						[
							FlowVizWorkspaceLocal::MakeSection(
								LOCTEXT("SliceHeading", "Slice"),
								SAssignNew(SlicePanel, SFlowVizSlicePanel)
									.ViewModel(&Model->Slice))
						]

						+ SScrollBox::Slot()
						[
							FlowVizWorkspaceLocal::MakeSection(
								LOCTEXT("ProbeHeading", "Probes"),
								SAssignNew(ProbePanel, SFlowVizProbePanel)
									.ViewModel(&Model->Probes))
						]
					]
				]
			]

			/* --- Transport bar, across the full width ---------------------- */

			+ SVerticalBox::Slot()
				.AutoHeight()
			[
				SAssignNew(TransportBar, SFlowVizTransportBar)
					.TimelineViewModel(&Model->Timeline)
			]
		]
	];
}

/* ========================================================================== */
/* The renderer this workspace drives                                          */
/* ========================================================================== */

void SFlowVizWorkspace::SetVolume(UCFDVizVolumeComponent* InVolume)
{
	Volume = InVolume;

	// THE PANEL'S DISCLOSURE FOLLOWS THE BINDING. Left unsaid, the clip panel
	// would keep telling users their controls do nothing after they started
	// working - the stale-advisory failure, which is worse than the original
	// because it sends someone away from a control that is now live.
	if (ClipPanel.IsValid())
	{
		ClipPanel->SetVolumeBound(InVolume != nullptr);
	}

	// PUSHED NOW, not on the next edit. A workspace with planes already authored
	// - from a session load, or from a case opened before the actor existed -
	// would otherwise render unclipped until the user touched something, which
	// looks like a control that needs wiggling.
	PushToVolume();
}

UCFDVizVolumeComponent* SFlowVizWorkspace::GetVolume() const
{
	// Get() on a weak pointer, so a component whose world was torn down reads as
	// null rather than as a live pointer into freed memory.
	return Volume.Get();
}

bool SFlowVizWorkspace::PushToVolume()
{
	// PushClipToVolume refuses a null component itself, so this could pass
	// Volume.Get() straight through. It is written out because the two null
	// cases mean different things and one of them is about to grow siblings:
	// "no volume bound" is a workspace state, "no case in the volume" is the
	// component's.
	UCFDVizVolumeComponent* Bound = Volume.Get();
	if (Bound == nullptr)
	{
		return false;
	}

	return FFlowVizWorkspaceModel::PushClipToVolume(Model->Clip, Bound);
}

void SFlowVizWorkspace::HandleClipChanged()
{
	// The return is deliberately dropped. A panel edit with no case open is
	// normal, not an error, and the clip panel's advisory already says the edits
	// are not reaching a renderer - reporting it twice would put a warning in the
	// log for every click during ordinary setup.
	PushToVolume();
}

#undef LOCTEXT_NAMESPACE
