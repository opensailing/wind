// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CFDViz/CFDVizColorMaps.h"
#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SLeafWidget.h"

class FFlowVizTransferFunctionViewModel;
class SButton;

/**
 * SEditableTextBox plus a way to commit text headlessly.
 *
 * SAME PROBLEM AS SFlowVizScrubSlider, SAME SHAPE OF FIX. SEditableTextBox's
 * OnTextCommitted delegate is PROTECTED (SEditableTextBox.h:495), and so is the
 * OnEditableTextCommitted() that fires it (:451). The public SetText() only
 * changes the displayed string - it does NOT commit, so it never runs the
 * handler. A test that called SetText and asserted the view model changed could
 * not pass no matter how correct the wiring was.
 *
 * SimulateCommit routes through the protected OnEditableTextCommitted, which is
 * the exact function the inner SEditableText calls when the user presses Enter -
 * so the verification and error-clearing behaviour a real commit gets is
 * exercised too, rather than bypassed by poking the delegate directly.
 *
 * Built in production by the panels, so this is the widget users type into.
 */
class FLOWVIZRUNTIME_API SFlowVizNumericEntry : public SEditableTextBox
{
public:
	/** Commit text exactly as pressing Enter does, running verification and firing OnTextCommitted. */
	void SimulateCommit(const FText& InText)
	{
		SetText(InText);
		OnEditableTextCommitted(InText, ETextCommit::OnEnter);
	}
};

/**
 * SSlider plus a way to drag it headlessly.
 *
 * SAME PROBLEM AS SFlowVizNumericEntry, SAME SHAPE OF FIX. SSlider::SetValue
 * (SSlider.h:133) only replaces the value ATTRIBUTE -- it moves the handle and
 * fires nothing. The function that actually notifies is CommitValue()
 * (SSlider.h:199), which is protected, and OnValueChanged (SSlider.h:314) is
 * private. So a test that called SetValue and asserted the view model changed
 * could not pass no matter how correct the wiring was.
 *
 * SimulateDrag routes through the protected CommitValue, which is the exact
 * function SSlider calls on a mouse drag -- so the notification path a real drag
 * takes is exercised rather than bypassed by poking the panel's handler.
 *
 * Built in production by the panel, so this is the slider users drag.
 */
class FLOWVIZRUNTIME_API SFlowVizOpacitySlider : public SSlider
{
public:
	/** Move the handle exactly as a drag does, firing OnValueChanged. */
	void SimulateDrag(float NewValue)
	{
		CommitValue(NewValue);
	}
};

/**
 * The colour ramp strip: the colormap drawn as a real gradient, with the opacity
 * curve overlaid on it.
 *
 * WHY THIS IS A CUSTOM-PAINTED LEAF AND NOT A ROW OF COLOURED BOXES. A ramp built
 * from N SImages is a QUANTIZED ramp - the user sees N bands and reads the banding
 * as structure in the data. Painting an actual gradient with per-stop colours
 * sampled from the real colormap table means what is on screen is the colormap,
 * at whatever resolution the display has, and the "colour bands" control then
 * shows quantization ONLY when the user asked for it.
 *
 * THE OPACITY CURVE IS DRAWN OVER THE RAMP, NOT BESIDE IT. Opacity multiplies
 * colour, so the two are one function of the same normalized axis. Two separate
 * widgets side by side force the user to mentally align two x-axes to answer
 * "what does a value of 0.7 look like" - which is the question the panel exists
 * to answer.
 */
class FLOWVIZRUNTIME_API SFlowVizColorRampStrip : public SLeafWidget
{
public:
	// nullptr initializer required - see the note on SFlowVizTransportBar's
	// FArguments. SLATE_ARGUMENT does not zero-initialize, so an omitted pointer
	// argument is indeterminate rather than null.
	SLATE_BEGIN_ARGS(SFlowVizColorRampStrip)
		: _ViewModel(nullptr)
	{
	}
		SLATE_ARGUMENT(FFlowVizTransferFunctionViewModel*, ViewModel)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId,
		const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

	virtual FVector2D ComputeDesiredSize(float) const override;

	/**
	 * The gradient stops this strip would paint right now.
	 *
	 * EXPOSED FOR TESTING, and it is the honest seam for a painted widget: OnPaint
	 * writes into a draw-element list that a headless test cannot meaningfully
	 * inspect, so the test would otherwise be reduced to "did constructing it
	 * crash". Having OnPaint and the test call the SAME function means a test that
	 * asserts "the stops follow the selected colormap" is asserting something about
	 * the pixels, not about a parallel code path built for it.
	 */
	TArray<FLinearColor> BuildRampColors(int32 SampleCount) const;

private:
	FFlowVizTransferFunctionViewModel* ViewModel = nullptr;
};

/**
 * The transfer-function editor (plan.md section 11 "Color/opacity editor", and
 * engineering rule 8).
 *
 * NO STATE. Every control binds to FFlowVizTransferFunctionViewModel.
 *
 * ENGINEERING RULE 8 IS THE REASON THE RANGE SECTION LOOKS THE WAY IT DOES.
 * "Stable global color ranges by default; per-frame ranges must be opt-in and
 * visibly indicated." So: Global is the default the view model already enforces,
 * and when the user opts into CurrentFrame the panel shows an ADVISORY-COLOURED
 * banner saying the colours will change between frames. That banner is not
 * decoration - without it, a user comparing two frames side by side would read a
 * colour difference as a physical difference when it is only a rescale.
 *
 * THE COLORMAP LIST WARNS ABOUT NON-UNIFORM MAPS for the same reason. Turbo is
 * offered because reviewers ask for it, and marked because a rainbow map
 * fabricates edges where the data is smooth.
 */
