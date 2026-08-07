// Copyright FlowViz contributors. All Rights Reserved.

#include "Scene/FlowVizOrbitCamera.h"

namespace FlowVizOrbitCameraLocal
{
	/**
	 * The unit vector from the focus TOWARD the camera for a yaw/pitch pair.
	 *
	 * Unreal's convention throughout: yaw rotates about +Z, pitch positive
	 * looks up. The camera VIEW direction for (yaw, pitch) is
	 * FRotator(pitch, yaw, 0).Vector(); the offset from the focus is its
	 * negation. Using the engine's own FRotator::Vector rather than
	 * hand-written trig means GetRotation and GetLocation cannot disagree
	 * about the convention -- one source of truth, used in both directions.
	 */
	FVector OffsetDirection(double YawDegrees, double PitchDegrees)
	{
		return -FRotator(PitchDegrees, YawDegrees, 0.0).Vector();
	}

	bool IsFiniteVector(const FVector& V)
	{
		return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z);
	}
}

bool FFlowVizOrbitCamera::FrameBox(const FVector& WorldCenter, const FVector& WorldExtent)
{
	using namespace FlowVizOrbitCameraLocal;

	if (!IsFiniteVector(WorldCenter) || !IsFiniteVector(WorldExtent)
		|| WorldExtent.GetMax() <= 0.0)
	{
		return false;
	}

	Focus = WorldCenter;
	YawDegrees = -45.0;
	PitchDegrees = -30.0;

	// The bounding sphere's radius over tan(half-fov) puts the sphere exactly
	// at the frustum edge for a symmetric ~60 degree fov; 1.2 backs off so the
	// box has margin and near-plane clipping cannot shave a corner.
	const double Radius = WorldExtent.Size() * 0.5;
	constexpr double HalfFovRadians = PI / 6.0;  // 30 degrees
	Distance = FMath::Clamp(
		Radius / FMath::Tan(HalfFovRadians) * 1.2, MinDistance, MaxDistance);
	return true;
}

void FFlowVizOrbitCamera::Orbit(double DeltaYawDegrees, double DeltaPitchDegrees)
{
	if (!FMath::IsFinite(DeltaYawDegrees) || !FMath::IsFinite(DeltaPitchDegrees))
	{
		return;
	}
	// Yaw wraps; letting it grow unbounded loses precision after enough spins.
	YawDegrees = FMath::Fmod(YawDegrees + DeltaYawDegrees, 360.0);
	PitchDegrees = FMath::Clamp(PitchDegrees + DeltaPitchDegrees, -PitchLimit, PitchLimit);
}

void FFlowVizOrbitCamera::Pan(double DeltaRight, double DeltaUp)
{
	if (!FMath::IsFinite(DeltaRight) || !FMath::IsFinite(DeltaUp))
	{
		return;
	}
	// The view basis, from the same rotation the camera renders with -- so
	// "right" on screen and "right" here cannot drift apart.
	const FRotator View = GetRotation();
	const FVector Right = FRotationMatrix(View).GetScaledAxis(EAxis::Y);
	const FVector Up = FRotationMatrix(View).GetScaledAxis(EAxis::Z);
	Focus += Right * DeltaRight + Up * DeltaUp;
}

void FFlowVizOrbitCamera::Zoom(double WheelNotches)
{
	if (!FMath::IsFinite(WheelNotches))
	{
		return;
	}
	// Positive notches zoom IN: factor < 1 per notch. Pow keeps a 3-notch
	// gesture identical to three 1-notch gestures.
	Distance = FMath::Clamp(
		Distance * FMath::Pow(ZoomStepFactor, WheelNotches), MinDistance, MaxDistance);
}

FVector FFlowVizOrbitCamera::GetLocation() const
{
	using namespace FlowVizOrbitCameraLocal;
	return Focus + OffsetDirection(YawDegrees, PitchDegrees) * Distance;
}

FRotator FFlowVizOrbitCamera::GetRotation() const
{
	return FRotator(PitchDegrees, YawDegrees, 0.0);
}

bool FFlowVizOrbitCamera::SetFromLocationAndFocus(
	const FVector& WorldLocation, const FVector& WorldFocus)
{
	using namespace FlowVizOrbitCameraLocal;

	if (!IsFiniteVector(WorldLocation) || !IsFiniteVector(WorldFocus))
	{
		return false;
	}
	const FVector Offset = WorldLocation - WorldFocus;
	const double NewDistance = Offset.Size();
	if (NewDistance < UE_DOUBLE_SMALL_NUMBER)
	{
		// Coincident: no direction to derive. Refused, keeping the previous
		// pose -- a camera snapped INTO its own focus renders the inside of
		// nothing.
		return false;
	}

	Focus = WorldFocus;
	Distance = FMath::Clamp(NewDistance, MinDistance, MaxDistance);

	// The view direction is -Offset (camera looks at the focus). Derive
	// yaw/pitch from it with the engine's own conversion, then clamp pitch to
	// the same limit Orbit enforces, so a restored pose cannot start AT a pole
	// that interaction cannot reach.
	const FRotator View = (-Offset).Rotation();
	YawDegrees = View.Yaw;
	PitchDegrees = FMath::Clamp(View.Pitch, -PitchLimit, PitchLimit);
	return true;
}
