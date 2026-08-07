// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CFDViz/CFDVizTypes.h"
#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

struct FFlowVizSessionState;
struct FFlowVizWorkspaceModel;
class UCFDVizVolumeComponent;
class SFlowVizClipPanel;
class SFlowVizDiagnosticsOverlay;
class SFlowVizPipelinePanel;
class SFlowVizProbePanel;
class SFlowVizRenderSettingsPanel;
class SFlowVizSlicePanel;
class SFlowVizTransferFunctionPanel;
class SFlowVizTransportBar;

/**
 * The workspace: the thing a user actually opens (plan.md section F).
 *
 * WHAT THIS IS RESPONSIBLE FOR, AND WHAT IT IS NOT. It owns the
 * FFlowVizWorkspaceModel - the player and the five view models - and lays the
 * panels out around a viewport region. It holds no display state of its own;
 * every panel binds through to the view models, as they already did
 * individually.
 *
 * THE LAYOUT IS A SPLITTER, NOT A FIXED GRID. plan.md section 11 asks for
 * resizable, collapsible panels. A fixed grid is the version of this that looks
 * right in a screenshot and is unusable at 1280x800 with the transfer-function
 * editor open, which is a real display size for a laptop at a conference.
 *
 * WHY THE VIEWPORT REGION IS A PLACEHOLDER HERE. The volume is drawn by
 * FFlowVizVolumeComponent into a level viewport, not into a Slate surface this
 * widget owns; wiring a dedicated scene capture into this panel is a separate
 * task (#25 is live on the volume path). Rather than draw a fake viewport, the
 * centre region says what it is. A convincing-looking empty 3D pane would be
 * indistinguishable from a volume that failed to load - the exact ambiguity
 * engineering rule 7 exists to prevent.
 */
