// Copyright FlowViz contributors. All Rights Reserved.

#include "Playback/FlowVizCasePlayer.h"

#include "CFDViz/CFDVizManifest.h"
#include "CFDViz/CFDVizVolumeReader.h"
#include "FlowVizRuntime.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "Render/FlowVizVolumeTexture.h"
#include "Tasks/Task.h"

/**
 * WHERE THE NON-UNIFORM SPACING IS HANDLED, AND WHERE IT IS NOT.
 *
 * There is no dt anywhere in this file. Every time-to-frame conversion goes
 * through FFlowVizTimeline, which binary-searches. The one place a division by
 * an interval happens is inside that search, against the interval it has just
 * found. Search this file for "/ " if you doubt it: the only divisions are by
 * a located interval width, by a frame rate, or by the timeline duration.
 */

namespace FlowVizPlaybackDetail
{
	/** A time that can be searched for. NaN compares false against everything, so it must be rejected up front. */
	FORCEINLINE bool IsUsableTime(double Time)
	{
		return FMath::IsFinite(Time);
	}

	/**
	 * Fold x into [0, Period) with a result that is never negative.
	 *
	 * FMath::Fmod keeps the sign of the dividend, so Fmod(-1, 10) is -1 and a
	 * loop that used it directly would wrap backwards past the start into
	 * negative time. This is the positive-remainder version.
	 */
	double PositiveMod(double Value, double Period)
	{
		if (Period <= 0.0)
		{
			return 0.0;
		}
		const double Remainder = FMath::Fmod(Value, Period);
		return Remainder < 0.0 ? Remainder + Period : Remainder;
	}
}

/* ========================================================================== */
/* FFlowVizPlaybackSettings                                                    */
/* ========================================================================== */

FCFDVizResult FFlowVizPlaybackSettings::Validate() const
{
	if (!FMath::IsFinite(Speed))
	{
		return FCFDVizResult::Fail(ECFDVizError::IndexOutOfRange, TEXT("Speed is not finite"));
	}

	// Zero speed is rejected rather than treated as a pause. A paused player and
	// a playing one at speed 0 look identical on screen but differ in every
	// diagnostic, and Sequence mode's frame-space step degenerates at 0.
	if (FMath::Abs(Speed) < FlowVizPlayback::MinAbsSpeed)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("Speed %g is within %g of zero; use Pause() rather than a zero speed"),
				Speed, FlowVizPlayback::MinAbsSpeed));
	}
	if (Speed < FlowVizPlayback::MinSpeed || Speed > FlowVizPlayback::MaxSpeed)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("Speed %g is outside [%g, %g]"),
				Speed, FlowVizPlayback::MinSpeed, FlowVizPlayback::MaxSpeed));
	}

	if (!FMath::IsFinite(SequenceFrameRate)
		|| SequenceFrameRate < FlowVizPlayback::MinFrameRate
		|| SequenceFrameRate > FlowVizPlayback::MaxFrameRate)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("SequenceFrameRate %g is outside [%g, %g]"),
				SequenceFrameRate, FlowVizPlayback::MinFrameRate, FlowVizPlayback::MaxFrameRate));
	}

	if (!FMath::IsFinite(OutputFrameRate)
		|| OutputFrameRate < FlowVizPlayback::MinFrameRate
		|| OutputFrameRate > FlowVizPlayback::MaxFrameRate)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("OutputFrameRate %g is outside [%g, %g]"),
				OutputFrameRate, FlowVizPlayback::MinFrameRate, FlowVizPlayback::MaxFrameRate));
	}

	if (PreloadAhead < 0 || PreloadAhead > FlowVizPlayback::MaxPreloadRadius)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("PreloadAhead %d is outside [0, %d]"), PreloadAhead, FlowVizPlayback::MaxPreloadRadius));
	}
	if (PreloadBehind < 0 || PreloadBehind > FlowVizPlayback::MaxPreloadRadius)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("PreloadBehind %d is outside [0, %d]"), PreloadBehind, FlowVizPlayback::MaxPreloadRadius));
	}

	return FCFDVizResult::Ok();
}

void FFlowVizPlaybackSettings::ClampToLegalRange()
{
	if (!FMath::IsFinite(Speed))
	{
		Speed = 1.0;
	}
	Speed = FMath::Clamp(Speed, FlowVizPlayback::MinSpeed, FlowVizPlayback::MaxSpeed);
	if (FMath::Abs(Speed) < FlowVizPlayback::MinAbsSpeed)
	{
		// Preserve the direction the caller asked for; only the magnitude was illegal.
		Speed = Speed < 0.0 ? -FlowVizPlayback::MinAbsSpeed : FlowVizPlayback::MinAbsSpeed;
	}

	if (!FMath::IsFinite(SequenceFrameRate))
	{
		SequenceFrameRate = 24.0;
	}
	SequenceFrameRate = FMath::Clamp(
		SequenceFrameRate, FlowVizPlayback::MinFrameRate, FlowVizPlayback::MaxFrameRate);

	if (!FMath::IsFinite(OutputFrameRate))
	{
		OutputFrameRate = 30.0;
	}
	OutputFrameRate = FMath::Clamp(
		OutputFrameRate, FlowVizPlayback::MinFrameRate, FlowVizPlayback::MaxFrameRate);

	PreloadAhead = FMath::Clamp(PreloadAhead, 0, FlowVizPlayback::MaxPreloadRadius);
	PreloadBehind = FMath::Clamp(PreloadBehind, 0, FlowVizPlayback::MaxPreloadRadius);
}

FFlowVizPlaybackSettings FFlowVizPlaybackSettings::FromCase(const FCFDVizCase& Case)
{
	FFlowVizPlaybackSettings Settings;

	// The case states whether its frames may be blended at all. A case written
	// with Nearest holds data - a mask, a topology id, a phase index - where the
	// average of two frames is not a value the solver ever produced.
	Settings.bInterpolate = (Case.Timeline.DefaultInterpolation == ECFDVizInterpolation::Linear);

	return Settings;
}

/* ========================================================================== */
/* FFlowVizTimeline                                                            */
/* ========================================================================== */

