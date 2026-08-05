// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizTransportBar.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSlider.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/FlowVizSlateAttributePump.h"

/**
 * Does pressing the actual button actually drive the actual view model?
 *
 * WHY THIS TEST IS SHAPED THE WAY IT IS. The view models already have thorough
 * suites, and every one of those tests calls the view model's methods directly.
 * That means all of them would pass on a workspace whose buttons were wired to
 * nothing at all - which is the exact defect this task was created to fix, and
 * the same one the render layer shipped with (see FlowVizRenderWiringTest.cpp:
 * 49/49 green while no volume drew).
 *
 * So these tests never call a transport method directly. They press the widget,
 * through SButton::SimulateClick, which invokes the SAME OnClicked delegate a
 * mouse release does. If Construct forgets to bind OnClicked, or binds it to a
 * lambda that does nothing, SimulateClick runs that nothing and the assertion
 * about the view model's state fails. THAT is what makes this a check rather
 * than a restatement.
 *
 * THE DIFFERENTIAL PROPERTY, STATED SO A FUTURE READER CAN RE-VERIFY IT.
 * Comment out the `.OnClicked(...)` line for the play/pause button in
 * SFlowVizTransportBar::Construct, rebuild, and
 * FlowViz.UI.TransportBar.ButtonsDriveTheViewModel must go RED on the "pressing
 * play starts playback" assertion. If it stays green, this file is decorative
 * and should be treated as a defect.
 *
 * WHY A REAL CASE RATHER THAN A HAND-BUILT PLAYER. CanPlay() is false without a
 * multi-frame case, so on an empty player every button is legitimately disabled
 * and every "did the click work" assertion would be vacuous - a test that
 * passes because nothing can happen. The sample case makes the controls live.
 */

