// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UI/FlowVizSliceViewModel.h"
#include "UI/SFlowVizTransferFunctionPanel.h"
#include "UI/SFlowVizTransportBar.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SButton;

/**
 * The slice plane's controls (plan.md sections 10.4 and 5F).
 *
 * NO STATE. Every control reads through to FFlowVizSliceViewModel on each paint,
 * for the reason given at length on SFlowVizClipPanel: a widget-side copy is a
 * second source of truth, and the first change that does not come through this
 * panel leaves the two disagreeing with no way for a user to tell which is real.
 *
 * WHAT THIS PANEL DOES NOT CLAIM. There is no slice renderer. The view model's
 * own header says so - "NO RENDERER CONSUMES THIS YET ... nothing draws a
 * slice" - and it is still true: there is no UCFDVizSliceComponent in the tree,
 * and MakeClipPlane()'s only callers are tests. So everything below authors,
 * validates and persists a slice that is not drawn.
 *
 * THAT IS DISCLOSED IN THE PANEL, not just here, and for a sharper reason than
 * on the clip panel. A clip plane that does nothing leaves the volume looking
 * uncut, which is at least suspicious. A slice that does nothing leaves the
 * volume looking exactly as it did - and since the ONLY evidence a slice exists
 * would be the slice itself, a user has no way to distinguish "not wired" from
 * "positioned outside the domain" or "opacity zero". Those are three different
 * afternoons of debugging, and the panel owes them the answer up front.
 *
 * THE SLAB CONTROLS ARE THE INTERESTING PART OF THE LAYOUT. The view model
 * REFUSES SetSlabOp and SetSlabSamples on a zero-thickness slice, so this panel
 * disables them when thickness is zero. Rule 15 is about that pair being ONE
 * fact: a control the model will refuse must not look pressable. Showing them
 * live and letting the refusal happen silently is the failure mode - the user
 * presses "Average", nothing happens, and there is nothing on screen to say why.
 *
 * RULE 7 - INTERPOLATION IS VISIBLY IDENTIFIED. Trilinear sampling reports
 * values the solver never computed. The toggle exists because interpolation is
 * genuinely wanted for a smooth slice, and the advisory beside it exists because
 * a smooth field and a smooth PICTURE of a coarse field look the same. The
 * advisory appears only in trilinear mode; an always-on notice would carry no
 * information.
 *
 * A NULL VIEW MODEL IS A LEGAL, INERT STATE - the workspace builds panels before
 * a case is open. Every control disables rather than crashing.
 */
class FLOWVIZRUNTIME_API SFlowVizSlicePanel : public SCompoundWidget
{
public:
	/**
	 * THE INITIALIZER IS LOAD-BEARING. SLATE_ARGUMENT expands to a bare
	 * `ArgType _ArgName;` with no initializer, so an omitted pointer argument
	 * holds INDETERMINATE memory rather than null - every null check then passes
	 * and the first call through it is a SIGBUS, which is what crashed
	 * FlowViz.UI.TransferFunctionPanel.Unbound before its header grew this line.
	 */
	SLATE_BEGIN_ARGS(SFlowVizSlicePanel)
		: _ViewModel(nullptr)
	{
	}
		/** Borrowed, not owned. The workspace owns both. */
		SLATE_ARGUMENT(FFlowVizSliceViewModel*, ViewModel)

