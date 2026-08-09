// Copyright FlowViz contributors. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Scene/FlowVizOrbitCamera.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizOrbitCameraTest
{
	/** An off-origin centre and three DIFFERENT extents, so an axis mix-up cannot pass. */
	const FVector TestCenter(600.0, 200.0, 50.0);
	const FVector TestExtent(1200.0, 400.0, 100.0);
}

/**
 * The orbit camera's invariants (#79 / Milestone D).
 *
 * Every rule is asserted as the INVARIANT, not as a stored number: orbit
 * preserves distance and focus, pan preserves distance and orientation, zoom
 * preserves focus and orientation. A camera that breaks one of these feels
 * broken long before anyone can say why -- the assertions name the why.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizOrbitCameraTest,
	"FlowViz.Scene.OrbitCamera",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizOrbitCameraTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizOrbitCameraTest;

	FFlowVizOrbitCamera Camera;

	/* == Framing ============================================================= */
	{
		TestTrue(TEXT("framing a real box is accepted"),
			Camera.FrameBox(TestCenter, TestExtent));
		TestTrue(TEXT("the focus is the box centre"), Camera.GetFocus().Equals(TestCenter, 1e-9));

		// The whole box fits: every corner within ~the frustum implies every
		// corner closer to the camera than sqrt(distance^2 + ...) -- asserted
		// via the simpler sufficient condition that the distance exceeds the
		// bounding radius (a camera INSIDE the box fails this first).
		const double Radius = TestExtent.Size() * 0.5;
		TestTrue(TEXT("the camera stands outside the bounding sphere"),
			Camera.GetDistance() > Radius);

		TestTrue(TEXT("a zero-extent box is refused"),
			!Camera.FrameBox(TestCenter, FVector::ZeroVector));
		TestTrue(TEXT("and the refusal left the pose untouched"),
			Camera.GetFocus().Equals(TestCenter, 1e-9));
	}

	/* == Orbit preserves distance and focus ================================== */
	{
		const double DistanceBefore = Camera.GetDistance();
		const FVector FocusBefore = Camera.GetFocus();
		const FVector LocationBefore = Camera.GetLocation();

		Camera.Orbit(37.0, 11.0);

		TestEqual(TEXT("orbit preserves the distance BY CONSTRUCTION"),
			Camera.GetDistance(), DistanceBefore);
		TestTrue(TEXT("orbit preserves the focus"),
			Camera.GetFocus().Equals(FocusBefore, 1e-9));
		TestFalse(TEXT("CONTROL: the camera actually moved -- a no-op orbit would pass "
					   "both invariants while proving nothing"),
			Camera.GetLocation().Equals(LocationBefore, 1e-6));

		// The derived location really is at the stated distance -- the two
		// representations agree, which is what "derived, never stored" buys.
		TestTrue(TEXT("the location sits at the stated distance from the focus"),
			FMath::IsNearlyEqual(
				(Camera.GetLocation() - Camera.GetFocus()).Size(), DistanceBefore, 1e-6));
	}

	/* == The pole clamp ====================================================== */
	{
		Camera.Orbit(0.0, 500.0);
		TestTrue(TEXT("pitch clamps short of the pole -- at 90 the yaw basis vanishes and "
					  "the next orbit reads as a broken mouse"),
			Camera.GetPitch() <= FFlowVizOrbitCamera::PitchLimit);

		Camera.Orbit(0.0, -1000.0);
		TestTrue(TEXT("and short of the south pole"),
			Camera.GetPitch() >= -FFlowVizOrbitCamera::PitchLimit);
	}

	/* == Pan moves the focus in the VIEW plane =============================== */
	{
		Camera.FrameBox(TestCenter, TestExtent);
		const double DistanceBefore = Camera.GetDistance();
		const FRotator RotationBefore = Camera.GetRotation();
		const FVector FocusBefore = Camera.GetFocus();

		Camera.Pan(10.0, 5.0);

		TestEqual(TEXT("pan preserves the distance"), Camera.GetDistance(), DistanceBefore);
		TestTrue(TEXT("pan preserves the orientation"),
			Camera.GetRotation().Equals(RotationBefore, 1e-9));

		const FVector FocusDelta = Camera.GetFocus() - FocusBefore;
		TestTrue(TEXT("the focus moved by the pan magnitude"),
			FMath::IsNearlyEqual(FocusDelta.Size(), FMath::Sqrt(125.0), 1e-6));
		// In the view plane: the delta is perpendicular to the view direction.
		const FVector ViewDirection = RotationBefore.Vector();
		TestTrue(TEXT("and in the view plane -- perpendicular to the view direction, so "
					  "panning never dollies"),
			FMath::Abs(FVector::DotProduct(FocusDelta.GetSafeNormal(), ViewDirection)) < 1e-6);
	}

	/* == Zoom clamps and composes ============================================ */
	{
		Camera.FrameBox(TestCenter, TestExtent);
		const double Before = Camera.GetDistance();

		Camera.Zoom(1.0);
		TestTrue(TEXT("zooming in shrinks the distance"), Camera.GetDistance() < Before);
		TestTrue(TEXT("by the step factor exactly"),
			FMath::IsNearlyEqual(
				Camera.GetDistance(), Before * FFlowVizOrbitCamera::ZoomStepFactor, 1e-9));

		Camera.Zoom(1000.0);
		TestTrue(TEXT("a huge zoom-in clamps at the minimum rather than passing through "
					  "the focus"),
			FMath::IsNearlyEqual(Camera.GetDistance(), FFlowVizOrbitCamera::MinDistance, 1e-9));

		Camera.Zoom(-1.0e6);
		TestTrue(TEXT("and a huge zoom-out clamps at the maximum"),
			FMath::IsNearlyEqual(Camera.GetDistance(), FFlowVizOrbitCamera::MaxDistance, 1e-9));
	}

	/* == The session round trip: location+focus in, same pose out ============ */
	{
		Camera.FrameBox(TestCenter, TestExtent);
		Camera.Orbit(23.0, -17.0);
		Camera.Zoom(2.0);
		const FVector Location = Camera.GetLocation();
		const FRotator Rotation = Camera.GetRotation();

		FFlowVizOrbitCamera Restored;
		TestTrue(TEXT("a location/focus pair is adopted"),
			Restored.SetFromLocationAndFocus(Location, Camera.GetFocus()));
		TestTrue(TEXT("the restored camera stands where the saved one stood"),
			Restored.GetLocation().Equals(Location, 1e-4));
		TestTrue(TEXT("looking the same way"),
			Restored.GetRotation().Equals(Rotation, 1e-4));

		TestFalse(TEXT("a location coincident with the focus is refused -- no direction "
					   "to derive"),
			Restored.SetFromLocationAndFocus(TestCenter, TestCenter));
	}

	return true;
}


