// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CFDViz/CFDVizTypes.h"
#include "Playback/FlowVizCasePlayer.h"

/**
 * The UI layer's transport state (plan.md section 5F, section 8).
 *
 * WHY A VIEW MODEL AND NOT A WIDGET GRAPH. plan.md section 5F requires core view
 * state in C++ rather than in a widget event graph, and the reason is testable
 * here: every rule below - what a button may do, what the badge says, which
 * frame a scrub lands on - is a pure function of the player's state, so it can
 * be asserted headlessly. Put the same logic in a Blueprint and the only way to
 * check it is to press the button.
 *
 * THIS OWNS NO PLAYBACK LOGIC. Frame selection, folding, loop turnaround, speed
 * validation and residency all live in FFlowVizCasePlayer and FlowVizPlayback,
 * which are already tested against a non-uniformly spaced timeline. This class
 * DRIVES that; it does not restate it. Any arithmetic on frame indices or times
 * that appears here is a bug, with two deliberate exceptions that are UI
 * decisions rather than playback ones:
 *
 *   1. GetSpeedPresetIndex, which asks "is the current speed exactly a preset",
 *      because the answer decides whether a preset button renders as selected.
 *   2. The badge below, which is a presentation of the DISPLAY state.
 *
 * THE BADGE IS THE POINT OF ENGINEERING RULE 7. "Any visual temporal
 * interpolation must be visibly identified as interpolation." A badge derived
 * from the SELECTION would claim "interpolated" for a blend the user is not
 * seeing, and claim "exact" while a stalled scrub holds an older pair on screen.
 * It is therefore derived from FFlowVizCasePlayer::GetDisplay - what is
 * resident and on screen - and never from GetSelection.
 *
 * ENABLEMENT PREDICATES EXIST BECAUSE OF RULE 15. "Do not ship nonfunctional
 * buttons." An unbound view model, or one bound to a case with no frames, must
 * report CanPlay() == false so the workspace can disable the control rather than
 * offer a button that silently does nothing.
 *
 * THREADING. Game thread only, because FFlowVizCasePlayer is game thread only.
 * The pointer is BORROWED and not owned; the workspace outlives the player at
 * its own peril, so Unbind() before destroying a player.
 */

/**
 * What the transport should tell the user about the frame on screen.
 *
 * Four values, not a bool, because "nothing is displayable yet" and "an exact
 * stored frame" are different answers and collapsing them is how a viewer ends
 * up claiming to show frame 0 of a case whose first frame has not finished
 * decoding.
 */
enum class EFlowVizFrameBadge : uint8
{
	/** No player is bound, or the bound player has no case. Transport controls should be disabled entirely. */
	NoCase = 0,
	/** A case is open but nothing complete is resident yet. Nothing is on screen; do not name a frame. */
	NoData = 1,
	/** Exactly one stored frame is displayed, at its own physical time. */
	Exact = 2,
	/** Two stored frames are blended. Rule 7 requires this to be visible. */
	Interpolated = 3,
	/**
	 * The requested time is not resident, so an older complete pair is held.
	 *
	 * Ranked above Interpolated deliberately: the displayed physical time is not
	 * the requested one, which is the more misleading of the two facts. Ask
	 * IsShowingInterpolatedData() as well when both matter - a held pair can also
	 * be an interpolated one.
	 */
	HeldStale = 4,
};

class FLOWVIZRUNTIME_API FFlowVizTimelineViewModel
{
public:
	FFlowVizTimelineViewModel() = default;

	/* --- Binding ---------------------------------------------------------- */

	/** Borrow a player. Passing nullptr is the same as Unbind(). */
	void BindPlayer(FFlowVizCasePlayer* InPlayer);
	void Unbind();
	bool IsBound() const { return Player != nullptr; }
	FFlowVizCasePlayer* GetPlayer() const { return Player; }

	/* --- Enablement, per engineering rule 15 ------------------------------ */

