// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CFDViz/CFDVizTypes.h"
#include "Playback/FlowVizFrameCache.h"

// CoreMinimal.h forward-declares TArrayView but does not define it, and the
// timeline below takes one by value.
#include "Containers/ArrayView.h"
#include "Templates/SharedPointer.h"

/**
 * Case playback - which frame is on screen, when, and what is loaded around it
 * (plan.md section 8, Milestone C "Animate frames" / "Scrub timeline").
 *
 * WHAT THIS FILE IS FOR. The readers in Public/CFDViz/ can decode any frame and
 * FFlowVizVolumeTextureSet can upload any frame. Nothing decided WHICH frame, or
 * WHEN, or kept anything resident. This file is that decision, and its two
 * halves are deliberately not the same kind of code:
 *
 *   THE PURE HALF - FFlowVizTimeline, FFlowVizPlayhead, FlowVizPlayback::* - is
 *   arithmetic over an array of doubles. Given frame times, a mode, a speed and
 *   an elapsed time it answers "frame A, frame B, alpha". It allocates nothing,
 *   opens nothing, and needs no RHI, which is why FlowVizCasePlayerTest can
 *   exercise every mode, both loop policies and the turnaround under -nullrhi.
 *
 *   THE SHELL - FFlowVizCasePlayer - owns the file I/O, the worker tasks, the
 *   cache and the texture set. It contains no frame arithmetic of its own; it
 *   calls the pure half and acts on the answer.
 *
 * FIVE THINGS THIS LAYER EXISTS TO GET RIGHT.
 *
 *  1. FRAME TIMES ARE NOT UNIFORMLY SPACED. FCFDVizTimeline::Times is any
 *     strictly increasing sequence. Every lookup here is a binary search over
 *     that array. Deriving a frame index by dividing a time by a dt - even a dt
 *     read from the first two entries - is a bug that is invisible on the shipped
 *     sample (whose spacing happens to be uniform) and wrong on any adaptive-
 *     timestep case. FFlowVizTimeline has no dt member for that reason.
 *
 *  2. NO INTERPOLATED CPU VOLUME IS EVER BUILT. plan.md section 8 forbids it
 *     outright. This layer emits (FrameA, FrameB, Alpha) and the shader does the
 *     blend; there is no function here that takes two frames' bytes and produces
 *     a third, and there must not be.
 *
 *  3. A PARTIAL FRAME IS NEVER DISPLAYED. The frames the playhead WANTS
 *     (FFlowVizFrameSelection) and the frames that are ON SCREEN
 *     (FFlowVizDisplaySelection) are different values with different types. The
 *     display only ever advances to frames that are complete and resident;
 *     otherwise it holds what it had and reports itself stale. This is why a
 *     scrub into an unloaded region shows the previous frame rather than a
 *     scalar texture from frame 7 beside a vector texture from frame 3.
 *
 *  4. MASKS AND TOPOLOGY USE THE NEAREST FRAME, NOT THE BLEND. A mask is a
 *     classification, not a measurement: blending 0 and 1 produces 0.5, which is
 *     not a state any cell can be in. FFlowVizFrameSelection::NearestFrame is
 *     computed for exactly this and is populated whether or not interpolation is
 *     enabled.
 *
 *  5. INTERPOLATION IS DISCLOSED. VISUAL_QA §1 rule 5 requires the UI to say
 *     when a frame is temporally interpolated. bInterpolated is true exactly when
 *     the shader will blend - alpha strictly between 0 and 1 with two distinct
 *     frames - and is a stored field rather than something each call site
 *     recomputes with its own epsilon.
 *
 * TIME IS DOUBLE, IN SOLVER UNITS, EVERYWHERE. The manifest's `units.time`
 * decides what a "second" means; nothing here converts. Wall-clock deltas passed
 * to Tick are real seconds, and Speed is the exchange rate between the two.
 */

// The tag must match the definition: FCFDVizCase and FCFDVizField are structs in
// CFDVizManifest.h. Declaring them as classes here builds on Clang but is a
// -Wmismatched-tags error under this project's warnings-as-errors, and a genuine
// linker mismatch under the Microsoft C++ ABI.
struct FCFDVizCase;
struct FCFDVizField;
class FFlowVizVolumeTextureSet;

/* -------------------------------------------------------------------------- */
/* Modes and settings                                                           */
/* -------------------------------------------------------------------------- */

/** The three playback modes plan.md section 8 requires. They differ in what a wall-clock second buys. */
enum class EFlowVizPlaybackMode : uint8
{
	/**
	 * Every stored frame in order, at SequenceFrameRate stored frames per wall
	 * second. The playhead moves in FRAME space and is converted to physical time
	 * through the times array, so a long gap between two stored frames still takes
	 * one frame-time to cross. This is the mode that shows all the data.
	 */
	Sequence = 0,

	/**
	 * Physical time advances at Speed solver-time units per wall second. Stored
	 * frames are skipped wherever the data is denser than the display, which is
	 * the point: this is the mode that shows the flow at its real rate.
	 */
	RealTime = 1,

