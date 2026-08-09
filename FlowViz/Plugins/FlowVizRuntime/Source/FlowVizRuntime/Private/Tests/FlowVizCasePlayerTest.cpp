// Copyright FlowViz contributors. All Rights Reserved.

#include "Playback/FlowVizCasePlayer.h"

#include "CFDViz/CFDVizManifest.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Playback/FlowVizFrameCache.h"
#include "Scene/FlowVizVolumeComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Playback: which frame, when, and what stays resident (plan.md section 8).
 *
 * THE FIXTURE IS NON-UNIFORMLY SPACED, AND THAT IS THE POINT. Every timeline
 * test below runs on
 *
 *     Times = { 0.0, 1.0, 3.0, 6.0, 10.0 }        gaps 1, 2, 3, 4
 *
 * chosen so that no two intervals are equal and no interval is a multiple of
 * the first. The shipped sample case is uniformly spaced at dt = 0.05, so a
 * player that derives a frame index by dividing a time by a constant dt passes
 * every test written against the sample and is wrong on any adaptive-timestep
 * case. On this fixture the naive dt = Times[1] - Times[0] = 1.0 answers
 * "frame 6" for t = 6.0, which is not even a legal index. Each assertion below
 * that would change under the naive rule says so at the assertion.
 *
 * EXPECTED VALUES ARE COMPUTED BY HAND, NOT BY THE CODE UNDER TEST. Asserting
 * that TryBracket agrees with FindNearestFrame proves only that they share a
 * mistake. The alphas, times and frame coordinates below were worked out from
 * the gap structure of the fixture and are written as literals.
 *
 * THE TWO PLACES A PLAUSIBLE IMPLEMENTATION IS USUALLY WRONG - non-uniform
 * spacing and the ping-pong turnaround - each have a test whose input a wrong
 * implementation cannot get right by coincidence: a step LARGER than the whole
 * timeline (which an if-statement turnaround sticks on), and a Sequence-mode
 * advance across gaps of different widths (which a constant-dt advance crosses
 * at the wrong rate).
 */

// A NAMED namespace, not an anonymous one: FlowVizRuntime is a unity build, so
// every .cpp is concatenated into one translation unit and the anonymous
// namespaces of all of them merge. Two test files with a helper of the same
// name collide, and the error points at a file that did nothing wrong.
namespace FlowVizCasePlayerTest
{
	/** Gaps of 1, 2, 3 and 4 - no two equal, none a multiple of the first. */
	TArray<double> MakeNonUniformTimes()
	{
		return TArray<double>({ 0.0, 1.0, 3.0, 6.0, 10.0 });
	}

	FFlowVizTimeline MakeNonUniformTimeline()
	{
		FFlowVizTimeline Timeline;
		const TArray<double> Times = MakeNonUniformTimes();
		Timeline.Initialize(Times);
		return Timeline;
	}