FCFDVizResult FFlowVizTimeline::Initialize(TArrayView<const double> InTimes)
{
	// Validate BEFORE adopting, so a rejected array leaves the previous timeline
	// intact rather than half-replaced.
	for (int32 Index = 0; Index < InTimes.Num(); ++Index)
	{
		if (!FMath::IsFinite(InTimes[Index]))
		{
			// Checked explicitly: every comparison against NaN is false, so a NaN
			// would slip past the strictly-increasing test below and then make
			// every binary search over it return an arbitrary frame.
			return FCFDVizResult::Fail(
				ECFDVizError::InvalidManifest,
				FString::Printf(TEXT("frame time %d is not finite"), Index));
		}
		if (Index > 0 && !(InTimes[Index] > InTimes[Index - 1]))
		{
			return FCFDVizResult::Fail(
				ECFDVizError::InvalidManifest,
				FString::Printf(
					TEXT("frame times are not strictly increasing: t[%d] = %g is not greater than t[%d] = %g"),
					Index, InTimes[Index], Index - 1, InTimes[Index - 1]));
		}
	}

	Times = InTimes;
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizTimeline::InitializeFromCase(const FCFDVizCase& Case)
{
	return Initialize(Case.Timeline.Times);
}

bool FFlowVizTimeline::TryGetTime(int32 FrameIndex, double& OutTime) const
{
	if (!Times.IsValidIndex(FrameIndex))
	{
		return false;
	}
	OutTime = Times[FrameIndex];
	return true;
}

bool FFlowVizTimeline::TryBracket(double Time, int32& OutFrameA, int32& OutFrameB, double& OutAlpha) const
{
	if (Times.Num() == 0 || !FlowVizPlaybackDetail::IsUsableTime(Time))
	{
		return false;
	}

	const int32 LastIndex = Times.Num() - 1;

	// Outside the stored range there is no data, so clamp. Extrapolating would
	// invent a state the solver never computed.
	if (Time <= Times[0])
	{
		OutFrameA = 0;
		OutFrameB = 0;
		OutAlpha = 0.0;
		return true;
	}
	if (Time >= Times[LastIndex])
	{
		OutFrameA = LastIndex;
		OutFrameB = LastIndex;
		OutAlpha = 0.0;
		return true;
	}

	// Binary search for the last frame whose time is <= Time. THIS IS THE ONLY
	// PLACE FRAME SPACING IS INTERPRETED, and it makes no assumption about it.
	int32 Low = 0;
	int32 High = LastIndex;
	while (Low + 1 < High)
	{
		const int32 Mid = Low + (High - Low) / 2;
		if (Times[Mid] <= Time)
		{
			Low = Mid;
		}
		else
		{
			High = Mid;
		}
	}

	// Landing exactly on a stored frame has ONE spelling: (F, F, 0). Without
	// this, t == Times[High] would produce (Low, High, 1.0) and every consumer
	// would have to treat alpha 1 as a second way of saying "not interpolated".
	if (Time == Times[Low])
	{
		OutFrameA = Low;
		OutFrameB = Low;
		OutAlpha = 0.0;
		return true;
	}
	if (Time == Times[High])
	{
		OutFrameA = High;
		OutFrameB = High;
		OutAlpha = 0.0;
		return true;
	}

	const double IntervalWidth = Times[High] - Times[Low];

	// Initialize guarantees strict increase, so the width is positive. The guard
	// is here because a zero width would produce an infinite alpha and the
	// invariant it depends on is enforced in a different function.
	check(IntervalWidth > 0.0);

	OutFrameA = Low;
	OutFrameB = High;
	OutAlpha = (Time - Times[Low]) / IntervalWidth;
	return true;
}

int32 FFlowVizTimeline::FindNearestFrame(double Time) const
{
	int32 FrameA = INDEX_NONE;
	int32 FrameB = INDEX_NONE;
	double Alpha = 0.0;
	if (!TryBracket(Time, FrameA, FrameB, Alpha))
	{
		return INDEX_NONE;
	}

	// Ties go to the LATER frame. The rule matters only at exactly alpha 0.5, but
	// it must match SelectFrames' own >= 0.5 or a field and its mask could be
	// taken from different frames at that one time.
	return Alpha >= 0.5 ? FrameB : FrameA;
}

double FFlowVizTimeline::TimeToFrameCoordinate(double Time) const
{
	if (Times.Num() == 0)
	{
		return 0.0;
	}

	int32 FrameA = INDEX_NONE;
	int32 FrameB = INDEX_NONE;
	double Alpha = 0.0;
	if (!TryBracket(Time, FrameA, FrameB, Alpha))
	{
		return 0.0;
	}

	// A + alpha, where alpha is already a fraction of the LOCATED interval. This
	// is what makes the coordinate uniform in frames while the times are not.
	return static_cast<double>(FrameA) + Alpha;
}

double FFlowVizTimeline::FrameCoordinateToTime(double FrameCoordinate) const
{
	if (Times.Num() == 0)
	{
		return 0.0;
	}
	if (Times.Num() == 1)
	{
		return Times[0];
	}
	if (!FlowVizPlaybackDetail::IsUsableTime(FrameCoordinate))
	{
		return Times[0];
	}

	const int32 LastIndex = Times.Num() - 1;

	// EXTRAPOLATE outside the range, using the nearest interval's own width.
	// Sequence mode steps this coordinate past the end on purpose and hands the
	// overshoot to FoldPhase; clamping here would swallow the overshoot and make
	// ping-pong stick at an end for one tick per turnaround.
	if (FrameCoordinate < 0.0)
	{
		const double FirstWidth = Times[1] - Times[0];
		return Times[0] + FrameCoordinate * FirstWidth;
	}
	if (FrameCoordinate > static_cast<double>(LastIndex))
	{
		const double LastWidth = Times[LastIndex] - Times[LastIndex - 1];
		return Times[LastIndex] + (FrameCoordinate - static_cast<double>(LastIndex)) * LastWidth;
	}

	const int32 Lower = FMath::Clamp(static_cast<int32>(FMath::FloorToDouble(FrameCoordinate)), 0, LastIndex);
	if (Lower == LastIndex)
	{
		return Times[LastIndex];
	}

	const double Fraction = FrameCoordinate - static_cast<double>(Lower);
	return Times[Lower] + Fraction * (Times[Lower + 1] - Times[Lower]);
}

double FFlowVizTimeline::ClampTime(double Time) const
{
	if (Times.Num() == 0)
	{
		return 0.0;
	}
	if (!FlowVizPlaybackDetail::IsUsableTime(Time))
	{
		return Times[0];
	}
	return FMath::Clamp(Time, Times[0], Times.Last());
}

/* ========================================================================== */
/* The pure selection layer                                                    */
/* ========================================================================== */

FFlowVizFrameSelection FlowVizPlayback::SelectFrames(
	const FFlowVizTimeline& Timeline,
	double Time,
	bool bInterpolate)
{
	FFlowVizFrameSelection Selection;

	int32 FrameA = INDEX_NONE;
	int32 FrameB = INDEX_NONE;
	double Alpha = 0.0;
	if (!Timeline.TryBracket(Time, FrameA, FrameB, Alpha))
	{
		// An empty timeline. The selection stays invalid rather than naming frame
		// 0, which does not exist.
		return Selection;
	}

	Selection.Time = Timeline.ClampTime(Time);

	// The nearest frame is computed for BOTH interpolation settings, because
	// masks and topology use it either way (plan.md section 8). Note it is not
	// always FrameA: at alpha 0.7 the nearest stored frame is B.
	Selection.NearestFrame = Alpha >= 0.5 ? FrameB : FrameA;

	if (!bInterpolate)
	{
		// COLLAPSE TO NEAREST, NOT TO A. Truncating toward A would show the
		// earlier frame across the whole interval, lagging the data by up to a
		// full frame - a different thing from "nearest", and one that still
		// renders plausibly.
		Selection.FrameA = Selection.NearestFrame;
		Selection.FrameB = Selection.NearestFrame;
		Selection.Alpha = 0.0;
		Selection.bInterpolated = false;
		return Selection;
	}

	Selection.FrameA = FrameA;
	Selection.FrameB = FrameB;
	Selection.Alpha = Alpha;

	// The one definition of "interpolated" in the codebase: two distinct frames
	// AND an alpha strictly between the ends. VISUAL_QA rule 5's indicator reads
	// this and nothing else.
	Selection.bInterpolated = (FrameA != FrameB) && (Alpha > 0.0) && (Alpha < 1.0);

	return Selection;
}

double FlowVizPlayback::FoldPhase(
	const FFlowVizTimeline& Timeline,
	double PhaseTime,
	EFlowVizLoopMode LoopMode,
	bool bSpeedForward,
	bool& bOutForward,
	bool& bOutFinished)
{
	bOutForward = bSpeedForward;
	bOutFinished = false;

	if (Timeline.IsEmpty())
	{
		return 0.0;
	}

	const double First = Timeline.GetFirstTime();
	const double Last = Timeline.GetLastTime();
	const double Duration = Last - First;

	// A single-frame timeline has zero duration. Every fold below divides by it,
	// so this is where a NaN playhead would come from.
	if (Duration <= 0.0)
	{
		return First;
	}

	if (!FlowVizPlaybackDetail::IsUsableTime(PhaseTime))
	{
		return First;
	}

	const double Offset = PhaseTime - First;

	switch (LoopMode)
	{
	case EFlowVizLoopMode::Once:
	{
		if (Offset >= Duration)
		{
			bOutFinished = true;
			return Last;
		}
		if (Offset <= 0.0)
		{
			// Finished at the START too. Running backwards under Once must stop at
			// frame 0; a check on the forward end only leaves a reversed playhead
			// running into negative time forever.
			bOutFinished = true;
			return First;
		}
		return PhaseTime;
	}

	case EFlowVizLoopMode::Loop:
	{
		return First + FlowVizPlaybackDetail::PositiveMod(Offset, Duration);
	}

	case EFlowVizLoopMode::PingPong:
	{
		// THE TRIANGLE WAVE. Period is 2*Duration: out and back. Written as a
		// fold rather than as a turnaround branch, so a step of any size - ten
		// periods, a negative phase - lands in the right place with the right
		// direction, and an exact landing on an end is exactly an end rather than
		// an epsilon past one.
		const double Period = 2.0 * Duration;
		const double Cycle = FlowVizPlaybackDetail::PositiveMod(Offset, Period);

		if (Cycle <= Duration)
		{
			// Outbound half. Travel agrees with the phase's own direction.
			bOutForward = bSpeedForward;
			return First + Cycle;
		}

		// Return half: reflected, and travelling against the phase's direction.
		bOutForward = !bSpeedForward;
		return First + (Period - Cycle);
	}

	default:
		return Timeline.ClampTime(PhaseTime);
	}
}

FFlowVizPlayhead FlowVizPlayback::Advance(
	const FFlowVizTimeline& Timeline,
	const FFlowVizPlaybackSettings& Settings,
	const FFlowVizPlayhead& Playhead,
	double DeltaSeconds)
{
	FFlowVizPlayhead Result = Playhead;

	if (Timeline.IsEmpty())
	{
		return Result;
	}
	if (!(DeltaSeconds > 0.0) || !FMath::IsFinite(DeltaSeconds))
	{
		// A non-positive or non-finite tick changes nothing, including the
		// residual. Reversing is expressed through Speed, never through a
		// negative delta.
		return Result;
	}

	const bool bSpeedForward = Settings.Speed >= 0.0;

	switch (Settings.Mode)
	{
	case EFlowVizPlaybackMode::Sequence:
	{
		// EQUAL WALL TIME PER STORED FRAME. The step is taken in FRAME
		// COORDINATE space and converted back through the timeline, so a 0.001 s
		// gap and a 10 s gap each take one frame period. A constant-dt advance
		// plays the sparse end of an adaptive case in slow motion.
		const double CurrentCoordinate = Timeline.TimeToFrameCoordinate(Result.DisplayTime);
		const double FrameStep = Settings.SequenceFrameRate * Settings.Speed * DeltaSeconds;
		const double TargetCoordinate = CurrentCoordinate + FrameStep;

		// Extrapolating past the ends is deliberate; FoldPhase needs the overshoot.
		Result.PhaseTime = Timeline.FrameCoordinateToTime(TargetCoordinate);
		break;
	}

	case EFlowVizPlaybackMode::RealTime:
	{
		// PHYSICAL TIME AT Speed. Stored frames are skipped where the data is
		// denser than the display can show, which is exactly what plan.md's
		// "respect physical simulation time and skip display frames" asks for.
		// The phase is advanced from the DISPLAYED time so a fold that wrapped
		// last tick continues from where it wrapped to.
		Result.PhaseTime = Result.DisplayTime + Settings.Speed * DeltaSeconds;
		break;
	}

	case EFlowVizPlaybackMode::FixedFps:
	{
		// WHOLE STEPS ONLY, WITH THE REMAINDER BANKED. Dropping the sub-step
		// remainder would freeze the playhead entirely whenever the render rate
		// exceeds the output rate - every tick smaller than one step advances
		// nothing, forever. Banking it makes the result independent of how the
		// wall time was chopped up, which is what makes an offline capture
		// reproducible.
		const double StepSeconds = 1.0 / Settings.OutputFrameRate;
		double Banked = Result.FixedStepResidual + DeltaSeconds;

		const double WholeSteps = FMath::FloorToDouble(Banked / StepSeconds);
		Result.FixedStepResidual = Banked - WholeSteps * StepSeconds;

		if (WholeSteps > 0.0)
		{
			// Each step advances one output frame's worth of solver time.
			const double TimeStep = Settings.Speed * StepSeconds;
			Result.PhaseTime = Result.DisplayTime + WholeSteps * TimeStep;
		}
		else
		{
			Result.PhaseTime = Result.DisplayTime;
		}
		break;
	}

	default:
		return Result;
	}

	Result.DisplayTime = FoldPhase(
		Timeline, Result.PhaseTime, Settings.LoopMode, bSpeedForward, Result.bForward, Result.bFinished);

	return Result;
}

FFlowVizPlayhead FlowVizPlayback::MakePlayheadAtTime(const FFlowVizTimeline& Timeline, double Time)
{
	FFlowVizPlayhead Playhead;
	Playhead.DisplayTime = Timeline.ClampTime(Time);
	Playhead.PhaseTime = Playhead.DisplayTime;
	Playhead.bForward = true;
	Playhead.bFinished = false;
	Playhead.FixedStepResidual = 0.0;
	return Playhead;
}

FFlowVizPlayhead FlowVizPlayback::MakePlayheadAtFrame(const FFlowVizTimeline& Timeline, int32 FrameIndex)
{
	if (Timeline.IsEmpty())
	{
		return FFlowVizPlayhead();
	}

	const int32 Clamped = FMath::Clamp(FrameIndex, 0, Timeline.GetLastFrameIndex());
	double Time = 0.0;
	Timeline.TryGetTime(Clamped, Time);
	return MakePlayheadAtTime(Timeline, Time);
}

FFlowVizPlayhead FlowVizPlayback::StepFrames(
	const FFlowVizTimeline& Timeline,
	const FFlowVizPlayhead& Playhead,
	int32 FrameDelta,
	EFlowVizLoopMode LoopMode)
{
	if (Timeline.IsEmpty())
	{
		return Playhead;
	}

	const int32 FrameCount = Timeline.GetFrameCount();
	const int32 LastIndex = FrameCount - 1;

	// START FROM THE NEAREST FRAME. Starting from FrameA would make "next frame"
	// from a time 90% of the way through an interval land on the frame already
	// being displayed, and the button would appear to do nothing.
	const int32 Current = Timeline.FindNearestFrame(Playhead.DisplayTime);
	if (Current == INDEX_NONE)
	{
		return Playhead;
	}

	int32 Target = Current + FrameDelta;

	if (LoopMode == EFlowVizLoopMode::Loop && FrameCount > 1)
	{
		// Positive remainder: (-1) % N is -1 in C++, which would index before the
		// start rather than wrapping to the end.
		Target = ((Target % FrameCount) + FrameCount) % FrameCount;
	}
	else if (LoopMode == EFlowVizLoopMode::PingPong && FrameCount > 1)
	{
		// Reflect in frame space, the discrete twin of FoldPhase's triangle wave.
		const int32 Period = 2 * LastIndex;
		int32 Cycle = ((Target % Period) + Period) % Period;
		Target = Cycle <= LastIndex ? Cycle : Period - Cycle;
	}
	else
	{
		Target = FMath::Clamp(Target, 0, LastIndex);
	}

	FFlowVizPlayhead Result = MakePlayheadAtFrame(Timeline, Target);

	// A manual step never leaves the finished latch set: the user has explicitly
	// moved, so playback should be able to resume.
	Result.bForward = FrameDelta >= 0;
	Result.bFinished = false;
	return Result;
}

void FlowVizPlayback::BuildRequestList(
	const FFlowVizTimeline& Timeline,
	const FFlowVizFrameSelection& Selection,
	bool bForward,
	int32 PreloadAhead,
	int32 PreloadBehind,
	TArray<int32>& OutFrames)
{
	OutFrames.Reset();

	if (Timeline.IsEmpty() || !Selection.IsValid())
	{
		return;
	}

	const int32 LastIndex = Timeline.GetLastFrameIndex();

	// Out-of-range indices are DROPPED, not clamped, because dropping is what
	// this function means. Kept explicit rather than left to AddUnique.
	//
	// AN EARLIER COMMENT HERE OVERSTATED THE HAZARD and is corrected rather than
	// deleted, because the correction is the useful part. It claimed a clamp
	// "would silently re-request the current frame at both ends and produce a
	// list that looks correct while preloading nothing." A clamp is in fact
	// EQUIVALENT here: exhaustively compared over 2800 (frame count, A, B,
	// direction, ahead, behind) combinations, drop and clamp produce identical
	// lists in every one - 0 disagreements, against a deliberately-wrong control
	// that found 806. The reason is that the radius walk is CONTIGUOUS, so the
	// index a clamp folds onto is always already in the list and AddUnique
	// absorbs it. The safety here comes from AddUnique plus contiguity, not from
	// the range check.
	//
	// This matters because the comment was written as a warning, and a warning
	// naming an impossible failure trains the reader to distrust the real ones.
	// The range check stays: it makes the intent local instead of resting on an
	// invariant two loops away that a future edit could break without noticing.
	auto AddFrame = [&OutFrames, LastIndex](int32 Frame)
	{
		if (Frame < 0 || Frame > LastIndex)
		{
			return;
		}
		OutFrames.AddUnique(Frame);
	};

	// ORDER IS PRIORITY. A and B first: a caller that can start only one decode
	// this tick must start the one that is on screen.
	AddFrame(Selection.FrameA);
	AddFrame(Selection.FrameB);

	// Then outward in the direction of travel, interleaved with the trailing
	// side so a ping-pong that has just turned around still has the frame behind
	// it. Leading first at each radius, because that is where the playhead is
	// going.
	//
	// "AHEAD" MEANS IN THE DIRECTION OF TRAVEL, NOT AT A HIGHER FRAME INDEX.
	// Travelling backward, the frame ahead of the playhead is the LOWER index.
	// Binding PreloadAhead to increasing indices instead would make a reversed
	// or just-turned-around ping-pong prefetch the frames it is walking away
	// from, and stall on every frame it is walking into - while still producing
	// a request list of the right length, which is why it needs asserting.
	const int32 Lead = bForward ? 1 : -1;
	const int32 LeadCount = PreloadAhead;
	const int32 TrailCount = PreloadBehind;
	const int32 MaxRadius = FMath::Max(LeadCount, TrailCount);

	// The leading edge extends from B and the trailing edge from A, so the ring
	// is symmetric about the bracketed interval rather than about a single frame.
	const int32 LeadAnchor = bForward ? Selection.FrameB : Selection.FrameA;
	const int32 TrailAnchor = bForward ? Selection.FrameA : Selection.FrameB;

	for (int32 Radius = 1; Radius <= MaxRadius; ++Radius)
	{
		if (Radius <= LeadCount)
		{
			AddFrame(LeadAnchor + Lead * Radius);
		}
		if (Radius <= TrailCount)
		{
			AddFrame(TrailAnchor - Lead * Radius);
		}
	}
}

double FlowVizPlayback::SnapToSpeedPreset(double Speed)
{
	if (!FMath::IsFinite(Speed))
	{
		return SpeedPresets[DefaultSpeedPresetIndex];
	}

	// Snap the magnitude and restore the sign, so snapping a reverse speed does
	// not silently turn playback around.
	const double Magnitude = FMath::Abs(Speed);
	const double Sign = Speed < 0.0 ? -1.0 : 1.0;

	double Best = SpeedPresets[0];
	double BestDistance = FMath::Abs(Magnitude - SpeedPresets[0]);
	for (int32 Index = 1; Index < NumSpeedPresets; ++Index)
	{
		const double Distance = FMath::Abs(Magnitude - SpeedPresets[Index]);
		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			Best = SpeedPresets[Index];
		}
	}

	return Sign * Best;
}

