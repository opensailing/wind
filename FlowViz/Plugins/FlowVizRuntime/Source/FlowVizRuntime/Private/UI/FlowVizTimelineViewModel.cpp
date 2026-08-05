// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizTimelineViewModel.h"

#include "CFDViz/CFDVizManifest.h"

/**
 * See FlowVizTimelineViewModel.h for the design rules. Two of them govern almost
 * every line below and are worth restating where the code is:
 *
 *   THIS FILE CONTAINS NO PLAYBACK ARITHMETIC. Every transport call forwards to
 *   FFlowVizCasePlayer. Where a computation appears to be happening here it is
 *   either a guard (is the control legal at all) or a presentation decision
 *   (which badge, which preset button is lit).
 *
 *   A REFUSED CALL CHANGES NOTHING. Every mutator validates BEFORE touching the
 *   player, so a rejected speed or a rejected mode leaves the previous state
 *   exactly as it was. This matters because the workspace shows the current
 *   value: a partially-applied setting would display as accepted.
 */

// Named rather than anonymous: see the note in FlowVizClipViewModel.cpp. Under a
// unity build these helpers share a translation unit with the sibling view
// models, and an anonymous namespace would collide with their same-named ones.
namespace FlowVizTimelineViewModelLocal
{
	/** One place for "there is nothing to drive", so the message is identical from every entry point. */
	FCFDVizResult MakeNoCaseResult()
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			TEXT("No case is open in the bound player, so there is no timeline to drive"));
	}

	FCFDVizResult MakeUnboundResult()
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			TEXT("The timeline view model is not bound to a player"));
	}
}


/* ========================================================================== */
/* Binding                                                                     */
/* ========================================================================== */

void FFlowVizTimelineViewModel::BindPlayer(FFlowVizCasePlayer* InPlayer)
{
	Player = InPlayer;
}

void FFlowVizTimelineViewModel::Unbind()
{
	Player = nullptr;
}

/* ========================================================================== */
/* Enablement (engineering rule 15)                                            */
/* ========================================================================== */

bool FFlowVizTimelineViewModel::HasFrames() const
{
	return Player != nullptr && Player->IsOpen() && Player->GetTimeline().GetFrameCount() > 0;
}

bool FFlowVizTimelineViewModel::CanPlay() const
{
	// A single-frame case is deliberately NOT playable: there is nothing to
	// advance to, and a play button that runs a timer over one frame is a
	// nonfunctional button that looks busy.
	return HasFrames() && Player->GetTimeline().GetFrameCount() > 1;
}

bool FFlowVizTimelineViewModel::CanPause() const
{
	return CanPlay();
}

bool FFlowVizTimelineViewModel::CanStep() const
{
	return CanPlay();
}

bool FFlowVizTimelineViewModel::CanScrub() const
{
	return CanPlay();
}

/* ========================================================================== */
/* Transport                                                                   */
/* ========================================================================== */

