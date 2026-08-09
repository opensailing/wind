// Copyright FlowViz contributors. All Rights Reserved.

/**
 * The adapter from FFlowVizCasePlayer onto IFlowVizVolumeFrameSource.
 *
 * Kept in its own translation unit because it is the only file in Playback/ that
 * includes Scene/. The player itself must stay buildable and testable without
 * the component - that separation is what lets every frame-selection test in
 * FlowVizCasePlayerTest.cpp run with no scene, no component and no GPU.
 *
 * There is exactly one piece of logic here, the double -> float narrowing, and
 * it is written out rather than folded into an assignment because the failure it
 * prevents is invisible: both the wrong and the right version compile, run, and
 * render something plausible. Only the on-screen "Interpolated" indicator
 * differs, and it differs by lying.
 */

#include "Playback/FlowVizCasePlayer.h"

#include "Scene/FlowVizVolumeComponent.h"

namespace FlowVizPlayback
{
	FFlowVizVolumeFrameSelection ToVolumeFrameSelection(const FFlowVizDisplaySelection& Display)
	{
		FFlowVizVolumeFrameSelection Out;

		// Nothing complete is resident. Draw nothing rather than frame 0: frame 0
		// is a real measurement and showing it here would be inventing data.
		if (!Display.IsValid())
		{
			return Out;
		}

		Out.FrameA = Display.FrameA;

		// A single-frame display is already collapsed. Note this also covers
		// FrameB == FrameA, which the component's IsInterpolated() treats as not
		// interpolated - blending a frame with itself synthesizes nothing.
		if (Display.FrameB == INDEX_NONE || Display.FrameB == Display.FrameA)
		{
			Out.FrameB = Out.FrameA;
			Out.Alpha = 0.0f;
			return Out;
		}

		// NARROW FIRST, THEN DECIDE. Deciding on the double and then narrowing is
		// the bug: alpha = 1 - 1e-11 is a genuine blend in double, so a
		// double-side test says "interpolated", and the float it produces is
		// exactly 1.0f, so the component says "not interpolated". The two answers
		// must be derived from the SAME value, and the value the shader will use
		// is the float one.
		const float NarrowedAlpha = (float)Display.Alpha;

		if (NarrowedAlpha <= 0.0f)
		{
			// Alpha vanished under narrowing: the blend is frame A exactly.
			Out.FrameB = Out.FrameA;
			Out.Alpha = 0.0f;
			return Out;
		}

		if (NarrowedAlpha >= 1.0f)
		{
			// Alpha saturated under narrowing: the blend is frame B exactly, so
			// frame B becomes the displayed frame and the pair collapses onto it.
			Out.FrameA = Display.FrameB;
			Out.FrameB = Out.FrameA;
			Out.Alpha = 0.0f;
			return Out;
		}

		Out.FrameB = Display.FrameB;
		Out.Alpha = NarrowedAlpha;
		return Out;
	}

	// NAMED namespace, not anonymous: FlowVizRuntime is a unity build, so the
	// anonymous namespaces of every .cpp in the blob merge and same-named types
	// collide in a file that did nothing wrong.
	namespace FlowVizCaseSeamLocal
	{
		/**
		 * File-local by convention: this type is only ever handed out behind the
		 * interface. It is defined in the .cpp so the player header does not have
		 * to include the component header.
		 */
		class FCasePlayerFrameSource final : public IFlowVizVolumeFrameSource
		{
		public:
			explicit FCasePlayerFrameSource(const FFlowVizCasePlayer& InPlayer)
				: Player(InPlayer)
			{
			}

			virtual FFlowVizVolumeFrameSelection GetFrameSelection() const override
			{
				// GetDisplay() is what is COMPLETE AND RESIDENT, not what the
				// playhead wants. Reading the desired selection here would put a
				// half-loaded frame on screen during a scrub, which is the
				// requirement this whole layer exists to satisfy.
				return ToVolumeFrameSelection(Player.GetDisplay());
			}

		private:
			const FFlowVizCasePlayer& Player;
		};
	}

	TSharedRef<IFlowVizVolumeFrameSource> MakeFrameSource(const FFlowVizCasePlayer& Player)
	{
		return MakeShared<FlowVizCaseSeamLocal::FCasePlayerFrameSource>(Player);
	}
}