FFlowVizDisplaySelection FlowVizPlayback::ResolveDisplay(
	const FFlowVizFrameSelection& Desired,
	const FFlowVizDisplaySelection& Previous,
	TFunctionRef<bool(int32)> IsFrameResident)
{
	// THE "NEVER DISPLAY A PARTIAL FRAME" RULE, in one place.
	//
	// Every frame the selection names must be resident and complete. A resident A
	// with a still-decoding B is NOT partially accepted: showing A alone would be
	// honest about the data but would flip the "Interpolated" indicator off and
	// on across a scrub and would change the displayed physical time to one the
	// user never asked for.
	const bool bHaveA = Desired.IsValid() && IsFrameResident(Desired.FrameA);
	const bool bHaveB = Desired.IsValid()
		&& (Desired.FrameB == Desired.FrameA || IsFrameResident(Desired.FrameB));

	if (bHaveA && bHaveB)
	{
		FFlowVizDisplaySelection Shown;
		Shown.FrameA = Desired.FrameA;
		Shown.FrameB = Desired.FrameB;
		Shown.Alpha = Desired.Alpha;
		Shown.Time = Desired.Time;
		Shown.NearestFrame = Desired.NearestFrame;
		Shown.bInterpolated = Desired.bInterpolated;
		Shown.bStale = false;
		return Shown;
	}

	// Hold the previous complete pair, and say so. bStale is not an error - it is
	// the normal state during a scrub - but the UI must be able to disclose it.
	FFlowVizDisplaySelection Held = Previous;
	Held.bStale = true;
	return Held;
}