FCFDVizResult FFlowVizTimelineViewModel::Play()
{
	if (Player == nullptr)
	{
		return FlowVizTimelineViewModelLocal::MakeUnboundResult();
	}
	if (!CanPlay())
	{
		// THE PREDICATE AND THE ACTION MUST AGREE. A CanPlay() that returns false
		// while Play() quietly succeeds is worse than either alone: the button is
		// disabled, so nobody discovers the disagreement until a keyboard shortcut
		// or a session reload calls the method directly.
		return FlowVizTimelineViewModelLocal::MakeNoCaseResult();
	}
	Player->Play();
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizTimelineViewModel::Pause()
{
	if (Player == nullptr)
	{
		return FlowVizTimelineViewModelLocal::MakeUnboundResult();
	}
	if (!CanPause())
	{
		return FlowVizTimelineViewModelLocal::MakeNoCaseResult();
	}
	Player->Pause();
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizTimelineViewModel::TogglePlayPause()
{
	if (Player == nullptr)
	{
		return FlowVizTimelineViewModelLocal::MakeUnboundResult();
	}
	return Player->IsPlaying() ? Pause() : Play();
}

FCFDVizResult FFlowVizTimelineViewModel::Stop()
{
	if (Player == nullptr)
	{
		return FlowVizTimelineViewModelLocal::MakeUnboundResult();
	}
	if (!HasFrames())
	{
		return FlowVizTimelineViewModelLocal::MakeNoCaseResult();
	}
	// Stop is legal on a single-frame case even though Play is not: it is a
	// rewind, and rewinding a one-frame case is a defined no-op rather than a
	// lie. Hence HasFrames() here and CanPlay() above.
	Player->Stop();
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizTimelineViewModel::GoToFirstFrame()
{
	if (Player == nullptr)
	{
		return FlowVizTimelineViewModelLocal::MakeUnboundResult();
	}
	if (!HasFrames())
	{
		return FlowVizTimelineViewModelLocal::MakeNoCaseResult();
	}
	Player->SeekToFirstFrame();
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizTimelineViewModel::GoToLastFrame()
{
	if (Player == nullptr)
	{
		return FlowVizTimelineViewModelLocal::MakeUnboundResult();
	}
	if (!HasFrames())
	{
		return FlowVizTimelineViewModelLocal::MakeNoCaseResult();
	}
	Player->SeekToLastFrame();
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizTimelineViewModel::StepForward()
{
	if (Player == nullptr)
	{
		return FlowVizTimelineViewModelLocal::MakeUnboundResult();
	}
	if (!CanStep())
	{
		return FlowVizTimelineViewModelLocal::MakeNoCaseResult();
	}
	// PAUSE FIRST, THEN STEP. The other order works today because StepFrames does
	// not consult bPlaying, but it depends on that and would break silently if it
	// ever did. Pausing first states the intent: after a step, the playhead the
	// user asked for is the one that stays.
	Player->Pause();
	Player->StepFrames(1);
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizTimelineViewModel::StepBackward()
{
	if (Player == nullptr)
	{
		return FlowVizTimelineViewModelLocal::MakeUnboundResult();
	}
	if (!CanStep())
	{
		return FlowVizTimelineViewModelLocal::MakeNoCaseResult();
	}
	Player->Pause();
	Player->StepFrames(-1);
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizTimelineViewModel::ScrubToNormalized(double Normalized)
{
	if (Player == nullptr)
	{
		return FlowVizTimelineViewModelLocal::MakeUnboundResult();
	}
	if (!CanScrub())
	{
		return FlowVizTimelineViewModelLocal::MakeNoCaseResult();
	}
	if (!FMath::IsFinite(Normalized))
	{
		// A non-finite slider position is refused rather than clamped. Clamping a
		// NaN gives whichever end the comparison happens to take, and a scrub that
		// silently jumps to an end of the timeline reads as a playback bug.
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange, TEXT("A normalized scrub position must be finite"));
	}
	// Out of [0,1] IS clamped, deliberately: a slider drag past its own end is an
	// ordinary interaction, not an error, and refusing it would strand the
	// playhead mid-drag.
	Player->SeekToNormalized(Normalized);
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizTimelineViewModel::ScrubToTime(double Time)
{
	if (Player == nullptr)
	{
		return FlowVizTimelineViewModelLocal::MakeUnboundResult();
	}
	if (!CanScrub())
	{
		return FlowVizTimelineViewModelLocal::MakeNoCaseResult();
	}
	if (!FMath::IsFinite(Time))
	{
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange, TEXT("A scrub time must be finite"));
	}
	Player->SeekToTime(Time);
	return FCFDVizResult::Ok();
}

FCFDVizResult FFlowVizTimelineViewModel::ScrubToFrame(int32 FrameIndex)
{
	if (Player == nullptr)
	{
		return FlowVizTimelineViewModelLocal::MakeUnboundResult();
	}
	if (!HasFrames())
	{
		return FlowVizTimelineViewModelLocal::MakeNoCaseResult();
	}
	const int32 FrameCount = Player->GetTimeline().GetFrameCount();
	if (FrameIndex < 0 || FrameIndex >= FrameCount)
	{
		// A frame index is TYPED, not dragged. Unlike a slider position, an
		// out-of-range index is a caller error - from a session file, a spin box,
		// or a script - and clamping it would load frame 19 while the field still
		// reads 200.
		return FCFDVizResult::Fail(
			ECFDVizError::IndexOutOfRange,
			FString::Printf(
				TEXT("Frame %d is outside the timeline's %d frames"), FrameIndex, FrameCount));
	}
	Player->SeekToFrame(FrameIndex);
	return FCFDVizResult::Ok();
}

/* ========================================================================== */
/* Rate and mode                                                               */
/* ========================================================================== */

FCFDVizResult FFlowVizTimelineViewModel::SetSpeedPresetIndex(int32 PresetIndex)
{
	if (Player == nullptr)
	{
		return FlowVizTimelineViewModelLocal::MakeUnboundResult();
	}
	// Forwarded, not re-validated: FFlowVizCasePlayer::SetSpeedPreset already
	// range-checks the index and applies nothing on failure.
	return Player->SetSpeedPreset(PresetIndex);
}

FCFDVizResult FFlowVizTimelineViewModel::SetCustomSpeed(double Speed)
{
	if (Player == nullptr)
	{
		return FlowVizTimelineViewModelLocal::MakeUnboundResult();
	}
	return Player->SetSpeed(Speed);
}

int32 FFlowVizTimelineViewModel::GetSpeedPresetIndex() const
{
	if (Player == nullptr)
	{
		return INDEX_NONE;
	}
	const double Speed = Player->GetSettings().Speed;
	for (int32 Index = 0; Index < FlowVizPlayback::NumSpeedPresets; ++Index)
	{
		// EXACT comparison against the preset MAGNITUDE, sign ignored: -2x is the
		// 2x preset played backwards, and the preset button should light for it.
		// The tolerance is a floating-point equality tolerance and nothing more -
		// a speed within 1e-9 of a preset came from that preset. Widening it would
		// light a preset button for a speed the user typed by hand, which is a
		// rule 15 lie in the other direction: a control that claims a state it is
		// not in.
		if (FMath::IsNearlyEqual(FMath::Abs(Speed), FlowVizPlayback::SpeedPresets[Index], 1.0e-9))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

double FFlowVizTimelineViewModel::GetSpeed() const
{
	return Player != nullptr ? Player->GetSettings().Speed : 1.0;
}

FCFDVizResult FFlowVizTimelineViewModel::SetLoopMode(EFlowVizLoopMode LoopMode)
{
	if (Player == nullptr)
	{
		return FlowVizTimelineViewModelLocal::MakeUnboundResult();
	}
	Player->SetLoopMode(LoopMode);
	return FCFDVizResult::Ok();
}

EFlowVizLoopMode FFlowVizTimelineViewModel::GetLoopMode() const
{
	return Player != nullptr ? Player->GetSettings().LoopMode : EFlowVizLoopMode::Loop;
}

FCFDVizResult FFlowVizTimelineViewModel::SetPlaybackMode(EFlowVizPlaybackMode Mode)
{
	if (Player == nullptr)
	{
		return FlowVizTimelineViewModelLocal::MakeUnboundResult();
	}
	Player->SetMode(Mode);
	return FCFDVizResult::Ok();
}

EFlowVizPlaybackMode FFlowVizTimelineViewModel::GetPlaybackMode() const
{
	return Player != nullptr ? Player->GetSettings().Mode : EFlowVizPlaybackMode::Sequence;
}

FCFDVizResult FFlowVizTimelineViewModel::SetInterpolationEnabled(bool bEnabled)
{
	if (Player == nullptr)
	{
		return FlowVizTimelineViewModelLocal::MakeUnboundResult();
	}
	Player->SetInterpolationEnabled(bEnabled);
	return FCFDVizResult::Ok();
}

bool FFlowVizTimelineViewModel::IsInterpolationEnabled() const
{
	return Player != nullptr && Player->IsInterpolationEnabled();
}

/* ========================================================================== */
/* Readouts                                                                    */
/* ========================================================================== */

double FFlowVizTimelineViewModel::GetPhysicalTime() const
{
	return Player != nullptr ? Player->GetPhysicalTime() : 0.0;
}

double FFlowVizTimelineViewModel::GetNormalizedTime() const
{
	return Player != nullptr ? Player->GetNormalizedTime() : 0.0;
}

int32 FFlowVizTimelineViewModel::GetFrameCount() const
{
	return Player != nullptr && Player->IsOpen() ? Player->GetTimeline().GetFrameCount() : 0;
}

int32 FFlowVizTimelineViewModel::GetDisplayedFrame() const
{
	if (Player == nullptr)
	{
		return INDEX_NONE;
	}
	// THE DISPLAY, NOT THE SELECTION. GetSelection().FrameA is what the playhead
	// WANTS; during a decode stall it names a frame that is not on screen. A
	// readout wired to it would print a frame number for pixels the user is not
	// looking at.
	const FFlowVizDisplaySelection& CurrentDisplay = Player->GetDisplay();
	return CurrentDisplay.IsValid() ? CurrentDisplay.FrameA : INDEX_NONE;
}

double FFlowVizTimelineViewModel::GetDisplayedTime() const
{
	if (Player == nullptr)
	{
		return 0.0;
	}
	const FFlowVizDisplaySelection& CurrentDisplay = Player->GetDisplay();
	return CurrentDisplay.IsValid() ? CurrentDisplay.Time : 0.0;
}

EFlowVizFrameBadge FFlowVizTimelineViewModel::GetBadge() const
{
	if (!HasFrames())
	{
		return EFlowVizFrameBadge::NoCase;
	}

	const FFlowVizDisplaySelection& CurrentDisplay = Player->GetDisplay();
	if (!CurrentDisplay.IsValid())
	{
		// A case is open but nothing complete is resident. Naming a frame here is
		// the specific failure the four-value badge exists to prevent.
		return EFlowVizFrameBadge::NoData;
	}

	if (CurrentDisplay.bStale)
	{
		// RANKED ABOVE Interpolated ON PURPOSE (see the header). The displayed
		// physical time is not the requested one, which is the more misleading of
		// the two facts, so it is the one the badge leads with. Callers that need
		// both ask IsShowingInterpolatedData() as well.
		return EFlowVizFrameBadge::HeldStale;
	}

	return CurrentDisplay.bInterpolated ? EFlowVizFrameBadge::Interpolated
										: EFlowVizFrameBadge::Exact;
}

bool FFlowVizTimelineViewModel::IsShowingInterpolatedData() const
{
	if (Player == nullptr)
	{
		return false;
	}
	const FFlowVizDisplaySelection& CurrentDisplay = Player->GetDisplay();
	return CurrentDisplay.IsValid() && CurrentDisplay.bInterpolated;
}

bool FFlowVizTimelineViewModel::IsPlaying() const
{
	return Player != nullptr && Player->IsPlaying();
}