		/**
		 * Fired after any control here has CHANGED the view model (#77). The
		 * workspace subscribes and re-composes the slice into the pushed clip
		 * model; the panel never mentions a volume, matching its siblings.
		 * Unbound is legal and inert.
		 */
		SLATE_EVENT(FSimpleDelegate, OnSliceChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/* --- Test seams. See SFlowVizTransportBar.h for why these are public. --- */

	/** The X/Y/Z preset button that sets the slice normal to that axis. */
	TSharedPtr<SButton> GetAxisButton(EFlowVizSliceAxis Axis) const;

	/** The position-along-the-normal slider, calibrated 0..1 over the projected extent. */
	TSharedPtr<SFlowVizScrubSlider> GetPositionSlider() const { return PositionSlider; }

	/** Slab thickness entry, in SOLVER units. */
	TSharedPtr<SFlowVizNumericEntry> GetThicknessBox() const { return ThicknessBox; }

	/** Slab sample-count entry. Disabled with the rest of the slab controls at zero thickness. */
	TSharedPtr<SFlowVizNumericEntry> GetSlabSamplesBox() const { return SlabSamplesBox; }

	/** Aggregation selector. Null for EFlowVizSlabOp::None, which is a state rather than a choice. */
	TSharedPtr<SButton> GetSlabOpButton(EFlowVizSlabOp Op) const;

	TSharedPtr<SButton> GetVisibleButton() const { return VisibleButton; }
	TSharedPtr<SButton> GetShowWidgetButton() const { return ShowWidgetButton; }
	TSharedPtr<SButton> GetTrilinearButton() const { return TrilinearButton; }

	/** Opacity slider, 0..1. */
	TSharedPtr<SFlowVizScrubSlider> GetOpacitySlider() const { return OpacitySlider; }

	/**
	 * True while the rule 7 interpolation notice is on screen.
	 *
	 * Exposed as a query rather than as a widget handle so a test asserts the
	 * VISIBLE state rather than the existence of a widget that might be collapsed.
	 */
	bool IsInterpolationAdvisoryVisible() const;

	/** The "no renderer draws this yet" disclosure. Never empty. */
	FText GetNotDrawnAdvisoryText() const;

private:
	bool IsBound() const { return ViewModel != nullptr; }

	/** Rule 15: the slab controls are live only when the model will accept them. */
	bool IsSlabActive() const;

	/** Bound to every control that needs a model at all. */
	bool IsPanelLive() const { return IsBound(); }

	EVisibility GetInterpolationAdvisoryVisibility() const;

	FReply OnAxisClicked(EFlowVizSliceAxis Axis);
	FReply OnSlabOpClicked(EFlowVizSlabOp Op);
	FReply OnVisibleClicked();
	FReply OnShowWidgetClicked();
	FReply OnTrilinearClicked();

	void OnPositionChanged(float NewValue);
	void OnOpacityChanged(float NewValue);
	void OnThicknessCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void OnSlabSamplesCommitted(const FText& NewText, ETextCommit::Type CommitType);

	/* --- Bound readers. Each one tolerates a null view model. -------------- */

	float GetPositionValue() const;
	float GetOpacityValue() const;
	FText GetThicknessText() const;
	FText GetSlabSamplesText() const;
	FText GetVisibleLabel() const;
	FText GetShowWidgetLabel() const;
	FText GetTrilinearLabel() const;
	FText GetOriginText() const;
	FText GetNormalText() const;

	/** True while Op is the selected aggregation - drives the button's highlight. */
	bool IsSlabOpSelected(EFlowVizSlabOp Op) const;

	void NotifySliceChanged() const;

	/** Subscriber for edits. Unbound is legal and inert -- see the SLATE_EVENT. */
	FSimpleDelegate OnSliceChanged;

	/** Borrowed. Null is legal and inert. */
	FFlowVizSliceViewModel* ViewModel = nullptr;

	TStaticArray<TSharedPtr<SButton>, 3> AxisButtons;

	/** Indexed by EFlowVizSlabOp. Slot 0 (None) stays null on purpose. */
	TStaticArray<TSharedPtr<SButton>, 4> SlabOpButtons;

	TSharedPtr<SFlowVizScrubSlider> PositionSlider;
	TSharedPtr<SFlowVizScrubSlider> OpacitySlider;
	TSharedPtr<SFlowVizNumericEntry> ThicknessBox;
	TSharedPtr<SFlowVizNumericEntry> SlabSamplesBox;
	TSharedPtr<SButton> VisibleButton;
	TSharedPtr<SButton> ShowWidgetButton;
	TSharedPtr<SButton> TrilinearButton;
};
