// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizTimelineViewModel.h"

#include "CFDViz/CFDVizManifest.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Playback/FlowVizCasePlayer.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The transport view model (plan.md sections 5F, 8; engineering rules 7 and 15).
 *
 * WHAT THESE TESTS ARE FOR, AND WHAT THEY DELIBERATELY ARE NOT. Frame selection
 * over a non-uniform timeline, ping-pong turnaround, speed validation and
 * residency are FFlowVizCasePlayer's, and FlowVizCasePlayerTest already asserts
 * them against a fixture built to defeat a constant-dt implementation. Repeating
 * those assertions here would test a copy of the rule rather than the rule.
 *
 * These tests assert the three things the VIEW MODEL adds, each of which is a
 * decision a widget event graph would otherwise make invisibly:
 *
 *  1. THE BADGE COMES FROM THE DISPLAY, NOT THE SELECTION (rule 7). This is
 *     checkable precisely because the two disagree during a stall: seeking to a
 *     frame that is not resident leaves the selection pointing at it while the
 *     display holds an older pair. A badge derived from the selection would say
 *     "Exact" over pixels that are a held blend. The test drives the player into
 *     that state and asserts the badge follows the pixels.
 *
 *  2. STEP PAUSES. A step taken while playing is overwritten by the next Tick,
 *     so a stepping button that left playback running is a button that appears
 *     to do nothing - rule 15's failure mode wearing a working disguise. The
 *     test asserts the state transition, not the frame index, because the frame
 *     index is the player's business.
 *
 *  3. REFUSALS LEAVE NOTHING HALF-APPLIED, AND THE PREDICATES MATCH THEM. A
 *     control that CanX() says is available must succeed, and one it says is
 *     unavailable must refuse rather than silently no-op. That pairing is what
 *     rule 15 actually requires; asserting only the refusal would pass on a view
 *     model that refuses everything.
 */

// NAMED namespace: FlowVizRuntime is a unity build, so anonymous namespaces from
// every .cpp merge and same-named helpers collide across test files.
namespace FlowVizTimelineViewModelTest
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

/* ========================================================================== */
/* Enablement and refusal, with no case at all                                */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTimelineViewModelUnboundTest,
	"FlowViz.UI.TimelineViewModel.Unbound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizTimelineViewModelUnboundTest::RunTest(const FString& Parameters)
{
	FFlowVizTimelineViewModel ViewModel;

	/* == Unbound: every control is unavailable AND refuses =================== */
	{
		TestFalse(TEXT("an unbound view model is not bound"), ViewModel.IsBound());
		TestFalse(TEXT("an unbound view model cannot play"), ViewModel.CanPlay());
		TestFalse(TEXT("an unbound view model cannot step"), ViewModel.CanStep());
		TestFalse(TEXT("an unbound view model cannot scrub"), ViewModel.CanScrub());

		// The predicate and the action must AGREE. A view model whose CanPlay()
		// is false but whose Play() quietly succeeds ships a button that lies in
		// the other direction.
		TestFalse(TEXT("Play on an unbound view model is refused, not silently ignored"),
			ViewModel.Play().IsOk());
		TestFalse(TEXT("StepForward on an unbound view model is refused"),
			ViewModel.StepForward().IsOk());
		TestFalse(TEXT("ScrubToNormalized on an unbound view model is refused"),
			ViewModel.ScrubToNormalized(0.5).IsOk());

		TestEqual(TEXT("an unbound view model shows the NoCase badge, not NoData"),
			ViewModel.GetBadge(), EFlowVizFrameBadge::NoCase);
		TestEqual(TEXT("an unbound view model displays no frame"),
			ViewModel.GetDisplayedFrame(), INDEX_NONE);
		TestFalse(TEXT("an unbound view model is not playing"), ViewModel.IsPlaying());
	}

	/* == Bound to a CLOSED player: still unavailable, but for a different reason */
	{
		FFlowVizCasePlayer Player;
		ViewModel.BindPlayer(&Player);

		TestTrue(TEXT("binding a closed player still counts as bound"), ViewModel.IsBound());
		TestFalse(TEXT("a closed player has no frames"), ViewModel.HasFrames());
		TestFalse(TEXT("a closed player cannot play"), ViewModel.CanPlay());
		TestFalse(TEXT("Play on a closed player is refused"), ViewModel.Play().IsOk());

		// NoCase, not NoData: there is no case, so naming a frame count or a time
		// would be inventing one.
		TestEqual(TEXT("a bound but closed player shows NoCase"),
			ViewModel.GetBadge(), EFlowVizFrameBadge::NoCase);
		TestEqual(TEXT("a closed player reports zero frames"), ViewModel.GetFrameCount(), 0);

		ViewModel.Unbind();
		TestFalse(TEXT("Unbind clears the binding"), ViewModel.IsBound());
	}

	return true;
}