class FLOWVIZRUNTIME_API SFlowVizWorkspace : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SFlowVizWorkspace)
	{
	}
	SLATE_END_ARGS()

	SFlowVizWorkspace();
	virtual ~SFlowVizWorkspace() override;

	void Construct(const FArguments& InArgs);

	/**
	 * The live model behind every panel.
	 *
	 * Returned so a test can drive the model and observe the panels follow -
	 * which is the only way to check that this widget handed the SAME model to
	 * its children rather than each panel getting its own.
	 */
	FFlowVizWorkspaceModel& GetModel() const;

	/* --- The renderer this workspace drives -------------------------------- */

	/**
	 * Point this workspace at the volume its panels should drive. Null unbinds.
	 *
	 * THIS IS THE OBJECT THAT HOLDS BOTH HALVES, and nothing did before. The
	 * panels edit view models; UCFDVizVolumeComponent renders one. Grep for a
	 * file mentioning both types and before this there were none - which is why
	 * the clip panel's controls were live, validated, persisted, and inert.
	 *
	 * BORROWED, NOT OWNED, AND HELD WEAKLY. The component belongs to a
	 * ACFDVizCaseActor in a world that can be torn down while this widget is
	 * still open - a docked tab outlives a PIE session routinely. A raw pointer
	 * would be a use-after-free on the next panel edit; a strong TObjectPtr would
	 * keep a dead world's component alive and render from it.
	 *
	 * PUSHES IMMEDIATELY. Binding a volume that has not yet received the current
	 * clip state would leave the render one edit behind until the user touched
	 * something, which reads as a control that needs to be wiggled to take.
	 */
	void SetVolume(UCFDVizVolumeComponent* InVolume);

	/** The bound volume, or null. */
	UCFDVizVolumeComponent* GetVolume() const;

	/**
	 * Copy the current model into the bound volume.
	 *
	 * Called automatically whenever a panel reports an edit; public so a caller
	 * that changed the model directly - a session load, a console command - can
	 * make the render follow without simulating a click.
	 *
	 * @return False when there was nothing to push into: no volume bound, or a
	 *         bound one with no case. "Did not happen", not "went wrong".
	 */
	bool PushToVolume();

	/* --- Sessions ---------------------------------------------------------- */

	/**
	 * Save the workspace to a `.cfdvizsession` file.
	 *
	 * A thin forward to the model's SaveSession. It exists rather than leaving
	 * callers to write `GetModel().SaveSession(...)` so that save and load are a
	 * matched pair at THIS level: load cannot be a forward, for the reason below,
	 * and offering only half the pair here is what invites a caller to reach past
	 * the widget for the other half.
	 *
	 * NO FILE DIALOG, HERE OR IN LoadSession. This module has no DesktopPlatform
	 * dependency and must not grow one - it is editor-only, and pulling it in
	 * would make this workspace unusable in a packaged build for the sake of a
	 * button. The picker belongs to whatever opens the workspace; these take a
	 * path.
	 */
	FCFDVizResult SaveSession(const FString& FilePath) const;

	/**
	 * Load a session AND make the bound volume show it.
	 *
	 * WHY THIS IS NOT `GetModel().LoadSession(...)`. Every other push in this
	 * widget is triggered by a panel's change delegate. A session load rewrites
	 * the transfer function and the clip planes UNDERNEATH the panels, and no
	 * panel announces anything - so a caller that went through the model directly
	 * would leave the render on the previous session's colours while every
	 * control on screen reads correctly. That is the shape #50 had one level
	 * down, and a screenshot of it looks entirely right.
	 *
	 * THE PUSH IS UNCONDITIONAL, AND THAT IS THE POINT. LoadSession reports the
	 * first failure AFTER applying what it could, so a non-Ok result routinely
	 * means "every panel is restored and the case is missing" - the relink case
	 * plan.md section 14 requires. Guarding the push on IsOk() reads as prudent
	 * and is exactly backwards: it would suppress the push in the one flow where
	 * the user is about to be asked to relink, and asked against a stale image.
	 *
	 * @return The load's result, unchanged. The push's outcome is deliberately
	 *         NOT folded in: "no volume bound" is the ordinary state of a
	 *         workspace whose scene has no case actor yet, and reporting that as
	 *         a session failure would make an ordinary load look broken.
	 */
	FCFDVizResult LoadSession(const FString& FilePath);

	/**
	 * Apply an already-parsed session, then push. The half of LoadSession after
	 * the read.
	 *
	 * Exposed for the relink flow, which must parse, discover the case is
	 * missing, ask the user, RelinkCase and only then apply - without writing the
	 * state back out to a temporary just to re-read it. The push obligation is
	 * identical either way, so it lives here and LoadSession routes through it
	 * rather than repeating the call.
	 */
	FCFDVizResult LoadState(const FFlowVizSessionState& State);

	/* --- The diagnostics overlay ------------------------------------------- */

	/**
	 * Show or hide the on-screen diagnostics readout (plan.md section 17).
	 *
	 * WHY THE SHOWN STATE LIVES HERE AND NOT ON THE OVERLAY. The overlay is a
	 * formatter; what it displays depends only on the model. Whether it is on
	 * screen is a property of the workspace's layout, and putting it on the
	 * widget as well would give two objects an opinion about one thing - the
	 * state where they disagree is an overlay that believes it is visible inside
	 * a collapsed slot, which reads from every angle except the user's as
	 * working.
	 *
	 * HIDDEN BY DEFAULT. It sits on top of the viewport region, which is the
	 * picture the user opened the tool to look at.
	 */
	void SetDiagnosticsOverlayShown(bool bShown);

	/** Whether the overlay is currently on screen. */
	bool IsDiagnosticsOverlayShown() const { return bDiagnosticsOverlayShown; }

	/* --- Test seams. The panels this workspace actually built. ------------- */

	/**
	 * The overlay this workspace built.
	 *
	 * Returned for the reason GetModel is: it is the only way a test can check
	 * that the workspace built ONE overlay over its OWN model, rather than that
	 * an overlay can be constructed - which every test that SNews its own
	 * already proves and no test that SNews its own can refute.
	 */
	TSharedPtr<SFlowVizDiagnosticsOverlay> GetDiagnosticsOverlay() const
	{
		return DiagnosticsOverlay;
	}

	TSharedPtr<SFlowVizTransportBar> GetTransportBar() const { return TransportBar; }
	TSharedPtr<SFlowVizTransferFunctionPanel> GetTransferFunctionPanel() const
	{
		return TransferFunctionPanel;
	}
	TSharedPtr<SFlowVizClipPanel> GetClipPanel() const { return ClipPanel; }
	TSharedPtr<SFlowVizSlicePanel> GetSlicePanel() const { return SlicePanel; }
	TSharedPtr<SFlowVizProbePanel> GetProbePanel() const { return ProbePanel; }
	TSharedPtr<SFlowVizRenderSettingsPanel> GetRenderSettingsPanel() const
	{
		return RenderSettingsPanel;
	}
	TSharedPtr<SFlowVizPipelinePanel> GetPipelinePanel() const { return PipelinePanel; }

