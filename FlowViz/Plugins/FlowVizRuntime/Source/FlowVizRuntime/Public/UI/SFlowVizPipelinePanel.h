// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

struct FFlowVizWorkspaceModel;
class SButton;
class SVerticalBox;

/**
 * The pipeline panel (#83 / Milestone F): the open case and its volume
 * fields as clickable rows.
 *
 * plan.md's "pipeline tree", scoped honestly. The runtime holds ONE case with
 * volume fields -- there is no multi-source pipeline to arrange -- so this is
 * a flat field list under the case's name, and switching fields is the one
 * action the tree can truly perform (FFlowVizWorkspaceModel::SetField, which
 * until this panel had session-load as its only production caller). A tree
 * control with one permanent branch would imply drag-and-drop pipeline
 * editing that does not exist -- rule 15 at the metaphor level.
 *
 * SWITCHING FIELDS IS EXPENSIVE AND SAYS SO. SetField re-opens the case:
 * playback resets and the cache drops. The row for the DISPLAYED field is
 * therefore inert (re-clicking does not announce), and the tooltip names the
 * cost.
 *
 * A NULL MODEL IS LEGAL AND INERT, matching every sibling panel.
 */
/** The path the user asked to open. */
DECLARE_DELEGATE_OneParam(FFlowVizOpenCaseRequested, const FString& /*CasePath*/);

class FLOWVIZRUNTIME_API SFlowVizPipelinePanel : public SCompoundWidget
{
public:
	// nullptr initializer required -- SLATE_ARGUMENT does not zero-initialize
	// (the SFlowVizTransportBar note).
	SLATE_BEGIN_ARGS(SFlowVizPipelinePanel)
		: _Model(nullptr)
	{
	}
		/** Borrowed, not owned. The workspace owns it. */
		SLATE_ARGUMENT(FFlowVizWorkspaceModel*, Model)

		/** Fired after a click CHANGED the displayed field. The workspace re-pushes. */
		SLATE_EVENT(FSimpleDelegate, OnFieldChanged)

		/**
		 * Fired when the user asks to open the case at Path (Open button or a
		 * committed path). The PANEL does not open anything -- the workspace
		 * routes this through the same console path the -case= flag uses, so
		 * actor spawning and both-halves loading stay one tested code path.
		 */
		SLATE_EVENT(FFlowVizOpenCaseRequested, OnOpenCaseRequested)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/**
	 * Rebuild the rows from the model's current case.
	 *
	 * EXPLICIT, not per-paint: the field list changes only on case open, and
	 * the workspace calls this from its open/load paths. Rebuilding rows in a
	 * paint pass would churn widgets every frame for a list that changes twice
	 * a session.
	 */
	void RefreshFields();

	/* --- Test seams (public for the reason SFlowVizTransportBar's are). ---- */

	int32 GetFieldRowCount() const { return FieldButtons.Num(); }

	/** The row button for the Nth offered field, in GetVolumeFieldIds order. */
	TSharedPtr<SButton> GetFieldButton(int32 Index) const;

	TSharedPtr<class SEditableTextBox> GetOpenPathBox() const { return OpenPathBox; }
	TSharedPtr<SButton> GetOpenButton() const { return OpenButton; }

	/** The mode toggles, ordinal: Obstacle, CutPlane, Iso, Streamlines, Particles, Volume. */
	static constexpr int32 ModeToggleCount = 6;
	TSharedPtr<SButton> GetModeToggleButton(int32 Index) const;

private:
	FReply OnFieldClicked(FName FieldId);
	FReply OnModeToggleClicked(int32 ModeIndex);
	bool IsModeToggleActive(int32 ModeIndex) const;
	void RefreshModeToggleStyles();
	FReply OnOpenClicked();
	FReply OnBrowseClicked();
	FText GetCaseLabel() const;

	/** Borrowed. Null is the unbound state. */
	FFlowVizWorkspaceModel* Model = nullptr;

	FSimpleDelegate OnFieldChanged;
	FFlowVizOpenCaseRequested OnOpenCaseRequested;

	TSharedPtr<class SEditableTextBox> OpenPathBox;
	TSharedPtr<SButton> OpenButton;
	TSharedPtr<SButton> ModeToggleButtons[ModeToggleCount];

	TSharedPtr<SVerticalBox> RowsBox;
	TArray<TSharedPtr<SButton>> FieldButtons;
	TArray<FName> FieldIds;
};