	/**
	 * Physical time advances in equal steps of Speed/OutputFrameRate, exactly
	 * OutputFrameRate times per wall second, and NOT at all in between. The
	 * playhead is therefore independent of the render frame rate, which is what
	 * makes a capture reproducible. Sub-step wall time accumulates rather than
	 * being dropped, so 60 ticks of 1/60 s advance a 30 fps playhead exactly 30
	 * steps regardless of how the ticks are spaced.
	 */
	FixedFps = 2
};

/** What happens at the ends of the timeline. */
enum class EFlowVizLoopMode : uint8
{
	/** Stop at the far end. FFlowVizPlayhead::bFinished latches and playback pauses. */
	Once = 0,

	/** Jump back to the near end. Always travels the same direction. */
	Loop = 1,

	/**
	 * Reverse at each end. The playhead is the triangle wave of a monotonically
	 * advancing phase, so a step larger than the whole timeline folds correctly
	 * instead of sticking at an end, and an exact landing on either end is exact
	 * rather than epsilon-dependent.
	 */
	PingPong = 2
};

namespace FlowVizPlayback
{
	/**
	 * Speed presets, as multiples of the mode's base rate. plan.md asks for
	 * presets AND a custom speed; these are the presets, and any value in
	 * [MinSpeed, MaxSpeed] is accepted as a custom one.
	 */
	inline constexpr double SpeedPresets[] = { 0.1, 0.25, 0.5, 1.0, 2.0, 4.0, 10.0 };
	inline constexpr int32 NumSpeedPresets = UE_ARRAY_COUNT(SpeedPresets);

	/** Index of the 1.0 preset, which is the default. Asserted in the .cpp rather than assumed. */
	inline constexpr int32 DefaultSpeedPresetIndex = 3;

	/**
	 * Speed bounds. Negative speeds run the case backwards and are legal - the
	 * fold handles a negative phase step symmetrically. Zero is not: it is a
	 * paused player spelled as a playing one, and it would make Sequence mode's
	 * frame-space step degenerate.
	 */
	inline constexpr double MinSpeed = -100.0;
	inline constexpr double MaxSpeed = 100.0;
	inline constexpr double MinAbsSpeed = 1.0e-6;

	/** Frame rate bounds for Sequence and FixedFps. An unbounded rate makes one tick request thousands of frames. */
	inline constexpr double MinFrameRate = 0.001;
	inline constexpr double MaxFrameRate = 1000.0;

	/** Stored frames requested ahead of and behind the current one. plan.md section 8 requires at least one of each. */
	inline constexpr int32 DefaultPreloadAhead = 1;
	inline constexpr int32 DefaultPreloadBehind = 1;
	inline constexpr int32 MaxPreloadRadius = 16;

	/** Concurrent decode tasks. Above this, an aggressive scrub queues more work than it can ever display. */
	inline constexpr int32 DefaultMaxConcurrentLoads = 2;
	inline constexpr int32 MaxConcurrentLoads = 16;
}

/** Everything the user can set about how playback runs. Plain data; validated by Validate(). */
struct FFlowVizPlaybackSettings
{
	EFlowVizPlaybackMode Mode = EFlowVizPlaybackMode::Sequence;
	EFlowVizLoopMode LoopMode = EFlowVizLoopMode::Loop;

	/** Multiplier on the mode's base rate. See FlowVizPlayback::MinSpeed. */
	double Speed = 1.0;

	/** Stored frames per wall second in Sequence mode. 24 shows a 20-frame case in under a second, which is what "animate" means here. */
	double SequenceFrameRate = 24.0;

	/** Playhead steps per wall second in FixedFps mode. */
	double OutputFrameRate = 30.0;

	/**
	 * Blend between the bracketing frames. When false the nearest stored frame is
	 * shown and alpha is always 0 - only data that exists on disk reaches the
	 * screen, which is what ECFDVizInterpolation::Nearest means.
	 */
	bool bInterpolate = true;

	/** Stored frames to keep resident ahead of / behind the playhead. */
	int32 PreloadAhead = FlowVizPlayback::DefaultPreloadAhead;
	int32 PreloadBehind = FlowVizPlayback::DefaultPreloadBehind;

	/**
	 * Bounds-check every member.
	 *
	 * @return Ok, or IndexOutOfRange naming the offending member and its value.
	 *         Nothing is silently clamped here: a speed of 0 or a frame rate of
	 *         -1 is a caller bug, and quietly substituting a default would hide it.
	 *         Use ClampToLegalRange when clamping is what you actually want.
	 */
	FLOWVIZRUNTIME_API FCFDVizResult Validate() const;

	/** Clamp every member into its legal range. For a UI slider, where clamping IS the correct behaviour. */
	FLOWVIZRUNTIME_API void ClampToLegalRange();

	/** Settings derived from the case's own timeline defaults - notably `timeline.defaultInterpolation`. */
	FLOWVIZRUNTIME_API static FFlowVizPlaybackSettings FromCase(const FCFDVizCase& Case);
};

/* -------------------------------------------------------------------------- */
/* The timeline                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * A strictly increasing array of frame times, and every lookup over it.
 *
 * NO dt MEMBER, DELIBERATELY. Frame times may be arbitrarily spaced. Every
 * function here binary-searches; none divides by an interval it did not first
 * find. A cached "dt" would be correct on the shipped sample and wrong on any
 * case written with an adaptive timestep, which is the worst combination -
 * it would pass every test written against the sample.
 */
