// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UI/FlowVizClipViewModel.h"
#include "UI/SFlowVizTransferFunctionPanel.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SButton;
class SVerticalBox;

/**
 * Clip planes and the crop box (plan.md sections 10.5 and 5F).
 *
 * NO STATE. Every control reads through to FFlowVizClipViewModel on each paint.
 * The plane list in particular is REBUILT from the view model rather than
 * mirrored into a widget-side array: a mirrored list is a second copy of the
 * model, and the first thing that changes the planes without going through this
 * panel - a session load, a preset applied from elsewhere - leaves the two
 * disagreeing, with the UI showing planes that are not being rendered.
 *
 * THE CHANNEL TO THE RENDERER, AND WHY IT IS AN EVENT RATHER THAN A CALL. This
 * panel edits a view model and fires OnClipChanged; the workspace subscribes and
 * calls FFlowVizWorkspaceModel::PushClipToVolume. The panel therefore never
 * mentions a volume component, which is what lets it be built and operated
 * before any case is open.
 *
 * THAT CHANNEL DID NOT EXIST UNTIL IT DID, and the panel used to say so. The
 * render path clipped long before this panel reached it: the dispatcher calls
 * FFlowVizClipViewModel::ApplyToRayMarchParameters on the proxy's own model, fed
 * from UCFDVizVolumeComponent::SetClip. What was missing was this direction -
 * every production SetClip caller was UFlowVizCaptureLibrary, building its own
 * model from Volume->GetClip(). So a plane added HERE was authored, validated,
 * listed and persisted, and did not change a pixel, while the identical plane
 * added from Python did.
 *
 * THE ADVISORY THAT SAID SO IS NOW CONDITIONAL, NOT DELETED. It is shown while
 * no volume is bound, because then the claim is still true - the workspace is
 * built before a case is open, and a panel whose controls really are inert must
 * say so. It retires on SetVolumeBound(true). Deleting it outright would have
 * been the same lie in reverse the first time a user opened the workspace with
 * no case: controls that look exactly like working ones, which is the failure
 * engineering rule 15 exists to prevent, in its more dangerous form - a plane
 * that does nothing looks identical to a plane pointing the wrong way, so the
 * afternoon goes on debugging normals.
 *
 * A NULL VIEW MODEL IS A LEGAL, INERT STATE - the workspace builds panels before
 * a case is open. Every accessor tolerates it by reporting "nothing available",
 * which disables the controls rather than crashing.
 */
class FLOWVIZRUNTIME_API SFlowVizClipPanel : public SCompoundWidget
{
public:
	/**
	 * THE INITIALIZER IS LOAD-BEARING. SLATE_ARGUMENT expands to a bare
	 * `ArgType _ArgName;` with no initializer, so an omitted pointer argument
	 * holds INDETERMINATE memory rather than null - every null check then passes
	 * and the first call through it is a SIGBUS. That is not hypothetical: it is
	 * what crashed FlowViz.UI.TransferFunctionPanel.Unbound before its sibling
	 * header grew this same line.
	 */
	SLATE_BEGIN_ARGS(SFlowVizClipPanel)
		: _ViewModel(nullptr)
	{
	}
		/** Borrowed, not owned. The workspace owns both. */
		SLATE_ARGUMENT(FFlowVizClipViewModel*, ViewModel)

