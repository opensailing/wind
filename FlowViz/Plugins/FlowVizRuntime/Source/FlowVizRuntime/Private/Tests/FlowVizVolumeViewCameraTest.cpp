// Copyright FlowViz contributors. All Rights Reserved.

#include "../Render/FlowVizVolumeRayMarchDispatcher.h"

#include "Misc/AutomationTest.h"
#include "Render/FlowVizVolumeRayMarchShader.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * DOES A REAL SCENE CAMERA REACH THE MARCHER AS THE SAME RAY?
 *
 * The ray-marcher works entirely in the volume's LOCAL space - the cbuffer rows
 * are named RayCameraOrigin/Forward/Right/Up and the .usf builds its ray from
 * them without ever seeing a world transform. World placement stays in the
 * proxy's double-precision LocalToWorld and never reaches the GPU, because a
 * kilometre-scale case would lose metres to float32.
 *
 * So something has to carry an FSceneView across that boundary, and the way it
 * gets that wrong is specific and silent: NORMALISING THE TRANSFORMED BASIS.
 * Every transformed basis vector "looks right" normalised - unit length, correct
 * direction, orthogonal-ish - and under a uniform scale the rendered image is
 * identical. Under a NON-UNIFORM scale it is not: the local-space basis is no
 * longer orthonormal, and re-normalising each axis independently rescales the
 * screen offsets by different amounts per axis. The result is a volume rendered
 * with a sheared field of view - a plausible, wrong image, which is the failure
 * class VISUAL_QA cares about most.
 *
 * Anisotropy is not a corner case here. ADR 002 pins the default mock domain at
 * 12 m x 4 m x 1 m over 128 x 64 x 24 cells precisely so that "any code assuming
 * cubic voxels is wrong on the primary dataset", and a volume actor scaled
 * non-uniformly in a map produces exactly the same asymmetry in LocalToWorld.
 *
 * THESE ARE DIFFERENTIAL TESTS, NOT SHAPE CHECKS. Each one builds the ray in
 * WORLD space, maps the resulting point through WorldToLocal, and requires the
 * shader's own local-space formula to land on the same point. A shape check
 * ("is it a unit vector", "is it finite") passes for the normalising
 * implementation too, so it would not be a check at all. Asserting the point
 * fails if and only if the mapping is wrong.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizVolumeViewCameraTest,
	"FlowViz.Render.ViewCamera",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

namespace FlowVizViewCameraTestFixture
{
	/**
	 * The .usf's perspective ray, rebuilt from the cbuffer rows.
	 *
	 * A COPY OF THE SHADER EXPRESSION, DELIBERATELY. The shader cannot be called
	 * from C++, so the alternative is asserting on the rows themselves - which
	 * pins an encoding rather than a ray, and would pass for any consistent
	 * mis-encoding. Keep this in step with FlowVizVolumeRayMarch.usf's ray
	 * generation block; it is quoted in the comment there.
	 */
	FVector3f MakePerspectiveRayDirection(
		const FFlowVizVolumeRayMarchParameters& Parameters,
		const FVector2f& Screen)
	{
		return Parameters.RayCameraForward
			+ Parameters.RayCameraRight * (Screen.X * Parameters.TanHalfFov.X)
			+ Parameters.RayCameraUp * (Screen.Y * Parameters.TanHalfFov.Y);
	}

	/** The .usf's orthographic ray origin, rebuilt from the cbuffer rows. */
	FVector3f MakeOrthographicRayOrigin(
		const FFlowVizVolumeRayMarchParameters& Parameters,
		const FVector2f& Screen)
	{
		return Parameters.RayCameraOrigin
			+ Parameters.RayCameraRight * (Screen.X * Parameters.OrthoHalfExtent.X)
			+ Parameters.RayCameraUp * (Screen.Y * Parameters.OrthoHalfExtent.Y);
	}

	/**
	 * A transform with NON-UNIFORM scale, a rotation and a translation.
	 *
	 * Every one of those three matters. Without the non-uniform scale the
	 * normalising implementation is exactly right and the test cannot fail;
	 * without the rotation an axis-aligned scale leaves the basis vectors
	 * parallel to their world counterparts, which hides a swapped axis; without
	 * the translation a position error is indistinguishable from a direction
	 * error.
	 */
	FMatrix MakeAnisotropicLocalToWorld()
	{
		return FScaleMatrix(FVector(3.0, 0.5, 1.25))
			* FRotationMatrix(FRotator(20.0, -35.0, 10.0))
			* FTranslationMatrix(FVector(1200.0, -450.0, 90.0));
	}

