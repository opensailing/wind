// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UI/SFlowVizTransferFunctionPanel.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/SCompoundWidget.h"

class FFlowVizTimelineViewModel;
class SButton;

/**
 * SSlider plus the one thing SSlider is missing: a way to drive it headlessly.
 *
 * WHY THIS SUBCLASS EXISTS AT ALL. SButton has SimulateClick() precisely so a
 * test can exercise the production OnClicked delegate without a mouse. SSlider
 * has no equivalent: its OnValueChanged member is PRIVATE (SSlider.h:314) and
 * the CommitValue() that fires it is PROTECTED (SSlider.h:199). So from outside,
 * the only public way to move a slider is SetValue() - and SetValue only assigns
 * the value attribute, it does NOT fire the delegate.
 *
 * That distinction is the whole reason for this class. A test written against
 * SetValue() would move the handle and assert the view model changed; that
 * assertion CANNOT PASS, because no handler ran. Worse is the mirror image: a
 * test that asserted the view model did NOT change would pass on a slider wired
 * to nothing, because the path it drove could never have fired the handler
 * either way. A check whose outcome does not depend on the wiring is not a
 * check. Subclassing puts CommitValue() in reach, so SimulateDrag() runs the
 * SAME code path a mouse drag runs (SSlider::OnMouseMove -> CommitValue).
 *
 * IT IS NOT TEST-ONLY. The workspace builds this type in production, so the code
 * under test is the code that ships. A slider swapped in only for tests would be
 * testing a widget no user ever touches.
 */
class FLOWVIZRUNTIME_API SFlowVizScrubSlider : public SSlider
{
public:
	/**
	 * Move the handle exactly as a drag does, firing OnValueChanged.
	 *
	 * ONE TRAP, DOCUMENTED BECAUSE IT LOOKS LIKE A BROKEN HANDLER. CommitValue is
	 * `if (NewValue != OldValue)` (SSlider.cpp:419), so committing the value the
	 * slider already holds is a silent no-op and the delegate never fires. A test
	 * that drags to the slider's current position therefore observes nothing and
	 * reads that as unwired. Always drag to a value you have established is
	 * different from the current one.
	 */
	void SimulateDrag(float NewValue) { CommitValue(NewValue); }
};

/**
 * The timeline transport: play/pause, step, scrub, speed, and the fidelity
 * badge (plan.md sections 11 "Timeline" and 8).
 *
 * IT OWNS NO STATE. Every control reads through to the bound
 * FFlowVizTimelineViewModel on each paint, via TAttribute lambdas. There is no
 * cached bIsPlaying, no cached frame index, no refresh timer.
 *
 * That is not a stylistic preference. Playback advances on its own, a session
 * load rewrites the playhead, and a keyboard shortcut can call the view model
 * directly - so any value this widget cached would be stale the moment
 * something other than a click changed it, and would stay stale until the user
 * happened to touch the control. The symptom is a pause button that still says
 * "Play" while the case is running, which reads as an input bug and is actually
 * a duplicated model. Binding through means the widget cannot disagree with the
 * view model, because it has nothing to disagree with.
 *
 * ENGINEERING RULE 15 IS ENFORCED BY BINDING IsEnabled TO THE VIEW MODEL'S OWN
 * PREDICATE. CanPlay(), CanStep() and CanScrub() already decide what is legal,
 * and the view model refuses a call those predicates reject. Wiring IsEnabled
 * to the same predicate that guards the action makes "disabled" and "refuses"
 * one fact rather than two that can drift apart.
 *
 * THE BADGE IS RULE 7'S DISCLOSURE AND IS NOT DECORATIVE. "Any visual temporal
 * interpolation must be visibly identified as interpolation." The badge renders
 * FFlowVizTimelineViewModel::GetBadge - which is derived from what is ON SCREEN,
 * not from what was requested - and colours an interpolated or held frame with
 * the advisory/warning tokens. Removing it would make the viewer silently
 * present a blend as though it were stored data.
 *
 * A NULL VIEW MODEL IS A LEGAL, INERT STATE. The workspace builds its panels
 * before a case is open, so every accessor here tolerates an unbound model by
 * reporting "nothing available" - which disables every control rather than
 * crashing or offering buttons that do nothing.
 */