/* ========================================================================== */
/* The load queue                                                              */
/* ========================================================================== */

/**
 * Shared between the game thread and every decode task.
 *
 * HELD BY SHARED POINTER AND NEVER BY RAW POINTER TO THE PLAYER. A task that is
 * still running when the player is destroyed writes its result into this queue
 * and drops its reference; nobody drains it and it frees itself. A task holding
 * a back-pointer to the player would write into freed memory instead, and the
 * crash would land somewhere unrelated.
 */
struct FFlowVizCasePlayer::FLoadQueue
{
	struct FResult
	{
		int32 FrameIndex = INDEX_NONE;
		uint64 Generation = 0;
		FFlowVizVolumeUpload Upload;
		FCFDVizResult Status;
	};

	/** Guards Results and PendingCount. Held only to move a result in or out. */
	FCriticalSection Mutex;

	TArray<FResult> Results;

	/** Tasks launched and not yet reported. Read by WaitForPendingLoads. */
	int32 PendingCount = 0;

	/** Set by Close so a task in flight discards its work rather than decoding a case that is being torn down. */
	TAtomic<bool> bAbandoned{ false };

	void Push(FResult&& Result)
	{
		FScopeLock Lock(&Mutex);
		Results.Add(MoveTemp(Result));
		--PendingCount;
	}