private:
	/**
	 * THE CLOCK. Without it, Play sets a flag nothing acts on.
	 *
	 * FFlowVizCasePlayer::Play does exactly one thing -- `bPlaying = true` -- and
	 * everything that reads that flag lives in FFlowVizCasePlayer::Tick. Before
	 * this handle existed, the only callers of that Tick in the whole plugin were
	 * test files: no FTSTicker, no FTickableGameObject, no SWidget::Tick override,
	 * and UCFDVizVolumeComponent sets `PrimaryComponentTick.bCanEverTick = false`.
	 * So the transport bar's button toggled its label, IsPlaying() answered true,
	 * every playback test passed, and the case sat on whatever frame the user had
	 * last dragged the playhead to.
	 *
	 * IT ALSO PRIMES THE DECODES, which is the half that is easy to miss.
	 * StartPendingLoads has exactly one call site and it is inside that same Tick,
	 * so an unticked player never even starts reading frame 0 off disk. Combined
	 * with WaitForPendingLoads returning true immediately against an empty queue,
	 * an un-driven player is observationally identical to a broken decode path.
	 *
	 * WHY FTSTicker AND NOT AN SCompoundWidget::Tick OVERRIDE. The two are not
	 * interchangeable, and they differ in a case that happens constantly: Slate
	 * ticks a widget only while it is in a visible, painted window, so a
	 * widget-driven clock stops playback whenever the workspace tab is hidden
	 * behind another. That would be defensible if the animation were IN this
	 * panel -- but it is not. The volume renders into the LEVEL VIEWPORT (see the
	 * class comment above), so the tab this widget lives in is precisely the thing
	 * a user docks away while watching the volume play. A widget clock would stop
	 * the picture they are looking at because they moved a panel they are not.
	 * FTSTicker is driven by the engine loop's Tick_Core every frame regardless of
	 * what is painted, which is the behaviour that matches where the image is.
	 *
	 * The cost of that choice is that this ticker keeps running for a workspace
	 * nobody can see, which is why it must be unregistered in the destructor
	 * rather than left to expire: the handler captures `this` and the model is
	 * destroyed with the widget. Registration is per-instance for the same
	 * reason -- a static handle would let the first workspace's destructor stop
	 * the clock for every workspace still open, and closing one of two tabs is an
	 * ordinary thing to do.
	 */
	FTSTicker::FDelegateHandle ClockHandle;

	/**
	 * One engine frame of playback. Returns true to stay registered.
	 *
	 * Takes the engine's real delta rather than a fixed step so playback runs in
	 * wall time rather than in frames-rendered; the player's own Sequence and
	 * FixedFps modes decide what that means for the playhead.
	 */
	bool TickClock(float DeltaSeconds);

	/**
	 * The last displayed pair this widget told the renderer about.
	 *
	 * The scene proxy renders from a marshalled SNAPSHOT, not from the live
	 * frame source, so an advancing playhead reaches the pixels only when
	 * something marks the component's dynamic data dirty. These three exist so
	 * that mark happens when the display actually CHANGES rather than on every
	 * engine frame -- the displayed pair moves at the case's frame rate, which is
	 * a small fraction of the tick rate, and each redundant mark is a
	 * render-thread command enqueued to publish bytes identical to the ones
	 * already there.
	 *
	 * Alpha is part of the comparison because an interpolated frame's blend
	 * weight changes the image while both frame indices stay put; comparing only
	 * the indices would hold the picture still through the whole blend and then
	 * jump.
	 *
	 * Seeded to the "nothing published yet" display so the first tick after a
	 * case opens always marks.
	 */
	int32 LastPublishedFrameA = INDEX_NONE;
	int32 LastPublishedFrameB = INDEX_NONE;
	double LastPublishedAlpha = -1.0;

	/**
	 * Heap-allocated rather than a by-value member.
	 *
	 * FFlowVizWorkspaceModel is non-copyable and owns a case player whose
	 * destruction waits on outstanding decodes; a unique_ptr keeps this header
	 * free of the player's full definition and keeps the destruction order
	 * explicit rather than implied by member order in a widget.
	 */
	TUniquePtr<FFlowVizWorkspaceModel> Model;

	/**
	 * WEAK, for the reason SetVolume's comment gives: the component's world can
	 * be torn down under a docked tab. TWeakObjectPtr turns that from a
	 * use-after-free into a null check.
	 */
	TWeakObjectPtr<UCFDVizVolumeComponent> Volume;

	/** Subscriber for the clip panel's edits. Pushes, and reports nothing. */
	void HandleClipChanged();
	void HandleTransferFunctionChanged();
	void HandleRenderSettingsChanged();
	void HandleSliceChanged();
	void HandleFieldChanged();

	TSharedPtr<SFlowVizTransportBar> TransportBar;
	TSharedPtr<SFlowVizTransferFunctionPanel> TransferFunctionPanel;
	TSharedPtr<SFlowVizClipPanel> ClipPanel;
	TSharedPtr<SFlowVizSlicePanel> SlicePanel;
	TSharedPtr<SFlowVizProbePanel> ProbePanel;
	TSharedPtr<SFlowVizRenderSettingsPanel> RenderSettingsPanel;
	TSharedPtr<SFlowVizPipelinePanel> PipelinePanel;

	/**
	 * The diagnostics readout, and the slot whose visibility carries its shown
	 * state.
	 *
	 * THE SLOT IS HELD, NOT LOOKED UP. The alternative - toggling the overlay
	 * widget's own visibility - collides with the HitTestInvisible it sets on
	 * itself in Construct, so hiding and re-showing would silently promote it to
	 * a hit-test target and it would start swallowing clicks meant for the
	 * viewport behind it. Toggling the CONTAINER leaves the overlay's own
	 * visibility alone.
	 */
	/**
	 * Last frame the sampling service ran against. Deliberately NOT INDEX_NONE:
	 * the display reads INDEX_NONE until the first frame is resident, and this
	 * must differ from that so the first tick with a case open still issues a
	 * request (which falls back to frame 0 inside RequestSampleUpdate).
	 */
	int32 LastSampledFrame = INDEX_NONE - 1;

	TSharedPtr<SFlowVizDiagnosticsOverlay> DiagnosticsOverlay;
	TSharedPtr<SWidget> DiagnosticsOverlayContainer;

	/** Hidden until asked for. See SetDiagnosticsOverlayShown. */
	bool bDiagnosticsOverlayShown = false;
};