		/**
		 * Fired after any control here has CHANGED the view model.
		 *
		 * THE PANEL DOES NOT KNOW WHAT A VOLUME IS, and this is how it stays that
		 * way. The workspace subscribes and pushes; a panel that reached for a
		 * component itself would need one to be operable, and it is built before
		 * any case is open.
		 *
		 * FIRED ONLY ON AN ACTUAL CHANGE, not on every click. AddPresetPlane
		 * refuses at MaxClipPlanes and with no domain, and a push on a refused
		 * edit would be a render-thread flush for a frame that is identical.
		 */
		SLATE_EVENT(FSimpleDelegate, OnClipChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/**
	 * Tell the panel whether its edits now reach a renderer.
	 *
	 * DRIVES THE ADVISORY, and nothing else. The panel does not use this to
	 * decide whether to fire OnClipChanged - a subscriber that is bound has
	 * already decided it wants the callbacks, and making the panel second-guess
	 * that would be two opinions about one question.
	 */
	void SetVolumeBound(bool bInVolumeBound);

	/* --- Test seams. See SFlowVizTransportBar.h for why these are public. --- */

	/**
	 * The button that adds a preset plane facing that way.
	 *
	 * A test must press the REAL button and observe the REAL view model change;
	 * calling Clip.AddPresetPlane() directly would pass on a panel whose OnClicked
	 * was never wired, which is exactly the defect this seam exists to expose.
	 */
	TSharedPtr<SButton> GetPresetButton(EFlowVizClipPreset Preset) const;

	/** Show/hide toggle for the plane at Index, or null if there is no such row. */
	TSharedPtr<SButton> GetPlaneEnableButton(int32 Index) const;

	/** Flip which side of the plane at Index is kept. */
	TSharedPtr<SButton> GetPlaneInvertButton(int32 Index) const;

	/** Delete the plane at Index. */
	TSharedPtr<SButton> GetPlaneRemoveButton(int32 Index) const;

	/**
	 * Edit the plane's distance in place (#75: SetPlane had no production
	 * caller -- planes could be added, hidden, inverted and deleted, but not
	 * MOVED without delete-and-re-add, which loses the enabled state).
	 */
	TSharedPtr<SFlowVizNumericEntry> GetPlaneDistanceBox(int32 Index) const;

	/** Clear every plane. */
	TSharedPtr<SButton> GetRemoveAllButton() const { return RemoveAllButton; }

	/** Crop box entry for Axis (0=X, 1=Y, 2=Z), in SOLVER units. */
	TSharedPtr<SFlowVizNumericEntry> GetCropMinBox(int32 Axis) const;
	TSharedPtr<SFlowVizNumericEntry> GetCropMaxBox(int32 Axis) const;

	TSharedPtr<SButton> GetResetCropButton() const { return ResetCropButton; }

	/** The "these planes do not reach the renderer yet" disclosure. Never empty. */
	FText GetNotWiredAdvisoryText() const;

	/**
	 * Whether that disclosure is currently shown.
	 *
	 * TRUE UNTIL A VOLUME IS BOUND. The advisory used to be unconditional, and
	 * nothing asserted on it - so it would have outlived its subject silently the
	 * moment the channel opened, telling users a live control does nothing. That
	 * is the same wasted afternoon it was written to prevent, pointed the other
	 * way.
	 */
	bool IsNotWiredAdvisoryVisible() const { return !bVolumeBound; }

	/** How many plane rows are currently built. Derived from the view model, never cached. */
	int32 GetPlaneRowCount() const { return PlaneRows.Num(); }

	/** Rebuild explicit rows after an external model rewrite such as session load. */
	void RefreshFromModel() { RebuildPlaneRows(); }

private:
	/** One built row of plane controls. Rebuilt wholesale when the plane count changes. */
	struct FPlaneRow
	{
		TSharedPtr<SButton> EnableButton;
		TSharedPtr<SButton> InvertButton;
		TSharedPtr<SButton> RemoveButton;
		TSharedPtr<SFlowVizNumericEntry> DistanceBox;
	};

	bool IsBound() const { return ViewModel != nullptr; }

	/** Visibility of the advisory strip, bound so it follows SetVolumeBound. */
	EVisibility GetNotWiredAdvisoryVisibility() const;

	/**
	 * Announce that the view model changed.
	 *
	 * Called ONLY where an edit actually took, never on a refused one - see the
	 * SLATE_EVENT comment above.
	 */
	void NotifyClipChanged() const;

	/** Rule 15: another plane would fit AND there is a model to put it in. */
	bool CanAddPlane() const;

	/** True while the plane at Index exists and is enabled - drives the toggle's label. */
	bool IsPlaneEnabled(int32 Index) const;

	FReply OnPresetClicked(EFlowVizClipPreset Preset);
	FReply OnPlaneEnableClicked(int32 Index);
	FReply OnPlaneInvertClicked(int32 Index);
	FReply OnPlaneRemoveClicked(int32 Index);
	void OnPlaneDistanceCommitted(const FText& NewText, ETextCommit::Type CommitType, int32 Index);
	FText GetPlaneDistanceText(int32 Index) const;
	FReply OnRemoveAllClicked();
	FReply OnResetCropClicked();

	void OnCropMinCommitted(const FText& NewText, ETextCommit::Type CommitType, int32 Axis);
	void OnCropMaxCommitted(const FText& NewText, ETextCommit::Type CommitType, int32 Axis);

	FText GetCropMinText(int32 Axis) const;
	FText GetCropMaxText(int32 Axis) const;
	FText GetPlaneLabelText(int32 Index) const;
	FText GetPlaneEnableGlyph(int32 Index) const;
	FText GetPlaneCountText() const;

	/**
	 * Rebuild the plane rows from the view model.
	 *
	 * Called from Construct and from every handler that changes the plane COUNT.
	 * Rows are rebuilt rather than patched because an index-keyed row is only
	 * valid for one arrangement of the array: removing plane 0 renumbers every
	 * row after it, and a patched list would leave row 1's buttons operating on
	 * what is now plane 2.
	 */
	void RebuildPlaneRows();

	FFlowVizClipViewModel* ViewModel = nullptr;

	/** Subscriber for edits. Unbound is legal and inert - see the SLATE_EVENT. */
	FSimpleDelegate OnClipChanged;

	/**
	 * NOT DERIVED FROM OnClipChanged.IsBound().
	 *
	 * The two are different questions and conflating them was tempting: a
	 * subscriber can be attached before any volume is, and the workspace does
	 * exactly that - it subscribes at Construct and binds a volume when a case
	 * opens. Deriving the advisory from the subscription would have retired it
	 * while the edits still reached nothing.
	 */
	bool bVolumeBound = false;

	TMap<EFlowVizClipPreset, TSharedPtr<SButton>> PresetButtons;
	TArray<FPlaneRow> PlaneRows;
	TSharedPtr<SVerticalBox> PlaneListBox;
	TSharedPtr<SButton> RemoveAllButton;
	TSharedPtr<SButton> ResetCropButton;

	/** Indexed by axis: 0=X, 1=Y, 2=Z. */
	TStaticArray<TSharedPtr<SFlowVizNumericEntry>, 3> CropMinBoxes;
	TStaticArray<TSharedPtr<SFlowVizNumericEntry>, 3> CropMaxBoxes;
};