class FLOWVIZRUNTIME_API FFlowVizTimeline
{
public:
	FFlowVizTimeline() = default;

	/**
	 * Adopt a frame-time array.
	 *
	 * @return Ok, or InvalidManifest when the times are not strictly increasing or
	 *         contain a non-finite value. Both are rejected rather than sorted or
	 *         repaired: a bracket search over an unsorted array returns a
	 *         plausible, wrong frame, and every comparison against NaN is false so
	 *         a NaN would pass a pairwise check. An EMPTY array is accepted - a
	 *         case may legitimately declare zero frames - and every query then
	 *         returns INDEX_NONE.
	 */
	FCFDVizResult Initialize(TArrayView<const double> InTimes);

	/** Adopt the case's `timeline.times`. Same rules as Initialize. */
	FCFDVizResult InitializeFromCase(const FCFDVizCase& Case);

	int32 GetFrameCount() const { return Times.Num(); }
	bool IsEmpty() const { return Times.Num() == 0; }
	TArrayView<const double> GetTimes() const { return Times; }

	/** Time of a stored frame. @return false, leaving OutTime untouched, when the index is out of range. */
	bool TryGetTime(int32 FrameIndex, double& OutTime) const;

	/** First stored time, or 0 for an empty timeline. Check IsEmpty rather than reading 0 as a time. */
	double GetFirstTime() const { return Times.Num() > 0 ? Times[0] : 0.0; }
	double GetLastTime() const { return Times.Num() > 0 ? Times.Last() : 0.0; }

	/** Last - First. Zero for an empty or single-frame timeline, which every fold treats as "nowhere to go". */
	double GetDuration() const { return GetLastTime() - GetFirstTime(); }

	/**
	 * Largest stored frame index, or INDEX_NONE when empty. Spelled out because
	 * `GetFrameCount() - 1` is -1 for an empty timeline, which is INDEX_NONE by
	 * numeric accident rather than by intent, and the two must not be confused at
	 * a call site that then indexes with it.
	 */
	int32 GetLastFrameIndex() const { return Times.Num() > 0 ? Times.Num() - 1 : INDEX_NONE; }

	/**
	 * Bracket a physical time: the last frame at or before Time, and the next one.
	 *
	 * Binary search, so it is O(log n) over any spacing. Outside the range both
	 * outputs are the nearest end and Alpha is 0 - the timeline is clamped, never
	 * extrapolated, because a time before the first frame has no data to show and
	 * inventing one would be fabrication.
	 *
	 * @param OutAlpha (Time - t[A]) / (t[B] - t[A]), in [0,1). Exactly 0 when Time
	 *                 lands on a stored frame, in which case A == B. It is never 1:
	 *                 a time equal to t[B] brackets as (B, B, 0), so there is one
	 *                 spelling of "on a frame" rather than two.
	 * @return false, leaving the outputs untouched, for an empty timeline or a
	 *         non-finite Time.
	 */
	bool TryBracket(double Time, int32& OutFrameA, int32& OutFrameB, double& OutAlpha) const;

	/**
	 * The stored frame closest in time. Ties - a time exactly midway between two
	 * frames - resolve to the LATER frame, matching the alpha >= 0.5 rule in
	 * FlowVizPlayback::SelectFrames so a mask and its field never disagree about
	 * which frame they came from.
	 *
	 * @return INDEX_NONE for an empty timeline or a non-finite time.
	 */
	int32 FindNearestFrame(double Time) const;

	/**
	 * Continuous frame coordinate: an integer lands exactly on that stored frame,
	 * and a fraction is linear in time WITHIN the enclosing interval only.
	 *
	 * This is the coordinate Sequence mode advances in, and it is why that mode
	 * spends the same wall time on a 0.001 s gap as on a 10 s one. Clamped to
	 * [0, FrameCount-1]; returns 0 for an empty timeline.
	 */
	double TimeToFrameCoordinate(double Time) const;

	/**
	 * Inverse of TimeToFrameCoordinate.
	 *
	 * EXTRAPOLATES OUTSIDE [0, FrameCount-1], using the first or last interval's
	 * own spacing. That is required, not incidental: Sequence mode steps the frame
	 * coordinate past the end and hands the resulting time to the fold, which
	 * needs a meaningful overshoot to reflect or wrap. Clamping here would make
	 * ping-pong stick at the last frame for one tick per turnaround.
	 */
	double FrameCoordinateToTime(double FrameCoordinate) const;

	/** Clamp a time into [First, Last]. Returns First for an empty timeline. */
	double ClampTime(double Time) const;

private:
	TArray<double> Times;
};

/* -------------------------------------------------------------------------- */
/* What the playhead wants                                                      */
/* -------------------------------------------------------------------------- */

/**
 * The answer this whole layer exists to produce: two frames and a blend factor.
 *
 * These are the values that go to FFlowVizVolumeTextureSet::SetDisplayFrames and
 * into the shader's blend, and the values the diagnostics panel shows.
 */
struct FFlowVizFrameSelection
{
	/** Earlier bracketing frame, INDEX_NONE for an empty timeline. */
	int32 FrameA = INDEX_NONE;