	/**
	 * True when there is a bound, open player with at least one frame.
	 *
	 * THESE ARE CAPABILITIES OF THE CASE, NOT MOMENTARY AVAILABILITIES. CanPause()
	 * is true on a paused player, because play and pause are one toggle button
	 * here and a predicate that went false while paused would disable the toggle
	 * in the state where its only job is to start playback. It would also carry
	 * no information: it would be exactly IsPlaying() && CanPlay(). A UI with
	 * separate buttons should write that conjunction itself.
	 *
	 * What rule 15 actually requires is that the predicate and the ACTION agree -
	 * never a disabled button whose method quietly works, or an enabled one whose
	 * method refuses. Every predicate here is asserted against its action in
	 * FlowViz.UI.TimelineViewModel.*.
	 */
	bool HasFrames() const;
	bool CanPlay() const;
	bool CanPause() const;
	/** A single-frame case can be scrubbed nowhere; stepping is disabled rather than a no-op button. */
	bool CanStep() const;
	bool CanScrub() const;

	/* --- Transport -------------------------------------------------------- */

	FCFDVizResult Play();
	FCFDVizResult Pause();
	/** Play when paused, pause when playing. Refused when CanPlay() is false. */
	FCFDVizResult TogglePlayPause();
	FCFDVizResult Stop();
	FCFDVizResult GoToFirstFrame();
	FCFDVizResult GoToLastFrame();

	/**
	 * Step one frame and PAUSE.
	 *
	 * The pause is a deliberate transition rather than an oversight: a step taken
	 * while playing is overwritten by the very next Tick, so a stepping button
	 * that left playback running would appear to do nothing at all - which is the
	 * "nonfunctional button" of rule 15 wearing a functional disguise.
	 */
	FCFDVizResult StepForward();
	FCFDVizResult StepBackward();

	/** Scrub the slider. Normalized runs over PHYSICAL TIME, matching FFlowVizCasePlayer::SeekToNormalized. */
	FCFDVizResult ScrubToNormalized(double Normalized);
	FCFDVizResult ScrubToTime(double Time);
	FCFDVizResult ScrubToFrame(int32 FrameIndex);

	/* --- Rate and mode ---------------------------------------------------- */

	/** @param PresetIndex Into FlowVizPlayback::SpeedPresets. Out of range is refused and nothing is applied. */
	FCFDVizResult SetSpeedPresetIndex(int32 PresetIndex);

	/**
	 * An arbitrary typed speed.
	 *
	 * Zero is REFUSED rather than treated as a pause, because the player rejects
	 * it: a paused player and a playing one at speed zero look identical on
	 * screen and differ in every diagnostic. The prior speed survives a refusal.
	 */
	FCFDVizResult SetCustomSpeed(double Speed);

	/** Index into SpeedPresets when the speed is exactly one, else INDEX_NONE - which is what makes a preset button render as selected. */
	int32 GetSpeedPresetIndex() const;
	double GetSpeed() const;

	FCFDVizResult SetLoopMode(EFlowVizLoopMode LoopMode);
	EFlowVizLoopMode GetLoopMode() const;
	FCFDVizResult SetPlaybackMode(EFlowVizPlaybackMode Mode);
	EFlowVizPlaybackMode GetPlaybackMode() const;
	FCFDVizResult SetInterpolationEnabled(bool bEnabled);
	bool IsInterpolationEnabled() const;

	/* --- Readouts --------------------------------------------------------- */

	/** Physical time in the case's own time unit. 0 when unbound - ask HasFrames() before showing it. */
	double GetPhysicalTime() const;
	double GetNormalizedTime() const;
	int32 GetFrameCount() const;

	/** The stored frame actually on screen, or INDEX_NONE when nothing complete is resident. */
	int32 GetDisplayedFrame() const;
	/** The physical time actually on screen, which during a stall is NOT GetPhysicalTime(). */
	double GetDisplayedTime() const;

	EFlowVizFrameBadge GetBadge() const;
	/** True when the pixels on screen are a blend of two stored frames, whether or not they are also stale. */
	bool IsShowingInterpolatedData() const;
	bool IsPlaying() const;

private:
	/** Borrowed. Never owned - the workspace does not control the player's lifetime. */
	FFlowVizCasePlayer* Player = nullptr;
};