// NAMED namespace: FlowVizRuntime is a unity build, so anonymous namespaces from
// every .cpp merge and same-named helpers collide across test files.
namespace FlowVizTransportBarTest
{
	FString GetSampleCaseDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FlowVizRuntime"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		const FString ProjectDir = FPaths::GetPath(FPaths::GetPath(Plugin->GetBaseDir()));
		return FPaths::Combine(ProjectDir, TEXT("Samples"), TEXT("MockCylinderWake.cfdviz"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTransportBarBindingTest,
	"FlowViz.UI.TransportBar.ButtonsDriveTheViewModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizTransportBarBindingTest::RunTest(const FString& Parameters)
{
	const FString CaseDir = FlowVizTransportBarTest::GetSampleCaseDir();
	if (CaseDir.IsEmpty() || !FPaths::DirectoryExists(CaseDir))
	{
		// An error, not a silent skip: a skipped test still reports Result={Success}.
		AddError(FString::Printf(
			TEXT("the sample case is required to make the transport controls live; not found at '%s'"),
			*CaseDir));
		return false;
	}

	FFlowVizWorkspaceModel Workspace;
	const FCFDVizResult OpenResult = Workspace.OpenCase(CaseDir);
	if (!OpenResult.IsOk())
	{
		AddError(FString::Printf(TEXT("could not open the sample case: %s"), *OpenResult.ToString()));
		return false;
	}

	// PRECONDITION, ASSERTED RATHER THAN ASSUMED. If the transport were not
	// enabled, every click below would be a legal no-op and this whole test would
	// pass while proving nothing.
	if (!TestTrue(
			TEXT("the sample case makes the transport live, so a click can have an observable effect"),
			Workspace.Timeline.CanPlay()))
	{
		return false;
	}

	const TSharedRef<SFlowVizTransportBar> Bar = SNew(SFlowVizTransportBar)
		.TimelineViewModel(&Workspace.Timeline);

	/* == The widget actually built its controls ============================= */

	const TSharedPtr<SButton> PlayPause = Bar->GetPlayPauseButton();
	const TSharedPtr<SButton> StepForward = Bar->GetStepForwardButton();
	const TSharedPtr<SButton> LastFrame = Bar->GetLastFrameButton();
	const TSharedPtr<SFlowVizScrubSlider> Scrub = Bar->GetScrubSlider();

	if (!TestTrue(TEXT("the transport bar built a play/pause button"), PlayPause.IsValid())
		|| !TestTrue(TEXT("the transport bar built a step-forward button"), StepForward.IsValid())
		|| !TestTrue(TEXT("the transport bar built a last-frame button"), LastFrame.IsValid())
		|| !TestTrue(TEXT("the transport bar built a scrub slider"), Scrub.IsValid()))
	{
		return false;
	}

	/* == Rule 15: enablement follows the view model's own predicate ========== */
	{
		/*
		 * A DIFFERENTIAL PAIR, not a single TestTrue.
		 *
		 * `IsEnabled()` returns a cached attribute whose constructor default is
		 * `true` (SWidget.cpp:248) - see FlowVizSlateAttributePump.h. So an
		 * isolated `TestTrue(Button->IsEnabled())` passes on a button with no
		 * `.IsEnabled()` binding at all, and would keep passing if the binding
		 * were deleted. It cannot fail, so it is not a check.
		 *
		 * What can fail is the button CHANGING when only the view model changed.
		 * Closing the case makes CanPlay() false - the timeline unbinds its player
		 * (FlowVizWorkspaceModel.cpp:73) - and nothing here touches the widget.
		 * A button wired to nothing stays enabled and this goes red.
		 */
		FlowVizSlateAttributePump::Pump(Bar);

		TestTrue(
			TEXT("the play button is enabled while the view model says play is available"),
			PlayPause->IsEnabled());
		TestTrue(
			TEXT("the step button is enabled while the view model says stepping is available"),
			StepForward->IsEnabled());

		Workspace.CloseCase();
		TestFalse(TEXT("precondition: a closed case is not playable"),
			Workspace.Timeline.CanPlay());

		FlowVizSlateAttributePump::Pump(Bar);

		// THE LOAD-BEARING HALF. The widget was not touched; only the model was.
		TestFalse(
			TEXT("closing the case disables the play button, so the control follows the view "
				 "model rather than its own construction-time default (engineering rule 15)"),
			PlayPause->IsEnabled());
		TestFalse(
			TEXT("closing the case disables the step button"),
			StepForward->IsEnabled());

		// Restore the case for the blocks below. OpenCase closes first, so this is
		// a clean re-open rather than a second case layered on the first.
		const FCFDVizResult ReopenResult = Workspace.OpenCase(CaseDir);
		if (!TestTrue(
				*FString::Printf(TEXT("re-opening the sample case succeeds: %s"),
					*ReopenResult.ToString()),
				ReopenResult.IsOk()))
		{
			return false;
		}

		FlowVizSlateAttributePump::Pump(Bar);
		TestTrue(TEXT("re-opening the case re-enables the play button"), PlayPause->IsEnabled());
	}

	/* == Pressing play starts playback ====================================== */
	{
		// Establish the opposite state first, so the assertion cannot pass by
		// accident on a player that was already playing.
		Workspace.Player.Pause();
		TestFalse(TEXT("precondition: the player is paused"), Workspace.Player.IsPlaying());

		PlayPause->SimulateClick();

		// THE LOAD-BEARING ASSERTION. This reads the VIEW MODEL, having pressed the
		// WIDGET. Nothing in this test called Play(). If Construct did not wire
		// OnClicked to the view model, this is where it goes red.
		TestTrue(
			TEXT("pressing play starts playback; if this fails, the button is not wired to the "
				 "timeline view model"),
			Workspace.Timeline.IsPlaying());
	}

	/* == And pressing it again pauses - it is a toggle, not a one-way switch = */
	{
		PlayPause->SimulateClick();
		TestFalse(
			TEXT("pressing play/pause again pauses, so the button reads the live state rather "
				 "than always sending Play"),
			Workspace.Timeline.IsPlaying());
	}

	/* == Stepping moves the playhead AND pauses ============================= */
	{
		Workspace.Timeline.GoToFirstFrame();
		const double TimeBefore = Workspace.Timeline.GetPhysicalTime();

		// Start playing, so the "step pauses" rule has something to turn off. A
		// step taken while playing is overwritten by the next Tick, which is why
		// the view model pauses - see FFlowVizTimelineViewModel::StepForward.
		Workspace.Timeline.Play();
		TestTrue(TEXT("precondition: playing before the step"), Workspace.Timeline.IsPlaying());

		StepForward->SimulateClick();

		TestTrue(
			*FString::Printf(
				TEXT("pressing step-forward advances the playhead; before %g, after %g"),
				TimeBefore, Workspace.Timeline.GetPhysicalTime()),
			Workspace.Timeline.GetPhysicalTime() > TimeBefore);

		TestFalse(
			TEXT("a step pauses playback, so the frame the user stepped to is the one that stays"),
			Workspace.Timeline.IsPlaying());
	}

	/* == The jump buttons move the playhead ================================= */
	{
		Workspace.Timeline.GoToFirstFrame();
		const double FirstTime = Workspace.Timeline.GetPhysicalTime();

		LastFrame->SimulateClick();

		TestTrue(
			*FString::Printf(
				TEXT("pressing last-frame jumps forward; first %g, after %g"),
				FirstTime, Workspace.Timeline.GetPhysicalTime()),
			Workspace.Timeline.GetPhysicalTime() > FirstTime);
	}

	/* == The scrub slider seeks ============================================= */
	{
		Workspace.Timeline.GoToFirstFrame();
		const double Before = Workspace.Timeline.GetNormalizedTime();

		// SimulateDrag, NOT SetValue. SetValue assigns the value attribute and
		// returns without firing OnValueChanged, so a test built on it could never
		// observe the handler running - green or red would say the same thing.
		// SimulateDrag calls the protected CommitValue that SSlider::OnMouseMove
		// calls, which is the production path.
		const float TargetValue = 0.75f;

		// CommitValue early-outs when the new value equals the old one, so a drag
		// to where the handle already sits fires nothing and looks like a dead
		// handler. Assert we are actually moving it.
		if (!TestTrue(
				*FString::Printf(
					TEXT("the drag target %g differs from the handle's current position %g, so "
						 "CommitValue cannot early-out and swallow the event"),
					TargetValue, Scrub->GetValue()),
				!FMath::IsNearlyEqual(Scrub->GetValue(), TargetValue)))
		{
			return false;
		}

		Scrub->SimulateDrag(TargetValue);

		const double After = Workspace.Timeline.GetNormalizedTime();
		TestTrue(
			*FString::Printf(
				TEXT("dragging the scrub slider seeks the player; normalized time before %g, "
					 "after %g"),
				Before, After),
			After > Before);
	}

	/* == Rule 7: the fidelity badge discloses what is on screen ============= */
	{
		/*
		 * THE BADGE MUST TRACK THE VIEW MODEL, NOT MERELY BE NON-EMPTY.
		 *
		 * "Not empty" was the original assertion here and it was not a check.
		 * Mutation proved it: replacing GetBadgeText's whole lookup with a
		 * hard-coded EXACT left this green (Tools/mutants/ui-transport-bindings.txt,
		 * `badge_always_exact`, SURVIVED). A badge frozen on "EXACT" is precisely
		 * the rule 7 failure - it presents interpolated or stale pixels as stored
		 * data - so the one defect the assertion existed to catch was the one it
		 * could not see.
		 *
		 * So: drive the view model to two DIFFERENT badge states and require the
		 * widget's text to differ. Any constant fails this, whatever the constant.
		 */
		Workspace.CloseCase();
		TestEqual(TEXT("precondition: a closed case badges as NoCase"),
			Workspace.Timeline.GetBadge(), EFlowVizFrameBadge::NoCase);
		const FText ClosedBadge = Bar->GetBadgeText();

		const FCFDVizResult ReopenResult = Workspace.OpenCase(CaseDir);
		if (!TestTrue(*FString::Printf(TEXT("re-opening the sample case succeeds: %s"),
					*ReopenResult.ToString()),
				ReopenResult.IsOk()))
		{
			return false;
		}

		// An open case is anything BUT NoCase - which exact value depends on what
		// has finished decoding, and asserting a specific one would make this test
		// about decode timing rather than about the badge.
		if (!TestNotEqual(TEXT("precondition: an open case does not badge as NoCase"),
				Workspace.Timeline.GetBadge(), EFlowVizFrameBadge::NoCase))
		{
			return false;
		}
		const FText OpenBadge = Bar->GetBadgeText();

		// THE LOAD-BEARING ASSERTION.
		TestFalse(
			*FString::Printf(
				TEXT("the badge follows the view model rather than being a constant; closed "
					 "'%s' vs open '%s' (engineering rule 7: what is on screen must be "
					 "disclosed, and a frozen badge discloses nothing)"),
				*ClosedBadge.ToString(), *OpenBadge.ToString()),
			ClosedBadge.EqualTo(OpenBadge));

		// Still never empty: a blank badge reads as "nothing to disclose".
		TestFalse(TEXT("the badge always says something"), OpenBadge.IsEmpty());

		// The readouts likewise. A blank frame counter reads as a broken widget.
		TestFalse(TEXT("the frame counter is populated"), Bar->GetFrameCounterText().IsEmpty());
		TestFalse(TEXT("the time readout is populated"), Bar->GetTimeText().IsEmpty());
	}

	return true;
}

/* ========================================================================== */
/* An unbound bar is inert, not a crash                                       */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTransportBarUnboundTest,
	"FlowViz.UI.TransportBar.Unbound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizTransportBarUnboundTest::RunTest(const FString& Parameters)
{
	/*
	 * The workspace builds its panels before any case is open, so this is the
	 * state the UI is in every time the application starts. Rule 15 requires the
	 * controls to be disabled rather than present-and-dead, and nothing here may
	 * dereference the null view model.
	 */
	const TSharedRef<SFlowVizTransportBar> Bar = SNew(SFlowVizTransportBar);

	const TSharedPtr<SButton> PlayPause = Bar->GetPlayPauseButton();
	if (!TestTrue(TEXT("an unbound transport bar still builds its controls"), PlayPause.IsValid()))
	{
		return false;
	}

	FlowVizSlateAttributePump::Pump(Bar);

	TestFalse(
		TEXT("with no view model the play button is disabled rather than offered and dead "
			 "(engineering rule 15)"),
		PlayPause->IsEnabled());

	// Clicking a disabled button must be a no-op rather than a null dereference.
	// SimulateClick bypasses the enabled check, which makes it a stronger probe
	// than a real click: it proves the HANDLER is null-safe, not merely that the
	// button is unreachable.
	PlayPause->SimulateClick();

	TestFalse(
		TEXT("an unbound bar still produces badge text rather than crashing"),
		Bar->GetBadgeText().IsEmpty());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
