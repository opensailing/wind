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
 * WHAT THIS PANEL DOES NOT CLAIM, AND WHY IT IS SAID HERE RATHER THAN LEFT TO BE
 * DISCOVERED. The RENDER PATH now clips: FlowVizVolumeRayMarchDispatcher calls
 * FFlowVizClipViewModel::ApplyToRayMarchParameters on the proxy's own clip model,
 * fed from UCFDVizVolumeComponent::SetClip. That is not the gap. The gap is the
 * CHANNEL FROM THIS PANEL: this panel edits FFlowVizWorkspaceModel::Clip, and
 * nothing copies that model into a volume component. Grep SetClip - every
 * production caller is UFlowVizCaptureLibrary (AddVolumeClipPlane /
 * ClearVolumeClipping), which builds its own model from Volume->GetClip(). So a
 * plane added HERE is authored, validated, listed and persisted, and still does
 * not change a pixel - while the identical plane added through the capture
 * library does.
 *
 * THE DISTINCTION MATTERS BECAUSE IT CHANGES WHAT WOULD RETIRE THE ADVISORY. It
 * is no longer "someone calls ApplyToRayMarchParameters from the render path" -
 * that has happened. It is "something pushes FFlowVizWorkspaceModel::Clip into a
 * UCFDVizVolumeComponent", and the same is true of the sibling RenderSettings
 * model, which has exactly this shape. Deleting the advisory on the strength of
 * the render path being wired would make the panel claim a channel it does not
 * have - the same lie, told the other way round.
 *
 * THAT IS DISCLOSED IN THE PANEL ITSELF, not only in this comment. Construct
 * builds an advisory strip saying so. The alternative - shipping controls that
 * look exactly like working ones - is the failure engineering rule 15 exists to
 * prevent, and it is the more dangerous version of it: a plane that does nothing
 * looks identical to a plane pointing the wrong way, so a user would spend the
 * afternoon debugging their normals. The controls are live against the model
 * because the model is what a session saves and what the render path will read
 * when someone wires this channel; the advisory is what keeps that from being a
 * lie.
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
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

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

	/** Clear every plane. */
	TSharedPtr<SButton> GetRemoveAllButton() const { return RemoveAllButton; }

	/** Crop box entry for Axis (0=X, 1=Y, 2=Z), in SOLVER units. */
	TSharedPtr<SFlowVizNumericEntry> GetCropMinBox(int32 Axis) const;
	TSharedPtr<SFlowVizNumericEntry> GetCropMaxBox(int32 Axis) const;

	TSharedPtr<SButton> GetResetCropButton() const { return ResetCropButton; }

	/** The "these planes do not reach the renderer yet" disclosure. Never empty. */
	FText GetNotWiredAdvisoryText() const;

	/** How many plane rows are currently built. Derived from the view model, never cached. */
	int32 GetPlaneRowCount() const { return PlaneRows.Num(); }

private:
	/** One built row of plane controls. Rebuilt wholesale when the plane count changes. */
	struct FPlaneRow
	{
		TSharedPtr<SButton> EnableButton;
		TSharedPtr<SButton> InvertButton;
		TSharedPtr<SButton> RemoveButton;
	};

	bool IsBound() const { return ViewModel != nullptr; }

	/** Rule 15: another plane would fit AND there is a model to put it in. */
	bool CanAddPlane() const;

	/** True while the plane at Index exists and is enabled - drives the toggle's label. */
	bool IsPlaneEnabled(int32 Index) const;

	FReply OnPresetClicked(EFlowVizClipPreset Preset);
	FReply OnPlaneEnableClicked(int32 Index);
	FReply OnPlaneInvertClicked(int32 Index);
	FReply OnPlaneRemoveClicked(int32 Index);
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

	TMap<EFlowVizClipPreset, TSharedPtr<SButton>> PresetButtons;
	TArray<FPlaneRow> PlaneRows;
	TSharedPtr<SVerticalBox> PlaneListBox;
	TSharedPtr<SButton> RemoveAllButton;
	TSharedPtr<SButton> ResetCropButton;

	/** Indexed by axis: 0=X, 1=Y, 2=Z. */
	TStaticArray<TSharedPtr<SFlowVizNumericEntry>, 3> CropMinBoxes;
	TStaticArray<TSharedPtr<SFlowVizNumericEntry>, 3> CropMaxBoxes;
};
