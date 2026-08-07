// Copyright FlowViz contributors. All Rights Reserved.

#include "Containers/Ticker.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Playback/FlowVizCasePlayer.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "UI/SFlowVizTransportBar.h"
#include "UI/SFlowVizWorkspace.h"
#include "Widgets/Input/SButton.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED NAMESPACE, NOT ANONYMOUS -- FlowVizRuntime is a unity build and an
 * anonymous namespace merges with every other one in the same blob rather than
 * being file-local. That collision already shipped once in this repo (#37).
 */
namespace FlowVizWorkspaceClockSeamTest
{
	/** The committed low-resolution sample, beside Plugins/ rather than inside the plugin. */
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

	/** One engine frame at 30 fps -- the order of delta the ticker is handed in production. */
	constexpr float FrameDelta = 1.0f / 30.0f;

	/**
	 * BOUNDED, AND THE BOUND IS DERIVED RATHER THAN GUESSED. A missing clock must
	 * fail in seconds, not hang on a condition that can never become true.
	 *
	 * Default Sequence mode steps SequenceFrameRate * Speed * Delta = 24 * 1.0 *
	 * 1/30 = 0.8 frame-coordinates per pump, so the playhead leaves frame 0's
	 * bracket on the second pump and the display follows on the third once the
	 * decode lands. 90 is two orders of margin over that and still terminates
	 * promptly. It is NOT set high "to be safe": every pump here ticks the
	 * ENGINE-WIDE core ticker, so an oversized bound spins every other registered
	 * ticker in the process thousands of times for nothing.
	 */
	constexpr int32 MaxPumpFrames = 90;

	/**
	 * ADVANCE ONLY WHAT PRODUCTION ADVANCES.
	 *
	 * This is the whole point of the file, and it is why there is no Player.Tick
	 * call anywhere below. Every existing player test calls Player.Tick itself and
	 * therefore CANNOT fail on a missing clock -- the test supplies the input whose
	 * absence is the defect (repo memory
	 * a-test-that-supplies-the-input-cannot-find-the-gap). What this pumps instead
	 * is FTSTicker::GetCoreTicker(), the literal object the engine loop drives
	 * every frame in LaunchEngineLoop's Tick_Core. If the workspace registered
	 * nothing with it, nothing here moves.
	 *
	 * SAFE TO CALL FROM A TEST BODY. The automation controller and worker are
	 * ticked DIRECTLY by the engine loop, not through the core ticker, so this
	 * test is not running nested inside FTSTicker::Tick and the pump is not
	 * re-entrant. Engine tests pump the same object the same way
	 * (Core/Tests/Containers/TickerTests.cpp).
	 *
	 * WHY THE WAIT IS NOT ALSO SUPPLYING THE INPUT. WaitForPendingLoads starts
	 * nothing and moves no playhead -- it blocks until decodes already in flight
	 * report. The work is started by StartPendingLoads, which has exactly one call
	 * site and it is inside Player::Tick. So the wait can only ever finish work the
	 * pump above it began, and on a clockless build it returns instantly against an
	 * empty queue and the loop still exhausts its budget having observed nothing.
	 * Repo memory seek-is-not-load.
	 *
	 * @return Pump frames spent. Reaching MaxPumpFrames means the predicate never
	 *         became true; the caller's assertion is what says so.
	 */
	int32 PumpEngineUntil(FFlowVizCasePlayer& Player, TFunctionRef<bool()> Predicate)
	{
		int32 Frames = 0;
		while (Frames < MaxPumpFrames && !Predicate())
		{
			FTSTicker::GetCoreTicker().Tick(FrameDelta);

			// Lets the decodes THIS pump started finish, so the next pump can drain
			// them into Display. Without it the loop would burn its budget racing
			// worker threads that have not landed yet.
			Player.WaitForPendingLoads(30.0);
			++Frames;
		}
		return Frames;
	}

	/** The same pump, run a fixed number of times. For asserting something did NOT happen. */
	void PumpEngineFrames(FFlowVizCasePlayer& Player, int32 FrameCount)
	{
		for (int32 Index = 0; Index < FrameCount; ++Index)
		{
			FTSTicker::GetCoreTicker().Tick(FrameDelta);
			Player.WaitForPendingLoads(30.0);
		}
	}
}

/**
 * DOES PRESSING PLAY MAKE ANYTHING HAPPEN, or does it only set a flag?
 *
 * FFlowVizCasePlayer::Play() sets `bPlaying = true` and returns; everything that
 * acts on that flag lives in FFlowVizCasePlayer::Tick, and grepping the whole
 * plugin for a call to it finds hits in three TEST files and nowhere else. No
 * FTSTicker, no FTickableGameObject, no SWidget::Tick override, and
 * UCFDVizVolumeComponent sets `PrimaryComponentTick.bCanEverTick = false`. So
 * the transport bar's button toggles a label, the view model reports IsPlaying()
 * true, every playback test passes -- and the case sits on whatever frame the
 * user last dragged the playhead to.
 *
 * WHY IT LOOKS SO CONVINCINGLY ALIVE, which is why this needed a test rather
 * than a glance. SCRUBBING WORKS: dragging the slider calls SeekToFrame, which
 * calls UpdateDisplay directly, and the frame source installed by SetVolume
 * (#57) carries that to the component. A screen recording of someone dragging
 * the playhead is indistinguishable from playback. Only the hands-off case --
 * press Play and watch -- is dead, and that is the case nobody demonstrates
 * because it is the boring one.
 *
 * THE THREE ARMS ARE NOT REDUNDANT, and no two of them imply the third:
 *
 *   paused    the display must reach frame 0 (proving a clock exists and reaches
 *             the player at all -- StartPendingLoads has one call site, inside
 *             Tick, so nothing loads without it) and must NOT pass frame 0
 *             (proving the clock reads bPlaying rather than free-running).
 *
 *   playing   the display must advance PAST frame 0, which is what the user
 *             pressed the button for. A clock that ran only while paused would
 *             satisfy the arm above.
 *
 *   per-      a workspace built AFTER two others have been destroyed must still
 *   instance  get a working clock. This is the arm that fails on the most
 *             natural wrong implementation -- one static handle, or a registration
 *             keyed on something shared -- where the first destructor tears down
 *             the clock for everyone and arms one and two still pass because they
 *             run first.
 *
 * WHAT THIS DELIBERATELY DOES NOT ASSERT. No volume component is bound. The
 * channel from the player's display to UCFDVizVolumeComponent::GetFrameSelection
 * is a live read and is already asserted by FlowViz.UI.Workspace.FrameSeam;
 * repeating it here would make this file's mutants score against two seams at
 * once and tell us less about each (repo memory
 * a-compound-mutant-scores-like-a-narrow-one). What is covered by NEITHER file
 * is that an advance reaches the SCENE PROXY: nothing marks the component's
 * render data dirty when the playhead moves on its own, so the pixels need not
 * follow even once this test is green. That is its own task, and
 * OPENFOAM_PARAVIEW_PARITY.md says so rather than claiming playback is finished.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceClockSeamTest,
	"FlowViz.UI.Workspace.ClockSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceClockSeamTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceClockSeamTest;

	/* == Paused: a clock exists, and it does not run a paused player ========== */

	{
		const TSharedRef<SFlowVizWorkspace> Workspace = SNew(SFlowVizWorkspace);

		if (!TestTrue(TEXT("CONTROL: the workspace opens the sample case"),
				Workspace->GetModel().OpenCase(GetSampleCaseDir()).IsOk()))
		{
			return false;
		}

		FFlowVizCasePlayer& Player = Workspace->GetModel().Player;

		/*
		 * ASSERTED, NOT ASSUMED. CanPlay() is false for a single-frame case, so on
		 * a shrunken sample the Play button in the next arm would refuse and that
		 * arm would fail for a reason having nothing to do with a clock.
		 */
		if (!TestTrue(TEXT("CONTROL: the sample has more than one frame, so it is playable at all"),
				Player.GetTimeline().GetFrameCount() > 1))
		{
			return false;
		}

		/*
		 * THE STATE A FRESHLY OPENED PLAYER IS IN, and the reason this arm can
		 * detect a missing clock at all. Open() resets Display to its default --
		 * FrameA = INDEX_NONE, bStale -- and primes no loads. Residency is only
		 * ever produced by StartPendingLoads, whose single call site is inside
		 * Tick. So an unticked player is stuck here forever, and anything below
		 * that shows a frame was produced by a clock.
		 */
		if (!TestEqual(TEXT("CONTROL: a freshly opened player displays nothing, because Open primes "
							"no loads"),
				Player.GetDisplay().FrameA, INDEX_NONE))
		{
			return false;
		}

		TestFalse(TEXT("CONTROL: and it is not playing, so this arm is about the clock rather than "
					   "about the transport"),
			Player.IsPlaying());

		const int32 FramesToFirst = PumpEngineUntil(
			Player, [&Player]() { return Player.GetDisplay().FrameA != INDEX_NONE; });

		/*
		 * THE ASSERTION FOR THE MISSING CLOCK. Nothing in this file called
		 * Player.Tick; the only thing pumped was the engine's own core ticker. If
		 * the workspace registered no clock, no load was ever started, Display is
		 * still INDEX_NONE, and this fails -- which is the state the codebase is in
		 * as this test is written.
		 */
		TestEqual(
			*FString::Printf(
				TEXT("ticking only the ENGINE's core ticker brings the display up on frame 0, so "
					 "the workspace registered a clock that reaches the player (%d pump frames)"),
				FramesToFirst),
			Player.GetDisplay().FrameA, 0);

		/*
		 * AND THE OTHER DIRECTION, which is what stops the arm above from being
		 * satisfied by a clock that free-runs. Tick drains and starts loads even
		 * when paused -- deliberately, so a scrub that stops mid-decode still lands
		 * -- but it must not move the playhead. 60 frames is two virtual seconds,
		 * against a 20-frame sample a running playhead crosses in under one.
		 */
		PumpEngineFrames(Player, 60);

		TestEqual(TEXT("and 60 further engine frames do NOT move a PAUSED playhead, so the clock "
					   "reads bPlaying rather than advancing whenever it is called"),
			Player.GetDisplay().FrameA, 0);

		TestEqual(TEXT("the paused playhead's physical time is still the first frame's"),
			Player.GetPhysicalTime(), Player.GetTimeline().GetFirstTime());
	}

	/* == Playing: the button the user presses makes frames arrive ============= */

	{
		const TSharedRef<SFlowVizWorkspace> Workspace = SNew(SFlowVizWorkspace);

		if (!TestTrue(TEXT("CONTROL: the workspace opens the sample case"),
				Workspace->GetModel().OpenCase(GetSampleCaseDir()).IsOk()))
		{
			return false;
		}

		FFlowVizCasePlayer& Player = Workspace->GetModel().Player;

		/*
		 * THE REAL BUTTON, not ViewModel->Play() and not Player.Play().
		 * SimulateClick invokes the same delegate a mouse release does, so this
		 * exercises the production wiring from the widget down. A test that called
		 * the view model directly would pass on a transport bar whose OnClicked was
		 * never bound -- the mocked-seam defect this codebase has already shipped
		 * twice (repo memory mocking-a-seam-hides-that-nothing-builds-it).
		 */
		const TSharedPtr<SFlowVizTransportBar> TransportBar = Workspace->GetTransportBar();
		if (!TestTrue(TEXT("CONTROL: the workspace built a transport bar with a play button"),
				TransportBar.IsValid() && TransportBar->GetPlayPauseButton().IsValid()))
		{
			return false;
		}

		TransportBar->GetPlayPauseButton()->SimulateClick();

		if (!TestTrue(TEXT("CONTROL: the click reached the player, so what follows is about the "
						   "clock and not about the button"),
				Player.IsPlaying()))
		{
			return false;
		}

		const int32 FramesToAdvance =
			PumpEngineUntil(Player, [&Player]() { return Player.GetDisplay().FrameA > 0; });

		/*
		 * THE ASSERTION THIS FILE EXISTS FOR. Play was pressed, the engine ticked,
		 * and nothing in this test touched the player. STRICTLY GREATER THAN ZERO,
		 * not "not INDEX_NONE": frame 0 is what a merely-drained player reaches
		 * while paused, so accepting it would let this arm pass on a clock that
		 * ignores Play entirely -- the exact degenerate value the previous arm is
		 * about (repo memory degenerate-data-defeats-assertions).
		 */
		TestTrue(
			*FString::Printf(
				TEXT("pressing Play and ticking only the ENGINE advances the display past frame 0, "
					 "so playback runs on a clock rather than on the user dragging the playhead; "
					 "reached frame %d after %d pump frames"),
				Player.GetDisplay().FrameA, FramesToAdvance),
			Player.GetDisplay().FrameA > 0);

		TestTrue(TEXT("and the physical time moved with it, so the playhead advanced rather than "
					  "the display jumping to a frame the playhead never asked for"),
			Player.GetPhysicalTime() > Player.GetTimeline().GetFirstTime());
	}

	/* == The clock is per-workspace, and outlives none of them ================ */

	/*
	 * Both workspaces above are destroyed by now. This one is built afterwards.
	 *
	 * THE IMPLEMENTATION THIS ARM IS AIMED AT is a clock registered once into
	 * something shared -- a static FDelegateHandle, a module-level ticker, a
	 * registration keyed on anything but the instance. Under that shape arms one
	 * and two pass (they run first, while the registration is live) and the SECOND
	 * destructor tears the clock down for every workspace that follows. A user
	 * closing and reopening the tab is exactly that sequence, so this is a real
	 * scenario rather than a hypothetical.
	 *
	 * It doubles as the crash probe for the opposite error -- a clock that keeps
	 * firing into a destroyed FFlowVizWorkspaceModel. That failure is a
	 * use-after-free, which no TestTrue can observe: it either reads plausible
	 * garbage or takes the session down, and a session that dies mid-suite reports
	 * as a truncated run rather than as this test failing (repo memory
	 * a-crash-reports-as-a-full-green). So the probe is the pump; the assertions
	 * below are about the live instance and can fail on their own.
	 */
	{
		const TSharedRef<SFlowVizWorkspace> Fresh = SNew(SFlowVizWorkspace);

		if (!TestTrue(TEXT("CONTROL: the third workspace opens the sample case"),
				Fresh->GetModel().OpenCase(GetSampleCaseDir()).IsOk()))
		{
			return false;
		}

		FFlowVizCasePlayer& Player = Fresh->GetModel().Player;

		const int32 FramesToFirst = PumpEngineUntil(
			Player, [&Player]() { return Player.GetDisplay().FrameA != INDEX_NONE; });

		TestEqual(
			*FString::Printf(
				TEXT("a workspace built AFTER two others were destroyed still gets its own clock, "
					 "so the registration is per-instance rather than shared (%d pump frames)"),
				FramesToFirst),
			Player.GetDisplay().FrameA, 0);

		TestFalse(TEXT("and it is not left playing by a destroyed workspace's clock"),
			Player.IsPlaying());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
