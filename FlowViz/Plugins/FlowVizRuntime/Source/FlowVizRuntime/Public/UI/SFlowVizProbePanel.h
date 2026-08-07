// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UI/FlowVizProbeViewModel.h"
#include "UI/SFlowVizTransferFunctionPanel.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SButton;
class SVerticalBox;

/**
 * The probe list (plan.md sections 10.11 and 5F; engineering rules 9 and 10).
 *
 * NO STATE. Every readout reads through to FFlowVizProbeViewModel on each paint,
 * and the row list is REBUILT from the model rather than mirrored - a mirrored
 * list is a second copy, and the first change that does not come through this
 * panel leaves the two disagreeing about which probes exist.
 *
 * POSITIONS ARE SHOWN AND TYPED IN SOLVER UNITS, which is ADR 004 section 8 and
 * engineering rule 4. The panel never displays a centimetre value, never
 * converts on its own, and does not offer an Unreal-space entry field: the view
 * model has AddProbeAtUnrealPosition for a viewport pick, and a typed field is
 * not a pick. Offering both would put two coordinate systems in one form with
 * nothing on screen to say which is which, and a position in the wrong frame is
 * a plausible number pointing at the wrong cell.
 *
 * WHAT THIS PANEL DOES NOT CLAIM.
 *
 *  - NOTHING SAMPLES. FlowVizProbe::SampleStoredField exists and is tested, but
 *    it has no production caller: nothing schedules a read when a probe moves or
 *    when the frame changes. So a probe placed here reports "not sampled"
 *    forever. That is disclosed in the panel, permanently.
 *
 *  - NOTHING DRAWS A PROBE MARKER in the viewport, so the position readout is
 *    the only evidence a probe exists.
 *
 *  - NO PLOT, NO CSV EXPORT, NO DRAG. plan.md 10.11 lists them; their consumers
 *    do not exist. They are ABSENT rather than present-and-inert, because a
 *    disabled "Export CSV" button is a promise and rule 15 forbids it. The line
 *    probe IS here - it is pure geometry the view model computes and tests.
 *
 * RULE 10 IS THE SHARP EDGE HERE. FFlowVizProbeReading::Magnitude defaults to
 * 0.0, so a readout that formats it unconditionally prints "0" for a probe that
 * was never sampled - and zero velocity is a real, interesting result in this
 * field. GetProbeValueText therefore checks bHasValue FIRST and returns a
 * non-numeric string; the alternative silently converts "no data" into a
 * measurement.
 *
 * A NULL VIEW MODEL IS A LEGAL, INERT STATE - the workspace builds panels before
 * a case is open. Every control disables rather than crashing.
 */
class FLOWVIZRUNTIME_API SFlowVizProbePanel : public SCompoundWidget
{
public:
	/**
	 * THE INITIALIZER IS LOAD-BEARING. SLATE_ARGUMENT expands to a bare
	 * `ArgType _ArgName;` with no initializer, so an omitted pointer argument
	 * holds INDETERMINATE memory rather than null - every null check then passes
	 * and the first call through it is a SIGBUS, which is what crashed
	 * FlowViz.UI.TransferFunctionPanel.Unbound before its header grew this line.
	 */
	SLATE_BEGIN_ARGS(SFlowVizProbePanel)
		: _ViewModel(nullptr)
	{
	}
		/** Borrowed, not owned. The workspace owns both. */
		SLATE_ARGUMENT(FFlowVizProbeViewModel*, ViewModel)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/* --- Test seams. See SFlowVizTransportBar.h for why these are public. --- */

	/** Placement entry for Axis (0=X, 1=Y, 2=Z), in SOLVER units. */
	TSharedPtr<SFlowVizNumericEntry> GetPlacementBox(int32 Axis) const;

	TSharedPtr<SButton> GetAddProbeButton() const { return AddProbeButton; }
	TSharedPtr<SButton> GetRemoveAllButton() const { return RemoveAllButton; }

	/** Name field for the probe at Index, or null if there is no such row. */
	TSharedPtr<SFlowVizNumericEntry> GetProbeNameBox(int32 Index) const;
	TSharedPtr<SButton> GetProbeVisibleButton(int32 Index) const;
	TSharedPtr<SButton> GetProbeRemoveButton(int32 Index) const;

	/**
	 * What the value column shows for the probe at Index.
	 *
	 * Exposed as TEXT rather than as a widget handle so a test can assert what a
	 * user would READ - which is where rule 10 is either honoured or broken.
	 */
	FText GetProbeValueText(int32 Index) const;

