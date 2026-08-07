// Copyright FlowViz contributors. All Rights Reserved.

#include "Flow/FlowVizCutPlane.h"
#include "Flow/FlowVizFieldMask.h"
#include "Flow/FlowVizFieldSampler.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizCutPlaneTest
{
	FString GetSampleManifest()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FlowVizRuntime"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		const FString ProjectDir = FPaths::GetPath(FPaths::GetPath(Plugin->GetBaseDir()));
		return FPaths::Combine(
			ProjectDir, TEXT("Samples"), TEXT("MockCylinderWake.cfdviz"), TEXT("manifest.json"));
	}

	/** Domain 12 x 4 x 1 m; cylinder at (4,2) r=0.45; |U| in [0.929, 13.497]. */
	const FVector DomainSize(12.0, 4.0, 1.0);
	const FVector ZMid(6.0, 2.0, 0.5);
}

/**
 * The cut plane builder (renderer overhaul P3): opaque interpolated plane,
 * hard-edged hole at the solid, scalars normalized for the LUT material.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizCutPlaneTest,
	"FlowViz.Flow.CutPlane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizCutPlaneTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizCutPlaneTest;

	FCFDVizCase Case;
	if (!TestTrue(TEXT("the sample case loads"),
			FCFDVizCase::LoadFromFile(GetSampleManifest(), Case).IsOk()))
	{
		return false;
	}

	FFlowVizFieldSampler Velocity;
	if (!TestTrue(TEXT("the U sampler builds"),
			Velocity.Build(Case, TEXT("U"), 0).IsOk()))
	{
		return false;
	}
	FFlowVizFieldMask Mask;
	Mask.Build(Velocity);

	/* == The z-mid plane: the demo's hero image ============================= */

	FlowVizCutPlane::FCutPlaneRequest Request;
	Request.Origin = ZMid;
	Request.Normal = FVector::ZAxisVector;
	Request.DomainSize = DomainSize;
	Request.Resolution = 96;

	FFlowVizMeshSection Section;
	double RangeMin = 0.0, RangeMax = 0.0;
	if (!TestTrue(TEXT("the z-mid cut plane builds"),
			FlowVizCutPlane::BuildCutPlaneMesh(
				Velocity, Mask, Request, Section, RangeMin, RangeMax)))
	{
		return false;
	}

	TestTrue(TEXT("it has real geometry"), Section.Vertices.Num() > 1000);
	TestEqual(TEXT("one scalar per vertex -- the LUT material's contract"),
		Section.ScalarUVs.Num(), Section.Vertices.Num());
	TestTrue(TEXT("whole triangles"),
		Section.Indices.Num() >= 3 && Section.Indices.Num() % 3 == 0);

	// The auto range must be the plane's own |U| spread: inside the declared
	// global bound and genuinely wide (the wake varies severalfold at z-mid).
	TestTrue(TEXT("the echoed range is non-degenerate"), RangeMax > RangeMin);
	TestTrue(TEXT("and within the field's declared |U| bounds"),
		RangeMin >= 0.9 && RangeMax <= 13.5);

	/* == Unreal space, on the plane ========================================= */
	//
	// Solver z=0.5 with the Y mirror: every vertex sits at Unreal Z=50, x in
	// [0,1200], y in [-400,0].
	{
		bool bAllOnPlane = true, bAllInBox = true;
		for (const FVector& Vertex : Section.Vertices)
		{
			bAllOnPlane &= FMath::IsNearlyEqual(Vertex.Z, 50.0, 1e-3);
			bAllInBox &= Vertex.X > -1e-3 && Vertex.X < 1200.0 + 1e-3
				&& Vertex.Y > -400.0 - 1e-3 && Vertex.Y < 1e-3;
		}
		TestTrue(TEXT("every vertex lies on the Unreal-space plane z=50"), bAllOnPlane);
		TestTrue(TEXT("and inside the mirrored domain box"), bAllInBox);
	}

	/* == Scalars are NORMALIZED against the echoed range ==================== */
	{
		float Lowest = 1.0f, Highest = 0.0f;
		for (const float Scalar : Section.ScalarUVs)
		{
			Lowest = FMath::Min(Lowest, Scalar);
			Highest = FMath::Max(Highest, Scalar);
		}
		// An auto range computed FROM these samples must be saturated at both
		// ends -- anything else means the normalization used a different range
		// than the one echoed back (the legend-vs-mesh drift this API bans).
		TestTrue(TEXT("the lowest scalar sits at 0 -- normalized against the echoed range"),
			Lowest < 1e-3f);
		TestTrue(TEXT("the highest sits at 1"), Highest > 1.0f - 1e-3f);
	}

	/* == The hole where the cylinder is ===================================== */
	//
	// HARD-EDGED, BY OMISSION: no vertex may sit inside the cylinder, and the
	// hole is real -- vertex count strictly below a full grid's.
	{
		// Unreal-space cylinder axis: solver (4,2) -> (400,-200), r=45.
		int32 InsideCylinder = 0;
		for (const FVector& Vertex : Section.Vertices)
		{
			const double DX = Vertex.X - 400.0;
			const double DY = Vertex.Y - (-200.0);
			if (FMath::Sqrt(DX * DX + DY * DY) < 45.0)
			{
				++InsideCylinder;
			}
		}
		TestEqual(TEXT("no vertex lies inside the cylinder -- the plane has a hole, "
					   "not a smear"),
			InsideCylinder, 0);

		// CONTROL that the hole exists at all: a full 96-wide grid over a 3:1
		// plane would have 96 * 33 vertices if nothing were dropped.
		TestTrue(TEXT("CONTROL: fewer vertices than an intact grid -- something was "
					  "actually dropped"),
			Section.Vertices.Num() < 96 * 33);
	}

	/* == A manual range is used verbatim ==================================== */
	{
		FlowVizCutPlane::FCutPlaneRequest Manual = Request;
		Manual.RangeMin = 0.0;
		Manual.RangeMax = 100.0;   // |U| tops out ~13.5, so scalars stay low.

		FFlowVizMeshSection ManualSection;
		double EchoMin = 0.0, EchoMax = 0.0;
		if (TestTrue(TEXT("the manual-range build succeeds"),
				FlowVizCutPlane::BuildCutPlaneMesh(
					Velocity, Mask, Manual, ManualSection, EchoMin, EchoMax)))
		{
			TestEqual(TEXT("the echo is the manual min"), EchoMin, 0.0);
			TestEqual(TEXT("the echo is the manual max"), EchoMax, 100.0);
			float Highest = 0.0f;
			for (const float Scalar : ManualSection.ScalarUVs)
			{
				Highest = FMath::Max(Highest, Scalar);
			}
			TestTrue(TEXT("scalars honour the manual range -- nothing near 1"),
				Highest < 0.2f);
		}
	}

	/* == Refusals =========================================================== */
	{
		FFlowVizMeshSection Empty;
		double A = 0.0, B = 0.0;

		FlowVizCutPlane::FCutPlaneRequest Outside = Request;
		Outside.Origin = FVector(6.0, 2.0, 50.0);   // far above the domain
		TestFalse(TEXT("a plane outside the domain refuses -- empty, not a zero-area "
					   "artifact"),
			FlowVizCutPlane::BuildCutPlaneMesh(Velocity, Mask, Outside, Empty, A, B));

		FlowVizCutPlane::FCutPlaneRequest NoDomain = Request;
		NoDomain.DomainSize = FVector::ZeroVector;
		TestFalse(TEXT("a degenerate domain refuses"),
			FlowVizCutPlane::BuildCutPlaneMesh(Velocity, Mask, NoDomain, Empty, A, B));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