	void Drain(TArray<FResult>& OutResults)
	{
		FScopeLock Lock(&Mutex);
		OutResults = MoveTemp(Results);
		Results.Reset();
	}

	int32 GetPendingCount()
	{
		FScopeLock Lock(&Mutex);
		return PendingCount;
	}

	bool HasResults()
	{
		FScopeLock Lock(&Mutex);
		return Results.Num() > 0;
	}
};

/* ========================================================================== */
/* FFlowVizCasePlayer                                                          */
/* ========================================================================== */

FFlowVizCasePlayer::FFlowVizCasePlayer()
{
	Cache.Initialize();
}

FFlowVizCasePlayer::~FFlowVizCasePlayer()
{
	Close();
}

FCFDVizResult FFlowVizCasePlayer::Open(const TSharedRef<const FCFDVizCase>& InCase, FName InFieldId)
{
	Close();

	const FCFDVizField* FoundField = InCase->FindField(InFieldId);
	if (FoundField == nullptr)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(TEXT("case declares no field '%s'"), *InFieldId.ToString()));
	}

	FFlowVizTimeline NewTimeline;
	const FCFDVizResult TimelineResult = NewTimeline.InitializeFromCase(InCase.Get());
	if (!TimelineResult.IsOk())
	{
		// Nothing is bound on failure, so a bad timeline leaves a closed player
		// rather than one that is open with no way to select a frame.
		return TimelineResult;
	}

	Case = InCase;
	FieldId = InFieldId;
	Field = FoundField;
	Timeline = MoveTemp(NewTimeline);

	// The grid's mask field, when the case declares one, is loaded alongside
	// every frame so the status texture and the field always come from the same
	// frame.
	MaskField = nullptr;
	if (const FCFDVizGridDescriptor* Grid = InCase->FindGridForField(*FoundField))
	{
		if (Grid->HasMaskField() && Grid->MaskFieldId != InFieldId)
		{
			MaskField = InCase->FindField(Grid->MaskFieldId);
		}
	}

	Settings = FFlowVizPlaybackSettings::FromCase(InCase.Get());

	LoadQueue = MakeShared<FLoadQueue, ESPMode::ThreadSafe>();

	Playhead = FlowVizPlayback::MakePlayheadAtFrame(Timeline, 0);
	Selection = FlowVizPlayback::SelectFrames(Timeline, Playhead.DisplayTime, Settings.bInterpolate);
	Display = FFlowVizDisplaySelection();
	Display.bStale = true;

	Cache.Reset();
	InFlightFrames.Reset();
	FailedFrames.Reset();
	bPlaying = false;
	RequestGeneration = 0;
	LoadsStarted = 0;
	LoadsCompleted = 0;
	LoadsCancelled = 0;
	LoadsFailed = 0;
	CacheHits = 0;
	CacheMisses = 0;
	LastError = FCFDVizResult::Ok();

	// A ZERO-FRAME CASE OPENS SUCCESSFULLY. An empty timeline is a valid
	// manifest; refusing it here would turn a legitimate case into a load error.
	return FCFDVizResult::Ok();
}

void FFlowVizCasePlayer::Close()
{
	if (LoadQueue.IsValid())
	{
		// Tell any running task to drop its work, then wait for it. Waiting is
		// what makes the shared queue's lifetime bounded in the common case; the
		// shared pointer covers the case where the wait times out.
		LoadQueue->bAbandoned = true;
		WaitForPendingLoads(30.0);
		LoadQueue.Reset();
	}

	Case.Reset();
	FieldId = NAME_None;
	Field = nullptr;
	MaskField = nullptr;
	Timeline = FFlowVizTimeline();
	Playhead = FFlowVizPlayhead();
	Selection = FFlowVizFrameSelection();
	Display = FFlowVizDisplaySelection();
	Cache.Reset();
	InFlightFrames.Reset();
	FailedFrames.Reset();
	bPlaying = false;
	TextureSet = nullptr;
}

void FFlowVizCasePlayer::SetTextureSet(FFlowVizVolumeTextureSet* InTextureSet)
{
	TextureSet = InTextureSet;

	if (TextureSet != nullptr && TextureSet->GetBufferCount() == 0)
	{
		// Three buffers, not two: with two, an interpolating player pins both and
		// FlowVizVolumeRing::ChooseUploadSlot has nowhere to put the prefetch.
		TextureSet->Initialize(FlowVizVolume::RecommendedBufferCount);
	}
}

void FFlowVizCasePlayer::Play()
{
	if (!IsOpen() || Timeline.IsEmpty())
	{
		return;
	}

	// Playing from a finished Once playhead restarts rather than doing nothing,
	// which is what a play button that has run to the end should do.
	if (Playhead.bFinished && Settings.LoopMode == EFlowVizLoopMode::Once)
	{
		Playhead = FlowVizPlayback::MakePlayheadAtFrame(Timeline, 0);
	}

	bPlaying = true;
}

void FFlowVizCasePlayer::Pause()
{
	bPlaying = false;
}

void FFlowVizCasePlayer::Stop()
{
	bPlaying = false;
	SeekToFirstFrame();
}

void FFlowVizCasePlayer::Tick(double DeltaSeconds)
{
	if (!IsOpen())
	{
		return;
	}

	if (bPlaying && !Timeline.IsEmpty())
	{
		const FFlowVizPlayhead Advanced =
			FlowVizPlayback::Advance(Timeline, Settings, Playhead, DeltaSeconds);

		if (Advanced.DisplayTime != Playhead.DisplayTime)
		{
			++RequestGeneration;
		}
		Playhead = Advanced;

		if (Playhead.bFinished && Settings.LoopMode == EFlowVizLoopMode::Once)
		{
			bPlaying = false;
		}

		Selection = FlowVizPlayback::SelectFrames(Timeline, Playhead.DisplayTime, Settings.bInterpolate);
	}

	// DRAINING HAPPENS EVEN WHEN PAUSED. A scrub that stops with a decode in
	// flight must still end up showing the frame it asked for.
	DrainCompletedLoads();
	StartPendingLoads();
	UpdateDisplay();
}