	/** Later bracketing frame. Equals FrameA when the time lands on a stored frame or interpolation is off. */
	int32 FrameB = INDEX_NONE;

	/** Blend factor in [0,1]. 0 means "frame A exactly". Always 0 when FrameA == FrameB. */
	double Alpha = 0.0;

	/** Physical time this selection was computed for, in solver units. */
	double Time = 0.0;

	/**
	 * The nearest stored frame - for masks, topology and anything else that must
	 * not be blended (plan.md section 8). Populated whether or not interpolation
	 * is on, and it is NOT always FrameA: at alpha 0.7 the nearest frame is B.
	 */
	int32 NearestFrame = INDEX_NONE;

	/**
	 * True exactly when the shader will blend: two distinct frames and an alpha
	 * strictly between 0 and 1. VISUAL_QA §1 rule 5's "Interpolated" indicator
	 * reads this field and nothing else, so there is one definition rather than
	 * one epsilon per call site.
	 */
	bool bInterpolated = false;

	/** True when FrameA is a usable stored frame. False for an empty timeline. */
	bool IsValid() const
	{
		return FrameA != INDEX_NONE;
	}

	bool operator==(const FFlowVizFrameSelection& Other) const
	{
		return FrameA == Other.FrameA && FrameB == Other.FrameB && NearestFrame == Other.NearestFrame
			&& Alpha == Other.Alpha && Time == Other.Time && bInterpolated == Other.bInterpolated;
	}

	bool operator!=(const FFlowVizFrameSelection& Other) const { return !(*this == Other); }
};

/**
 * Where the playhead is, in a form that survives a mode change.
 *
 * PHASE, NOT TIME, IS THE STATE. PhaseTime advances monotonically in the
 * direction of Speed and is never folded; the displayed time is the fold of it.
 * Keeping the raw phase is what makes ping-pong exact: the turnaround is a
 * property of the triangle wave, not an if-statement that fires when a comparison
 * happens to trip, so a step ten times the timeline's length folds correctly and
 * an exact landing on an end is exactly an end.
 */
struct FFlowVizPlayhead
{
	/** Unfolded position, in solver time units. May lie far outside the timeline; that is the point. */
	double PhaseTime = 0.0;

	/** The fold of PhaseTime into [First, Last]. This is the time everything else uses. */
	double DisplayTime = 0.0;

	/** True when the displayed time is currently increasing. Meaningful for PingPong; equals Speed >= 0 otherwise. */
	bool bForward = true;

	/** Latched by Once when an end is reached. FFlowVizCasePlayer pauses on it; the pure layer only reports it. */
	bool bFinished = false;

	/** Unspent wall seconds in FixedFps mode. Carried between ticks so no sub-step time is lost. */
	double FixedStepResidual = 0.0;
};

/* -------------------------------------------------------------------------- */
/* The pure frame-selection layer                                               */
/* -------------------------------------------------------------------------- */

/**
 * Pure functions. No allocation, no I/O, no RHI, no engine globals - every one
 * of these is a function of its arguments alone, which is what lets
 * FlowVizCasePlayerTest exercise the whole policy under -nullrhi.
 */
namespace FlowVizPlayback
{
	/**
	 * Frames and alpha for a physical time.
	 *
	 * With bInterpolate false this collapses to the NEAREST stored frame - A == B,
	 * alpha 0 - rather than to frame A. Truncating toward A would show the earlier
	 * frame for the whole interval, which lags the data by up to a full frame and
	 * is a different thing from "nearest".
	 */
	FLOWVIZRUNTIME_API FFlowVizFrameSelection SelectFrames(
		const FFlowVizTimeline& Timeline,
		double Time,
		bool bInterpolate);

	/**
	 * Fold an unbounded phase into the timeline under a loop policy.
	 *
	 * Once     clamps, and reports finished at either end.
	 * Loop     wraps modulo the duration, in the direction of travel.
	 * PingPong reflects: the triangle wave of period 2*Duration.
	 *
	 * A single-frame or empty timeline has zero duration and folds to its only
	 * time with bOutForward left as given - there is nowhere to travel, and a
	 * division by that zero duration is the obvious way to produce a NaN playhead.
	 *
	 * @param bSpeedForward Sign of the phase's own motion. Ping-pong composes this
	 *                      with which half of the period the phase is in, so
	 *                      running backwards through a ping-pong is symmetric.
	 * @param bOutForward   Whether the DISPLAYED time is increasing.
	 * @param bOutFinished  Once only: the phase reached or passed an end.
	 */
	FLOWVIZRUNTIME_API double FoldPhase(
		const FFlowVizTimeline& Timeline,
		double PhaseTime,
		EFlowVizLoopMode LoopMode,
		bool bSpeedForward,
		bool& bOutForward,
		bool& bOutFinished);

	/**
	 * Advance the playhead by one tick of wall-clock time.
	 *
	 * PURE: takes a playhead, returns a new one. It does not know whether anything
	 * is loaded and never waits - a stalled loader must not stop physical time, or
	 * a case with a slow disk would play in slow motion and look like a different
	 * simulation.
	 *
	 * @param DeltaSeconds Real seconds since the last tick. Non-positive returns
	 *                     the playhead unchanged (a paused or reversed clock is the
	 *                     caller's business, expressed through Speed).
	 */
	FLOWVIZRUNTIME_API FFlowVizPlayhead Advance(
		const FFlowVizTimeline& Timeline,
		const FFlowVizPlaybackSettings& Settings,
		const FFlowVizPlayhead& Playhead,
		double DeltaSeconds);

