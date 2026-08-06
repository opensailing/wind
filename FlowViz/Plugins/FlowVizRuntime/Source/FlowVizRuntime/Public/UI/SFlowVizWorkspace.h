// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

struct FFlowVizWorkspaceModel;
class UCFDVizVolumeComponent;
class SFlowVizClipPanel;
class SFlowVizProbePanel;
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

	/* --- Test seams. The panels this workspace actually built. ------------- */

	TSharedPtr<SFlowVizTransportBar> GetTransportBar() const { return TransportBar; }
	TSharedPtr<SFlowVizTransferFunctionPanel> GetTransferFunctionPanel() const
	{
		return TransferFunctionPanel;
	}
	TSharedPtr<SFlowVizClipPanel> GetClipPanel() const { return ClipPanel; }
	TSharedPtr<SFlowVizSlicePanel> GetSlicePanel() const { return SlicePanel; }
	TSharedPtr<SFlowVizProbePanel> GetProbePanel() const { return ProbePanel; }

private:
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

	TSharedPtr<SFlowVizTransportBar> TransportBar;
	TSharedPtr<SFlowVizTransferFunctionPanel> TransferFunctionPanel;
	TSharedPtr<SFlowVizClipPanel> ClipPanel;
	TSharedPtr<SFlowVizSlicePanel> SlicePanel;
	TSharedPtr<SFlowVizProbePanel> ProbePanel;
};