	/** Line-probe endpoint entries, per axis, in SOLVER units. */
	TSharedPtr<SFlowVizNumericEntry> GetLineStartBox(int32 Axis) const;
	TSharedPtr<SFlowVizNumericEntry> GetLineEndBox(int32 Axis) const;
	TSharedPtr<SFlowVizNumericEntry> GetLineSamplesBox() const { return LineSamplesBox; }
	TSharedPtr<SButton> GetSetLineButton() const { return SetLineButton; }

	/**
	 * Toggle the line plot's x axis: solver-unit distance <-> normalized 0..1
	 * (#75: SetLineAxisMode had no production caller; normalized is what makes
	 * two lines of different lengths comparable, and nothing could select it).
	 */
	TSharedPtr<SButton> GetLineAxisButton() const { return LineAxisButton; }

	/** The "nothing samples these yet" disclosure. Never empty. */
	FText GetNotSampledAdvisoryText() const;

	/** How many probe rows are currently built. Derived from the view model, never cached. */
	int32 GetProbeRowCount() const { return ProbeRows.Num(); }

private:
	/** One built row of probe controls. Rebuilt wholesale when the probe count changes. */
	struct FProbeRow
	{
		/**
		 * The probe this row is FOR, captured by id rather than by index.
		 *
		 * An index would be invalidated by any removal that is not the last row,
		 * and the failure is silent: the row would still work, on a different
		 * probe. An id either finds its probe or does not.
		 */
		FGuid Id;
		TSharedPtr<SFlowVizNumericEntry> NameBox;
		TSharedPtr<SButton> VisibleButton;
		TSharedPtr<SButton> RemoveButton;
	};

	bool IsBound() const { return ViewModel != nullptr; }

	/** Rule 15: there is something to clear AND a model to clear it from. */
	bool CanRemoveAll() const;

	/** Rebuild every row from the view model. Called whenever the probe COUNT changes. */
	void RebuildProbeRows();

	FReply OnAddProbeClicked();
	FReply OnRemoveAllClicked();
	FReply OnProbeVisibleClicked(FGuid Id);
	FReply OnProbeRemoveClicked(FGuid Id);
	FReply OnSetLineClicked();
	FReply OnLineAxisClicked();
	FText GetLineAxisLabel() const;

	void OnPlacementCommitted(const FText& NewText, ETextCommit::Type CommitType, int32 Axis);
	void OnProbeNameCommitted(const FText& NewText, ETextCommit::Type CommitType, FGuid Id);
	void OnLineStartCommitted(const FText& NewText, ETextCommit::Type CommitType, int32 Axis);
	void OnLineEndCommitted(const FText& NewText, ETextCommit::Type CommitType, int32 Axis);
	void OnLineSamplesCommitted(const FText& NewText, ETextCommit::Type CommitType);

	/* --- Bound readers. Each one tolerates a null view model. -------------- */

	FText GetProbeCountText() const;
	FText GetProbePositionText(FGuid Id) const;
	FText GetProbeNameText(FGuid Id) const;
	FText GetProbeVisibleGlyph(FGuid Id) const;
	FText GetLineSummaryText() const;
	FText GetLineSamplesText() const;

	/** Borrowed. Null is legal and inert. */
	FFlowVizProbeViewModel* ViewModel = nullptr;

	/**
	 * The placement fields are WIDGET-SIDE state, and deliberately so.
	 *
	 * They are a composition buffer for a probe that does not exist yet, not a
	 * view of one that does - there is nothing in the view model for them to
	 * mirror until Add is pressed. That is the one case where local state is not
	 * a second copy of the model.
	 */
	FVector PendingPosition = FVector::ZeroVector;
	FVector PendingLineStart = FVector::ZeroVector;
	FVector PendingLineEnd = FVector::OneVector;

	TStaticArray<TSharedPtr<SFlowVizNumericEntry>, 3> PlacementBoxes;
	TStaticArray<TSharedPtr<SFlowVizNumericEntry>, 3> LineStartBoxes;
	TStaticArray<TSharedPtr<SFlowVizNumericEntry>, 3> LineEndBoxes;

	TSharedPtr<SFlowVizNumericEntry> LineSamplesBox;
	TSharedPtr<SButton> AddProbeButton;
	TSharedPtr<SButton> RemoveAllButton;
	TSharedPtr<SButton> SetLineButton;
	TSharedPtr<SButton> LineAxisButton;

	/** The container the rows are built into. */
	TSharedPtr<SVerticalBox> ProbeListBox;
	TArray<FProbeRow> ProbeRows;
};