	/** A playhead parked at a physical time: phase = that time, forward, not finished, no residual. */
	FLOWVIZRUNTIME_API FFlowVizPlayhead MakePlayheadAtTime(const FFlowVizTimeline& Timeline, double Time);

	/** A playhead parked on a stored frame. Out-of-range indices clamp to an end. */
	FLOWVIZRUNTIME_API FFlowVizPlayhead MakePlayheadAtFrame(const FFlowVizTimeline& Timeline, int32 FrameIndex);

	/**
	 * Step by whole stored frames from the current position, ignoring speed and mode.
	 *
	 * The "previous/next stored frame" buttons. Stepping starts from the NEAREST
	 * frame, so stepping forward from a time two thirds of the way through an
	 * interval lands on the frame after the one you are nearest to, never on the
	 * frame you are already showing.
	 *
	 * @param bWrap Follow the loop policy at the ends. False clamps regardless.
	 */
	FLOWVIZRUNTIME_API FFlowVizPlayhead StepFrames(
		const FFlowVizTimeline& Timeline,
		const FFlowVizPlayhead& Playhead,
		int32 FrameDelta,
		EFlowVizLoopMode LoopMode);

	/**
	 * Frames to have resident for this selection, most important first.
	 *
	 * Order IS the priority (plan.md section 8, "prioritize current frame over
	 * preload"): A, then B, then outward in the direction of travel, then the
	 * other way. A caller that can only start one load this tick must start
	 * element 0. Duplicates are removed and out-of-range indices are dropped, so
	 * the result is directly usable as a request list.
	 */
	FLOWVIZRUNTIME_API void BuildRequestList(
		const FFlowVizTimeline& Timeline,
		const FFlowVizFrameSelection& Selection,
		bool bForward,
		int32 PreloadAhead,
		int32 PreloadBehind,
		TArray<int32>& OutFrames);

	/** The nearest legal speed preset to a custom speed. For a UI that snaps. */
	FLOWVIZRUNTIME_API double SnapToSpeedPreset(double Speed);
}

/* -------------------------------------------------------------------------- */
/* What is actually on screen                                                   */
/* -------------------------------------------------------------------------- */

/**
 * The frames the renderer may sample RIGHT NOW.
 *
 * A DIFFERENT TYPE FROM FFlowVizFrameSelection ON PURPOSE. The selection is what
 * the playhead wants; this is what is complete and resident. During a scrub they
 * differ for as long as a decode takes, and a codebase that used one struct for
 * both would have no way to express "hold the last complete frame while the new
 * frame loads" other than by remembering to. Here it is a type error to confuse
 * them.
 */
struct FFlowVizDisplaySelection
{
	int32 FrameA = INDEX_NONE;
	int32 FrameB = INDEX_NONE;

	/**
	 * DOUBLE HERE, FLOAT AT THE SEAM. Scene/FlowVizVolumeComponent.h's
	 * FFlowVizVolumeFrameSelection stores Alpha as float, so the adapter that
	 * feeds IFlowVizVolumeFrameSource must narrow this value - and narrowing is
	 * not order-preserving near 1.
	 *
	 * An alpha of 1 - 1e-8 is a genuine blend in double and rounds to exactly
	 * 1.0f. Their IsInterpolated() then reports NOT interpolated while FrameA
	 * and FrameB are still distinct, so the shader blends a frame the UI has
	 * just declared un-blended - the disclosure failure VISUAL_QA section 1
	 * rule 5 exists to prevent. It is reachable on the shipped sample's own
	 * 0.05 spacing: t = 0.10 - 1e-11 gives alpha 0.9999999998 -> 1.0f.
	 *
	 * So the adapter must not merely cast. It must re-derive the pair after
	 * narrowing: if float(Alpha) lands on 0.0f or 1.0f, collapse to the single
	 * frame that alpha names (A or B respectively) and emit FrameB == FrameA,
	 * which is the same (F, F, 0) spelling TryBracket already uses for an exact
	 * landing. Then the two predicates agree on every input.
	 *
	 * AND THE COLLAPSE IS NOT MERELY COSMETIC, which corrects an earlier note
	 * here that called this a disclosure-only bug. Whether a collapsed alpha
	 * also moves a PIXEL depends on which blend the shader writes:
	 *
	 *     a + t*(b-a)        the mad/HLSL-lerp form: NOT exact at t == 1
	 *     (1-t)*a + t*b      the two-product form:   exact at t == 1
	 *
	 * The first form is wrong at t = 1.0f whenever (b-a) is not representable,
	 * which happens at ordinary CFD magnitudes, not just extreme ones:
	 * a = -1000, b = 0.001 returns 0.000976562 rather than b. So with the mad
	 * form a narrowed alpha renders a value the solver never produced, at a
	 * timestep the UI simultaneously reports as un-interpolated.
	 *
	 * The identity DOES hold for both forms when a and b are within a factor of
	 * two (Sterbenz: b-a is then exact), so a sweep drawn from a narrow range
	 * confirms it and proves nothing. That is how the "disclosure-only" reading
	 * arose. Fields here span sign changes and several decades.
	 *
	 * The choice is a REAL TRADE, not a free win, and the two-product form is
	 * preferred here on the merits rather than because it is better everywhere:
	 *
	 *   mad          monotonic in t, but not exact at t == 1
	 *   two-product  exact at both endpoints, but NOT monotonic in t
	 *
	 * The two-product form can step backwards as t advances by one ULP - on
	 * a=1, b=2 it does so at 62503 of 3e6 consecutive t. Note that boundedness
	 * and monotonicity are different properties: the result never leaves
	 * [min, max], so a sweep that checks only bounds finds nothing and reads as
	 * a clean bill of health.
	 *
	 * That wobble is accepted here because of its SIZE. It is 1 ULP
	 * (1.19e-07 of the A..B span), which is far below one step of any
	 * pseudocolor ramp, and it is non-monotonic only in the sense that a value
	 * repeats or retreats by that ULP - the field is still visually smooth. The
	 * mad form's endpoint error is not comparable: at a = -1000, b = 0.001 it
	 * is 2.3e-2 relative, a wrong scalar pseudocolored as measurement.
	 * A 1-ULP wobble in a smooth interior is a rounding artifact; a wrong
	 * endpoint is a false measurement. Only the second is a provenance lie.
	 *
	 * The temporal blend is not written yet. When it is, it must use the
	 * two-product form, and the collapse above must still happen so the pair
	 * and the disclosure agree.
	 *
	 * Verified by compiling both forms and sweeping, not by inspection. Every
	 * sweep behind these numbers needs its coverage checked before it is
	 * believed: a shared loop guard, a fixed decimal t-step instead of
	 * consecutive floats, and an a/b range inside a factor of two each produced
	 * a confident zero here before being corrected.
	 */
	double Alpha = 0.0;