class FLOWVIZRUNTIME_API SFlowVizTransportBar : public SCompoundWidget
{
public:
	/**
	 * THE INITIALIZER IS LOAD-BEARING. SLATE_ARGUMENT expands to a bare
	 * `ArgType _ArgName;` with NO initializer (DeclarativeSyntaxSupport.h:180), so
	 * a pointer argument left unset holds INDETERMINATE memory rather than null.
	 * `SNew(SFlowVizTransportBar)` with no .TimelineViewModel() therefore hands
	 * Construct a garbage pointer, every null check passes, and the first call
	 * through it is a SIGBUS.
	 *
	 * This is not hypothetical: the sibling panel shipped without this line and
	 * crashed the automation worker inside its own "unbound" test, which is the
	 * one test whose entire purpose is to exercise the no-view-model path. The
	 * transport bar passed at the time only because its garbage happened to be
	 * benign that run - a latent crash, not a working widget.
	 */
	SLATE_BEGIN_ARGS(SFlowVizTransportBar)
		: _TimelineViewModel(nullptr)
	{
	}
		/** Borrowed, not owned. Must outlive this widget - the workspace owns both. */
		SLATE_ARGUMENT(FFlowVizTimelineViewModel*, TimelineViewModel)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/**
	 * The play/pause button, exposed so a test can drive it.
	 *
	 * WHY THIS IS PUBLIC. A binding test must press the REAL button and observe
	 * the REAL view model change. A test that instead called
	 * ViewModel->TogglePlayPause() directly would pass on a widget whose OnClicked
	 * was never wired - which is precisely the mocked-seam defect this whole task
	 * exists to close. SButton::SimulateClick() invokes the same delegate a mouse
	 * release does, so pressing this in a test exercises the production wiring.
	 */
	TSharedPtr<SButton> GetPlayPauseButton() const { return PlayPauseButton; }

	TSharedPtr<SButton> GetStepForwardButton() const { return StepForwardButton; }
	TSharedPtr<SButton> GetStepBackwardButton() const { return StepBackwardButton; }
	TSharedPtr<SButton> GetFirstFrameButton() const { return FirstFrameButton; }
	TSharedPtr<SButton> GetLastFrameButton() const { return LastFrameButton; }

	/** The scrub slider, exposed for the same reason as the buttons - see SFlowVizScrubSlider::SimulateDrag. */
	TSharedPtr<SFlowVizScrubSlider> GetScrubSlider() const { return ScrubSlider; }

	/* --- The options cluster (#75). ---------------------------------------- */
	/*
	 * These five view model setters had NO production caller: loop, playback
	 * mode, speed and interpolation were welded to the player's defaults the
	 * way the render settings were welded to the view model's (#74). Same
	 * defect, one panel over -- check_uncalled_setters.sh is what found it.
	 */

	/** Cycles Loop -> PingPong -> Once -> Loop. Its label names the mode IN FORCE. */
	TSharedPtr<SButton> GetLoopModeButton() const { return LoopModeButton; }

	/** Toggles Sequence <-> RealTime. */
	TSharedPtr<SButton> GetPlaybackModeButton() const { return PlaybackModeButton; }

	/** The button for FlowVizPlayback::SpeedPresets[Index], or null out of range. */
	TSharedPtr<SButton> GetSpeedPresetButton(int32 Index) const;

	/** Free-typed speed. Zero and non-finite are refused by the model and the box snaps back. */
	TSharedPtr<SFlowVizNumericEntry> GetCustomSpeedBox() const { return CustomSpeedBox; }

	/** Interpolation on/off. Off shows stored frames only. */
	TSharedPtr<SButton> GetInterpolationButton() const { return InterpolationButton; }

	/** What the fidelity badge currently reads. Exposed so a test can assert rule 7's disclosure is present. */
	FText GetBadgeText() const;

	/** The badge's colour, which distinguishes an advisory from a warning. */
	FSlateColor GetBadgeColor() const;

	/** The frame counter, e.g. "12 / 40". */
	FText GetFrameCounterText() const;

	/** Physical time with its unit, e.g. "0.240 s". */
	FText GetTimeText() const;

private:
	/* --- Bound reads. Each tolerates a null view model. -------------------- */

	bool IsPlayEnabled() const;
	bool IsStepEnabled() const;
	bool IsScrubEnabled() const;
	float GetScrubValue() const;
	FText GetPlayPauseLabel() const;

	/* --- Actions ----------------------------------------------------------- */

	FReply OnPlayPauseClicked();
	FReply OnStepForwardClicked();
	FReply OnStepBackwardClicked();
	FReply OnFirstFrameClicked();
	FReply OnLastFrameClicked();
	void OnScrubValueChanged(float NewValue);

	FReply OnLoopModeClicked();
	FReply OnPlaybackModeClicked();
	FReply OnSpeedPresetClicked(int32 PresetIndex);
	void OnCustomSpeedCommitted(const FText& NewText, ETextCommit::Type CommitType);
	FReply OnInterpolationClicked();

	FText GetLoopModeLabel() const;
	FText GetPlaybackModeLabel() const;
	FText GetInterpolationLabel() const;
	FText GetCustomSpeedText() const;

	/** Borrowed. Null until a workspace hands one over, which is a legal inert state. */
	FFlowVizTimelineViewModel* ViewModel = nullptr;

	TSharedPtr<SButton> PlayPauseButton;
	TSharedPtr<SButton> StepForwardButton;
	TSharedPtr<SButton> StepBackwardButton;
	TSharedPtr<SButton> FirstFrameButton;
	TSharedPtr<SButton> LastFrameButton;
	TSharedPtr<SFlowVizScrubSlider> ScrubSlider;

	TSharedPtr<SButton> LoopModeButton;
	TSharedPtr<SButton> PlaybackModeButton;
	TArray<TSharedPtr<SButton>> SpeedPresetButtons;
	TSharedPtr<SFlowVizNumericEntry> CustomSpeedBox;
	TSharedPtr<SButton> InterpolationButton;
};