/* ========================================================================== */
/* The badge follows the pixels, not the request                              */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTimelineViewModelBadgeTest,
	"FlowViz.UI.TimelineViewModel.Badge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizTimelineViewModelBadgeTest::RunTest(const FString& Parameters)
{
	const FString CaseDir = FlowVizTimelineViewModelTest::GetSampleCaseDir();
	const FString ManifestPath = FPaths::Combine(CaseDir, TEXT("manifest.json"));
	if (CaseDir.IsEmpty() || !FPaths::FileExists(ManifestPath))
	{
		AddError(FString::Printf(
			TEXT("the sample case is required for this test and is missing at '%s'. "
				 "Generate it with 'python3 -m cfdviz generate-mock --low-res'."),
			*ManifestPath));
		return false;
	}

	TSharedRef<FCFDVizCase> Case = MakeShared<FCFDVizCase>();
	const FCFDVizResult Load = FCFDVizCase::LoadFromFile(ManifestPath, Case.Get());
	if (!Load.IsOk())
	{
		AddError(FString::Printf(TEXT("failed to load the sample manifest: %s"), *Load.ToString()));
		return false;
	}

	FFlowVizCasePlayer Player;
	const FCFDVizResult Opened = Player.Open(Case, FName(TEXT("U")));
	if (!Opened.IsOk())
	{
		AddError(FString::Printf(TEXT("failed to open field U: %s"), *Opened.ToString()));
		return false;
	}

	FFlowVizTimelineViewModel ViewModel;
	ViewModel.BindPlayer(&Player);

	/* == Open, nothing decoded yet: NoData, and it must not name a frame ===== */
	{
		TestTrue(TEXT("an open 20-frame case has frames"), ViewModel.HasFrames());
		TestEqual(TEXT("the frame count comes from the timeline"), ViewModel.GetFrameCount(), 20);
		TestTrue(TEXT("an open case can be played"), ViewModel.CanPlay());
		TestTrue(TEXT("a 20-frame case can be stepped"), ViewModel.CanStep());

		// NOT NoCase - there is a case - and NOT Exact, because nothing is on
		// screen. Collapsing these is how a viewer claims to show frame 0 of a
		// case whose first frame has not finished decoding.
		TestEqual(TEXT("an open case with nothing resident shows NoData"),
			ViewModel.GetBadge(), EFlowVizFrameBadge::NoData);
		TestEqual(TEXT("NoData names no displayed frame"),
			ViewModel.GetDisplayedFrame(), INDEX_NONE);
	}

	/* == Frame 0 resident: Exact ============================================ */
	{
		ViewModel.ScrubToFrame(0);
		Player.Tick(0.0);
		if (!Player.WaitForPendingLoads(60.0))
		{
			AddError(TEXT("the sample frames did not finish decoding"));
			return false;
		}
		Player.Tick(0.0);

		TestEqual(TEXT("frame 0 is displayed once resident"), ViewModel.GetDisplayedFrame(), 0);
		TestEqual(TEXT("a resident exact frame shows the Exact badge"),
			ViewModel.GetBadge(), EFlowVizFrameBadge::Exact);
		TestFalse(TEXT("an exact frame is not reported as interpolated data"),
			ViewModel.IsShowingInterpolatedData());
		TestEqual(TEXT("the displayed time is frame 0's own time"),
			ViewModel.GetDisplayedTime(), 0.0, 1.0e-12);
	}

	/* == THE CASE THAT SEPARATES DISPLAY FROM SELECTION ===================== */
	{
		// The sample's frames are 0.05 apart. t = 0.475 sits between frames 9 and
		// 10, neither of which has been decoded, so the player HOLDS frame 0.
		//
		// A badge derived from GetSelection() would report Interpolated here and
		// name frame 9, over pixels that are frame 0. That is precisely the rule 7
		// violation this whole design exists to prevent, and it is only
		// observable in this state.
		ViewModel.ScrubToTime(0.475);

		// The selection is what the playhead WANTS.
		TestEqual(TEXT("the selection follows the scrub to frame 9"),
			Player.GetSelection().FrameA, 9);
		TestTrue(TEXT("the selection wants an interpolated pair"),
			Player.GetSelection().bInterpolated);

		// The display is what is ON SCREEN, and it is still frame 0.
		TestTrue(TEXT("the display is stale during the stall"), Player.GetDisplay().bStale);
		TestEqual(TEXT("the display still holds frame 0"), Player.GetDisplay().FrameA, 0);

		// THE ASSERTION THAT MATTERS.
		TestEqual(TEXT("the badge reports HeldStale, NOT the Interpolated the selection would claim"),
			ViewModel.GetBadge(), EFlowVizFrameBadge::HeldStale);
		TestEqual(TEXT("the displayed frame is the one on screen, not the one requested"),
			ViewModel.GetDisplayedFrame(), 0);
		TestEqual(TEXT("the displayed TIME is the held frame's time, not the scrub target"),
			ViewModel.GetDisplayedTime(), 0.0, 1.0e-12);

		// And the requested time is still available, separately, so a readout can
		// show both. If these were the same number the stall would be invisible.
		TestEqual(TEXT("the requested physical time is the scrub target"),
			ViewModel.GetPhysicalTime(), 0.475, 1.0e-12);
		TestNotEqual(TEXT("requested and displayed time differ during a stall - which is the disclosure"),
			ViewModel.GetPhysicalTime(), ViewModel.GetDisplayedTime());
	}

	/* == Interpolated, once both frames are resident ========================= */
	{
		Player.Tick(0.0);
		if (!Player.WaitForPendingLoads(60.0))
		{
			AddError(TEXT("frames 9 and 10 did not finish decoding"));
			return false;
		}
		Player.Tick(0.0);

		TestFalse(TEXT("the display is no longer stale"), Player.GetDisplay().bStale);
		TestEqual(TEXT("a resident blended pair shows the Interpolated badge"),
			ViewModel.GetBadge(), EFlowVizFrameBadge::Interpolated);
		TestTrue(TEXT("an interpolated display reports interpolated data"),
			ViewModel.IsShowingInterpolatedData());
	}

	/* == Turning interpolation off changes the badge, not just a flag ======== */
	{
		// Rule 7 in the other direction: with interpolation disabled the same
		// time must NOT be advertised as interpolated.
		TestTrue(TEXT("disabling interpolation is accepted"),
			ViewModel.SetInterpolationEnabled(false).IsOk());
		Player.Tick(0.0);
		Player.WaitForPendingLoads(60.0);
		Player.Tick(0.0);

		TestFalse(TEXT("interpolation is off"), ViewModel.IsInterpolationEnabled());
		TestFalse(TEXT("with interpolation off nothing is reported as interpolated data"),
			ViewModel.IsShowingInterpolatedData());
		TestNotEqual(TEXT("with interpolation off the badge is not Interpolated"),
			ViewModel.GetBadge(), EFlowVizFrameBadge::Interpolated);

		TestTrue(TEXT("re-enabling interpolation is accepted"),
			ViewModel.SetInterpolationEnabled(true).IsOk());
	}

	return true;
}