	/** The committed low-resolution sample, which lives beside Plugins/, not inside the plugin. */
	FString GetPlaybackSampleCaseDir()
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
/* Frame selection: the pure layer                                            */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizPlaybackFramesTest,
	"FlowViz.Playback.Frames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizPlaybackFramesTest::RunTest(const FString& Parameters)
{
	const FFlowVizTimeline Timeline = FlowVizCasePlayerTest::MakeNonUniformTimeline();

	/* == The timeline rejects what it cannot search ========================== */
	{
		FFlowVizTimeline Bad;

		// A bracket search over an unsorted array returns a plausible, wrong frame.
		const TArray<double> NotIncreasing({ 0.0, 3.0, 1.0, 6.0 });
		TestFalse(TEXT("non-monotonic times are rejected"), Bad.Initialize(NotIncreasing).IsOk());

		// Equal times give a zero-width interval, and alpha divides by that width.
		const TArray<double> Duplicated({ 0.0, 1.0, 1.0, 3.0 });
		TestFalse(TEXT("duplicated times are rejected"), Bad.Initialize(Duplicated).IsOk());

		// Every comparison against NaN is false, so a pairwise-only check passes this.
		const TArray<double> WithNaN({ 0.0, 1.0, FMath::Sqrt(-1.0), 6.0 });
		TestFalse(TEXT("a NaN time is rejected"), Bad.Initialize(WithNaN).IsOk());

		// An empty timeline is a legal manifest, not a malformed one.
		const TArray<double> Empty;
		TestTrue(TEXT("an empty timeline is accepted"), Bad.Initialize(Empty).IsOk());
		TestEqual(TEXT("an empty timeline has no frames"), Bad.GetFrameCount(), 0);
		TestEqual(TEXT("an empty timeline has no last frame"), Bad.GetLastFrameIndex(), INDEX_NONE);

		int32 A = 7;
		int32 B = 7;
		double Alpha = 7.0;
		TestFalse(TEXT("an empty timeline brackets nothing"), Bad.TryBracket(0.0, A, B, Alpha));
		TestEqual(TEXT("a refused bracket leaves its outputs untouched"), A, 7);
		TestEqual(TEXT("FindNearestFrame on an empty timeline is INDEX_NONE"), Bad.FindNearestFrame(0.0), INDEX_NONE);
	}

	/* == Bracketing over unequal gaps ======================================= */
	{
		int32 A = INDEX_NONE;
		int32 B = INDEX_NONE;
		double Alpha = -1.0;

		// t = 4.5 is inside the [3.0, 6.0] gap, which is THREE wide. The naive
		// dt = 1.0 rule puts this in frame 4, which is the last frame - a wrong
		// answer that still renders.
		TestTrue(TEXT("t=4.5 brackets"), Timeline.TryBracket(4.5, A, B, Alpha));
		TestEqual(TEXT("t=4.5 -> A = 2 (NOT 4, which a constant dt would give)"), A, 2);
		TestEqual(TEXT("t=4.5 -> B = 3"), B, 3);
		TestEqual(TEXT("t=4.5 -> alpha = 1.5/3"), Alpha, 0.5, 1.0e-12);

		// The same absolute offset of 1.5 into the FIRST gap, which is 1 wide,
		// lands past it entirely. Alpha is a fraction of the enclosing interval,
		// never of a global dt.
		TestTrue(TEXT("t=1.5 brackets"), Timeline.TryBracket(1.5, A, B, Alpha));
		TestEqual(TEXT("t=1.5 -> A = 1"), A, 1);
		TestEqual(TEXT("t=1.5 -> alpha = 0.5/2 in a 2-wide gap"), Alpha, 0.25, 1.0e-12);

		// The last gap is 4 wide, so the same 1.5 offset is a smaller fraction again.
		TestTrue(TEXT("t=7.5 brackets"), Timeline.TryBracket(7.5, A, B, Alpha));
		TestEqual(TEXT("t=7.5 -> A = 3"), A, 3);
		TestEqual(TEXT("t=7.5 -> alpha = 1.5/4 in a 4-wide gap"), Alpha, 0.375, 1.0e-12);

		// Landing exactly on a stored frame has ONE spelling: (F, F, 0). Alpha is
		// never 1, so no call site has to handle both (F, F+1, 1) and (F+1, F+1, 0).
		TestTrue(TEXT("t=3.0 brackets"), Timeline.TryBracket(3.0, A, B, Alpha));
		TestEqual(TEXT("an exact frame time gives A == B"), A, 2);
		TestEqual(TEXT("an exact frame time gives B == A"), B, 2);
		TestEqual(TEXT("an exact frame time gives alpha 0, never 1"), Alpha, 0.0);

		// Outside the timeline the data does not exist. Clamp, never extrapolate.
		TestTrue(TEXT("t=-5 brackets"), Timeline.TryBracket(-5.0, A, B, Alpha));
		TestEqual(TEXT("before the first frame clamps to frame 0"), A, 0);
		TestEqual(TEXT("before the first frame has B == A"), B, 0);
		TestEqual(TEXT("before the first frame has alpha 0"), Alpha, 0.0);

		TestTrue(TEXT("t=99 brackets"), Timeline.TryBracket(99.0, A, B, Alpha));
		TestEqual(TEXT("after the last frame clamps to the last frame"), A, 4);
		TestEqual(TEXT("after the last frame has B == A"), B, 4);
		TestEqual(TEXT("after the last frame has alpha 0"), Alpha, 0.0);

		TestFalse(TEXT("a non-finite time brackets nothing"),
			Timeline.TryBracket(FMath::Sqrt(-1.0), A, B, Alpha));
	}

	/* == Nearest frame - for masks and topology, which must not blend ======== */
	{
		// Inside the 3-wide gap [3.0, 6.0]. 4.4 is 1.4 from frame 2 and 1.6 from
		// frame 3; 4.6 is the other way round. A "nearest = A" shortcut gets 4.6
		// wrong, and a mask a whole frame stale looks like plausible geometry.
		TestEqual(TEXT("t=4.4 is nearest frame 2"), Timeline.FindNearestFrame(4.4), 2);
		TestEqual(TEXT("t=4.6 is nearest frame 3"), Timeline.FindNearestFrame(4.6), 3);

		// The exact midpoint resolves to the LATER frame, matching the alpha >= 0.5
		// rule in SelectFrames. If the two disagreed, a field and its mask could
		// come from different frames at exactly this time.
		TestEqual(TEXT("the exact midpoint of a gap resolves to the later frame"),
			Timeline.FindNearestFrame(4.5), 3);

		TestEqual(TEXT("t before the start is nearest frame 0"), Timeline.FindNearestFrame(-3.0), 0);
		TestEqual(TEXT("t past the end is nearest the last frame"), Timeline.FindNearestFrame(50.0), 4);
	}

	/* == The frame coordinate: uniform in FRAMES, not in time ================ */
	{
		// An integer coordinate is exactly that stored frame, on any spacing.
		TestEqual(TEXT("t=0 is frame coordinate 0"), Timeline.TimeToFrameCoordinate(0.0), 0.0, 1.0e-12);
		TestEqual(TEXT("t=6 is frame coordinate 3"), Timeline.TimeToFrameCoordinate(6.0), 3.0, 1.0e-12);

		// Half way through the 3-wide gap is coordinate 2.5 - the coordinate is
		// linear in time WITHIN an interval and renormalises at every frame.
		TestEqual(TEXT("t=4.5 is frame coordinate 2.5"), Timeline.TimeToFrameCoordinate(4.5), 2.5, 1.0e-12);

		// The same coordinate offset costs different amounts of physical time in
		// different gaps. This is the whole difference between Sequence and RealTime.
		TestEqual(TEXT("frame coordinate 0.5 is t=0.5 in the 1-wide gap"),
			Timeline.FrameCoordinateToTime(0.5), 0.5, 1.0e-12);
		TestEqual(TEXT("frame coordinate 3.5 is t=8.0 in the 4-wide gap"),
			Timeline.FrameCoordinateToTime(3.5), 8.0, 1.0e-12);

		// EXTRAPOLATION PAST THE END IS REQUIRED, not incidental: Sequence mode
		// steps the coordinate past the end and hands the overshoot to the fold.
		// Clamping here makes ping-pong stick at the last frame for one tick per
		// turnaround, which reads as a stutter rather than as a bug.
		TestEqual(TEXT("coordinate 4.5 extrapolates using the LAST gap's width"),
			Timeline.FrameCoordinateToTime(4.5), 12.0, 1.0e-12);
		TestEqual(TEXT("coordinate -0.5 extrapolates using the FIRST gap's width"),
			Timeline.FrameCoordinateToTime(-0.5), -0.5, 1.0e-12);
	}

	/* == SelectFrames, and the interpolation disclosure ====================== */
	{
		// Interpolating, strictly inside a gap: two distinct frames and a blend.
		const FFlowVizFrameSelection Blend = FlowVizPlayback::SelectFrames(Timeline, 4.5, true);
		TestEqual(TEXT("blended selection A"), Blend.FrameA, 2);
		TestEqual(TEXT("blended selection B"), Blend.FrameB, 3);
		TestEqual(TEXT("blended selection alpha"), Blend.Alpha, 0.5, 1.0e-12);
		TestTrue(TEXT("VISUAL_QA rule 5: a blend reports itself interpolated"), Blend.bInterpolated);

		// NearestFrame is NOT FrameA here. A mask taken from A at alpha 0.7 is a
		// whole frame stale; this is the assertion that catches that shortcut.
		const FFlowVizFrameSelection Late = FlowVizPlayback::SelectFrames(Timeline, 8.0, true);
		TestEqual(TEXT("t=8.0 blends frames 3 and 4"), Late.FrameA, 3);
		TestEqual(TEXT("t=8.0 alpha = 2/4"), Late.Alpha, 0.5, 1.0e-12);
		const FFlowVizFrameSelection Later = FlowVizPlayback::SelectFrames(Timeline, 9.0, true);
		TestEqual(TEXT("at alpha 0.75 the nearest frame is B, not A"), Later.NearestFrame, 4);
		TestEqual(TEXT("...while frame A is still 3"), Later.FrameA, 3);

		// Exactly on a stored frame: not interpolated, and the indicator is off.
		const FFlowVizFrameSelection Exact = FlowVizPlayback::SelectFrames(Timeline, 6.0, true);
		TestEqual(TEXT("an exact frame time selects one frame"), Exact.FrameA, 3);
		TestEqual(TEXT("an exact frame time has B == A"), Exact.FrameB, 3);
		TestFalse(TEXT("an exact frame time is NOT interpolated"), Exact.bInterpolated);

		// INTERPOLATION OFF collapses to the NEAREST frame, not to frame A.
		// Truncating toward A would lag the data by up to a whole frame, which is
		// a different thing from "nearest" and renders as plausible flow.
		const FFlowVizFrameSelection Off = FlowVizPlayback::SelectFrames(Timeline, 4.6, false);
		TestEqual(TEXT("interpolation off selects the NEAREST frame (3), not A (2)"), Off.FrameA, 3);
		TestEqual(TEXT("interpolation off has B == A"), Off.FrameB, 3);
		TestEqual(TEXT("interpolation off has alpha 0"), Off.Alpha, 0.0);
		TestFalse(TEXT("interpolation off is never marked interpolated"), Off.bInterpolated);
		TestEqual(TEXT("interpolation off still reports a nearest frame for masks"), Off.NearestFrame, 3);

		// The other side of the same gap picks the other frame.
		const FFlowVizFrameSelection OffEarly = FlowVizPlayback::SelectFrames(Timeline, 4.4, false);
		TestEqual(TEXT("interpolation off just before the midpoint selects frame 2"), OffEarly.FrameA, 2);
	}

	/* == The fold: loop policies at the ends ================================= */
	{
		bool bForward = true;
		bool bFinished = false;

		// --- Once: clamp, and latch finished at BOTH ends.
		TestEqual(TEXT("Once clamps past the end"),
			FlowVizPlayback::FoldPhase(Timeline, 12.0, EFlowVizLoopMode::Once, true, bForward, bFinished),
			10.0, 1.0e-12);
		TestTrue(TEXT("Once reports finished past the end"), bFinished);

		TestEqual(TEXT("Once clamps before the start"),
			FlowVizPlayback::FoldPhase(Timeline, -2.0, EFlowVizLoopMode::Once, false, bForward, bFinished),
			0.0, 1.0e-12);
		TestTrue(TEXT("Once reports finished before the start"), bFinished);

		TestEqual(TEXT("Once mid-timeline is unchanged"),
			FlowVizPlayback::FoldPhase(Timeline, 4.5, EFlowVizLoopMode::Once, true, bForward, bFinished),
			4.5, 1.0e-12);
		TestFalse(TEXT("Once mid-timeline is not finished"), bFinished);

		// --- Loop: wrap modulo the duration, in both directions.
		TestEqual(TEXT("Loop wraps 12 to 2"),
			FlowVizPlayback::FoldPhase(Timeline, 12.0, EFlowVizLoopMode::Loop, true, bForward, bFinished),
			2.0, 1.0e-12);
		TestFalse(TEXT("Loop never finishes"), bFinished);
		TestTrue(TEXT("Loop travelling forward stays forward"), bForward);

		// A backward loop must wrap to the FAR end, not clamp to the near one.
		TestEqual(TEXT("Loop wraps -2 to 8"),
			FlowVizPlayback::FoldPhase(Timeline, -2.0, EFlowVizLoopMode::Loop, false, bForward, bFinished),
			8.0, 1.0e-12);
		TestFalse(TEXT("Loop travelling backward stays backward"), bForward);

		// --- PingPong: the triangle wave. Period is 2 * duration = 20.
		TestEqual(TEXT("PingPong reflects 12 to 8"),
			FlowVizPlayback::FoldPhase(Timeline, 12.0, EFlowVizLoopMode::PingPong, true, bForward, bFinished),
			8.0, 1.0e-12);
		TestFalse(TEXT("PingPong past the end is travelling backward"), bForward);

		// AN EXACT LANDING ON THE END IS EXACT. A turnaround written as an
		// if-statement with an epsilon overshoots or sticks here.
		TestEqual(TEXT("PingPong at exactly the end is the end"),
			FlowVizPlayback::FoldPhase(Timeline, 10.0, EFlowVizLoopMode::PingPong, true, bForward, bFinished),
			10.0, 1.0e-12);

		// A FULL PERIOD RETURNS TO THE START, TRAVELLING FORWARD. An
		// if-statement turnaround that flips a stored direction flag lands here
		// with the flag inverted, and the case then plays backwards forever.
		TestEqual(TEXT("PingPong after a full period is back at the start"),
			FlowVizPlayback::FoldPhase(Timeline, 20.0, EFlowVizLoopMode::PingPong, true, bForward, bFinished),
			0.0, 1.0e-12);
		TestTrue(TEXT("PingPong after a full period is travelling forward again"), bForward);

		// A STEP LARGER THAN THE WHOLE TIMELINE. This is the input a turnaround
		// implemented as "if (t > end) { t = end; reverse(); }" cannot get right:
		// it clamps to the end and loses the rest of the step. 25 folds to 5
		// travelling forward, having gone out, back, and out again.
		TestEqual(TEXT("PingPong folds a phase of 25 (2.5 periods) to 5"),
			FlowVizPlayback::FoldPhase(Timeline, 25.0, EFlowVizLoopMode::PingPong, true, bForward, bFinished),
			5.0, 1.0e-12);
		TestTrue(TEXT("PingPong at 25 is travelling forward"), bForward);

		TestEqual(TEXT("PingPong folds a phase of 38 to 2"),
			FlowVizPlayback::FoldPhase(Timeline, 38.0, EFlowVizLoopMode::PingPong, true, bForward, bFinished),
			2.0, 1.0e-12);
		TestFalse(TEXT("PingPong at 38 is travelling backward"), bForward);

		// NEGATIVE PHASE, NEGATIVE SPEED. The triangle wave is even, so the
		// position mirrors; the DIRECTION is the composition of the phase's own
		// sign with which half of the period it lands in. Getting only the
		// position right leaves a player that reports the wrong travel direction,
		// which then preloads the wrong side of the playhead.
		TestEqual(TEXT("PingPong reflects a phase of -3 to 3"),
			FlowVizPlayback::FoldPhase(Timeline, -3.0, EFlowVizLoopMode::PingPong, false, bForward, bFinished),
			3.0, 1.0e-12);
		TestTrue(TEXT("a negative phase in the reflected half travels FORWARD"), bForward);

		// --- A zero-duration timeline is where a fold divides by zero.
		FFlowVizTimeline Single;
		const TArray<double> OneTime({ 7.5 });
		Single.Initialize(OneTime);
		const double Folded = FlowVizPlayback::FoldPhase(
			Single, 999.0, EFlowVizLoopMode::PingPong, true, bForward, bFinished);
		TestEqual(TEXT("a single-frame timeline folds to its only time"), Folded, 7.5, 1.0e-12);
		TestFalse(TEXT("a single-frame fold is not NaN"), FMath::IsNaN(Folded));
	}

	/* == Advance: the three modes differ in what a wall second buys =========== */
	{
		FFlowVizPlaybackSettings Settings;
		Settings.LoopMode = EFlowVizLoopMode::Once;
		Settings.Speed = 1.0;

		// --- SEQUENCE: equal wall time per STORED FRAME, over unequal gaps.
		// At 1 frame/second, one second of wall time must cross exactly one
		// stored frame whether that frame spans 1 second of physical time or 4.
		// A constant-dt advance crosses the 1-wide gap in one tick and needs four
		// ticks for the 4-wide one - it plays the sparse end in slow motion.
		Settings.Mode = EFlowVizPlaybackMode::Sequence;
		Settings.SequenceFrameRate = 1.0;

		FFlowVizPlayhead Head = FlowVizPlayback::MakePlayheadAtFrame(Timeline, 0);
		TestEqual(TEXT("the playhead starts at t=0"), Head.DisplayTime, 0.0, 1.0e-12);

		Head = FlowVizPlayback::Advance(Timeline, Settings, Head, 1.0);
		TestEqual(TEXT("Sequence: 1s crosses the 1-wide gap to t=1.0"), Head.DisplayTime, 1.0, 1.0e-9);

		Head = FlowVizPlayback::Advance(Timeline, Settings, Head, 1.0);
		TestEqual(TEXT("Sequence: the next 1s crosses the 2-wide gap to t=3.0"), Head.DisplayTime, 3.0, 1.0e-9);

		Head = FlowVizPlayback::Advance(Timeline, Settings, Head, 1.0);
		TestEqual(TEXT("Sequence: the next 1s crosses the 3-wide gap to t=6.0"), Head.DisplayTime, 6.0, 1.0e-9);

		Head = FlowVizPlayback::Advance(Timeline, Settings, Head, 1.0);
		TestEqual(TEXT("Sequence: the next 1s crosses the 4-wide gap to t=10.0"), Head.DisplayTime, 10.0, 1.0e-9);
		TestTrue(TEXT("Sequence with Once latches finished at the end"), Head.bFinished);

		// Half a second is half a frame in COORDINATE space, which is a different
		// amount of physical time in each gap.
		Head = FlowVizPlayback::MakePlayheadAtFrame(Timeline, 2);
		Head = FlowVizPlayback::Advance(Timeline, Settings, Head, 0.5);
		TestEqual(TEXT("Sequence: half a frame into the 3-wide gap is t=4.5"),
			Head.DisplayTime, 4.5, 1.0e-9);

		// --- REAL TIME: physical time advances at Speed, ignoring frame density.
		// The same one second now covers one unit of solver time everywhere, so
		// it crosses the whole 1-wide gap and only a quarter of the 4-wide one.
		Settings.Mode = EFlowVizPlaybackMode::RealTime;
		Head = FlowVizPlayback::MakePlayheadAtFrame(Timeline, 0);
		Head = FlowVizPlayback::Advance(Timeline, Settings, Head, 1.0);
		TestEqual(TEXT("RealTime: 1 wall second is 1 solver second"), Head.DisplayTime, 1.0, 1.0e-12);

		Head = FlowVizPlayback::Advance(Timeline, Settings, Head, 2.5);
		TestEqual(TEXT("RealTime: 2.5 more solver seconds lands mid-gap at 3.5"),
			Head.DisplayTime, 3.5, 1.0e-12);

		// RealTime SKIPS stored frames where the data is denser than the display.
		// One 5-second tick crosses three stored frames at once, and the selection
		// must be the bracket around where it landed - not the next frame in order.
		Head = FlowVizPlayback::MakePlayheadAtFrame(Timeline, 0);
		Head = FlowVizPlayback::Advance(Timeline, Settings, Head, 5.0);
		TestEqual(TEXT("RealTime: a 5s tick lands at t=5.0"), Head.DisplayTime, 5.0, 1.0e-12);
		const FFlowVizFrameSelection Skipped = FlowVizPlayback::SelectFrames(Timeline, Head.DisplayTime, true);
		TestEqual(TEXT("RealTime: a skipping tick brackets where it LANDED"), Skipped.FrameA, 2);

		// Speed scales solver time per wall second.
		Settings.Speed = 2.0;
		Head = FlowVizPlayback::MakePlayheadAtFrame(Timeline, 0);
		Head = FlowVizPlayback::Advance(Timeline, Settings, Head, 1.0);
		TestEqual(TEXT("RealTime at 2x covers 2 solver seconds per wall second"),
			Head.DisplayTime, 2.0, 1.0e-12);

		// A NEGATIVE speed runs the case backwards.
		Settings.Speed = -1.0;
		Settings.LoopMode = EFlowVizLoopMode::Loop;
		Head = FlowVizPlayback::MakePlayheadAtTime(Timeline, 5.0);
		Head = FlowVizPlayback::Advance(Timeline, Settings, Head, 2.0);
		TestEqual(TEXT("a negative speed runs time backwards"), Head.DisplayTime, 3.0, 1.0e-12);
		TestFalse(TEXT("a negative speed reports travelling backward"), Head.bForward);

		// --- FIXED FPS: the playhead is independent of the tick rate.
		// 60 ticks of 1/60 s at 30 steps/s must advance exactly 30 steps. An
		// implementation that drops the sub-step remainder advances 0 - every tick
		// is smaller than one step - and the playhead never moves at all.
		Settings.Mode = EFlowVizPlaybackMode::FixedFps;
		Settings.Speed = 1.0;
		Settings.OutputFrameRate = 30.0;
		Settings.LoopMode = EFlowVizLoopMode::Loop;

		Head = FlowVizPlayback::MakePlayheadAtTime(Timeline, 0.0);
		for (int32 Tick = 0; Tick < 60; ++Tick)
		{
			Head = FlowVizPlayback::Advance(Timeline, Settings, Head, 1.0 / 60.0);
		}
		// 30 steps of (Speed / OutputFrameRate) = 30 * (1/30) = 1.0 solver seconds.
		TestEqual(TEXT("FixedFps: 60 ticks of 1/60s advance exactly 30 steps = 1.0s"),
			Head.DisplayTime, 1.0, 1.0e-9);

		// The SAME wall time delivered in ONE tick must land in the same place.
		// This is what makes an offline capture reproducible.
		FFlowVizPlayhead OneShot = FlowVizPlayback::MakePlayheadAtTime(Timeline, 0.0);
		OneShot = FlowVizPlayback::Advance(Timeline, Settings, OneShot, 1.0);
		TestEqual(TEXT("FixedFps: one 1s tick lands where 60 small ticks did"),
			OneShot.DisplayTime, Head.DisplayTime, 1.0e-9);

		// A tick shorter than one step advances NOTHING but banks the time. The
		// step is 1/30 s and each tick is 1/40 s, so one tick is under it and two
		// are over - deliberately not summing to exactly one step, since an exact
		// boundary would turn this into a test of floating-point luck rather than
		// of the banking.
		FFlowVizPlayhead Tiny = FlowVizPlayback::MakePlayheadAtTime(Timeline, 0.0);
		Tiny = FlowVizPlayback::Advance(Timeline, Settings, Tiny, 1.0 / 40.0);
		TestEqual(TEXT("FixedFps: a sub-step tick does not move the playhead"),
			Tiny.DisplayTime, 0.0, 1.0e-12);
		Tiny = FlowVizPlayback::Advance(Timeline, Settings, Tiny, 1.0 / 40.0);
		TestEqual(TEXT("FixedFps: the banked remainder then delivers exactly ONE whole step"),
			Tiny.DisplayTime, 1.0 / 30.0, 1.0e-9);

		// A non-positive delta must not move or corrupt the playhead.
		const FFlowVizPlayhead Before = Tiny;
		Tiny = FlowVizPlayback::Advance(Timeline, Settings, Tiny, 0.0);
		TestEqual(TEXT("a zero delta leaves the playhead where it was"),
			Tiny.DisplayTime, Before.DisplayTime, 1.0e-12);
		Tiny = FlowVizPlayback::Advance(Timeline, Settings, Tiny, -1.0);
		TestEqual(TEXT("a negative delta leaves the playhead where it was"),
			Tiny.DisplayTime, Before.DisplayTime, 1.0e-12);
	}

	/* == Advance x PingPong: the turnaround is phase state, not a re-fold ==== */
	{
		// THE RE-SEED BUG THIS PINS. Advance used to rebuild the phase from the
		// FOLDED display time every tick. Once and Loop cannot tell (their folds
		// are idempotent under that re-seed), but PingPong's triangle wave is
		// not: at an end the fold and the re-derived phase meet one step apart
		// and the playhead oscillates inside the last gap forever. The header's
		// own contract (FFlowVizPlayhead: "PHASE, NOT TIME, IS THE STATE...
		// never folded") is what is asserted here, through every mode.
		FFlowVizPlaybackSettings Settings;
		Settings.LoopMode = EFlowVizLoopMode::PingPong;
		Settings.Speed = 1.0;

		// --- REAL TIME through the turnaround. Duration 10, speed 1, dt 1: the
		// display must run 0..10, reflect, run back to 0 in exactly 20 ticks,
		// and set off FORWARD again -- not oscillate between 9 and 10.
		Settings.Mode = EFlowVizPlaybackMode::RealTime;
		FFlowVizPlayhead Head = FlowVizPlayback::MakePlayheadAtFrame(Timeline, 0);
		for (int32 Tick = 0; Tick < 11; ++Tick)
		{
			Head = FlowVizPlayback::Advance(Timeline, Settings, Head, 1.0);
		}
		TestEqual(TEXT("RealTime PingPong: tick 11 has reflected to t=9"),
			Head.DisplayTime, 9.0, 1.0e-9);
		TestFalse(TEXT("RealTime PingPong: the reflected leg travels backward"), Head.bForward);
		TestEqual(TEXT("RealTime PingPong: the phase is NOT folded (11 after 11 ticks)"),
			Head.PhaseTime, 11.0, 1.0e-9);

		Head = FlowVizPlayback::Advance(Timeline, Settings, Head, 1.0);
		TestEqual(TEXT("RealTime PingPong: tick 12 keeps walking back to t=8"),
			Head.DisplayTime, 8.0, 1.0e-9);
		TestFalse(TEXT("RealTime PingPong: tick 12 is still backward"), Head.bForward);

		for (int32 Tick = 12; Tick < 20; ++Tick)
		{
			Head = FlowVizPlayback::Advance(Timeline, Settings, Head, 1.0);
		}
		TestEqual(TEXT("RealTime PingPong: one full period returns to First"),
			Head.DisplayTime, 0.0, 1.0e-9);
		TestTrue(TEXT("RealTime PingPong: after a full period travel is forward again"),
			Head.bForward);
		Head = FlowVizPlayback::Advance(Timeline, Settings, Head, 1.0);
		TestEqual(TEXT("RealTime PingPong: the second period sets off forward"),
			Head.DisplayTime, 1.0, 1.0e-9);

		// --- FIXED FPS, same schedule (1 step/s at speed 1): the banked-step
		// path must accumulate on the same unfolded phase.
		Settings.Mode = EFlowVizPlaybackMode::FixedFps;
		Settings.OutputFrameRate = 1.0;
		Head = FlowVizPlayback::MakePlayheadAtFrame(Timeline, 0);
		for (int32 Tick = 0; Tick < 12; ++Tick)
		{
			Head = FlowVizPlayback::Advance(Timeline, Settings, Head, 1.0);
		}
		TestEqual(TEXT("FixedFps PingPong: tick 12 has walked back to t=8"),
			Head.DisplayTime, 8.0, 1.0e-9);
		TestFalse(TEXT("FixedFps PingPong: tick 12 travels backward"), Head.bForward);

		// --- SEQUENCE: equal wall time per STORED FRAME through the
		// turnaround. One frame period per tick must walk the stored frames
		// 0,1,2,3,4 and then back down 3,2,1,0 -- one stored frame per tick on
		// the reflected leg too, whatever the gaps' widths. Hand-computed over
		// the 1/2/3/4-wide gaps; the period is 2 * 4 = 8 ticks.
		Settings.Mode = EFlowVizPlaybackMode::Sequence;
		Settings.SequenceFrameRate = 1.0;
		Head = FlowVizPlayback::MakePlayheadAtFrame(Timeline, 0);
		const double ExpectedWalk[] = { 1.0, 3.0, 6.0, 10.0, 6.0, 3.0, 1.0, 0.0, 1.0, 3.0 };
		for (int32 Tick = 0; Tick < UE_ARRAY_COUNT(ExpectedWalk); ++Tick)
		{
			Head = FlowVizPlayback::Advance(Timeline, Settings, Head, 1.0);
			TestEqual(FString::Printf(
				TEXT("Sequence PingPong: tick %d lands on t=%g (one stored frame per tick)"),
					Tick + 1, ExpectedWalk[Tick]),
				Head.DisplayTime, ExpectedWalk[Tick], 1.0e-9);
			if (Tick == 4)
			{
				TestFalse(TEXT("Sequence PingPong: the reflected leg travels backward"),
					Head.bForward);
				TestTrue(TEXT("Sequence PingPong: the phase lies past the timeline while reflected"),
					Head.PhaseTime > Timeline.GetLastTime());
			}
			if (Tick == 7)
			{
				TestTrue(TEXT("Sequence PingPong: a full period returns to First, forward"),
					Head.bForward);
			}
		}
	}

	/* == Stepping by whole frames - the previous/next buttons ================ */
	{
		// Stepping starts from the NEAREST frame. From t=4.6 (nearest frame 3),
		// stepping forward must land on 4 - not on 3, which is the frame already
		// being shown. A step that starts from frame A instead lands on 3 and the
		// button appears to do nothing.
		FFlowVizPlayhead Head = FlowVizPlayback::MakePlayheadAtTime(Timeline, 4.6);
		Head = FlowVizPlayback::StepFrames(Timeline, Head, 1, EFlowVizLoopMode::Loop);
		TestEqual(TEXT("stepping forward from a nearly-frame-3 time lands on frame 4"),
			Head.DisplayTime, 10.0, 1.0e-12);

		// Boundary: stepping forward off the END wraps under Loop...
		Head = FlowVizPlayback::MakePlayheadAtFrame(Timeline, 4);
		Head = FlowVizPlayback::StepFrames(Timeline, Head, 1, EFlowVizLoopMode::Loop);
		TestEqual(TEXT("stepping past the last frame wraps to frame 0 under Loop"),
			Head.DisplayTime, 0.0, 1.0e-12);

		// ...and clamps under Once. Both boundaries, both policies.
		Head = FlowVizPlayback::MakePlayheadAtFrame(Timeline, 4);
		Head = FlowVizPlayback::StepFrames(Timeline, Head, 1, EFlowVizLoopMode::Once);
		TestEqual(TEXT("stepping past the last frame clamps under Once"),
			Head.DisplayTime, 10.0, 1.0e-12);

		Head = FlowVizPlayback::MakePlayheadAtFrame(Timeline, 0);
		Head = FlowVizPlayback::StepFrames(Timeline, Head, -1, EFlowVizLoopMode::Once);
		TestEqual(TEXT("stepping before frame 0 clamps under Once"),
			Head.DisplayTime, 0.0, 1.0e-12);

		Head = FlowVizPlayback::MakePlayheadAtFrame(Timeline, 0);
		Head = FlowVizPlayback::StepFrames(Timeline, Head, -1, EFlowVizLoopMode::Loop);
		TestEqual(TEXT("stepping before frame 0 wraps to the last frame under Loop"),
			Head.DisplayTime, 10.0, 1.0e-12);

		// A multi-frame step over unequal gaps.
		Head = FlowVizPlayback::MakePlayheadAtFrame(Timeline, 0);
		Head = FlowVizPlayback::StepFrames(Timeline, Head, 3, EFlowVizLoopMode::Once);
		TestEqual(TEXT("a 3-frame step from 0 lands on frame 3, t=6.0"),
			Head.DisplayTime, 6.0, 1.0e-12);
	}

	/* == Seeking to the two boundaries ====================================== */
	{
		// Frame 0 and the final frame are where an off-by-one lives. Both are
		// asserted by their TIME, which is a value the index arithmetic cannot
		// fake: frame 4 is t=10.0 and no other frame is.
		FFlowVizPlayhead First = FlowVizPlayback::MakePlayheadAtFrame(Timeline, 0);
		TestEqual(TEXT("seek to frame 0 is t=0"), First.DisplayTime, 0.0, 1.0e-12);

		FFlowVizPlayhead Last = FlowVizPlayback::MakePlayheadAtFrame(Timeline, 4);
		TestEqual(TEXT("seek to the final frame (4) is t=10"), Last.DisplayTime, 10.0, 1.0e-12);

		// One PAST the last legal index must clamp to the last frame, not wrap to
		// 0 and not read out of bounds.
		FFlowVizPlayhead Past = FlowVizPlayback::MakePlayheadAtFrame(Timeline, 5);
		TestEqual(TEXT("seek past the last frame clamps to the last frame"),
			Past.DisplayTime, 10.0, 1.0e-12);

		FFlowVizPlayhead Negative = FlowVizPlayback::MakePlayheadAtFrame(Timeline, -1);
		TestEqual(TEXT("seek to a negative frame clamps to frame 0"),
			Negative.DisplayTime, 0.0, 1.0e-12);

		// A seek to a time outside the timeline clamps rather than extrapolating.
		FFlowVizPlayhead Outside = FlowVizPlayback::MakePlayheadAtTime(Timeline, 99.0);
		TestEqual(TEXT("seek to a time past the end clamps"), Outside.DisplayTime, 10.0, 1.0e-12);
	}

	/* == The request list: priority and the two ends ========================= */
	{
		const FFlowVizFrameSelection Mid = FlowVizPlayback::SelectFrames(Timeline, 4.5, true);
		TArray<int32> Requests;

		// PRIORITY IS ORDER. The current frames come first, so a caller that can
		// start only one load this tick starts the one on screen (plan.md section
		// 8: "prioritize current frame over preload").
		FlowVizPlayback::BuildRequestList(Timeline, Mid, true, 1, 1, Requests);
		TestTrue(TEXT("the request list is non-empty"), Requests.Num() >= 2);
		TestEqual(TEXT("frame A is requested first"), Requests[0], 2);
		TestEqual(TEXT("frame B is requested second"), Requests[1], 3);
		TestTrue(TEXT("travelling forward preloads the frame ahead"), Requests.Contains(4));
		TestTrue(TEXT("travelling forward still preloads one behind"), Requests.Contains(1));

		// No duplicates: A, B and the preload ring overlap, and a duplicate would
		// start the same decode twice and charge the cache twice for it.
		TSet<int32> Unique(Requests);
		TestEqual(TEXT("the request list has no duplicates"), Unique.Num(), Requests.Num());

		// BOUNDARY: at frame 0 there is nothing behind. Out-of-range indices must
		// be dropped, not clamped to 0 - a clamp re-requests the current frame and
		// looks exactly like a correct list.
		const FFlowVizFrameSelection AtStart = FlowVizPlayback::SelectFrames(Timeline, 0.0, true);
		FlowVizPlayback::BuildRequestList(Timeline, AtStart, true, 2, 2, Requests);
		TestEqual(TEXT("at frame 0 the first request is frame 0"), Requests[0], 0);
		TestFalse(TEXT("at frame 0 nothing before frame 0 is requested"), Requests.Contains(-1));
		for (int32 Frame : Requests)
		{
			TestTrue(TEXT("every requested frame is a legal index"), Frame >= 0 && Frame <= 4);
		}

		// COUNT, NOT JUST MEMBERSHIP. The assertions above are all of the form
		// "nothing illegal appears", which a list that preloads NOTHING also
		// satisfies. Pinning the exact size is what makes a lost preload slot
		// visible: at frame 0 with radius 2 the legal neighbourhood is exactly
		// {0, 1, 2} - A is 0, B collapses onto 0 at an exact frame time, and only
		// the forward side has anywhere to go.
		TestEqual(TEXT("at frame 0 a radius-2 preload requests exactly the legal frames"),
			Requests.Num(), 3);
		TestTrue(TEXT("at frame 0 the forward preload reaches frame 1"), Requests.Contains(1));
		TestTrue(TEXT("at frame 0 the forward preload reaches frame 2"), Requests.Contains(2));

		// BOUNDARY: at the last frame there is nothing ahead.
		const FFlowVizFrameSelection AtEnd = FlowVizPlayback::SelectFrames(Timeline, 10.0, true);
		FlowVizPlayback::BuildRequestList(Timeline, AtEnd, true, 2, 2, Requests);
		TestEqual(TEXT("at the last frame the first request is the last frame"), Requests[0], 4);
		TestFalse(TEXT("at the last frame nothing past it is requested"), Requests.Contains(5));
		for (int32 Frame : Requests)
		{
			TestTrue(TEXT("every requested frame is still a legal index"), Frame >= 0 && Frame <= 4);
		}

		// DIRECTION MATTERS. Travelling backward the preload must lead the
		// playhead the other way, or a ping-pong that has just turned around
		// prefetches the frames it is walking away from.
		TArray<int32> Backward;
		FlowVizPlayback::BuildRequestList(Timeline, Mid, false, 1, 1, Backward);
		const int32 ForwardPreloadPos = Requests.Num();
		TArray<int32> Fwd;
		FlowVizPlayback::BuildRequestList(Timeline, Mid, true, 1, 0, Fwd);
		TArray<int32> Bwd;
		FlowVizPlayback::BuildRequestList(Timeline, Mid, false, 1, 0, Bwd);
		TestTrue(TEXT("forward preload asks for the frame after B"), Fwd.Contains(4));
		TestFalse(TEXT("forward preload does not ask for the frame before A"), Fwd.Contains(1));
		TestTrue(TEXT("backward preload asks for the frame before A"), Bwd.Contains(1));
		TestFalse(TEXT("backward preload does not ask for the frame after B"), Bwd.Contains(4));
		(void)ForwardPreloadPos;

		// An empty timeline requests nothing rather than requesting frame 0.
		FFlowVizTimeline Empty;
		const TArray<double> NoTimes;
		Empty.Initialize(NoTimes);
		const FFlowVizFrameSelection Nothing = FlowVizPlayback::SelectFrames(Empty, 0.0, true);
		TestFalse(TEXT("an empty timeline yields an invalid selection"), Nothing.IsValid());
		FlowVizPlayback::BuildRequestList(Empty, Nothing, true, 1, 1, Requests);
		TestEqual(TEXT("an empty timeline requests nothing"), Requests.Num(), 0);
	}

	/* == Display promotion: never show a partial frame ======================= */
	{
		const FFlowVizFrameSelection Want = FlowVizPlayback::SelectFrames(Timeline, 4.5, true);

		FFlowVizDisplaySelection Previous;
		Previous.FrameA = 0;
		Previous.FrameB = 1;
		Previous.Alpha = 0.25;
		Previous.Time = 0.5;
		Previous.NearestFrame = 0;
		Previous.bInterpolated = true;

		// Both resident: promote, and the display is not stale.
		{
			TSet<int32> Resident({ 2, 3 });
			auto IsResident = [&Resident](int32 Frame) { return Resident.Contains(Frame); };
			const FFlowVizDisplaySelection Shown =
				FlowVizPlayback::ResolveDisplay(Want, Previous, IsResident);
			TestEqual(TEXT("with both frames resident the display advances to A"), Shown.FrameA, 2);
			TestEqual(TEXT("with both frames resident the display advances to B"), Shown.FrameB, 3);
			TestEqual(TEXT("the promoted display carries the new alpha"), Shown.Alpha, 0.5, 1.0e-12);
			TestEqual(TEXT("the promoted display carries the new physical time"), Shown.Time, 4.5, 1.0e-12);
			TestFalse(TEXT("a promoted display is not stale"), Shown.bStale);
			TestTrue(TEXT("a promoted blend still discloses interpolation"), Shown.bInterpolated);
		}

		// THE CORRECTNESS REQUIREMENT. B is still decoding. Displaying A alone
		// would be a complete frame of data, but it would flip the "Interpolated"
		// indicator off and change the displayed physical time to one the user did
		// not ask for - on every frame of a scrub. Hold the previous pair.
		{
			TSet<int32> Resident({ 2 });
			auto IsResident = [&Resident](int32 Frame) { return Resident.Contains(Frame); };
			const FFlowVizDisplaySelection Shown =
				FlowVizPlayback::ResolveDisplay(Want, Previous, IsResident);
			TestEqual(TEXT("with B missing the display HOLDS the previous A"), Shown.FrameA, 0);
			TestEqual(TEXT("with B missing the display HOLDS the previous B"), Shown.FrameB, 1);
			TestEqual(TEXT("with B missing the display holds the previous time"),
				Shown.Time, 0.5, 1.0e-12);
			TestTrue(TEXT("a held display reports itself stale"), Shown.bStale);
		}

		// The symmetric case: A missing, B resident. Same answer.
		{
			TSet<int32> Resident({ 3 });
			auto IsResident = [&Resident](int32 Frame) { return Resident.Contains(Frame); };
			const FFlowVizDisplaySelection Shown =
				FlowVizPlayback::ResolveDisplay(Want, Previous, IsResident);
			TestEqual(TEXT("with A missing the display HOLDS the previous frame"), Shown.FrameA, 0);
			TestTrue(TEXT("with A missing the display is stale"), Shown.bStale);
		}

		// Nothing resident and nothing previously shown: an INVALID display, not a
		// display of frame 0. Showing frame 0 because it is the default index is
		// how a case that failed to load renders as its own first frame.
		{
			TSet<int32> Resident;
			auto IsResident = [&Resident](int32 Frame) { return Resident.Contains(Frame); };
			const FFlowVizDisplaySelection Shown =
				FlowVizPlayback::ResolveDisplay(Want, FFlowVizDisplaySelection(), IsResident);
			TestFalse(TEXT("with nothing resident and no history the display is invalid"), Shown.IsValid());
			TestTrue(TEXT("an invalid display is stale"), Shown.bStale);
		}

		// A NON-interpolated selection needs only its single frame. Requiring two
		// would stall interpolation-off playback on a cache that holds exactly one.
		{
			const FFlowVizFrameSelection Single = FlowVizPlayback::SelectFrames(Timeline, 4.6, false);
			TestEqual(TEXT("the non-interpolated selection names frame 3 twice"), Single.FrameA, Single.FrameB);
			TSet<int32> Resident({ 3 });
			auto IsResident = [&Resident](int32 Frame) { return Resident.Contains(Frame); };
			const FFlowVizDisplaySelection Shown =
				FlowVizPlayback::ResolveDisplay(Single, Previous, IsResident);
			TestEqual(TEXT("a single-frame selection promotes on one resident frame"), Shown.FrameA, 3);
			TestFalse(TEXT("a single-frame promotion is not stale"), Shown.bStale);
			TestFalse(TEXT("a single-frame display is not marked interpolated"), Shown.bInterpolated);
		}
	}

	/* == Settings validation ================================================= */
	{
		FFlowVizPlaybackSettings Settings;
		TestTrue(TEXT("the default settings are legal"), Settings.Validate().IsOk());

		// Zero speed is a paused player spelled as a playing one, and it makes
		// Sequence mode's frame-space step degenerate.
		Settings.Speed = 0.0;
		TestFalse(TEXT("zero speed is rejected"), Settings.Validate().IsOk());

		Settings.Speed = 1000.0;
		TestFalse(TEXT("a speed past the maximum is rejected"), Settings.Validate().IsOk());

		Settings = FFlowVizPlaybackSettings();
		Settings.OutputFrameRate = 0.0;
		TestFalse(TEXT("a zero output frame rate is rejected"), Settings.Validate().IsOk());

		Settings = FFlowVizPlaybackSettings();
		Settings.PreloadAhead = -1;
		TestFalse(TEXT("a negative preload radius is rejected"), Settings.Validate().IsOk());

		// Clamping is offered explicitly, for a UI slider, and never applied silently.
		Settings = FFlowVizPlaybackSettings();
		Settings.Speed = 1.0e9;
		Settings.OutputFrameRate = -5.0;
		Settings.ClampToLegalRange();
		TestTrue(TEXT("clamping produces legal settings"), Settings.Validate().IsOk());

		// The documented default preset really is 1.0.
		TestEqual(TEXT("the default speed preset is 1.0"),
			FlowVizPlayback::SpeedPresets[FlowVizPlayback::DefaultSpeedPresetIndex], 1.0);
		TestEqual(TEXT("snapping picks the nearest preset"),
			FlowVizPlayback::SnapToSpeedPreset(1.9), 2.0);
		TestEqual(TEXT("snapping a negative speed keeps its sign"),
			FlowVizPlayback::SnapToSpeedPreset(-1.9), -2.0);
	}

	return true;
}

/* ========================================================================== */
/* The LRU cache                                                              */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizFrameCacheTest,
	"FlowViz.Playback.FrameCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizFrameCacheTest::RunTest(const FString& Parameters)
{
	/* == Eviction at EXACTLY the budget, not one before or after ============= */
	{
		// Three 100-byte frames in a 300-byte budget fit EXACTLY. This is the
		// input that separates `Used + New > Budget` from `>=`: at 300 of 300 the
		// first form admits and the second evicts. Both look correct on a budget
		// with slack, which is why the fixture has none.
		FFlowVizFrameCache Cache;
		TestTrue(TEXT("the cache initializes"), Cache.Initialize(300, 300).IsOk());

		TArray<int32> Evicted;
		TestTrue(TEXT("frame 0 is admitted"), Cache.Admit(0, 100, 100, Evicted).IsOk());
		TestTrue(TEXT("frame 1 is admitted"), Cache.Admit(1, 100, 100, Evicted).IsOk());
		TestTrue(TEXT("frame 2 is admitted"), Cache.Admit(2, 100, 100, Evicted).IsOk());

		TestEqual(TEXT("three frames exactly filling the budget evict NOTHING"), Evicted.Num(), 0);
		TestEqual(TEXT("all three frames are present"), Cache.Num(), 3);
		TestEqual(TEXT("the accounting is exactly at the budget"), Cache.GetCpuBytes(), (int64)300);

		// One more byte over the line must evict, and evict EXACTLY ONE - the
		// least recently used. Evicting two is a budget check applied against the
		// wrong total; evicting the wrong one is an LRU that stamps on the wrong
		// event.
		TestEqual(TEXT("the eviction candidate is the least recently used"),
			Cache.PeekEvictionCandidate(), 0);
		TestTrue(TEXT("frame 3 is admitted"), Cache.Admit(3, 100, 100, Evicted).IsOk());
		TestEqual(TEXT("crossing the budget evicts exactly one frame"), Evicted.Num(), 1);
		TestEqual(TEXT("the evicted frame is the least recently used, frame 0"), Evicted[0], 0);
		TestFalse(TEXT("frame 0 is gone"), Cache.Contains(0));
		TestTrue(TEXT("frame 1 survived"), Cache.Contains(1));
		TestTrue(TEXT("frame 3 is present"), Cache.Contains(3));
		TestEqual(TEXT("the total is still exactly the budget"), Cache.GetCpuBytes(), (int64)300);
	}

	/* == Touch reorders eviction ============================================= */
	{
		// Frames carry DISTINCT byte counts here, so an accounting slip that
		// charges every frame the same amount is visible in the total. With three
		// identical 100-byte frames, adding one and removing another leaves the
		// total unchanged and hides the error.
		FFlowVizFrameCache Cache;
		TestTrue(TEXT("the cache initializes"), Cache.Initialize(1000, 1000).IsOk());

		TArray<int32> Evicted;
		Cache.Admit(0, 100, 110, Evicted);
		Cache.Admit(1, 200, 220, Evicted);
		Cache.Admit(2, 300, 330, Evicted);
		TestEqual(TEXT("distinct CPU sizes sum correctly"), Cache.GetCpuBytes(), (int64)600);
		TestEqual(TEXT("distinct GPU sizes sum independently of the CPU sizes"),
			Cache.GetGpuBytes(), (int64)660);

		// Touching frame 0 makes frame 1 the oldest.
		TestEqual(TEXT("before touching, frame 0 is the candidate"), Cache.PeekEvictionCandidate(), 0);
		TestTrue(TEXT("frame 0 can be touched"), Cache.Touch(0));
		TestEqual(TEXT("after touching frame 0, frame 1 is the candidate"),
			Cache.PeekEvictionCandidate(), 1);
		TestFalse(TEXT("touching an absent frame reports failure"), Cache.Touch(99));

		// 400 more needs 600+400 = 1000, which is exactly the budget, so nothing
		// is evicted - the same boundary as above, reached from the other side.
		Cache.Admit(3, 400, 340, Evicted);
		TestEqual(TEXT("filling the budget exactly still evicts nothing"), Evicted.Num(), 0);
		TestEqual(TEXT("the CPU total is exactly the budget"), Cache.GetCpuBytes(), (int64)1000);

		// Now one byte over: frame 1 goes, and the total must drop by 200 - frame
		// 1's own size - not by an average or by the newcomer's size.
		Cache.Admit(4, 1, 1, Evicted);
		TestEqual(TEXT("going one byte over evicts one frame"), Evicted.Num(), 1);
		TestEqual(TEXT("the touched frame 0 survived; frame 1 was evicted"), Evicted[0], 1);
		TestEqual(TEXT("the total dropped by frame 1's OWN 200 bytes"),
			Cache.GetCpuBytes(), (int64)(1000 - 200 + 1));
	}

	/* == The same frame requested twice ===================================== */
	{
		// A re-admit is the path a re-decode after a cancelled load takes. Double
		// counting it is the classic version of this bug: the cache reports twice
		// the memory it holds and evicts frames it did not need to.
		FFlowVizFrameCache Cache;
		Cache.Initialize(1000, 1000);

		TArray<int32> Evicted;
		Cache.Admit(7, 100, 150, Evicted);
		TestEqual(TEXT("one admit, one entry"), Cache.Num(), 1);
		TestEqual(TEXT("one admit, 100 CPU bytes"), Cache.GetCpuBytes(), (int64)100);

		Cache.Admit(7, 100, 150, Evicted);
		TestEqual(TEXT("re-admitting the SAME frame does not add an entry"), Cache.Num(), 1);
		TestEqual(TEXT("re-admitting the same frame does not double-count CPU bytes"),
			Cache.GetCpuBytes(), (int64)100);
		TestEqual(TEXT("re-admitting the same frame does not double-count GPU bytes"),
			Cache.GetGpuBytes(), (int64)150);
		TestEqual(TEXT("re-admitting evicts nothing"), Evicted.Num(), 0);

		// A re-admit with a DIFFERENT size replaces the old charge rather than
		// adding to it.
		Cache.Admit(7, 250, 150, Evicted);
		TestEqual(TEXT("a resized re-admit replaces the old byte count"),
			Cache.GetCpuBytes(), (int64)250);
		TestEqual(TEXT("a resized re-admit still holds one entry"), Cache.Num(), 1);
	}

	/* == Present is not displayable ========================================= */
	{
		// An admitted-but-incomplete frame holds its budget - that reservation is
		// what stops four concurrent decodes overcommitting - but must NOT be
		// displayable. Conflating the two is exactly how a half-decoded frame
		// reaches the screen.
		FFlowVizFrameCache Cache;
		Cache.Initialize(1000, 1000);

		TArray<int32> Evicted;
		Cache.Admit(4, 100, 100, Evicted);
		TestTrue(TEXT("an in-flight frame is PRESENT for budget purposes"), Cache.Contains(4));
		TestFalse(TEXT("an in-flight frame is NOT resident for display purposes"), Cache.IsResident(4));
		TestEqual(TEXT("an in-flight frame still costs its bytes"), Cache.GetCpuBytes(), (int64)100);

		TestTrue(TEXT("the frame can be marked complete"), Cache.MarkComplete(4));
		TestTrue(TEXT("a complete frame is resident"), Cache.IsResident(4));
		TestFalse(TEXT("marking an absent frame complete reports failure"), Cache.MarkComplete(99));

		// A frame that was never admitted is neither.
		TestFalse(TEXT("an unrequested frame is not present"), Cache.Contains(5));
		TestFalse(TEXT("an unrequested frame is not resident"), Cache.IsResident(5));
	}

	/* == Pinned frames are never evicted ==================================== */
	{
		FFlowVizFrameCache Cache;
		Cache.Initialize(300, 300);

		TArray<int32> Evicted;
		Cache.Admit(0, 100, 100, Evicted);
		Cache.Admit(1, 100, 100, Evicted);
		Cache.Admit(2, 100, 100, Evicted);

		// Pin the two OLDEST frames - the ones LRU would take first. If pinning is
		// ignored, frame 0 is evicted and the visible image tears.
		Cache.SetPinnedFrames(0, 1);
		TestTrue(TEXT("frame 0 is pinned"), Cache.IsPinned(0));
		TestTrue(TEXT("frame 1 is pinned"), Cache.IsPinned(1));
		TestFalse(TEXT("frame 2 is not pinned"), Cache.IsPinned(2));

		TestEqual(TEXT("the only evictable frame is the unpinned one"),
			Cache.PeekEvictionCandidate(), 2);

		Cache.Admit(3, 100, 100, Evicted);
		TestEqual(TEXT("admitting evicts the unpinned frame, not a pinned one"), Evicted.Num(), 1);
		TestEqual(TEXT("the evicted frame is 2, despite 0 and 1 being older"), Evicted[0], 2);
		TestTrue(TEXT("pinned frame 0 survived"), Cache.Contains(0));
		TestTrue(TEXT("pinned frame 1 survived"), Cache.Contains(1));

		// Now shrink to a 200-byte budget holding exactly the two pinned frames,
		// so NOTHING is evictable. Note the earlier state was NOT this: after
		// frame 2 was evicted the cache held 0 and 1 pinned plus 3 unpinned, and
		// an admission then legitimately succeeds by taking frame 3. The refusal
		// only becomes the correct answer once every remaining frame is pinned,
		// which is what these two calls establish.
		Cache.SetPinnedFrames(0, 1);
		TArray<int32> ShrinkEvicted;
		Cache.SetBudgets(200, 200, ShrinkEvicted);
		TestEqual(TEXT("shrinking to 200 evicts the one unpinned frame"), ShrinkEvicted.Num(), 1);
		TestEqual(TEXT("the frame it evicted is the unpinned 3"), ShrinkEvicted[0], 3);
		TestEqual(TEXT("only the two pinned frames remain"), Cache.Num(), 2);
		TestEqual(TEXT("nothing is evictable"), Cache.PeekEvictionCandidate(), INDEX_NONE);

		TArray<int32> Evicted2;
		const FCFDVizResult Refused = Cache.Admit(9, 100, 100, Evicted2);
		TestFalse(TEXT("an admission that would evict a pinned frame is refused"), Refused.IsOk());
		TestEqual(TEXT("the refusal is AllocationTooLarge - retry, not an error"),
			(int32)Refused.Error, (int32)ECFDVizError::AllocationTooLarge);

		// RULE 2: A REFUSAL LEAVES THE CACHE UNCHANGED. The obvious loop - evict
		// until it fits, then discover it does not - empties the cache and then
		// fails, so an impossible request costs every resident frame and buys
		// nothing. That failure renders as a black screen after one bad seek.
		TestEqual(TEXT("a refused admission evicts NOTHING"), Evicted2.Num(), 0);
		TestTrue(TEXT("a refused admission leaves pinned frame 0 resident"), Cache.Contains(0));
		TestTrue(TEXT("a refused admission leaves pinned frame 1 resident"), Cache.Contains(1));
		TestEqual(TEXT("a refused admission leaves the entry count alone"), Cache.Num(), 2);
		TestEqual(TEXT("a refused admission changes no byte total"), Cache.GetCpuBytes(), (int64)200);
		TestFalse(TEXT("the refused frame was not admitted"), Cache.Contains(9));

		// Unpinning makes the SAME admission succeed. This is the control: it
		// proves the refusal above was caused by the pinning and not by the frame
		// being too large or by some unrelated refusal, which an
		// AllocationTooLarge result alone cannot distinguish.
		Cache.SetPinnedFrames(INDEX_NONE, INDEX_NONE);
		TestFalse(TEXT("clearing the pins unpins frame 0"), Cache.IsPinned(0));
		TArray<int32> Evicted3;
		TestTrue(TEXT("with nothing pinned the SAME admission succeeds"),
			Cache.Admit(9, 100, 100, Evicted3).IsOk());
		TestEqual(TEXT("it evicted one frame"), Evicted3.Num(), 1);
		TestEqual(TEXT("it evicted the least recently used, frame 0"), Evicted3[0], 0);
	}

	/* == A frame too large for the whole budget ============================= */
	{
		// Same rule-2 property, reached the other way: a frame that cannot fit
		// even in an EMPTY cache must leave the cache untouched.
		FFlowVizFrameCache Cache;
		Cache.Initialize(300, 300);

		TArray<int32> Evicted;
		Cache.Admit(0, 100, 100, Evicted);
		Cache.Admit(1, 100, 100, Evicted);

		TArray<int32> Evicted2;
		const FCFDVizResult TooBig = Cache.Admit(5, 5000, 100, Evicted2);
		TestFalse(TEXT("a frame larger than the entire budget is refused"), TooBig.IsOk());
		TestEqual(TEXT("an oversized frame evicts nothing"), Evicted2.Num(), 0);
		TestEqual(TEXT("an oversized refusal leaves both frames resident"), Cache.Num(), 2);
		TestEqual(TEXT("an oversized refusal leaves the totals alone"), Cache.GetCpuBytes(), (int64)200);
	}

	/* == The CPU and GPU budgets are independent ============================ */
	{
		// A 3-component float16 field is 6 CPU bytes per voxel and 8 GPU bytes
		// after the 3 -> 4 channel widening, so the two budgets bind at different
		// times. A cache with one budget derived from the other overcommits
		// whichever it did not measure.
		FFlowVizFrameCache Cache;
		Cache.Initialize(10000, 300);

		TArray<int32> Evicted;
		Cache.Admit(0, 60, 100, Evicted);
		Cache.Admit(1, 60, 100, Evicted);
		Cache.Admit(2, 60, 100, Evicted);
		TestEqual(TEXT("three frames well inside the CPU budget"), Cache.GetCpuBytes(), (int64)180);
		TestEqual(TEXT("...are exactly at the GPU budget"), Cache.GetGpuBytes(), (int64)300);
		TestEqual(TEXT("nothing was evicted"), Evicted.Num(), 0);

		// The CPU budget has room for many more; the GPU budget does not. The
		// eviction must be driven by the GPU side.
		Cache.Admit(3, 60, 100, Evicted);
		TestEqual(TEXT("the GPU budget alone forces an eviction"), Evicted.Num(), 1);
		TestEqual(TEXT("the GPU total stays at its budget"), Cache.GetGpuBytes(), (int64)300);
		TestTrue(TEXT("the CPU total is still far under its budget"), Cache.GetCpuBytes() < 300);
	}

	/* == Shrinking a budget evicts ========================================== */
	{
		FFlowVizFrameCache Cache;
		Cache.Initialize(1000, 1000);

		TArray<int32> Evicted;
		Cache.Admit(0, 300, 300, Evicted);
		Cache.Admit(1, 300, 300, Evicted);
		Cache.Admit(2, 300, 300, Evicted);
		TestEqual(TEXT("three 300-byte frames are resident"), Cache.Num(), 3);

		TArray<int32> Shrunk;
		TestTrue(TEXT("the budget can be shrunk"), Cache.SetBudgets(650, 1000, Shrunk).IsOk());
		TestEqual(TEXT("shrinking to 650 evicts one 300-byte frame"), Shrunk.Num(), 1);
		TestEqual(TEXT("shrinking evicts the least recently used"), Shrunk[0], 0);
		TestEqual(TEXT("the survivors fit the new budget"), Cache.GetCpuBytes(), (int64)600);

		// A budget below the minimum is refused and changes nothing.
		TArray<int32> Untouched;
		TestFalse(TEXT("a zero budget is refused"), Cache.SetBudgets(0, 1000, Untouched).IsOk());
		TestEqual(TEXT("a refused budget change evicts nothing"), Untouched.Num(), 0);
		TestEqual(TEXT("a refused budget change leaves the budget alone"),
			Cache.GetCpuBudgetBytes(), (int64)650);
	}

	/* == Bad arguments and bookkeeping ====================================== */
	{
		FFlowVizFrameCache Cache;
		TestFalse(TEXT("a zero CPU budget is refused at Initialize"), Cache.Initialize(0, 100).IsOk());
		TestFalse(TEXT("a negative GPU budget is refused at Initialize"), Cache.Initialize(100, -1).IsOk());
		TestTrue(TEXT("a legal budget initializes"), Cache.Initialize(1000, 1000).IsOk());

		TArray<int32> Evicted;
		TestFalse(TEXT("a negative frame index is refused"), Cache.Admit(-1, 10, 10, Evicted).IsOk());
		TestFalse(TEXT("a negative byte count is refused"), Cache.Admit(0, -10, 10, Evicted).IsOk());
		TestEqual(TEXT("a refused argument admits nothing"), Cache.Num(), 0);

		Cache.Admit(0, 10, 10, Evicted);
		Cache.MarkComplete(0);
		Cache.Admit(1, 20, 20, Evicted);

		const FFlowVizFrameCacheStats Stats = Cache.GetStats();
		TestEqual(TEXT("stats count both entries"), Stats.EntryCount, 2);
		TestEqual(TEXT("stats count only the complete one as complete"), Stats.CompleteCount, 1);
		TestEqual(TEXT("stats sum the CPU bytes"), Stats.CpuBytes, (int64)30);
		TestEqual(TEXT("stats report the budget"), Stats.CpuBudgetBytes, (int64)1000);

		TestTrue(TEXT("a present frame can be removed"), Cache.Remove(0));
		TestFalse(TEXT("removing it again reports failure"), Cache.Remove(0));
		TestEqual(TEXT("removing releases its bytes"), Cache.GetCpuBytes(), (int64)20);

		Cache.Reset();
		TestEqual(TEXT("Reset drops every entry"), Cache.Num(), 0);
		TestEqual(TEXT("Reset zeroes the totals"), Cache.GetCpuBytes(), (int64)0);
		TestEqual(TEXT("Reset keeps the budgets"), Cache.GetCpuBudgetBytes(), (int64)1000);
		TestEqual(TEXT("an empty cache has no eviction candidate"),
			Cache.PeekEvictionCandidate(), INDEX_NONE);
	}

	return true;
}

/* ========================================================================== */
/* The player, against the committed sample case                              */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizCasePlayerTest,
	"FlowViz.Playback.CasePlayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizCasePlayerTest::RunTest(const FString& Parameters)
{
	const FString CaseDir = FlowVizCasePlayerTest::GetPlaybackSampleCaseDir();
	const FString ManifestPath = FPaths::Combine(CaseDir, TEXT("manifest.json"));

	if (CaseDir.IsEmpty() || !FPaths::FileExists(ManifestPath))
	{
		// A skip with a stated reason, never a silent pass.
		AddInfo(FString::Printf(
			TEXT("SKIPPED: no sample case at '%s'. Generate one with "
				 "'python3 -m cfdviz generate-mock --low-res' to exercise the decode path."),
			*ManifestPath));
		return true;
	}

	TSharedRef<FCFDVizCase> Case = MakeShared<FCFDVizCase>();
	const FCFDVizResult Load = FCFDVizCase::LoadFromFile(ManifestPath, Case.Get());
	if (!Load.IsOk())
	{
		AddError(FString::Printf(TEXT("failed to load the sample manifest: %s"), *Load.ToString()));
		return false;
	}

	FFlowVizCasePlayer Player;

	// A field that does not exist must be refused by name, not silently ignored.
	TestFalse(TEXT("opening an unknown field is refused"),
		Player.Open(Case, FName(TEXT("noSuchField"))).IsOk());
	TestFalse(TEXT("a refused Open leaves the player closed"), Player.IsOpen());

	const FCFDVizResult Opened = Player.Open(Case, FName(TEXT("U")));
	if (!Opened.IsOk())
	{
		AddError(FString::Printf(TEXT("failed to open field U: %s"), *Opened.ToString()));
		return false;
	}
	TestTrue(TEXT("the player is open"), Player.IsOpen());
	TestEqual(TEXT("the timeline has the sample's 20 frames"), Player.GetTimeline().GetFrameCount(), 20);

	// The size the cache reserves against is computed from the MANIFEST, before
	// anything is read - engineering rule 12 wants the check before the
	// allocation it guards. U is 3-component float16 on a 56x28x6 cell grid.
	int64 CpuBytes = 0;
	int64 GpuBytes = 0;
	TestTrue(TEXT("the per-frame size is estimable from the manifest alone"),
		Player.TryEstimateFrameBytes(CpuBytes, GpuBytes));
	TestEqual(TEXT("U decodes to 56*28*6*3*2 bytes on the CPU"),
		CpuBytes, (int64)56 * 28 * 6 * 3 * 2);
	// The 3 -> 4 channel widening plus the one-byte-per-voxel status texture.
	TestEqual(TEXT("U uploads as 56*28*6*(4*2 + 1) bytes on the GPU"),
		GpuBytes, (int64)56 * 28 * 6 * (4 * 2 + 1));

	/* == Boundary: frame 0 and the final frame ============================== */
	{
		// Frame 0. Asserted by its TIME, which index arithmetic cannot fake.
		Player.SeekToFrame(0);
		TestEqual(TEXT("seek to frame 0 selects frame 0"), Player.GetSelection().FrameA, 0);
		TestEqual(TEXT("seek to frame 0 is t=0"), Player.GetPhysicalTime(), 0.0, 1.0e-12);
		TestFalse(TEXT("an exact frame is not interpolated"), Player.GetSelection().bInterpolated);

		// A frame that is NOT resident: the display must not name it.
		TestFalse(TEXT("before any load, frame 0 is not resident"),
			Player.GetCache().IsResident(0));
		TestFalse(TEXT("before any load the display is invalid"), Player.GetDisplay().IsValid());
		TestTrue(TEXT("before any load the display is stale"), Player.GetDisplay().bStale);

		// Tick starts the decode; the wait is a test-only convenience.
		Player.Tick(0.0);
		TestTrue(TEXT("a tick starts at least one load"), Player.GetDiagnostics().LoadsStarted > 0);
		TestTrue(TEXT("the loads complete"), Player.WaitForPendingLoads(60.0));
		Player.Tick(0.0);

		TestTrue(TEXT("frame 0 is now resident"), Player.GetCache().IsResident(0));
		TestEqual(TEXT("the display now names frame 0"), Player.GetDisplay().FrameA, 0);
		TestFalse(TEXT("the display is no longer stale"), Player.GetDisplay().bStale);
		TestEqual(TEXT("the display's physical time is frame 0's"),
			Player.GetDisplay().Time, 0.0, 1.0e-12);

		// The FINAL frame - index 19, t = 0.95. One past it must clamp.
		Player.SeekToFrame(19);
		TestEqual(TEXT("seek to the final frame selects frame 19"), Player.GetSelection().FrameA, 19);
		TestEqual(TEXT("the final frame is t=0.95"), Player.GetPhysicalTime(), 0.95, 1.0e-12);

		Player.SeekToFrame(20);
		TestEqual(TEXT("seek one past the end clamps to frame 19"), Player.GetSelection().FrameA, 19);
		Player.SeekToFrame(-1);
		TestEqual(TEXT("seek before the start clamps to frame 0"), Player.GetSelection().FrameA, 0);

		// The normalized slider hits both ends exactly.
		Player.SeekToNormalized(0.0);
		TestEqual(TEXT("slider at 0 is the first frame"), Player.GetSelection().FrameA, 0);
		Player.SeekToNormalized(1.0);
		TestEqual(TEXT("slider at 1 is the last frame"), Player.GetSelection().FrameA, 19);
		TestEqual(TEXT("slider at 1 is the last time"), Player.GetPhysicalTime(), 0.95, 1.0e-12);
	}

	/* == The same frame requested twice is a cache hit ====================== */
	{
		Player.SeekToFrame(5);
		Player.Tick(0.0);
		TestTrue(TEXT("frame 5 loads"), Player.WaitForPendingLoads(60.0));
		Player.Tick(0.0);
		TestTrue(TEXT("frame 5 is resident"), Player.GetCache().IsResident(5));

		const FFlowVizPlaybackDiagnostics AfterFirst = Player.GetDiagnostics();

		// Seeking away and back must NOT decode frame 5 again - it is still
		// resident. A player that re-requests a resident frame does redundant file
		// I/O on every scrub and is impossible to notice by looking at the screen.
		Player.SeekToFrame(5);
		Player.Tick(0.0);
		const FFlowVizPlaybackDiagnostics AfterSecond = Player.GetDiagnostics();

		TestEqual(TEXT("re-selecting a resident frame starts no new load"),
			AfterSecond.LoadsStarted, AfterFirst.LoadsStarted);
		TestTrue(TEXT("re-selecting a resident frame counts a cache hit"),
			AfterSecond.CacheHits > AfterFirst.CacheHits);
		TestTrue(TEXT("frame 5 is still resident"), Player.GetCache().IsResident(5));
	}

	/* == Holding the last complete frame across a seek ====================== */
	{
		// Seek somewhere not yet loaded. Until it arrives the display must keep
		// showing what it had - NOT the new frame's index with old pixels, and not
		// nothing.
		Player.SeekToFrame(5);
		Player.Tick(0.0);
		Player.WaitForPendingLoads(60.0);
		Player.Tick(0.0);
		const FFlowVizDisplaySelection Held = Player.GetDisplay();
		TestEqual(TEXT("the display shows frame 5"), Held.FrameA, 5);

		Player.SeekToFrame(17);
		// Deliberately do NOT wait: this is the mid-load state.
		TestEqual(TEXT("the selection has moved to frame 17"), Player.GetSelection().FrameA, 17);
		if (!Player.GetCache().IsResident(17))
		{
			TestEqual(TEXT("the display still HOLDS frame 5 while 17 loads"),
				Player.GetDisplay().FrameA, 5);
			TestEqual(TEXT("the held display keeps frame 5's physical time"),
				Player.GetDisplay().Time, Held.Time, 1.0e-12);
			TestTrue(TEXT("the held display reports itself stale"), Player.GetDisplay().bStale);
		}
		else
		{
			AddInfo(TEXT("frame 17 was already resident; the mid-load hold was not exercised here "
						 "(it is covered exhaustively by ResolveDisplay in FlowViz.Playback.Frames)."));
		}

		Player.Tick(0.0);
		TestTrue(TEXT("frame 17 loads"), Player.WaitForPendingLoads(60.0));
		Player.Tick(0.0);
		TestEqual(TEXT("the display catches up to frame 17"), Player.GetDisplay().FrameA, 17);
		TestFalse(TEXT("the caught-up display is not stale"), Player.GetDisplay().bStale);
	}

	/* == An aggressive scrub cancels what it no longer wants ================= */
	{
		// Walk the playhead across the whole case without waiting. The bound that
		// matters is that in-flight work stays bounded: a player that never
		// cancels queues one decode per seek and finishes them all long after the
		// user has stopped scrubbing.
		Player.GetCache().Reset();
		for (int32 Frame = 0; Frame < 20; ++Frame)
		{
			Player.SeekToFrame(Frame);
			Player.Tick(0.0);
			TestTrue(TEXT("in-flight loads stay within the configured limit"),
				Player.GetDiagnostics().LoadsInFlight <= Player.GetMaxConcurrentLoads());
		}
		TestTrue(TEXT("the scrub's outstanding loads finish"), Player.WaitForPendingLoads(120.0));
		Player.Tick(0.0);

		const FFlowVizPlaybackDiagnostics Diag = Player.GetDiagnostics();
		TestTrue(TEXT("the scrub decoded something"), Diag.LoadsCompleted > 0);
		TestEqual(TEXT("no decode failed on the sample case"), Diag.LoadsFailed, (int64)0);
		TestTrue(TEXT("the cache respects its CPU budget"),
			Diag.Cache.CpuBytes <= Diag.Cache.CpuBudgetBytes);
		TestTrue(TEXT("the cache respects its GPU budget"),
			Diag.Cache.GpuBytes <= Diag.Cache.GpuBudgetBytes);
	}

	/* == The scrub actually CANCELS, and starvation is the reason it must ==== */
	{
		// WHAT THIS CATCHES THAT NOTHING ELSE HERE DOES. Every assertion in the
		// block above is satisfied by a player whose cancellation is deleted
		// outright - verified by mutation (drop_cancel SURVIVED the whole
		// FlowViz.Playback suite). "LoadsInFlight <= MaxConcurrentLoads" cannot
		// fail at all: StartPendingLoads breaks at that cap by construction, so
		// the bound is enforced by the loop being tested rather than observed.
		// A requirement with no falsifiable assertion is not covered, and
		// "cancel obsolete requests during aggressive scrubbing" is a plan.md
		// section 8 bullet.
		//
		// THE CONSEQUENCE IS STARVATION, NOT WASTE. Cancellation's job is not
		// tidiness - obsolete decodes hold the concurrency budget, so with the
		// sweep gone the frame the user is looking at is never even requested.
		// Pinning MaxConcurrentLoads to 1 makes that deterministic instead of a
		// race: exactly one slot exists, and a stale occupant owns it forever.
		// PRECONDITION, ASSERTED RATHER THAN ASSUMED. Earlier blocks leave decodes
		// in flight, and "how many slots are free" is the entire subject here, so
		// inheriting that state would make the first measurement meaningless.
		TestTrue(TEXT("the prior blocks' loads are drained"), Player.WaitForPendingLoads(120.0));
		Player.Tick(0.0);
		Player.GetCache().Reset();
		TestEqual(TEXT("the cancellation block starts with no load in flight"),
			Player.GetDiagnostics().LoadsInFlight, 0);

		const int32 RestoreMaxLoads = Player.GetMaxConcurrentLoads();
		TestTrue(TEXT("a single-slot concurrency limit is accepted"),
			Player.SetMaxConcurrentLoads(1).IsOk());

		// Occupy the one slot with frame 0, then abandon it WITHOUT draining.
		Player.SeekToFrame(0);
		Player.Tick(0.0);
		TestEqual(TEXT("the abandoned frame holds the only load slot"),
			Player.GetDiagnostics().LoadsInFlight, 1);
		TestEqual(TEXT("the occupant is the frame we are about to abandon"),
			Player.GetFrameLoadState(0), EFlowVizLoadState::Loading);

		// Now jump far away. Frame 0 is not in frame 19's request list (preload
		// radius 1), so it is obsolete the moment this tick runs.
		Player.SeekToFrame(19);
		const int64 CancelledBefore = Player.GetDiagnostics().LoadsCancelled;
		Player.Tick(0.0);
		const FFlowVizPlaybackDiagnostics AfterJump = Player.GetDiagnostics();

		TestTrue(TEXT("abandoning an in-flight frame cancels it"),
			AfterJump.LoadsCancelled > CancelledBefore);
		TestEqual(TEXT("the abandoned frame no longer holds a load slot"),
			Player.GetFrameLoadState(0), EFlowVizLoadState::Absent);

		// The point of the cancel: the freed slot went to the frame on screen.
		// Without the sweep frame 19 is never requested and this is Absent.
		TestEqual(TEXT("the freed slot went to the frame the playhead wants"),
			Player.GetFrameLoadState(19), EFlowVizLoadState::Loading);

		TestTrue(TEXT("the starved frame still arrives"), Player.WaitForPendingLoads(120.0));
		Player.Tick(0.0);
		TestTrue(TEXT("the frame the playhead wants becomes resident"),
			Player.GetCache().IsResident(19));

		TestTrue(TEXT("the concurrency limit is restored"),
			Player.SetMaxConcurrentLoads(RestoreMaxLoads).IsOk());
	}

	/* == Playing advances the playhead ====================================== */
	{
		Player.SeekToFrame(0);
		FFlowVizPlaybackSettings Settings = Player.GetSettings();
		Settings.Mode = EFlowVizPlaybackMode::Sequence;
		Settings.SequenceFrameRate = 10.0;
		Settings.LoopMode = EFlowVizLoopMode::Loop;
		Settings.Speed = 1.0;
		TestTrue(TEXT("the settings are accepted"), Player.SetSettings(Settings).IsOk());

		TestFalse(TEXT("the player starts paused"), Player.IsPlaying());
		const double BeforeTime = Player.GetPhysicalTime();
		Player.Tick(0.5);
		TestEqual(TEXT("a paused player does not advance"),
			Player.GetPhysicalTime(), BeforeTime, 1.0e-12);

		Player.Play();
		TestTrue(TEXT("the player is playing"), Player.IsPlaying());
		Player.Tick(0.5);
		// 10 frames/s for 0.5 s is 5 stored frames; the sample is uniform at 0.05.
		TestEqual(TEXT("Sequence playback advances 5 frames in half a second"),
			Player.GetPhysicalTime(), 0.25, 1.0e-9);

		Player.Pause();
		TestFalse(TEXT("the player is paused"), Player.IsPlaying());

		Player.Stop();
		TestFalse(TEXT("Stop pauses"), Player.IsPlaying());
		TestEqual(TEXT("Stop returns to the first frame"), Player.GetPhysicalTime(), 0.0, 1.0e-12);

		// A bad speed is refused and changes nothing.
		const double GoodSpeed = Player.GetSettings().Speed;
		TestFalse(TEXT("a zero speed is refused"), Player.SetSpeed(0.0).IsOk());
		TestEqual(TEXT("a refused speed leaves the old one in place"),
			Player.GetSettings().Speed, GoodSpeed);
		TestTrue(TEXT("a legal speed is accepted"), Player.SetSpeed(2.0).IsOk());
		TestEqual(TEXT("the legal speed took effect"), Player.GetSettings().Speed, 2.0);

		TestFalse(TEXT("an out-of-range preset index is refused"),
			Player.SetSpeedPreset(FlowVizPlayback::NumSpeedPresets).IsOk());
		TestTrue(TEXT("a legal preset index is accepted"),
			Player.SetSpeedPreset(FlowVizPlayback::DefaultSpeedPresetIndex).IsOk());
		TestEqual(TEXT("the default preset is 1.0"), Player.GetSettings().Speed, 1.0);
	}

	/* == Interpolation between two stored frames ============================ */
	{
		// The sample's frames are 0.05 apart; 0.075 is exactly half way between
		// frames 1 and 2. This is the state VISUAL_QA §1 rule 5 requires the UI to
		// disclose.
		Player.SetInterpolationEnabled(true);
		Player.SeekToTime(0.075);
		const FFlowVizFrameSelection Blend = Player.GetSelection();
		TestEqual(TEXT("t=0.075 blends frame 1..."), Blend.FrameA, 1);
		TestEqual(TEXT("...with frame 2"), Blend.FrameB, 2);
		TestEqual(TEXT("t=0.075 is exactly half way"), Blend.Alpha, 0.5, 1.0e-9);
		TestTrue(TEXT("the blend is disclosed as interpolated"), Blend.bInterpolated);

		// Turning interpolation off must collapse to ONE frame and clear the flag.
		Player.SetInterpolationEnabled(false);
		const FFlowVizFrameSelection Nearest = Player.GetSelection();
		TestEqual(TEXT("with interpolation off A and B are the same frame"),
			Nearest.FrameA, Nearest.FrameB);
		TestEqual(TEXT("with interpolation off alpha is 0"), Nearest.Alpha, 0.0);
		TestFalse(TEXT("with interpolation off nothing is disclosed as interpolated"),
			Nearest.bInterpolated);

		// The diagnostics panel's four required readouts (plan.md section 8).
		Player.SetInterpolationEnabled(true);
		Player.SeekToTime(0.075);
		const FFlowVizPlaybackDiagnostics Diag = Player.GetDiagnostics();
		TestEqual(TEXT("diagnostics report frame A"), Diag.Selection.FrameA, 1);
		TestEqual(TEXT("diagnostics report frame B"), Diag.Selection.FrameB, 2);
		TestEqual(TEXT("diagnostics report alpha"), Diag.Selection.Alpha, 0.5, 1.0e-9);
		TestEqual(TEXT("diagnostics report the physical time"), Diag.PhysicalTime, 0.075, 1.0e-12);
		TestEqual(TEXT("diagnostics report the frame count"), Diag.FrameCount, 20);

		// WHAT THIS CATCHES THAT NOTHING ELSE HERE DOES. Every assertion above
		// reads Diag.Selection - what the playhead WANTS. Nothing read
		// Diag.Display - what the renderer may actually SAMPLE. The struct's own
		// comment says why both are carried: "showing only one would make a
		// stalled loader indistinguishable from a stopped playhead, which is
		// precisely the thing a diagnostics panel is for."
		//
		// That comment named a hazard and no assertion covered it. A mutant that
		// copies Selection into Display - the panel reporting the wanted frame as
		// though it were on screen, with bStale hardcoded false - passed all four
		// FlowViz.Playback tests (drop_diag_display SURVIVED). It is the precise
		// failure the comment describes, and it is the WORST failure a
		// diagnostics panel can have: it is the instrument you would reach for to
		// diagnose a stall, reporting that nothing is stalled.
		//
		// Found by grepping my own prose for two-state hazard words
		// ("indistinguishable", "look identical"). A comment describing a hazard
		// is a receipt for a debt, not a payment.
		Player.GetCache().Reset();
		Player.SeekToFrame(11);
		const FFlowVizPlaybackDiagnostics Stalled = Player.GetDiagnostics();

		TestEqual(TEXT("the panel reports the playhead's wanted frame"),
			Stalled.Selection.FrameA, 11);
		TestNotEqual(TEXT("the panel does NOT report the wanted frame as shown"),
			Stalled.Display.FrameA, Stalled.Selection.FrameA);
		TestTrue(TEXT("the panel discloses that the display is stale"),
			Stalled.Display.bStale);

		// And it must stop saying so once the frame actually arrives, or "stale"
		// is a constant rather than a readout.
		Player.Tick(0.0);
		TestTrue(TEXT("the stalled frame loads"), Player.WaitForPendingLoads(120.0));
		Player.Tick(0.0);
		const FFlowVizPlaybackDiagnostics CaughtUp = Player.GetDiagnostics();
		TestEqual(TEXT("the caught-up panel shows the frame the playhead wants"),
			CaughtUp.Display.FrameA, CaughtUp.Selection.FrameA);
		TestFalse(TEXT("the caught-up panel no longer reports staleness"),
			CaughtUp.Display.bStale);
	}

	Player.Close();
	TestFalse(TEXT("Close unbinds the player"), Player.IsOpen());

	return true;
}

/* ========================================================================== */
/* The seam onto IFlowVizVolumeFrameSource                                    */
/* ========================================================================== */

/**
 * WHAT THIS TEST CATCHES THAT NO OTHER TEST IN THIS FILE DOES.
 *
 * Every other test here works in double, because the player works in double.
 * This one covers the ONE place the value is narrowed to float, which is where
 * the component's FFlowVizVolumeFrameSelection stores Alpha. Narrowing is not
 * order-preserving near the endpoints, and the failure it produces is a
 * DISCLOSURE failure, not an arithmetic one: an alpha that is a genuine blend
 * in double can round to exactly 1.0f, at which point the component's
 * IsInterpolated() reports "not interpolated" while FrameA and FrameB are still
 * distinct - so the shader blends two frames that the UI has just told the
 * scientist are not blended. That is the VISUAL_QA section 1 rule 5 violation.
 *
 * The fixture is chosen so the bug is REACHABLE ON THE SHIPPED SAMPLE rather
 * than only on a synthetic pathology: the sample is spaced at 0.05, and
 * t = 0.10 - 1e-11 sits inside frame 1..2 with a double alpha of
 * 0.9999999998, which narrows to exactly 1.0f. A player that simply assigns
 * (float)Alpha passes every other test in this file and fails here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizPlaybackSeamTest,
	"FlowViz.Playback.Seam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizPlaybackSeamTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizPlayback;

	// ---------------------------------------------------------------------
	// The narrowing collapse. This is the whole reason the adapter exists.
	// ---------------------------------------------------------------------
	{
		// A genuine blend in double that narrows to exactly 1.0f.
		FFlowVizDisplaySelection Display;
		Display.FrameA = 1;
		Display.FrameB = 2;
		Display.Alpha = 1.0 - 1.0e-11;
		Display.Time = 0.0999999999;
		Display.NearestFrame = 2;
		Display.bInterpolated = true;

		// Establish the hazard is real before asserting the fix, so this test
		// cannot silently become vacuous if the constant is ever edited. EXACT
		// comparison on purpose: TestEqual on floats carries a 1e-4 tolerance,
		// which would report "narrows to 1.0f" for any alpha above 0.9999, and a
		// guard that passes without the hazard is not a guard.
		TestTrue(TEXT("the fixture alpha really does narrow to EXACTLY 1.0f"),
			(float)Display.Alpha == 1.0f);
		TestTrue(TEXT("...while being a genuine blend in double"),
			Display.Alpha < 1.0 && Display.Alpha > 0.0);

		const FFlowVizVolumeFrameSelection Seam = ToVolumeFrameSelection(Display);

		// The pair must collapse so that geometry and disclosure AGREE. Either
		// answer alone is defensible; disagreement is not.
		TestFalse(TEXT("a pair that narrows to alpha 1 is not disclosed as interpolated"),
			Seam.IsInterpolated());
		TestEqual(TEXT("...and it names frame B alone, which is the frame at alpha 1"),
			Seam.FrameA, 2);
		TestEqual(TEXT("...with B collapsed onto A so no blend is issued"),
			Seam.FrameB, Seam.FrameA);
		TestEqual(TEXT("...at alpha 0"), Seam.Alpha, 0.0f);
	}

	// The same hazard at the other end: an alpha just above 0 narrowing to 0.0f.
	//
	// 1e-45 does NOT work here and the first draft of this test used it: float's
	// smallest subnormal is 1.4013e-45, so (float)1e-45 is subnormal-but-nonzero
	// and the collapse correctly does not fire. 1e-46 is below the subnormal
	// floor and flushes to a true zero.
	//
	// Worse, the guard below originally used TestEqual on floats, which applies a
	// 1e-4 tolerance - so it reported the fixture as "narrowing to 0.0f" when it
	// narrowed to 1.4e-45. A guard written to prove the hazard is real must use
	// an EXACT comparison, or it is the same unfireable check it was added to
	// prevent. TestTrue with == is deliberate.
	{
		FFlowVizDisplaySelection Display;
		Display.FrameA = 3;
		Display.FrameB = 4;
		Display.Alpha = 1.0e-46;   // below float's smallest subnormal, 1.4013e-45
		Display.bInterpolated = true;

		TestTrue(TEXT("the low-end fixture really does narrow to EXACTLY 0.0f"),
			(float)Display.Alpha == 0.0f);
		TestTrue(TEXT("...while being nonzero in double"), Display.Alpha > 0.0);

		const FFlowVizVolumeFrameSelection Seam = ToVolumeFrameSelection(Display);
		TestFalse(TEXT("a pair that narrows to alpha 0 is not disclosed as interpolated"),
			Seam.IsInterpolated());
		TestEqual(TEXT("...and it names frame A alone"), Seam.FrameA, 3);
		TestEqual(TEXT("...with B collapsed onto A"), Seam.FrameB, Seam.FrameA);
		TestEqual(TEXT("...at alpha 0"), Seam.Alpha, 0.0f);
	}

	// ---------------------------------------------------------------------
	// A real blend must SURVIVE the seam. Without this, an adapter that
	// collapsed everything to frame A would pass both cases above.
	// ---------------------------------------------------------------------
	{
		FFlowVizDisplaySelection Display;
		Display.FrameA = 1;
		Display.FrameB = 2;
		Display.Alpha = 0.5;
		Display.bInterpolated = true;

		const FFlowVizVolumeFrameSelection Seam = ToVolumeFrameSelection(Display);
		TestEqual(TEXT("a genuine blend keeps frame A"), Seam.FrameA, 1);
		TestEqual(TEXT("a genuine blend keeps frame B"), Seam.FrameB, 2);
		TestEqual(TEXT("a genuine blend keeps its alpha"), Seam.Alpha, 0.5f);
		TestTrue(TEXT("a genuine blend IS disclosed as interpolated"),
			Seam.IsInterpolated());
	}

	// ---------------------------------------------------------------------
	// ORDER INDEPENDENCE. The adapter is a chain of guards, and a chain is
	// exactly where a later branch can mask an earlier one without any single
	// case noticing - the same shape as an accumulator whose fixture only ever
	// writes it once, where max(Acc,X) and Acc=X agree because nothing looser
	// ever arrives second.
	//
	// Here the equivalent is reusing one selection and writing the alpha TWICE,
	// the second time with the value that must win. If the collapse ever became
	// sticky (say by caching the first narrowed alpha, or by testing FrameB
	// before re-reading Alpha), every case above would still pass because each
	// builds a fresh struct.
	// ---------------------------------------------------------------------
	{
		FFlowVizDisplaySelection Display;
		Display.FrameA = 7;
		Display.FrameB = 8;
		Display.bInterpolated = true;

		// First write: a value that COLLAPSES.
		Display.Alpha = 1.0 - 1.0e-11;
		const FFlowVizVolumeFrameSelection Collapsed = ToVolumeFrameSelection(Display);
		TestEqual(TEXT("first write collapses onto frame B"), Collapsed.FrameA, 8);
		TestFalse(TEXT("first write is not interpolated"), Collapsed.IsInterpolated());

		// Second write on the SAME struct: a value that must NOT collapse.
		Display.Alpha = 0.25;
		const FFlowVizVolumeFrameSelection Blended = ToVolumeFrameSelection(Display);
		TestEqual(TEXT("second write restores frame A"), Blended.FrameA, 7);
		TestEqual(TEXT("second write restores frame B"), Blended.FrameB, 8);
		TestEqual(TEXT("second write keeps its alpha"), Blended.Alpha, 0.25f);
		TestTrue(TEXT("second write IS interpolated"), Blended.IsInterpolated());

		// And back the other way, so neither ordering is the privileged one.
		Display.Alpha = 1.0e-46;
		const FFlowVizVolumeFrameSelection Vanished = ToVolumeFrameSelection(Display);
		TestEqual(TEXT("third write collapses onto frame A"), Vanished.FrameA, 7);
		TestEqual(TEXT("...with B collapsed onto A"), Vanished.FrameB, 7);
		TestFalse(TEXT("third write is not interpolated"), Vanished.IsInterpolated());
	}

	// ---------------------------------------------------------------------
	// A SELF-PAIR IS ALREADY COLLAPSED, at every alpha.
	//
	// Found by mutation: removing the `FrameB == FrameA` arm of the early-out
	// SURVIVED the rest of this test. Nothing else here builds a selection whose
	// two frames are equal while alpha is a strict mid-range blend, so nothing
	// noticed that such a pair shipped a nonzero alpha.
	//
	// It is not a disclosure bug - IsInterpolated() is already false when the
	// frames match, so the UI stays honest - which is exactly why no existing
	// assertion caught it. It is a WASTED BLEND: the shader is handed a weight
	// and told to interpolate frame N against frame N. The reason to reject it
	// is that "alpha is meaningless when the frames are the same" then stops
	// being true of the value actually crossing the seam, and the next person to
	// read alpha without also checking FrameB gets a number that means nothing.
	// ---------------------------------------------------------------------
	{
		for (const double SelfAlpha : { 0.0, 0.25, 0.5, 1.0 - 1.0e-11, 1.0 })
		{
			FFlowVizDisplaySelection Display;
			Display.FrameA = 5;
			Display.FrameB = 5;
			Display.Alpha = SelfAlpha;

			const FFlowVizVolumeFrameSelection Seam = ToVolumeFrameSelection(Display);
			TestEqual(TEXT("a self-pair keeps its frame"), Seam.FrameA, 5);
			TestEqual(TEXT("a self-pair stays a self-pair"), Seam.FrameB, 5);
			TestTrue(TEXT("a self-pair carries alpha 0 whatever alpha was asked for"),
				Seam.Alpha == 0.0f);
			TestFalse(TEXT("a self-pair is never disclosed as interpolated"),
				Seam.IsInterpolated());
		}
	}

	// ---------------------------------------------------------------------
	// A SINGLE-FRAME DISPLAY (FrameB == INDEX_NONE) IS ALREADY COLLAPSED, at
	// every alpha.
	//
	// Found by the same clause-enumeration a peer applied to their predicate:
	// list the conditions the code branches on, then check each has a fixture
	// that can distinguish it. The early-out has two arms and only one was
	// covered - this is the second, and dropping it SURVIVED the test as it
	// stood.
	//
	// The failure it admits is the worst one available at this seam. With the
	// arm gone, a single-frame display falls through to the narrowing branches
	// carrying FrameB == INDEX_NONE. At a narrowed alpha of 1 the saturation
	// branch then assigns FrameA = Display.FrameB, i.e. INDEX_NONE, and the
	// component draws NOTHING while a perfectly good frame is resident. Sixteen
	// enumerated (frame, alpha) cases differ; the alpha == 1 ones black the
	// volume out entirely.
	//
	// A single frame with a stale nonzero alpha is a real state, not a
	// contrivance: the player produces it whenever interpolation is off or a
	// blend partner has not finished decoding, and Alpha is documented as
	// "meaningless when FrameB is INDEX_NONE" - which is only safe if the seam
	// enforces it rather than trusting every caller to remember.
	// ---------------------------------------------------------------------
	{
		for (const double StaleAlpha : { 0.0, 0.25, 0.5, 1.0 - 1.0e-11, 1.0 })
		{
			FFlowVizDisplaySelection Display;
			Display.FrameA = 6;
			Display.FrameB = INDEX_NONE;
			Display.Alpha = StaleAlpha;   // stale/meaningless, must not be honoured

			const FFlowVizVolumeFrameSelection Seam = ToVolumeFrameSelection(Display);
			TestEqual(TEXT("a single-frame display still draws its frame"), Seam.FrameA, 6);
			TestEqual(TEXT("a single-frame display never yields an unset frame A"),
				Seam.FrameA, 6);
			TestEqual(TEXT("a single-frame display collapses B onto A"), Seam.FrameB, 6);
			TestTrue(TEXT("a single-frame display carries alpha 0 whatever alpha was asked for"),
				Seam.Alpha == 0.0f);
			TestFalse(TEXT("a single-frame display is never disclosed as interpolated"),
				Seam.IsInterpolated());
		}
	}

	// ---------------------------------------------------------------------
	// An empty display must produce nothing drawable, not frame 0. Distinct
	// from the collapse cases: those have a frame, this has none.
	// ---------------------------------------------------------------------
	{
		const FFlowVizDisplaySelection Empty;
		TestFalse(TEXT("the empty display is not valid"), Empty.IsValid());

		const FFlowVizVolumeFrameSelection Seam = ToVolumeFrameSelection(Empty);
		TestEqual(TEXT("an empty display draws nothing"), Seam.FrameA, (int32)INDEX_NONE);
		TestFalse(TEXT("an empty display discloses no interpolation"),
			Seam.IsInterpolated());
	}

	// ---------------------------------------------------------------------
	// The adapter must be usable AS the interface, not merely convertible.
	// This catches a signature that compiles standalone but cannot be bound
	// to the component, which is the only thing the seam is for.
	// ---------------------------------------------------------------------
	{
		FFlowVizCasePlayer Player;
		const TSharedRef<IFlowVizVolumeFrameSource> Source =
			MakeFrameSource(Player);

		// A closed player is a legal state; the seam must not invent a frame.
		const FFlowVizVolumeFrameSelection Seam = Source->GetFrameSelection();
		TestEqual(TEXT("a closed player selects no frame through the seam"),
			Seam.FrameA, (int32)INDEX_NONE);
		TestFalse(TEXT("a closed player discloses no interpolation"),
			Seam.IsInterpolated());
	}

	// ---------------------------------------------------------------------
	// End to end on the shipped sample, at the exact time that produces the
	// narrowing hazard. This is what proves the constant above is not a
	// synthetic value chosen to make the adapter look necessary.
	// ---------------------------------------------------------------------
	{
		const FString CaseDir = FlowVizCasePlayerTest::GetPlaybackSampleCaseDir();
		const FString ManifestPath = FPaths::Combine(CaseDir, TEXT("manifest.json"));

		TSharedRef<FCFDVizCase> Shared = MakeShared<FCFDVizCase>();
		const bool bHaveCase = !CaseDir.IsEmpty() && FPaths::FileExists(ManifestPath)
			&& FCFDVizCase::LoadFromFile(ManifestPath, Shared.Get()).IsOk();
		if (!bHaveCase)
		{
			AddInfo(FString::Printf(
				TEXT("SKIPPED the end-to-end seam check: no sample case at '%s'. "
					 "The narrowing cases above still ran and are the substance of this test."),
				*ManifestPath));
		}
		else
		{
			FFlowVizCasePlayer Player;
			if (Player.Open(Shared, NAME_None).IsOk())
			{
				const TSharedRef<IFlowVizVolumeFrameSource> Source = MakeFrameSource(Player);

				Player.SetInterpolationEnabled(true);
				Player.SeekToTime(0.10 - 1.0e-11);

				const FFlowVizFrameSelection Want = Player.GetSelection();
				const FFlowVizVolumeFrameSelection Seam = Source->GetFrameSelection();

				// Whatever the player wanted in double, the seam must not tell
				// the UI "not interpolated" while handing the shader two frames.
				const bool bSeamBlends = Seam.FrameB != INDEX_NONE
					&& Seam.FrameB != Seam.FrameA
					&& Seam.Alpha > 0.0f && Seam.Alpha < 1.0f;
				TestEqual(
					TEXT("on the shipped sample the seam's disclosure matches what it "
						 "actually blends"),
					Seam.IsInterpolated(), bSeamBlends);

				AddInfo(FString::Printf(
					TEXT("sample seam: player alpha %.17g -> seam alpha %.9g, frames %d/%d"),
					Want.Alpha, Seam.Alpha, Seam.FrameA, Seam.FrameB));
			}
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