	/** Physical time of THIS pair - the time the pixels correspond to, which is what the diagnostics panel must show. */
	double Time = 0.0;

	int32 NearestFrame = INDEX_NONE;
	bool bInterpolated = false;

	/**
	 * The playhead has moved past what is resident: these pixels are older than
	 * the timeline position. The UI should say so; it must not be treated as an
	 * error, since it is the normal state during a scrub.
	 */
	bool bStale = false;

	bool IsValid() const { return FrameA != INDEX_NONE; }
};

namespace FlowVizPlayback
{
	/**
	 * Promote a selection to the display only if every frame it names is resident
	 * and complete; otherwise hold the previous display and mark it stale.
	 *
	 * THIS FUNCTION IS THE "never display a partially updated set of field
	 * components" requirement. It is pure and takes residency as a predicate so
	 * the rule can be tested exhaustively with no cache, no files and no GPU.
	 *
	 * A selection naming a resident A and a NON-resident B is not partially
	 * accepted. Displaying A alone would be honest about the data but would flip
	 * the "Interpolated" indicator off and on for every frame of a scrub, and -
	 * worse - would change the displayed physical time to something the user did
	 * not ask for. The previous complete pair is held instead.
	 */
	FLOWVIZRUNTIME_API FFlowVizDisplaySelection ResolveDisplay(
		const FFlowVizFrameSelection& Desired,
		const FFlowVizDisplaySelection& Previous,
		TFunctionRef<bool(int32)> IsFrameResident);
}

/* -------------------------------------------------------------------------- */
/* Diagnostics                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * One snapshot for the diagnostics panel (plan.md section 8: "Display frame A,
 * frame B, alpha, and physical time"; VISUAL_QA §1 rule 5: the interpolation
 * disclosure).
 *
 * Both the wanted and the shown selections are here. Showing only one would make
 * a stalled loader indistinguishable from a stopped playhead, which is precisely
 * the thing a diagnostics panel is for.
 */
struct FFlowVizPlaybackDiagnostics
{
	/** What the playhead wants. */
	FFlowVizFrameSelection Selection;

	/** What the renderer may sample. Differs from Selection while frames load. */
	FFlowVizDisplaySelection Display;

	/** Playhead position, in solver units - the authoritative "physical time" readout. */
	double PhysicalTime = 0.0;

	/** Unfolded phase, for debugging ping-pong. Not user-facing. */
	double PhaseTime = 0.0;

	bool bPlaying = false;
	bool bForward = true;
	bool bFinished = false;

	EFlowVizPlaybackMode Mode = EFlowVizPlaybackMode::Sequence;
	EFlowVizLoopMode LoopMode = EFlowVizLoopMode::Loop;
	double Speed = 1.0;
	bool bInterpolationEnabled = true;

	int32 FrameCount = 0;

	/** Decodes started, finished, and dropped because the playhead moved on. */
	int64 LoadsStarted = 0;
	int64 LoadsCompleted = 0;
	int64 LoadsCancelled = 0;
	int64 LoadsFailed = 0;

	/** Decodes in flight right now. */
	int32 LoadsInFlight = 0;

	/** Selections that found their frames already resident, and those that did not. */
	int64 CacheHits = 0;
	int64 CacheMisses = 0;

	FFlowVizFrameCacheStats Cache;