/* ========================================================================== */
/* Transport transitions                                                       */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTimelineViewModelTransportTest,
	"FlowViz.UI.TimelineViewModel.Transport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizTimelineViewModelTransportTest::RunTest(const FString& Parameters)
{
	const FString CaseDir = FlowVizTimelineViewModelTest::GetSampleCaseDir();
	const FString ManifestPath = FPaths::Combine(CaseDir, TEXT("manifest.json"));
	if (CaseDir.IsEmpty() || !FPaths::FileExists(ManifestPath))
	{
		AddError(FString::Printf(TEXT("the sample case is required and is missing at '%s'"), *ManifestPath));
		return false;
	}

	TSharedRef<FCFDVizCase> Case = MakeShared<FCFDVizCase>();
	if (!FCFDVizCase::LoadFromFile(ManifestPath, Case.Get()).IsOk())
	{
		AddError(TEXT("failed to load the sample manifest"));
		return false;
	}

	FFlowVizCasePlayer Player;
	if (!Player.Open(Case, FName(TEXT("U"))).IsOk())
	{
		AddError(TEXT("failed to open field U"));
		return false;
	}

	FFlowVizTimelineViewModel ViewModel;
	ViewModel.BindPlayer(&Player);

	/* == Play / pause / toggle ============================================== */
	{
		TestFalse(TEXT("a freshly opened player is paused"), ViewModel.IsPlaying());

		/*
		 * CanPause() IS A CAPABILITY, NOT AN AVAILABILITY. It answers "does this
		 * case support a transport at all", so it is TRUE on a paused player.
		 *
		 * An earlier draft of this test asserted the opposite - false while paused
		 * - and it is worth recording why that is the wrong design rather than
		 * just deleting it. Play and pause are one toggle button in this
		 * workspace (TogglePlayPause exists for exactly that), so a CanPause()
		 * that went false while paused would DISABLE the toggle in the state where
		 * its only job is to start playback. It would also carry no information:
		 * it would be exactly IsPlaying() && CanPlay(), which any caller wanting
		 * the availability sense can write, and which a separate-buttons UI should
		 * write.
		 *
		 * The rule-15 obligation is then discharged by the predicate agreeing with
		 * the action, which is asserted directly below rather than assumed.
		 */
		TestTrue(TEXT("CanPause is a capability of the case, so it holds while paused"),
			ViewModel.CanPause());
		TestTrue(TEXT("and Pause() AGREES with it: pausing an already-paused player succeeds"),
			ViewModel.Pause().IsOk());
		TestFalse(TEXT("pausing an already-paused player leaves it paused"), ViewModel.IsPlaying());

		TestTrue(TEXT("Play is accepted"), ViewModel.Play().IsOk());
		TestTrue(TEXT("Play starts playback"), ViewModel.IsPlaying());
		TestTrue(TEXT("a playing player can be paused"), ViewModel.CanPause());

		TestTrue(TEXT("Toggle from playing is accepted"), ViewModel.TogglePlayPause().IsOk());
		TestFalse(TEXT("Toggle from playing pauses"), ViewModel.IsPlaying());

		TestTrue(TEXT("Toggle from paused is accepted"), ViewModel.TogglePlayPause().IsOk());
		TestTrue(TEXT("Toggle from paused plays"), ViewModel.IsPlaying());
	}

	/* == STEPPING PAUSES ==================================================== */
	{
		// Still playing from the block above - which is the precondition that
		// makes this assertion meaningful.
		TestTrue(TEXT("playback is running before the step"), ViewModel.IsPlaying());

		TestTrue(TEXT("StepForward is accepted"), ViewModel.StepForward().IsOk());

		// THE ASSERTION. Without it, a step during playback is erased by the very
		// next Tick and the button does nothing observable.
		TestFalse(TEXT("stepping pauses playback, so the stepped-to frame survives the next tick"),
			ViewModel.IsPlaying());

		// Demonstrate that it actually survives, rather than asserting the flag
		// and hoping. A tick of a paused player must not move the playhead.
		const double AfterStep = ViewModel.GetPhysicalTime();
		Player.Tick(1.0);
		TestEqual(TEXT("a tick after a step does not move the paused playhead"),
			ViewModel.GetPhysicalTime(), AfterStep, 1.0e-12);

		TestTrue(TEXT("StepBackward is accepted"), ViewModel.StepBackward().IsOk());
		TestFalse(TEXT("stepping backward also pauses"), ViewModel.IsPlaying());
	}

	/* == Stop rewinds ======================================================= */
	{
		ViewModel.ScrubToFrame(10);
		TestTrue(TEXT("Play is accepted"), ViewModel.Play().IsOk());
		TestTrue(TEXT("Stop is accepted"), ViewModel.Stop().IsOk());
		TestFalse(TEXT("Stop pauses"), ViewModel.IsPlaying());
		TestEqual(TEXT("Stop rewinds to the first frame's time"),
			ViewModel.GetPhysicalTime(), 0.0, 1.0e-12);
	}

	/* == First / last ======================================================= */
	{
		TestTrue(TEXT("GoToLastFrame is accepted"), ViewModel.GoToLastFrame().IsOk());
		// 20 frames at dt = 0.05 starting at 0 puts the last frame at 0.95. Written
		// as a literal rather than derived from the timeline, so an off-by-one in
		// GetLastFrameIndex cannot agree with itself.
		TestEqual(TEXT("the last frame of the sample is t = 0.95"),
			ViewModel.GetPhysicalTime(), 0.95, 1.0e-9);
		TestEqual(TEXT("the last frame is normalized 1.0"),
			ViewModel.GetNormalizedTime(), 1.0, 1.0e-9);

		TestTrue(TEXT("GoToFirstFrame is accepted"), ViewModel.GoToFirstFrame().IsOk());
		TestEqual(TEXT("the first frame is t = 0"), ViewModel.GetPhysicalTime(), 0.0, 1.0e-12);
		TestEqual(TEXT("the first frame is normalized 0.0"),
			ViewModel.GetNormalizedTime(), 0.0, 1.0e-12);
	}

	/* == Scrubbing is uniform in TIME ======================================= */
	{
		TestTrue(TEXT("a mid scrub is accepted"), ViewModel.ScrubToNormalized(0.5).IsOk());
		// Half of [0, 0.95] is 0.475 - a time BETWEEN two stored frames. A slider
		// calibrated in frame index would give 0.475 too on this uniformly spaced
		// sample, which is why the non-uniform assertions live in
		// FlowVizCasePlayerTest where the fixture can tell them apart.
		TestEqual(TEXT("normalized 0.5 is half way through physical time"),
			ViewModel.GetPhysicalTime(), 0.475, 1.0e-9);

		// Out of range is CLAMPED by the player, not refused - a slider cannot
		// produce an out-of-range value, and refusing would strand the playhead.
		TestTrue(TEXT("an over-range scrub is accepted"), ViewModel.ScrubToNormalized(1.5).IsOk());
		TestEqual(TEXT("an over-range scrub clamps to the end"),
			ViewModel.GetPhysicalTime(), 0.95, 1.0e-9);
	}

	/* == Speed ============================================================== */
	{
		// The default is preset index 3, which is 1.0.
		TestEqual(TEXT("the default speed is 1.0"), ViewModel.GetSpeed(), 1.0, 1.0e-12);
		TestEqual(TEXT("the default speed is recognised as preset 3"),
			ViewModel.GetSpeedPresetIndex(), 3);

		TestTrue(TEXT("selecting preset 5 is accepted"), ViewModel.SetSpeedPresetIndex(5).IsOk());
		TestEqual(TEXT("preset 5 is 4x"), ViewModel.GetSpeed(), 4.0, 1.0e-12);
		TestEqual(TEXT("preset 5 reports itself as preset 5"), ViewModel.GetSpeedPresetIndex(), 5);

		// A custom speed is NOT snapped, and must therefore report no preset - so
		// the preset buttons all render unselected rather than one of them lying.
		TestTrue(TEXT("a custom speed of 3.0 is accepted"), ViewModel.SetCustomSpeed(3.0).IsOk());
		TestEqual(TEXT("a custom speed is stored exactly, not snapped to a preset"),
			ViewModel.GetSpeed(), 3.0, 1.0e-12);
		TestEqual(TEXT("a custom speed matches no preset"), ViewModel.GetSpeedPresetIndex(), INDEX_NONE);

		/*
		 * REFUSALS LEAVE THE PRIOR SPEED INTACT. Zero is rejected by the player
		 * because a paused player and a playing one at speed zero look identical
		 * on screen and differ in every diagnostic.
		 */
		TestFalse(TEXT("a zero speed is refused"), ViewModel.SetCustomSpeed(0.0).IsOk());
		TestEqual(TEXT("a refused speed leaves the previous one in place"),
			ViewModel.GetSpeed(), 3.0, 1.0e-12);

		TestFalse(TEXT("a NaN speed is refused"),
			ViewModel.SetCustomSpeed(FMath::Sqrt(-1.0)).IsOk());
		TestEqual(TEXT("a refused NaN leaves the previous speed in place"),
			ViewModel.GetSpeed(), 3.0, 1.0e-12);

		TestFalse(TEXT("a speed past the maximum is refused"),
			ViewModel.SetCustomSpeed(1000.0).IsOk());
		TestEqual(TEXT("a refused over-range speed leaves the previous one in place"),
			ViewModel.GetSpeed(), 3.0, 1.0e-12);

		TestFalse(TEXT("an out-of-range preset index is refused"),
			ViewModel.SetSpeedPresetIndex(FlowVizPlayback::NumSpeedPresets).IsOk());
		TestFalse(TEXT("a negative preset index is refused"),
			ViewModel.SetSpeedPresetIndex(-1).IsOk());
		TestEqual(TEXT("a refused preset leaves the previous speed in place"),
			ViewModel.GetSpeed(), 3.0, 1.0e-12);
	}

	/* == Loop and playback mode reach the player ============================ */
	{
		// Read back through the PLAYER, not through the view model's own getter.
		// A getter that returns a private copy would pass a round-trip test while
		// the player kept playing in the old mode.
		TestTrue(TEXT("setting PingPong is accepted"),
			ViewModel.SetLoopMode(EFlowVizLoopMode::PingPong).IsOk());
		TestEqual(TEXT("the loop mode reached the player's settings"),
			Player.GetSettings().LoopMode, EFlowVizLoopMode::PingPong);
		TestEqual(TEXT("the view model reports what the player holds"),
			ViewModel.GetLoopMode(), EFlowVizLoopMode::PingPong);

		TestTrue(TEXT("setting RealTime is accepted"),
			ViewModel.SetPlaybackMode(EFlowVizPlaybackMode::RealTime).IsOk());
		TestEqual(TEXT("the playback mode reached the player's settings"),
			Player.GetSettings().Mode, EFlowVizPlaybackMode::RealTime);

		TestTrue(TEXT("disabling interpolation is accepted"),
			ViewModel.SetInterpolationEnabled(false).IsOk());
		TestFalse(TEXT("the interpolation flag reached the player"),
			Player.IsInterpolationEnabled());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