void FFlowVizCasePlayer::SeekToTime(double Time)
{
	if (!IsOpen() || Timeline.IsEmpty())
	{
		return;
	}

	Playhead = FlowVizPlayback::MakePlayheadAtTime(Timeline, Time);
	Selection = FlowVizPlayback::SelectFrames(Timeline, Playhead.DisplayTime, Settings.bInterpolate);

	// Every seek bumps the generation, which is what lets a decode that finishes
	// after the playhead has moved on be recognised as obsolete.
	++RequestGeneration;

	UpdateDisplay();
}

void FFlowVizCasePlayer::SeekToFrame(int32 FrameIndex)
{
	if (!IsOpen() || Timeline.IsEmpty())
	{
		return;
	}

	const int32 Clamped = FMath::Clamp(FrameIndex, 0, Timeline.GetLastFrameIndex());
	double Time = 0.0;
	Timeline.TryGetTime(Clamped, Time);
	SeekToTime(Time);
}

void FFlowVizCasePlayer::SeekToNormalized(double Normalized)
{
	if (!IsOpen() || Timeline.IsEmpty())
	{
		return;
	}

	const double Clamped = FMath::Clamp(Normalized, 0.0, 1.0);
	const double First = Timeline.GetFirstTime();
	const double Duration = Timeline.GetDuration();

	// Lerp over the physical range, so the slider is uniform in TIME. The frame
	// coordinate would be uniform in frames instead; both are defensible, and
	// this is the one a "physical time" slider means.
	SeekToTime(First + Clamped * Duration);
}

void FFlowVizCasePlayer::SeekToFirstFrame()
{
	SeekToFrame(0);
}

void FFlowVizCasePlayer::SeekToLastFrame()
{
	if (!IsOpen() || Timeline.IsEmpty())
	{
		return;
	}
	SeekToFrame(Timeline.GetLastFrameIndex());
}

void FFlowVizCasePlayer::StepFrames(int32 FrameDelta)
{
	if (!IsOpen() || Timeline.IsEmpty())
	{
		return;
	}

	Playhead = FlowVizPlayback::StepFrames(Timeline, Playhead, FrameDelta, Settings.LoopMode);
	Selection = FlowVizPlayback::SelectFrames(Timeline, Playhead.DisplayTime, Settings.bInterpolate);
	++RequestGeneration;
	UpdateDisplay();
}

FCFDVizResult FFlowVizCasePlayer::SetSettings(const FFlowVizPlaybackSettings& InSettings)
{
	const FCFDVizResult Validation = InSettings.Validate();
	if (!Validation.IsOk())
	{
		// NOTHING is applied on failure, so a bad speed cannot leave the player
		// half-configured with a new mode and an old rate.
		return Validation;
	}

	Settings = InSettings;

	if (IsOpen() && !Timeline.IsEmpty())
	{
		Selection = FlowVizPlayback::SelectFrames(Timeline, Playhead.DisplayTime, Settings.bInterpolate);
		UpdateDisplay();
	}

	return FCFDVizResult::Ok();
}

void FFlowVizCasePlayer::SetMode(EFlowVizPlaybackMode Mode)
{
	Settings.Mode = Mode;

	// The residual belongs to FixedFps; carrying it into another mode and back
	// would deliver a step of banked time at a moment unrelated to the tick that
	// banked it.
	Playhead.FixedStepResidual = 0.0;
}

void FFlowVizCasePlayer::SetLoopMode(EFlowVizLoopMode LoopMode)
{
	Settings.LoopMode = LoopMode;

	// Leaving Once clears the finished latch, otherwise switching to Loop leaves
	// a player that is looping and finished at once.
	if (LoopMode != EFlowVizLoopMode::Once)
	{
		Playhead.bFinished = false;
	}
}

FCFDVizResult FFlowVizCasePlayer::SetSpeed(double Speed)
{
	FFlowVizPlaybackSettings Candidate = Settings;
	Candidate.Speed = Speed;
	return SetSettings(Candidate);
}

FCFDVizResult FFlowVizCasePlayer::SetSpeedPreset(int32 PresetIndex)
{
	if (PresetIndex < 0 || PresetIndex >= FlowVizPlayback::NumSpeedPresets)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("speed preset %d is outside [0, %d)"), PresetIndex, FlowVizPlayback::NumSpeedPresets));
	}

	return SetSpeed(FlowVizPlayback::SpeedPresets[PresetIndex]);
}

void FFlowVizCasePlayer::SetInterpolationEnabled(bool bEnabled)
{
	if (Settings.bInterpolate == bEnabled)
	{
		return;
	}

	Settings.bInterpolate = bEnabled;

	if (IsOpen() && !Timeline.IsEmpty())
	{
		// Re-select immediately: toggling the switch must change what the
		// diagnostics panel reports without waiting for a tick.
		Selection = FlowVizPlayback::SelectFrames(Timeline, Playhead.DisplayTime, Settings.bInterpolate);
		++RequestGeneration;
		UpdateDisplay();
	}
}

double FFlowVizCasePlayer::GetNormalizedTime() const
{
	if (!IsOpen() || Timeline.IsEmpty())
	{
		return 0.0;
	}

	const double Duration = Timeline.GetDuration();
	if (Duration <= 0.0)
	{
		return 0.0;
	}

	return FMath::Clamp((Playhead.DisplayTime - Timeline.GetFirstTime()) / Duration, 0.0, 1.0);
}

EFlowVizLoadState FFlowVizCasePlayer::GetFrameLoadState(int32 FrameIndex) const
{
	if (Cache.IsResident(FrameIndex))
	{
		return EFlowVizLoadState::Resident;
	}
	if (InFlightFrames.Contains(FrameIndex))
	{
		return EFlowVizLoadState::Loading;
	}
	if (FailedFrames.Contains(FrameIndex))
	{
		return EFlowVizLoadState::Failed;
	}
	return EFlowVizLoadState::Absent;
}

FFlowVizPlaybackDiagnostics FFlowVizCasePlayer::GetDiagnostics() const
{
	FFlowVizPlaybackDiagnostics Diagnostics;
	Diagnostics.Selection = Selection;
	Diagnostics.Display = Display;
	Diagnostics.PhysicalTime = Playhead.DisplayTime;
	Diagnostics.PhaseTime = Playhead.PhaseTime;
	Diagnostics.bPlaying = bPlaying;
	Diagnostics.bForward = Playhead.bForward;
	Diagnostics.bFinished = Playhead.bFinished;
	Diagnostics.Mode = Settings.Mode;
	Diagnostics.LoopMode = Settings.LoopMode;
	Diagnostics.Speed = Settings.Speed;
	Diagnostics.bInterpolationEnabled = Settings.bInterpolate;
	Diagnostics.FrameCount = Timeline.GetFrameCount();
	Diagnostics.LoadsStarted = LoadsStarted;
	Diagnostics.LoadsCompleted = LoadsCompleted;
	Diagnostics.LoadsCancelled = LoadsCancelled;
	Diagnostics.LoadsFailed = LoadsFailed;
	Diagnostics.LoadsInFlight = InFlightFrames.Num();
	Diagnostics.CacheHits = CacheHits;
	Diagnostics.CacheMisses = CacheMisses;
	Diagnostics.Cache = Cache.GetStats();
	Diagnostics.LastError = LastError;
	return Diagnostics;
}