	/** The first failure any decode reported since Open, kept so a broken frame is reported once rather than per tick. */
	FCFDVizResult LastError;
};

/* -------------------------------------------------------------------------- */
/* The player                                                                   */
/* -------------------------------------------------------------------------- */

/** How a frame request is progressing. */
enum class EFlowVizLoadState : uint8
{
	/** Not requested, not resident. */
	Absent = 0,
	/** A decode task is running or queued. */
	Loading = 1,
	/** Decoded and uploaded; displayable. */
	Resident = 2,
	/** A decode failed for this frame. It is not retried every tick; see FFlowVizCasePlayer::RetryFailedFrames. */
	Failed = 3
};

/**
 * The stateful shell: timeline, playhead, cache, decode tasks and the texture set.
 *
 * THREADING. Construct, Tick and every mutator are GAME THREAD ONLY. Frame
 * decode - file I/O and zlib, which engineering rule 1 forbids on the game
 * thread - runs on the engine task system, and its results are drained in Tick.
 * FFlowVizVolumeTextureSet::EnqueueUpload is then called from the game thread,
 * which is what its contract asks for: it enqueues the render command itself.
 *
 * LIFETIME. ~FFlowVizCasePlayer waits for outstanding decode tasks. Close() does
 * the same and is the explicit form. A task holds only shared state, never a
 * pointer back to the player, so a task still running during shutdown cannot
 * touch a destroyed object - it writes into a shared queue nobody will drain.
 *
 * THE TEXTURE SET IS OPTIONAL. With none attached the player decodes and caches
 * but uploads nothing, which is a real configuration (a headless validation
 * pass) and is also how the decode path is tested under -nullrhi.
 */
class FLOWVIZRUNTIME_API FFlowVizCasePlayer
{
public:
	FFlowVizCasePlayer();
	~FFlowVizCasePlayer();

	FFlowVizCasePlayer(const FFlowVizCasePlayer&) = delete;
	FFlowVizCasePlayer& operator=(const FFlowVizCasePlayer&) = delete;

	/* --- Opening ---------------------------------------------------------- */

	/**
	 * Bind to a case and a field.
	 *
	 * @param Case    Shared because decode tasks read it on a worker thread while
	 *                the game thread may Close the player. Must be a validated
	 *                case - FCFDVizCase::ParseFromString has already run Validate.
	 * @param FieldId The volume field to play. Its grid's `maskField`, when
	 *                declared, is loaded alongside it for the status texture.
	 * @return Ok, or IndexOutOfRange when no such field exists, or InvalidManifest
	 *         for a case whose timeline is not strictly increasing.
	 *         A ZERO-FRAME case opens successfully and plays nothing: an empty
	 *         timeline is a valid manifest, and refusing it here would turn a
	 *         legitimate case into a load error.
	 */
	FCFDVizResult Open(const TSharedRef<const FCFDVizCase>& Case, FName FieldId);

	/** Wait for outstanding decodes, drop every reservation, and unbind. Safe to call when not open. */
	void Close();

	bool IsOpen() const { return Case.IsValid(); }
	const FCFDVizCase* GetCase() const { return Case.Get(); }
	FName GetFieldId() const { return FieldId; }
	const FFlowVizTimeline& GetTimeline() const { return Timeline; }

	/**
	 * Attach the GPU destination. Pass nullptr to detach.
	 *
	 * The set is NOT owned and must outlive the player, or be detached first. The
	 * player calls Initialize on it only if it has no buffers yet.
	 */
	void SetTextureSet(FFlowVizVolumeTextureSet* InTextureSet);
	FFlowVizVolumeTextureSet* GetTextureSet() const { return TextureSet; }

	/* --- Transport -------------------------------------------------------- */

	void Play();
	void Pause();

	/** Pause and return to the first frame, clearing the finished latch. */
	void Stop();

	bool IsPlaying() const { return bPlaying; }

	/**
	 * Advance by DeltaSeconds of wall time, drain finished decodes, start new
	 * ones, and update the display.
	 *
	 * Draining happens even when paused, so a scrub that stops with a decode in
	 * flight still shows the frame it asked for. Game thread only.
	 */
	void Tick(double DeltaSeconds);

	/** Jump to a physical time in solver units. Clamped to the timeline. Does not change play/pause. */
	void SeekToTime(double Time);

	/** Jump to a stored frame. Out-of-range indices clamp to an end. */
	void SeekToFrame(int32 FrameIndex);

	/** Normalised scrub position in [0,1] over [FirstTime, LastTime]. What a timeline slider drives. */
	void SeekToNormalized(double Normalized);

	void SeekToFirstFrame();
	void SeekToLastFrame();

	/** Move by whole stored frames. Negative goes back. Honours the loop mode at the ends. */
	void StepFrames(int32 FrameDelta);

	/* --- Settings --------------------------------------------------------- */

	const FFlowVizPlaybackSettings& GetSettings() const { return Settings; }

	/**
	 * Replace the settings.
	 *
	 * @return the result of FFlowVizPlaybackSettings::Validate. Nothing is applied
	 *         on failure, so a bad speed cannot leave the player half-configured.
	 */
	FCFDVizResult SetSettings(const FFlowVizPlaybackSettings& InSettings);

	void SetMode(EFlowVizPlaybackMode Mode);
	void SetLoopMode(EFlowVizLoopMode LoopMode);

