// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The orbit camera's state and transitions, as pure values (#79 / Milestone D).
 *
 * WHY A VALUE TYPE AND NOT A PAWN. Every rule here -- orbit preserves
 * distance, pan moves the focus in the view plane, zoom clamps, pitch stops
 * short of the poles -- is testable with no world, no viewport and no input
 * stack, which is the only kind of test this environment can run headless.
 * The engine-facing binding (a pawn or viewport client calling these from
 * input events and writing GetLocation/GetRotation to a camera) is a thin
 * consumer; the maths that can be silently wrong lives here.
 *
 * CONVENTIONS. Focus is the point orbited, in WORLD units. Yaw and pitch are
 * degrees, Unreal's own convention (pitch positive looking up). Distance is
 * from the camera to the focus, always positive. The camera looks AT the
 * focus by construction, so GetRotation is derived, never stored -- a stored
 * rotation is a second copy of state that drifts.
 *
 * THREADING. This value owns no UObject state and calls no engine service, but
 * it is mutable and has no internal synchronization. One instance belongs to
 * one thread at a time; transferring a copied bookmark between threads is safe.
 *
 * THE POLE CLAMP. Pitch is clamped short of +/-90 by PitchLimit: at the pole
 * the view direction and the up vector are parallel, the yaw basis vanishes,
 * and the next orbit delta spins the camera around its own axis -- which
 * reads as a broken mouse rather than gimbal lock.
 */
struct FLOWVIZRUNTIME_API FFlowVizOrbitCamera
{
	/** Degrees short of the poles that pitch stops at. */
	static constexpr double PitchLimit = 89.0;

	/** Zoom bounds, world units. Min > 0: distance 0 puts the focus behind the near plane forever. */
	static constexpr double MinDistance = 1.0;
	static constexpr double MaxDistance = 1.0e7;

	/** Multiplicative zoom step per wheel notch. */
	static constexpr double ZoomStepFactor = 0.9;

	/**
	 * Frame a domain: focus at its centre, distance chosen so the whole box is
	 * comfortably inside a ~60 degree field of view, from a three-quarter view
	 * (yaw -45, pitch -30) -- the orientation every CFD screenshot defaults to,
	 * with the flow axis running left to right.
	 *
	 * A non-positive or non-finite extent is REFUSED (return false, state
	 * untouched): framing a zero box would put the camera at MinDistance from
	 * a point, which renders something and means nothing.
	 */
	bool FrameBox(const FVector& WorldCenter, const FVector& WorldExtent);

	/** Orbit by screen-space deltas, degrees. Distance and focus are unchanged BY CONSTRUCTION. */
	void Orbit(double DeltaYawDegrees, double DeltaPitchDegrees);

	/**
	 * Pan in the VIEW plane: right and up as the user sees them, world units.
	 * Distance and orientation are unchanged; only the focus moves.
	 */
	void Pan(double DeltaRight, double DeltaUp);

	/**
	 * Zoom by wheel notches. Positive zooms IN (distance shrinks). Multiplicative,
	 * so ten notches from far away and ten from close both feel proportional,
	 * and clamped to [MinDistance, MaxDistance].
	 */
	void Zoom(double WheelNotches);

	/** Camera position: focus backed off along the view direction. */
	FVector GetLocation() const;

	/** Looking at the focus. Derived, never stored. */
	FRotator GetRotation() const;

	const FVector& GetFocus() const { return Focus; }
	double GetDistance() const { return Distance; }
	double GetYaw() const { return YawDegrees; }
	double GetPitch() const { return PitchDegrees; }

	/**
	 * Adopt a location/rotation pair, e.g. from a restored session. The pair is
	 * decomposed against the CURRENT focus: the focus is preserved and
	 * distance/yaw/pitch are derived from the location's offset. A location
	 * coincident with the focus is refused (no direction to derive).
	 */
	bool SetFromLocationAndFocus(const FVector& WorldLocation, const FVector& WorldFocus);

	/* --- View presets (renderer overhaul P9) ------------------------------- */

	/**
	 * The standard axis-aligned views, the genre's camera vocabulary: +X looks
	 * DOWNSTREAM (from upstream), -X looks upstream, +/-Y across, Top looks
	 * straight down (pitch clamped just off the pole so the orbit basis stays
	 * defined), ThreeQuarter is FrameBox's default orientation. Focus and
	 * distance are unchanged -- a preset changes WHERE YOU LOOK FROM, not what
	 * you look at.
	 */
	enum class EViewPreset : uint8
	{
		DownstreamX,
		UpstreamX,
		SideY,
		OtherSideY,
		Top,
		ThreeQuarter,
	};
	void SetViewPreset(EViewPreset Preset);

	/**
	 * Bookmarks: the whole pose (focus, distance, yaw, pitch) as a value.
	 * Save/restore round-trips exactly -- the reproducible-shot primitive the
	 * capture library and MRQ share.
	 */
	struct FBookmark
	{
		FVector Focus = FVector::ZeroVector;
		double Distance = 100.0;
		double YawDegrees = 0.0;
		double PitchDegrees = 0.0;
	};
	FBookmark SaveBookmark() const;
	void RestoreBookmark(const FBookmark& Bookmark);

private:
	FVector Focus = FVector::ZeroVector;
	double Distance = 100.0;
	double YawDegrees = -45.0;
	double PitchDegrees = -30.0;
};