	/** A camera basis that is orthonormal in WORLD space, as a real view's always is. */
	FFlowVizVolumeViewCamera MakeWorldCamera(bool bPerspective)
	{
		FFlowVizVolumeViewCamera Camera;
		Camera.WorldOrigin = FVector(-320.0, 175.0, 260.0);

		const FRotator Orientation(-12.0, 47.0, 0.0);
		const FRotationMatrix Basis(Orientation);
		Camera.WorldForward = Basis.GetUnitAxis(EAxis::X);
		Camera.WorldRight = Basis.GetUnitAxis(EAxis::Y);
		Camera.WorldUp = Basis.GetUnitAxis(EAxis::Z);

		// Deliberately different per axis. Equal values would let a swapped
		// TanHalfFov.X/Y pass.
		Camera.HalfExtentOrTanFov = FVector2f(0.9f, 0.55f);
		Camera.bPerspective = bPerspective;
		return Camera;
	}
}

bool FFlowVizVolumeViewCameraTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizViewCameraTestFixture;

	// Tolerant of float32 rounding on values in the hundreds of centimetres,
	// and nowhere near loose enough to absorb a normalised basis - the errors
	// that produces are whole percent of the extent, not fractions of a unit.
	constexpr float Tolerance = 0.05f;

	/* -- Identity: local IS world ----------------------------------------- */
	{
		const FFlowVizVolumeViewCamera Camera = MakeWorldCamera(/*bPerspective=*/true);

		FFlowVizVolumeRayMarchParameters Params;
		FlowVizRayMarch::FillDefaults(Params);
		FlowVizVolumeRayMarchProduction::SetViewCamera(
			Params, Camera, FMatrix::Identity, FIntPoint(320, 200));

		TestEqual(TEXT("an identity placement leaves the camera origin in world coordinates"),
			FVector(Params.RayCameraOrigin), Camera.WorldOrigin, 0.01);
		TestTrue(TEXT("an identity placement leaves the forward axis untouched"),
			FVector(Params.RayCameraForward).Equals(Camera.WorldForward, 0.001));

		TestEqual(TEXT("the output size is the marched resolution, X"), Params.OutputSizeX, 320u);
		TestEqual(TEXT("the output size is the marched resolution, Y"), Params.OutputSizeY, 200u);
		TestEqual(TEXT("a perspective view is not marked orthographic"), Params.bOrthographic, 0u);
	}

	/* -- Perspective under a non-uniform placement ------------------------- */
	{
		const FMatrix LocalToWorld = MakeAnisotropicLocalToWorld();
		const FMatrix WorldToLocal = LocalToWorld.Inverse();
		const FFlowVizVolumeViewCamera Camera = MakeWorldCamera(/*bPerspective=*/true);

		FFlowVizVolumeRayMarchParameters Params;
		FlowVizRayMarch::FillDefaults(Params);
		FlowVizVolumeRayMarchProduction::SetViewCamera(
			Params, Camera, LocalToWorld, FIntPoint(64, 64));

		// A corner of the frustum, where a per-axis rescale shows up largest.
		// The centre pixel would pass for the normalising implementation too,
		// because there the screen offsets are zero and only the forward axis
		// contributes - so testing only the centre would be no test at all.
		const FVector2f Screen(-0.75f, 0.6f);

		const FVector WorldDirection = Camera.WorldForward
			+ Camera.WorldRight * (Screen.X * Camera.HalfExtentOrTanFov.X)
			+ Camera.WorldUp * (Screen.Y * Camera.HalfExtentOrTanFov.Y);

		// Two distances, because a direction that is wrong only in LENGTH still
		// describes the same ray - and one that is wrong in DIRECTION diverges
		// with distance. Checking a single T conflates them.
		for (const double Distance : {150.0, 900.0})
		{
			const FVector WorldPoint = Camera.WorldOrigin + WorldDirection * Distance;
			const FVector ExpectedLocal = WorldToLocal.TransformPosition(WorldPoint);

			const FVector3f MarchedLocal = Params.RayCameraOrigin
				+ MakePerspectiveRayDirection(Params, Screen) * static_cast<float>(Distance);

			TestTrue(
				FString::Printf(
					TEXT("the marcher's local-space ray reaches the same point as the world-space ray ")
					TEXT("at t=%.0f; a normalised basis rescales the screen offsets per axis under a ")
					TEXT("non-uniform placement and shears the field of view"),
					Distance),
				FVector(MarchedLocal).Equals(ExpectedLocal, Tolerance));
		}
	}

	/* -- Orthographic under the same placement ----------------------------- */
	{
		const FMatrix LocalToWorld = MakeAnisotropicLocalToWorld();
		const FMatrix WorldToLocal = LocalToWorld.Inverse();
		const FFlowVizVolumeViewCamera Camera = MakeWorldCamera(/*bPerspective=*/false);

		FFlowVizVolumeRayMarchParameters Params;
		FlowVizRayMarch::FillDefaults(Params);
		FlowVizVolumeRayMarchProduction::SetViewCamera(
			Params, Camera, LocalToWorld, FIntPoint(64, 64));

		TestEqual(TEXT("an orthographic view is marked orthographic"), Params.bOrthographic, 1u);

		const FVector2f Screen(0.8f, -0.45f);

		// Orthographic moves the ORIGIN across the frustum face rather than
		// fanning the direction, so this exercises the other half of the .usf's
		// branch. The same normalisation bug corrupts it the same way.
		const FVector WorldOrigin = Camera.WorldOrigin
			+ Camera.WorldRight * (Screen.X * Camera.HalfExtentOrTanFov.X)
			+ Camera.WorldUp * (Screen.Y * Camera.HalfExtentOrTanFov.Y);

		const FVector ExpectedLocal = WorldToLocal.TransformPosition(WorldOrigin);
		const FVector3f MarchedLocal = MakeOrthographicRayOrigin(Params, Screen);

		TestTrue(
			TEXT("the marcher's orthographic ray starts where the world-space ray starts"),
			FVector(MarchedLocal).Equals(ExpectedLocal, Tolerance));
	}

	/* -- The mirrored placement -------------------------------------------- */
	{
		// A NEGATIVE SCALE IS NOT HYPOTHETICAL. FFlowVizVolumeSceneProxy's render
		// matrix is GetVolumeLocalToUnrealMatrix() * component transform, and the
		// first factor exists precisely because the solver's handedness may not
		// be Unreal's - so a mirror is the ordinary case, not an abuse. A
		// normalised basis loses the flip entirely (unit length discards sign
		// information carried by the transform), which renders the volume
		// inside-out with no other symptom.
		const FMatrix LocalToWorld = FScaleMatrix(FVector(1.0, -1.0, 1.0))
			* FRotationMatrix(FRotator(0.0, 15.0, 0.0));
		const FMatrix WorldToLocal = LocalToWorld.Inverse();
		const FFlowVizVolumeViewCamera Camera = MakeWorldCamera(/*bPerspective=*/true);

		FFlowVizVolumeRayMarchParameters Params;
		FlowVizRayMarch::FillDefaults(Params);
		FlowVizVolumeRayMarchProduction::SetViewCamera(
			Params, Camera, LocalToWorld, FIntPoint(32, 32));

		const FVector2f Screen(0.5f, 0.5f);
		const FVector WorldDirection = Camera.WorldForward
			+ Camera.WorldRight * (Screen.X * Camera.HalfExtentOrTanFov.X)
			+ Camera.WorldUp * (Screen.Y * Camera.HalfExtentOrTanFov.Y);

		constexpr double Distance = 400.0;
		const FVector ExpectedLocal =
			WorldToLocal.TransformPosition(Camera.WorldOrigin + WorldDirection * Distance);
		const FVector3f MarchedLocal = Params.RayCameraOrigin
			+ MakePerspectiveRayDirection(Params, Screen) * static_cast<float>(Distance);

		TestTrue(
			TEXT("a mirrored placement still maps the ray to the same local point"),
			FVector(MarchedLocal).Equals(ExpectedLocal, Tolerance));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