/**
 * View presets and bookmarks (renderer overhaul P9): the genre's camera
 * vocabulary as pure pose edits, and the reproducible-shot primitive.
 *
 * NOT "FlowViz.Scene.OrbitCamera.Presets". A dotted sibling of an existing
 * test name turns that exact name into an interior TREE NODE: the test
 * registered at "FlowViz.Scene.OrbitCamera" silently stops enumerating, never
 * runs, and the suite total hides the loss. Undotting the leaf keeps both
 * tests as leaves under FlowViz.Scene.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizCameraPresetsTest,
	"FlowViz.Scene.OrbitCameraPresets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizCameraPresetsTest::RunTest(const FString& Parameters)
{
	FFlowVizOrbitCamera Camera;
	Camera.FrameBox(FVector(600.0, -200.0, 50.0), FVector(600.0, 200.0, 50.0));

	const FVector FocusBefore = Camera.GetFocus();
	const double DistanceBefore = Camera.GetDistance();

	/* == Presets change orientation ONLY ==================================== */
	Camera.SetViewPreset(FFlowVizOrbitCamera::EViewPreset::Top);
	TestEqual(TEXT("Top preset leaves the focus alone"), Camera.GetFocus(), FocusBefore);
	TestEqual(TEXT("and the distance"), Camera.GetDistance(), DistanceBefore);
	TestTrue(TEXT("Top looks steeply down, just off the pole so the orbit basis "
				  "stays defined"),
		Camera.GetPitch() <= -88.9 && Camera.GetPitch() >= -89.1);

	Camera.SetViewPreset(FFlowVizOrbitCamera::EViewPreset::DownstreamX);
	TestEqual(TEXT("Downstream is level"), Camera.GetPitch(), 0.0);
	// Looking +X means the camera sits at focus - X*distance: location.X < focus.X.
	TestTrue(TEXT("the camera sits UPSTREAM of the focus, facing downstream"),
		Camera.GetLocation().X < Camera.GetFocus().X);

	Camera.SetViewPreset(FFlowVizOrbitCamera::EViewPreset::ThreeQuarter);
	TestEqual(TEXT("ThreeQuarter is FrameBox's default orientation, yaw"),
		Camera.GetYaw(), -45.0);
	TestEqual(TEXT("and pitch"), Camera.GetPitch(), -30.0);

	/* == Bookmarks round-trip exactly ======================================= */
	Camera.Orbit(13.0, -7.0);
	Camera.Pan(25.0, -10.0);
	Camera.Zoom(2.0);
	const FFlowVizOrbitCamera::FBookmark Saved = Camera.SaveBookmark();
	const FVector LocationAtSave = Camera.GetLocation();

	Camera.SetViewPreset(FFlowVizOrbitCamera::EViewPreset::Top);
	Camera.Zoom(-5.0);
	Camera.Pan(500.0, 500.0);

	Camera.RestoreBookmark(Saved);
	TestTrue(TEXT("a restored bookmark reproduces the exact camera location -- the "
				  "reproducible-shot primitive"),
		Camera.GetLocation().Equals(LocationAtSave, 1e-9));

	/* == A hostile bookmark is clamped, not obeyed ========================== */
	FFlowVizOrbitCamera::FBookmark Hostile;
	Hostile.Distance = -50.0;
	Hostile.PitchDegrees = 90.0;   // the pole
	Camera.RestoreBookmark(Hostile);
	TestTrue(TEXT("a negative distance clamps positive"), Camera.GetDistance() > 0.0);
	TestTrue(TEXT("a pole pitch clamps off the pole"), Camera.GetPitch() <= 89.0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