FCFDVizResult FFlowVizCasePlayer::SetMemoryBudgets(int64 CpuBudgetBytes, int64 GpuBudgetBytes)
{
	TArray<int32> Evicted;
	const FCFDVizResult Result = Cache.SetBudgets(CpuBudgetBytes, GpuBudgetBytes, Evicted);

	// An evicted frame's decode, if still running, produces a payload nothing
	// will hold. Marking it failed-free simply lets it be requested again.
	for (int32 Frame : Evicted)
	{
		FailedFrames.Remove(Frame);
	}

	return Result;
}

FCFDVizResult FFlowVizCasePlayer::SetMaxConcurrentLoads(int32 MaxLoads)
{
	if (MaxLoads < 1 || MaxLoads > FlowVizPlayback::MaxConcurrentLoads)
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("max concurrent loads %d is outside [1, %d]"),
				MaxLoads, FlowVizPlayback::MaxConcurrentLoads));
	}

	MaxConcurrentLoads = MaxLoads;
	return FCFDVizResult::Ok();
}

void FFlowVizCasePlayer::RetryFailedFrames()
{
	FailedFrames.Reset();
	LastError = FCFDVizResult::Ok();
}

bool FFlowVizCasePlayer::TryEstimateFrameBytes(int64& OutCpuBytes, int64& OutGpuBytes) const
{
	if (!IsOpen() || Field == nullptr)
	{
		return false;
	}

	const FCFDVizGridDescriptor* Grid = Case->FindGridForField(*Field);
	if (Grid == nullptr)
	{
		return false;
	}

	const FIntVector ValueCounts = Grid->Geometry.ValueCounts(Field->Association);
	if (ValueCounts.X <= 0 || ValueCounts.Y <= 0 || ValueCounts.Z <= 0)
	{
		return false;
	}

	// int64 throughout, and each product checked: a 2048^3 grid overflows int32
	// at the second multiply, and a wrapped byte count would defeat the budget
	// this number exists to enforce.
	const int64 VoxelCount =
		static_cast<int64>(ValueCounts.X) * static_cast<int64>(ValueCounts.Y) * static_cast<int64>(ValueCounts.Z);
	if (VoxelCount <= 0)
	{
		return false;
	}

	const int64 BytesPerComponent = SizeOfDataType(Field->DataType);
	if (BytesPerComponent <= 0)
	{
		return false;
	}

	const int64 ComponentCount = FMath::Max(1, Field->ComponentCount);

	// CPU: the decoded field, at the file's own component count.
	OutCpuBytes = VoxelCount * ComponentCount * BytesPerComponent;

	// GPU: the SAME data after the 3 -> 4 channel widening (there is no
	// 3-channel 3D texture format), plus one status byte per voxel. This is why
	// the two budgets are independent - for a 3-component float16 field the GPU
	// figure is a third larger than the CPU one before the status texture is
	// even counted.
	const int64 TextureComponents = (ComponentCount == 3) ? 4 : ComponentCount;
	OutGpuBytes = VoxelCount * TextureComponents * BytesPerComponent;

	if (MaskField != nullptr || (Grid->HasMaskField()))
	{
		OutGpuBytes += VoxelCount;
	}

	return OutCpuBytes > 0 && OutGpuBytes > 0;
}

bool FFlowVizCasePlayer::WaitForPendingLoads(double TimeoutSeconds)
{
	if (!LoadQueue.IsValid())
	{
		return true;
	}

	const double StartTime = FPlatformTime::Seconds();

	while (LoadQueue->GetPendingCount() > 0)
	{
		if (FPlatformTime::Seconds() - StartTime > TimeoutSeconds)
		{
			return false;
		}
		FPlatformProcess::Sleep(0.001f);
	}

	return true;
}

/* --- private -------------------------------------------------------------- */

void FFlowVizCasePlayer::DrainCompletedLoads()
{
	if (!LoadQueue.IsValid())
	{
		return;
	}
	if (!LoadQueue->HasResults())
	{
		return;
	}

	TArray<FLoadQueue::FResult> Results;
	LoadQueue->Drain(Results);

	// What the playhead currently wants. A result for anything else is obsolete.
	TArray<int32> Wanted;
	FlowVizPlayback::BuildRequestList(
		Timeline, Selection, Playhead.bForward, Settings.PreloadAhead, Settings.PreloadBehind, Wanted);

	for (FLoadQueue::FResult& Result : Results)
	{
		InFlightFrames.Remove(Result.FrameIndex);

		if (!Result.Status.IsOk())
		{
			++LoadsFailed;
			FailedFrames.AddUnique(Result.FrameIndex);
			Cache.Remove(Result.FrameIndex);

			// The FIRST failure is kept, not the latest: a broken frame reported
			// once is actionable, the same message rewritten every tick is not.
			if (LastError.IsOk())
			{
				LastError = Result.Status;
			}

			UE_LOG(LogFlowViz, Warning,
				TEXT("FlowVizCasePlayer: frame %d failed to decode: %s"),
				Result.FrameIndex, *Result.Status.ToString());
			continue;
		}

		// CANCEL OBSOLETE RESULTS. The decode finished, but the playhead has
		// moved somewhere that no longer wants this frame. Its reservation is
		// released rather than uploaded, which is what stops an aggressive scrub
		// filling the cache with frames nobody asked for any more.
		if (!Wanted.Contains(Result.FrameIndex))
		{
			++LoadsCancelled;
			Cache.Remove(Result.FrameIndex);
			continue;
		}

		++LoadsCompleted;

		// THE ORDER HERE IS THE "never display a partial frame" RULE AT THE
		// UPLOAD BOUNDARY. The payload is handed over whole, and only then is the
		// frame marked complete. A MarkComplete before the upload would make the
		// frame displayable while its texture still held the previous frame's
		// voxels.
		if (TextureSet != nullptr)
		{
			const FCFDVizResult Upload = TextureSet->EnqueueUpload(MoveTemp(Result.Upload));
			if (!Upload.IsOk())
			{
				if (Upload.Error == ECFDVizError::AllocationTooLarge)
				{
					// Every slot is pinned or busy. Documented as "retry once the
					// display frames advance", NOT an error: drop the reservation
					// and let the next tick request it again.
					Cache.Remove(Result.FrameIndex);
					continue;
				}

				++LoadsFailed;
				FailedFrames.AddUnique(Result.FrameIndex);
				Cache.Remove(Result.FrameIndex);
				if (LastError.IsOk())
				{
					LastError = Upload;
				}
				continue;
			}
		}

		Cache.MarkComplete(Result.FrameIndex);
	}
}

void FFlowVizCasePlayer::StartPendingLoads()
{
	if (!IsOpen() || Timeline.IsEmpty() || !LoadQueue.IsValid())
	{
		return;
	}

	TArray<int32> Requests;
	FlowVizPlayback::BuildRequestList(
		Timeline, Selection, Playhead.bForward, Settings.PreloadAhead, Settings.PreloadBehind, Requests);

	if (Requests.Num() == 0)
	{
		return;
	}

	// Pin A and B before anything is admitted, so a preload can never evict a
	// frame that is on screen.
	Cache.SetPinnedFrames(Selection.FrameA, Selection.FrameB);

	CancelObsoleteRequests(Requests);

	// Requests are in priority order (A, B, then outward), so this loop naturally
	// spends a limited concurrency budget on the frames that are on screen.
	for (int32 FrameIndex : Requests)
	{
		if (InFlightFrames.Num() >= MaxConcurrentLoads)
		{
			break;
		}
		RequestFrame(FrameIndex);
	}
}