	/** @return IndexOutOfRange for a speed outside [MinSpeed, MaxSpeed] or too near zero; the speed is unchanged. */
	FCFDVizResult SetSpeed(double Speed);

	/** @return IndexOutOfRange for an index outside the preset table. */
	FCFDVizResult SetSpeedPreset(int32 PresetIndex);

	void SetInterpolationEnabled(bool bEnabled);
	bool IsInterpolationEnabled() const { return Settings.bInterpolate; }

	/* --- State ------------------------------------------------------------ */

	/** What the playhead wants right now. */
	const FFlowVizFrameSelection& GetSelection() const { return Selection; }

	/** What the renderer may sample right now. Never names a frame that is not complete. */
	const FFlowVizDisplaySelection& GetDisplay() const { return Display; }

	/** Playhead time in solver units. */
	double GetPhysicalTime() const { return Playhead.DisplayTime; }

	/** Position in [0,1] over the timeline, for a slider. 0 for an empty or single-frame timeline. */
	double GetNormalizedTime() const;

	EFlowVizLoadState GetFrameLoadState(int32 FrameIndex) const;

	FFlowVizPlaybackDiagnostics GetDiagnostics() const;

	/* --- Cache ------------------------------------------------------------ */

	FFlowVizFrameCache& GetCache() { return Cache; }
	const FFlowVizFrameCache& GetCache() const { return Cache; }

	/** Set the residency budgets, evicting whatever no longer fits. Forwards FFlowVizFrameCache::SetBudgets. */
	FCFDVizResult SetMemoryBudgets(int64 CpuBudgetBytes, int64 GpuBudgetBytes);

	/**
	 * Concurrent decode tasks. Above ~2 an aggressive scrub queues more work than
	 * it can display, which is the load the cancellation path exists to shed.
	 *
	 * @return IndexOutOfRange for a value outside 1..FlowVizPlayback::MaxConcurrentLoads.
	 */
	FCFDVizResult SetMaxConcurrentLoads(int32 MaxLoads);
	int32 GetMaxConcurrentLoads() const { return MaxConcurrentLoads; }

	/** Clear the failed set so those frames are attempted again. For a "retry" button. */
	void RetryFailedFrames();

	/**
	 * Estimated bytes one frame of this field occupies, computed from the manifest
	 * BEFORE anything is read.
	 *
	 * This is what the cache reserves against, which is the point: engineering
	 * rule 12 wants the size checked before the allocation it guards, and a budget
	 * applied after a decode has already allocated is decoration. Includes the
	 * 3 -> 4 channel widening and the status texture.
	 *
	 * @return false for a field whose size does not fit an int64, leaving the
	 *         outputs untouched.
	 */
	bool TryEstimateFrameBytes(int64& OutCpuBytes, int64& OutGpuBytes) const;

	/**
	 * Block until every in-flight decode has finished and been drained.
	 *
	 * FOR TESTS AND FOR SHUTDOWN ONLY. Calling it from a Tick would put file I/O
	 * on the game thread, which is the thing this design exists to avoid.
	 *
	 * @param TimeoutSeconds Give up after this long. @return false on timeout.
	 */
	bool WaitForPendingLoads(double TimeoutSeconds = 30.0);

private:
	/** Shared with worker tasks; outlives the player if a task is still running. */
	struct FLoadQueue;

	void DrainCompletedLoads();
	void StartPendingLoads();
	void RequestFrame(int32 FrameIndex);
	void CancelObsoleteRequests(TArrayView<const int32> KeepFrames);
	void UpdateDisplay();
	void ApplyDisplayToTextureSet();

	TSharedPtr<const FCFDVizCase> Case;
	FName FieldId;

	/** Resolved once at Open so no tick walks the field array. Points into Case. */
	const FCFDVizField* Field = nullptr;
	const FCFDVizField* MaskField = nullptr;

	FFlowVizTimeline Timeline;
	FFlowVizPlaybackSettings Settings;
	FFlowVizPlayhead Playhead;

	FFlowVizFrameSelection Selection;
	FFlowVizDisplaySelection Display;

	FFlowVizFrameCache Cache;

	FFlowVizVolumeTextureSet* TextureSet = nullptr;

	TSharedPtr<FLoadQueue, ESPMode::ThreadSafe> LoadQueue;

	/** Frames with a decode in flight, and frames whose decode failed. Small sets; linear search is cheaper than a map. */
	TArray<int32> InFlightFrames;
	TArray<int32> FailedFrames;

	bool bPlaying = false;

	int32 MaxConcurrentLoads = FlowVizPlayback::DefaultMaxConcurrentLoads;

	/**
	 * Incremented on every seek and every selection change. A completed decode
	 * whose request generation is older than this AND whose frame is no longer
	 * wanted is dropped - that is the "cancel obsolete requests during aggressive
	 * scrubbing" requirement.
	 */
	uint64 RequestGeneration = 0;

	int64 LoadsStarted = 0;
	int64 LoadsCompleted = 0;
	int64 LoadsCancelled = 0;
	int64 LoadsFailed = 0;
	int64 CacheHits = 0;
	int64 CacheMisses = 0;

	FCFDVizResult LastError;
};
