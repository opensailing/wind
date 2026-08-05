// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

struct FFlowVizWorkspaceModel;
class SFlowVizClipPanel;
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

	/* --- Test seams. The panels this workspace actually built. ------------- */

	TSharedPtr<SFlowVizTransportBar> GetTransportBar() const { return TransportBar; }
	TSharedPtr<SFlowVizTransferFunctionPanel> GetTransferFunctionPanel() const
	{
		return TransferFunctionPanel;
	}
	TSharedPtr<SFlowVizClipPanel> GetClipPanel() const { return ClipPanel; }

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

	TSharedPtr<SFlowVizTransportBar> TransportBar;
	TSharedPtr<SFlowVizTransferFunctionPanel> TransferFunctionPanel;
	TSharedPtr<SFlowVizClipPanel> ClipPanel;
};
