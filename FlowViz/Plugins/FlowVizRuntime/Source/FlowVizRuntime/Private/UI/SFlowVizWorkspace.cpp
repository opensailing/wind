// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizWorkspace.h"

#include "FlowVizRuntime.h"
#include "Playback/FlowVizCasePlayer.h"
#include "Scene/FlowVizVolumeComponent.h"
#include "UI/FlowVizSession.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "UI/FlowVizWorkspaceRegistry.h"
#include "UI/FlowVizWorkspaceStyle.h"
#include "UI/SFlowVizClipPanel.h"
#include "UI/SFlowVizDiagnosticsOverlay.h"
#include "UI/SFlowVizPipelinePanel.h"
#include "UI/SFlowVizProbePanel.h"
#include "UI/SFlowVizRenderSettingsPanel.h"
#include "UI/SFlowVizSlicePanel.h"
#include "UI/SFlowVizTransferFunctionPanel.h"
#include "UI/SFlowVizTransportBar.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
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
SFlowVizWorkspace::~SFlowVizWorkspace()
{
	/*
	 * OUT OF THE REGISTRY FIRST, so nothing can resolve this workspace as a
	 * console command's target while the members below are being torn down.
	 *
	 * The weak entry would go null on its own once the shared reference
	 * controller released -- but that happens AFTER this body, so a command
	 * executing between here and then would pin a workspace whose player has
	 * already dropped its texture set. Removing it explicitly closes that
	 * window instead of relying on the compaction pass to notice afterwards.
	 */
	FlowVizWorkspaceRegistry::Unregister(*this);

	/*
	 * THE CLOCK GOES SECOND, AND IT MUST PRECEDE THE MODEL.
	 *
	 * TickClock captures `this` and reads Model, which is destroyed immediately
	 * after this body. FTSTicker::RemoveTicker is documented to block until an
	 * in-progress execution of the handler finishes, so after this line no
	 * further call can be in flight -- which is the guarantee that makes the
	 * capture safe rather than merely usually-safe.
	 *
	 * Unconditional: RemoveTicker on a default-constructed (or already removed)
	 * handle is a no-op, so there is no "was it registered" state to track and
	 * no branch here to get wrong.
	 */
	FTSTicker::RemoveTicker(ClockHandle);

	/*
	 * NOT `= default`, AND THIS IS THE TEARDOWN THAT ACTUALLY HAPPENS.
	 *
	 * SetVolume(nullptr) releases the frame source on an explicit unbind, but
	 * nothing in the codebase calls it that way. The real end of a workspace is
	 * its last reference going away with a volume still bound -- close the tab
	 * while the level keeps rendering -- and that runs this destructor instead.
	 *
	 * The source handed to the component holds Model->Player by RAW REFERENCE
	 * (FlowVizCaseSeam.cpp). Model is destroyed immediately after this body, so a
	 * component still holding the source would read into freed memory on its next
	 * tick. Releasing it here drops the component to its documented no-source
	 * policy, which is a defensible image rather than a crash.
	 *
	 * Volume is weak, so a component whose world died first is already null and
	 * there is nothing to release -- the case this ordering has to survive is the
	 * OTHER one, where the component outlives the widget.
	 */
	if (UCFDVizVolumeComponent* Bound = Volume.Get())
	{
		Bound->SetFrameSource(nullptr);
	}

	/*
	 * THE PLAYER'S BORROWED TEXTURE SET, released UNCONDITIONALLY and NOT inside
	 * the branch above.
	 *
	 * Volume is weak, so the case where the component died first reads null
	 * there -- and that is exactly the case where the player is still holding a
	 * pointer to it. Putting this release inside the `if` would skip it in the
	 * one situation it exists for.
	 *
	 * Strictly, Model is destroyed on the next line and the player goes with it,
	 * so nothing can dereference the stale pointer afterwards. It is cleared
	 * anyway because the ORDER of members in this class is what makes that true,
	 * and a release that depends on declaration order is one reordering away
	 * from being a use-after-free with no comment to warn the person doing it.
	 */
	if (Model.IsValid())
	{
		Model->Player.SetTextureSet(nullptr);
	}
}

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

	/*
	 * ANNOUNCE THIS WORKSPACE, and do it HERE rather than in the constructor.
	 *
	 * The registry derives a weak pointer via AsShared(), which is only legal
	 * once a TSharedRef owns the widget -- SNew constructs, wraps, then calls
	 * Construct, so this is the earliest legal point. It is also the correct
	 * one: after Model exists, so a command that resolves this workspace the
	 * instant it appears finds a usable one rather than a half-built one.
	 *
	 * WITHOUT THIS LINE the eight FlowViz.* console commands are registered,
	 * discoverable, documented and inert -- they would resolve no target and
	 * report "no workspace" forever, which reads to a user as "the UI is not
	 * open" rather than as a wiring gap. FlowViz.UI.Console.Wiring asks the
	 * console manager what startup registered; FlowViz.UI.Console.Target is the
	 * one that fails when this line is missing.
	 */
	FlowVizWorkspaceRegistry::Register(*this);

	/*
	 * THE CLOCK, REGISTERED BEFORE ANY PANEL EXISTS. See the header for why this
	 * is an engine ticker rather than an SCompoundWidget::Tick override, and why
	 * the handle is per-instance.
	 *
	 * Delay 0 means "every frame" rather than "once": the handler returns true,
	 * which re-arms it at CurrentTime + 0. A non-zero delay here would quantise
	 * playback to that interval no matter what the player's own frame rate
	 * settings said.
	 *
	 * Capturing `this` raw is safe only because the destructor removes the
	 * handle and RemoveTicker blocks on an in-flight call; nothing else about
	 * this widget's lifetime guarantees it.
	 */
	ClockHandle = FTSTicker::GetCoreTicker().AddTicker(
		TEXT("FlowVizWorkspaceClock"), 0.0f,
		[this](float DeltaSeconds) { return TickClock(DeltaSeconds); });

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
					/*
					 * THE DIAGNOSTICS OVERLAY SITS ON THE VIEWPORT REGION, which
					 * is where plan.md section 17 wants it and the only place it
					 * makes sense: the numbers describe the picture, so reading
					 * them anywhere else means looking away from the thing they
					 * are about.
					 */
					SNew(SOverlay)

					+ SOverlay::Slot()
					[
						FlowVizWorkspaceLocal::MakePendingRegion(
							LOCTEXT("ViewportPending",
								"The volume renders in the level viewport.\n"
								"An embedded view is not wired into this panel yet."))
					]

					+ SOverlay::Slot()
						// TOP-LEFT, the convention every engine stat overlay uses,
						// and away from the transport bar along the bottom.
						.HAlign(HAlign_Left)
						.VAlign(VAlign_Top)
						.Padding(FMargin(2.0f * U))
					[
						SAssignNew(DiagnosticsOverlayContainer, SBox)
							/*
							 * COLLAPSED, NOT HIDDEN. Hidden still costs layout;
							 * collapsed costs nothing, and this widget's text
							 * attribute re-collects every paint. The container
							 * carries the state rather than the overlay itself --
							 * see the header for why toggling the overlay's own
							 * visibility would clobber its HitTestInvisible.
							 */
							.Visibility(EVisibility::Collapsed)
						[
							SAssignNew(DiagnosticsOverlay, SFlowVizDiagnosticsOverlay)
								// THE WORKSPACE'S OWN MODEL. An overlay handed its
								// own would report numbers about a session the user
								// has never seen, which is worse than reporting
								// nothing because it looks like an answer.
								.Model(Model.Get())
						]
					]
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
								LOCTEXT("PipelineHeading", "Pipeline"),
								SAssignNew(PipelinePanel, SFlowVizPipelinePanel)
									.Model(Model.Get())
									// A field switch re-binds the transfer
									// function and resets playback; the re-push
									// is what makes the render follow (#83).
									.OnFieldChanged(FSimpleDelegate::CreateSP(
										this, &SFlowVizWorkspace::HandleFieldChanged)))
						]

						+ SScrollBox::Slot()
							.Padding(FMargin(0.0f, 0.0f, 0.0f, 2.0f * U))
						[
							FlowVizWorkspaceLocal::MakeSection(
								LOCTEXT("TransferFunctionHeading", "Color & Opacity"),
								SAssignNew(TransferFunctionPanel, SFlowVizTransferFunctionPanel)
									.ViewModel(&Model->TransferFunction)
									// THE CHANNEL TO THE RENDERER for colour, and
									// it did not exist at all until #48: this
									// panel had no delegate of any kind, so every
									// colormap, range and opacity control edited a
									// model nothing downstream read.
									.OnTransferFunctionChanged(FSimpleDelegate::CreateSP(
										this, &SFlowVizWorkspace::HandleTransferFunctionChanged)))
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
								LOCTEXT("RenderSettingsHeading", "Render"),
								SAssignNew(RenderSettingsPanel, SFlowVizRenderSettingsPanel)
									.ViewModel(&Model->RenderSettings)
									// THE CHANNEL TO THE RENDERER for mode,
									// lighting, marching and sampling. Until #74
									// this panel did not exist: the view model
									// had 14 setters with no production caller,
									// so the 16 parameters 2e names were still
									// welded -- to the view model's defaults
									// instead of FillDefaults'.
									.OnRenderSettingsChanged(FSimpleDelegate::CreateSP(
										this, &SFlowVizWorkspace::HandleRenderSettingsChanged)))
						]

						+ SScrollBox::Slot()
							.Padding(FMargin(0.0f, 0.0f, 0.0f, 2.0f * U))
						[
							FlowVizWorkspaceLocal::MakeSection(
								LOCTEXT("SliceHeading", "Slice"),
								SAssignNew(SlicePanel, SFlowVizSlicePanel)
									.ViewModel(&Model->Slice)
									// THE CHANNEL TO THE RENDERER (#77): the
									// slice composes into the pushed clip, so a
									// slice edit re-pushes the same channel a
									// clip edit does.
									.OnSliceChanged(FSimpleDelegate::CreateSP(
										this, &SFlowVizWorkspace::HandleSliceChanged)))
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
	/*
	 * THE OLD VOLUME IS RELEASED BEFORE THE NEW ONE IS BOUND, and this is the
	 * unbind path as well as the rebind path -- SetVolume(nullptr) runs exactly
	 * this branch.
	 *
	 * The frame source handed out below holds this workspace's player by RAW
	 * REFERENCE (FlowVizCaseSeam.cpp). Nothing about the volume's lifetime is
	 * tied to the workspace's: the component lives in the world and the player
	 * lives in Model, so closing the tab while the level keeps rendering is the
	 * ordinary case, not the exotic one. A component left holding the source
	 * would read through that reference on its next tick, after the player it
	 * names has been destroyed.
	 *
	 * Clearing it on the way out means the component falls back to its
	 * documented no-source policy (hold frame 0) rather than reading freed
	 * memory -- a defensible thing to see, and the reason GetFrameSelection has
	 * that fallback at all.
	 */
	if (UCFDVizVolumeComponent* Previous = Volume.Get())
	{
		if (Previous != InVolume)
		{
			Previous->SetFrameSource(nullptr);

			/*
			 * AND THE REVERSE POINTER, which is the dangerous one.
			 *
			 * The frame source above is held BY the component, so a component
			 * that dies takes it along. The texture set is held BY THE PLAYER,
			 * as a raw pointer into a UObject this widget only tracks weakly --
			 * so the player is the survivor holding a reference to the corpse.
			 * Releasing it here is what keeps DrainCompletedLoads from
			 * enqueuing a decoded frame into a component that has moved on.
			 */
			Model->Player.SetTextureSet(nullptr);
		}
	}

	Volume = InVolume;

	// THE PANEL'S DISCLOSURE FOLLOWS THE BINDING. Left unsaid, the clip panel
	// would keep telling users their controls do nothing after they started
	// working - the stale-advisory failure, which is worse than the original
	// because it sends someone away from a control that is now live.
	if (ClipPanel.IsValid())
	{
		ClipPanel->SetVolumeBound(InVolume != nullptr);
	}

	/*
	 * AND THE OVERLAY'S RESOLUTION ROW, which is the one diagnostic that lives
	 * on the component rather than on the model. Without this the row reports
	 * zeros forever - and FlowVizDiagnostics documents zeros there as "no upload
	 * has happened", so an overlay that never learned about the volume makes a
	 * true-sounding claim about a volume that is uploading fine.
	 */
	if (DiagnosticsOverlay.IsValid())
	{
		DiagnosticsOverlay->SetVolume(InVolume);
	}

	/*
	 * THE TIMELINE'S CHANNEL TO THE RENDERER, and it is a LIVE READ rather than
	 * a copy of the current selection.
	 *
	 * The source reads the player every time the component asks, so a scrub that
	 * happens long after this call still moves the image. Pushing a selection
	 * here instead would bind the frame the playhead happened to be on, and the
	 * transport bar would then drive a value nobody re-reads -- the same shape as
	 * the clip panel editing a model no renderer saw (#50), which is what this
	 * seam was found alongside.
	 *
	 * GetDisplay, not the desired selection: see FlowVizCaseSeam.cpp. What is
	 * complete and resident is what may be drawn.
	 */
	if (InVolume != nullptr)
	{
		InVolume->SetFrameSource(FlowVizPlayback::MakeFrameSource(Model->Player));

		/*
		 * AND THE CHANNEL THE VOXELS THEMSELVES TRAVEL, which is a separate
		 * wire from the one above and the one that puts pixels on screen.
		 *
		 * The frame source is a LABEL: it tells the component which frame to
		 * display. Without this line the player decodes frames on its workers,
		 * marks them complete, and drops them -- DrainCompletedLoads only
		 * uploads when it has a set, and it had none -- so the transport bar
		 * advances, the component agrees which frame is showing, and the
		 * textures behind that label are whatever was in them before. That is
		 * the state this whole plugin was in until #66: a scrub that moved a
		 * number and nothing else.
		 *
		 * BORROWED, NOT OWNED. The set belongs to the component (a TUniquePtr
		 * member); the player holds a bare pointer to it. Every path that can
		 * separate the two has to clear it -- the rebind above, the unbind that
		 * is the same branch, TickClock when a bound component is destroyed
		 * under us, and this widget's destructor.
		 */
		Model->Player.SetTextureSet(&InVolume->GetTextureSet());
	}
	else
	{
		// SetVolume(nullptr) took the release branch above only if something was
		// bound. Unconditional here so an unbind with nothing bound still leaves
		// the player detached rather than relying on it already being so.
		Model->Player.SetTextureSet(nullptr);
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

void SFlowVizWorkspace::SetDiagnosticsOverlayShown(bool bShown)
{
	bDiagnosticsOverlayShown = bShown;

	/*
	 * THE CONTAINER, NOT THE OVERLAY. The overlay sets itself HitTestInvisible
	 * in Construct so it cannot swallow clicks meant for the viewport behind it;
	 * writing Visible onto it here to show it would quietly undo that and make
	 * the volume unclickable the first time a user turned diagnostics on.
	 *
	 * COLLAPSED rather than Hidden: Hidden still participates in layout, and
	 * this widget is a multi-line text block sitting in a corner slot.
	 */
	if (DiagnosticsOverlayContainer.IsValid())
	{
		DiagnosticsOverlayContainer->SetVisibility(
			bShown ? EVisibility::SelfHitTestInvisible : EVisibility::Collapsed);
	}
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

	/*
	 * BOTH CHANNELS, AND BOTH ARE ATTEMPTED. Written as two statements rather
	 * than `A(...) && B(...)` on purpose: && short-circuits, so a clip push that
	 * returned false -- which happens routinely, whenever no case is open yet --
	 * would silently skip the transfer function, whose push has no such
	 * precondition and would have succeeded.
	 *
	 * The return is the AND of the two because the caller's question is "did the
	 * bound volume receive everything", and a partial push is not a yes.
	 */
	// COMPOSED, NOT RAW (#77): the pushed clip is the user's model plus the
	// visible slice's slab planes. The models the panels edit are untouched.
	const bool bClipPushed = FFlowVizWorkspaceModel::PushClipToVolume(
		FFlowVizWorkspaceModel::ComposeClipWithSlice(Model->Clip, Model->Slice), Bound);
	const bool bTransferFunctionPushed =
		FFlowVizWorkspaceModel::PushTransferFunctionToVolume(Model->TransferFunction, Bound);
	const bool bRenderSettingsPushed =
		FFlowVizWorkspaceModel::PushRenderSettingsToVolume(Model->RenderSettings, Bound);

	return bClipPushed && bTransferFunctionPushed && bRenderSettingsPushed;
}

/* ========================================================================== */
/* Sessions                                                                    */
/* ========================================================================== */

FCFDVizResult SFlowVizWorkspace::SaveSession(const FString& FilePath) const
{
	return Model->SaveSession(FilePath);
}

FCFDVizResult SFlowVizWorkspace::LoadSession(const FString& FilePath)
{
	FFlowVizSessionState State;
	const FCFDVizResult Read = FlowVizSession::LoadFromFile(FilePath, State);
	if (!Read.IsOk())
	{
		/*
		 * NOTHING WAS APPLIED, so nothing is pushed - and this early return is
		 * the ONE case where skipping the push is right.
		 *
		 * Distinct from a missing CASE, which LoadFromFile reports as Ok with
		 * bCaseFound false and which LoadState below pushes for. A read failure
		 * means there is no state at all: the models are untouched, so a push
		 * here would republish what the volume already has while a failure is
		 * being reported - work that cannot help and a log line that cannot be
		 * explained.
		 */
		return Read;
	}

	return LoadState(State);
}

FCFDVizResult SFlowVizWorkspace::LoadState(const FFlowVizSessionState& State)
{
	const FCFDVizResult Applied = Model->LoadState(State);

	// UNCONDITIONAL, and the header says why at length: a missing case is
	// REPORTED and still applies every panel, so a guard on IsOk() would leave
	// the render stale in exactly the relink flow.
	PushToVolume();

	// THE PUSH'S RESULT IS DROPPED rather than folded into the return. A
	// workspace with no volume bound - the ordinary state before a case actor is
	// placed - would otherwise turn every successful load into a reported
	// failure.
	return Applied;
}

bool SFlowVizWorkspace::TickClock(float DeltaSeconds)
{
	/*
	 * ONE ENGINE FRAME OF PLAYBACK.
	 *
	 * UNCONDITIONAL, RATHER THAN GATED ON IsPlaying(). The player's own Tick
	 * decides what a paused frame means, and the answer is not "nothing": it
	 * drains completed decodes and starts pending ones even when paused, so a
	 * scrub that stops mid-decode still lands on the frame the user asked for.
	 * A `if (Player.IsPlaying())` guard here would break scrubbing while looking
	 * like a harmless optimisation, and the paused arm of
	 * FlowViz.UI.Workspace.ClockSeam exists to catch exactly that.
	 *
	 * Tick is a no-op on a player with no case open, which is the state a
	 * freshly built workspace is in, so there is nothing to guard for that
	 * either.
	 */
	/*
	 * BEFORE THE TICK, because the tick is what would dereference it.
	 *
	 * A bound component can die without anyone calling SetVolume(nullptr) -- the
	 * level unloads, the actor is destroyed, the tab stays open. Volume is weak
	 * so it reads null the moment that happens, but the PLAYER's pointer to that
	 * component's texture set is raw and still reads the old address.
	 * DrainCompletedLoads, inside Tick below, enqueues into it.
	 *
	 * This is the only place that can catch it. There is no notification to hook
	 * -- the component does not know the workspace exists -- so the weak pointer
	 * going null IS the signal, and this ticker is what next observes it.
	 *
	 * NOT INSIDE the `if (Volume.Get())` block further down: that block runs when
	 * a component IS alive, and this is the case where one is not. The two are
	 * mutually exclusive, which is why this cannot be folded into it.
	 */
	if (Volume.Get() == nullptr && Model->Player.GetTextureSet() != nullptr)
	{
		UE_LOG(LogFlowViz, Log,
			TEXT("SFlowVizWorkspace: the bound volume was destroyed; detaching playback from its "
				 "texture set. Playback continues without a renderer until a volume is bound."));
		Model->Player.SetTextureSet(nullptr);
	}

	Model->Player.Tick(DeltaSeconds);

	/*
	 * AND THE RENDER SIDE HAS TO BE TOLD, because the proxy holds a SNAPSHOT.
	 *
	 * The frame source the component reads is live (FlowVizCaseSeam), so
	 * GetFrameSelection() follows the playhead the instant it moves -- but the
	 * scene proxy does not call that. It reads FFlowVizVolumeProxyDynamicData,
	 * which is built on the game thread and marshalled across only when
	 * something marks the component's dynamic data dirty. Every other writer on
	 * this widget's side (SetRenderSettings, SetClip, SetTransferFunction) marks
	 * it as part of the setter; a playhead that advances on its own has no
	 * setter to piggyback on, so without this the player would advance, the
	 * component would agree, and the picture would not move.
	 *
	 * Marked only when the DISPLAY changed rather than every frame. The
	 * displayed pair is what the proxy renders from, and it moves far less often
	 * than the playhead does -- at 24 stored frames per second against a
	 * 120 Hz tick, an unconditional mark would enqueue four redundant
	 * render-thread updates for every one that changes a pixel.
	 */
	if (UCFDVizVolumeComponent* Bound = Volume.Get())
	{
		const FFlowVizDisplaySelection& Display = Model->Player.GetDisplay();
		if (Display.FrameA != LastPublishedFrameA || Display.FrameB != LastPublishedFrameB
			|| Display.Alpha != LastPublishedAlpha)
		{
			LastPublishedFrameA = Display.FrameA;
			LastPublishedFrameB = Display.FrameB;
			LastPublishedAlpha = Display.Alpha;
			Bound->MarkRenderDynamicDataDirty();
		}
	}

	/*
	 * THE SAMPLING SERVICE (#75): probe readings and the per-frame range.
	 *
	 * Drained every tick (cheap: a lock and usually an empty array) and
	 * REQUESTED only when the displayed frame moved -- the same displayed-pair
	 * gate as the render publish above, tracked separately because the render
	 * mark and the resample have different costs. Requesting every tick would
	 * queue a disk read per frame; requesting on display change re-samples
	 * exactly when the numbers on screen stop describing the picture.
	 */
	Model->DrainSampleResults();
	{
		const int32 DisplayedFrame = Model->Player.GetDisplay().FrameA;
		if (DisplayedFrame != LastSampledFrame && Model->IsCaseOpen())
		{
			LastSampledFrame = DisplayedFrame;
			Model->RequestSampleUpdate();
		}
	}

	// True re-arms for the next engine frame. Returning false here would make
	// playback work exactly once.
	return true;
}

void SFlowVizWorkspace::HandleTransferFunctionChanged()
{
	// The return is dropped for the same reason HandleClipChanged drops it: a
	// panel edit with no case open is normal rather than an error.
	PushToVolume();
}

void SFlowVizWorkspace::HandleClipChanged()
{
	// The return is deliberately dropped. A panel edit with no case open is
	// normal, not an error, and the clip panel's advisory already says the edits
	// are not reaching a renderer - reporting it twice would put a warning in the
	// log for every click during ordinary setup.
	PushToVolume();
}

void SFlowVizWorkspace::HandleFieldChanged()
{
	PushToVolume();
}

void SFlowVizWorkspace::HandleSliceChanged()
{
	// Same policy as every sibling: the return is dropped, "no volume bound"
	// is the ordinary setup state.
	PushToVolume();
}

void SFlowVizWorkspace::HandleRenderSettingsChanged()
{
	// Same policy as the clip handler: the return is dropped because "no volume
	// bound yet" is the ordinary setup state, and the settings are value state
	// that will be pushed whole the moment SetVolume binds one.
	PushToVolume();
}

#undef LOCTEXT_NAMESPACE