class FLOWVIZRUNTIME_API SFlowVizTransferFunctionPanel : public SCompoundWidget
{
public:
	// nullptr initializer required - see the note on SFlowVizTransportBar's
	// FArguments. Omitting it here is what crashed
	// FlowViz.UI.TransferFunctionPanel.Unbound with a SIGBUS inside SetColorMap.
	SLATE_BEGIN_ARGS(SFlowVizTransferFunctionPanel)
		: _ViewModel(nullptr)
	{
	}
		/** Borrowed, not owned. The workspace owns both. */
		SLATE_ARGUMENT(FFlowVizTransferFunctionViewModel*, ViewModel)

		/**
		 * Fired after an edit that CHANGES THE IMAGE. The workspace subscribes and
		 * pushes the transfer function into the bound volume; nobody else should.
		 *
		 * THIS PANEL HAD NO SUCH CHANNEL AT ALL until the transfer function was
		 * wired to the renderer. SFlowVizClipPanel has carried OnClipChanged since
		 * it was written, so "Clipping" edits reached the render thread while
		 * "Color & Opacity" edits reached only the view model -- every colormap,
		 * range and opacity control in the workspace was inert by construction,
		 * and the panel looked correct because the ramp strip it paints reads the
		 * same view model the buttons write.
		 *
		 * Unbound is legal and inert, exactly as OnClipChanged is: the panel is
		 * constructed before the workspace has a volume to push into, and a
		 * standalone panel in a test has nothing to announce to.
		 */
		SLATE_EVENT(FSimpleDelegate, OnTransferFunctionChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/* --- Test seams. See SFlowVizTransportBar.h for why these are public. --- */

	/** The button that selects a given colormap, or null if that map has no button. */
	TSharedPtr<SButton> GetColorMapButton(ECFDVizColorMap Map) const;

	/** The button that selects a given range source. */
	TSharedPtr<SButton> GetRangeSourceButton(int32 SourceIndex) const;

	TSharedPtr<SButton> GetReverseButton() const { return ReverseButton; }
	TSharedPtr<SButton> GetResetRangeButton() const { return ResetRangeButton; }

	TSharedPtr<SFlowVizNumericEntry> GetRangeMinBox() const { return RangeMinBox; }
	TSharedPtr<SFlowVizNumericEntry> GetRangeMaxBox() const { return RangeMaxBox; }

	TSharedPtr<SFlowVizColorRampStrip> GetRampStrip() const { return RampStrip; }

	/** The opacity slider. Drag it headlessly with SimulateDrag -- see the type. */
	TSharedPtr<SFlowVizOpacitySlider> GetOpacitySlider() const { return OpacitySlider; }

	/** The rule 8 advisory. Empty when the range is stable, non-empty when it is per-frame. */
	FText GetRangeAdvisoryText() const;

	/** Whether the rule 8 advisory is currently visible. */
	bool IsRangeAdvisoryVisible() const;

	/** The perceptual-uniformity warning for the selected map. Empty when the map is uniform. */
	FText GetColorMapWarningText() const;

private:
	bool IsBound() const;

	/**
	 * Fire OnTransferFunctionChanged.
	 *
	 * CALLED ONLY WHEN THE EDIT ACTUALLY TOOK. Five of the six setters return
	 * FCFDVizResult and can refuse -- an out-of-order manual range, a range
	 * source with no field bound, an opacity multiplier outside its domain. A
	 * refused edit that announced anyway would push a byte-identical transfer
	 * function to the render thread and rebuild the LUT for a frame that cannot
	 * have changed.
	 *
	 * SetReverseColorMap returns void and cannot refuse, so its handler
	 * announces unconditionally. That asymmetry is real rather than an
	 * oversight, and the test asserts both halves.
	 */
	void NotifyTransferFunctionChanged() const;

	FReply OnColorMapClicked(ECFDVizColorMap Map);
	FReply OnRangeSourceClicked(int32 SourceIndex);
	FReply OnReverseClicked();
	FReply OnResetRangeClicked();
	void OnRangeMinCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void OnRangeMaxCommitted(const FText& NewText, ETextCommit::Type CommitType);
	void OnOpacityMultiplierChanged(float NewValue);

	FText GetRangeMinText() const;
	FText GetRangeMaxText() const;
	FText GetFieldLabelText() const;
	float GetOpacityMultiplier() const;
	bool IsManualRangeEditable() const;

	FFlowVizTransferFunctionViewModel* ViewModel = nullptr;

	/** Subscriber for edits. Unbound is legal and inert -- see the SLATE_EVENT. */
	FSimpleDelegate OnTransferFunctionChanged;

	TMap<ECFDVizColorMap, TSharedPtr<SButton>> ColorMapButtons;
	TArray<TSharedPtr<SButton>> RangeSourceButtons;
	TSharedPtr<SButton> ReverseButton;
	TSharedPtr<SButton> ResetRangeButton;
	TSharedPtr<SFlowVizNumericEntry> RangeMinBox;
	TSharedPtr<SFlowVizNumericEntry> RangeMaxBox;
	TSharedPtr<SFlowVizColorRampStrip> RampStrip;
	TSharedPtr<SFlowVizOpacitySlider> OpacitySlider;
};