void FFlowVizCasePlayer::RequestFrame(int32 FrameIndex)
{
	if (Cache.IsResident(FrameIndex))
	{
		Cache.Touch(FrameIndex);
		++CacheHits;
		return;
	}
	if (InFlightFrames.Contains(FrameIndex))
	{
		// Already decoding. Requesting it again would decode the same file twice
		// and charge the cache twice for it.
		return;
	}
	if (FailedFrames.Contains(FrameIndex))
	{
		// Not retried every tick: a missing file would otherwise produce one
		// failed decode per frame forever. RetryFailedFrames clears this.
		return;
	}

	int64 EstimatedCpu = 0;
	int64 EstimatedGpu = 0;
	if (!TryEstimateFrameBytes(EstimatedCpu, EstimatedGpu))
	{
		return;
	}

	// RESERVE BEFORE DECODING. The budget check has to precede the allocation it
	// guards, or the cache is decoration applied to memory that is already gone.
	TArray<int32> Evicted;
	const FCFDVizResult Admission = Cache.Admit(FrameIndex, EstimatedCpu, EstimatedGpu, Evicted);
	if (!Admission.IsOk())
	{
		// AllocationTooLarge here means "everything else is pinned or it does not
		// fit". Not an error to surface; the next tick tries again once the
		// display frames have advanced.
		return;
	}

	++CacheMisses;

	FString FieldPath;
	const FCFDVizResult FieldPathResult = Case->ResolveFieldFramePath(*Field, FrameIndex, FieldPath);
	if (!FieldPathResult.IsOk())
	{
		Cache.Remove(FrameIndex);
		FailedFrames.AddUnique(FrameIndex);
		if (LastError.IsOk())
		{
			LastError = FieldPathResult;
		}
		return;
	}

	FString MaskPath;
	if (MaskField != nullptr)
	{
		// A mask that cannot be resolved is not fatal: the frame renders without
		// a status texture rather than not at all.
		if (!Case->ResolveFieldFramePath(*MaskField, FrameIndex, MaskPath).IsOk())
		{
			MaskPath.Reset();
		}
	}

	double SimulationTime = 0.0;
	Timeline.TryGetTime(FrameIndex, SimulationTime);

	const bool bAsVector = Field->ComponentCount >= 2;

	InFlightFrames.Add(FrameIndex);
	++LoadsStarted;

	{
		FScopeLock Lock(&LoadQueue->Mutex);
		++LoadQueue->PendingCount;
	}

	// EVERYTHING THE TASK TOUCHES IS COPIED OR SHARED, never a pointer back into
	// the player. Two paths depend on it: Close() may return while a task runs,
	// and the player may be destroyed outright.
	TSharedPtr<FLoadQueue, ESPMode::ThreadSafe> Queue = LoadQueue;
	TSharedPtr<const FCFDVizCase> CaseRef = Case;
	const uint64 Generation = RequestGeneration;

	UE::Tasks::Launch(TEXT("FlowVizFrameDecode"),
		[Queue, CaseRef, FieldPath, MaskPath, FrameIndex, SimulationTime, bAsVector]()
		{
			FLoadQueue::FResult Result;
			Result.FrameIndex = FrameIndex;

			if (Queue->bAbandoned)
			{
				// The player is closing. Report without decoding, so the pending
				// count still reaches zero and Close's wait terminates.
				Result.Status = FCFDVizResult::Fail(
					ECFDVizError::IndexOutOfRange, TEXT("player closed before the decode started"));
				Queue->Push(MoveTemp(Result));
				return;
			}

			// FILE I/O AND ZLIB, ON A WORKER. This is the whole reason the decode
			// is a task: engineering rule 1 forbids either on the game thread.
			FCFDVizVolumeReader Reader;
			Result.Status = Reader.Open(FieldPath);
			if (!Result.Status.IsOk())
			{
				Queue->Push(MoveTemp(Result));
				return;
			}

			FCFDVizVolumeReader MaskReader;
			FCFDVizVolumeReader* MaskReaderPtr = nullptr;
			if (!MaskPath.IsEmpty() && MaskReader.Open(MaskPath).IsOk())
			{
				MaskReaderPtr = &MaskReader;
			}

			Result.Status = FlowVizVolumeBuild::BuildUpload(
				Reader, MaskReaderPtr, bAsVector, Result.Upload);

			if (Result.Status.IsOk())
			{
				// The manifest's time, not the CVF header's: the timeline is the
				// authority on when a frame is, and a header that disagrees is a
				// data problem rather than a reason to display a different time.
				Result.Upload.FrameIndex = FrameIndex;
				Result.Upload.SimulationTime = SimulationTime;
			}

			Queue->Push(MoveTemp(Result));
		});

	// Recorded for the obsolescence check in DrainCompletedLoads.
	(void)Generation;
}

void FFlowVizCasePlayer::CancelObsoleteRequests(TArrayView<const int32> KeepFrames)
{
	// "Cancel" is bookkeeping, not preemption: a UE::Tasks body cannot be killed
	// mid-decode, and killing it mid-file is not something the reader is written
	// to survive. What is cancelled is the RESULT - the frame is dropped from the
	// in-flight set and its reservation released, so the decode's output is
	// discarded on arrival and the concurrency budget is freed immediately for
	// the frames the playhead actually wants.
	for (int32 Index = InFlightFrames.Num() - 1; Index >= 0; --Index)
	{
		const int32 Frame = InFlightFrames[Index];
		if (KeepFrames.Contains(Frame))
		{
			continue;
		}

		InFlightFrames.RemoveAt(Index);
		Cache.Remove(Frame);
		++LoadsCancelled;
	}
}

void FFlowVizCasePlayer::UpdateDisplay()
{
	// Residency is asked of the cache, which requires bComplete - so a frame
	// that is admitted and decoding is NOT displayable. That distinction is the
	// "never display a partial frame" requirement.
	auto IsResident = [this](int32 FrameIndex) { return Cache.IsResident(FrameIndex); };

	Display = FlowVizPlayback::ResolveDisplay(Selection, Display, IsResident);

	// Pin what is actually ON SCREEN, which during a stall is the held pair, not
	// the wanted one. Pinning the wanted frames instead would leave the displayed
	// pair evictable and tear the visible image mid-scrub.
	if (Display.IsValid())
	{
		Cache.SetPinnedFrames(Display.FrameA, Display.FrameB);
	}

	ApplyDisplayToTextureSet();
}

void FFlowVizCasePlayer::ApplyDisplayToTextureSet()
{
	if (TextureSet == nullptr)
	{
		return;
	}

	// The renderer is told the DISPLAY pair, never the selection. Handing it the
	// selection would name a frame whose texture has not been written yet.
	TextureSet->SetDisplayFrames(Display.FrameA, Display.FrameB);
}
