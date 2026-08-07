// Copyright FlowViz contributors. All Rights Reserved.

#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Scene/FlowVizBoundaryMesh.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizBoundaryMeshTest
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
}

/**
 * Boundary patch geometry (#78 / Milestone D, DoD 13).
 *
 * One section PER PATCH is the load-bearing property: it is what makes
 * per-patch visibility a section toggle rather than a per-triangle filter,
 * and hiding one patch structurally incapable of hiding another's triangles.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizBoundaryMeshTest,
	"FlowViz.Scene.BoundaryPatches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizBoundaryMeshTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizBoundaryMeshTest;

	FCFDVizCase Case;
	if (!TestTrue(TEXT("CONTROL: the sample case loads"),
			FCFDVizCase::LoadFromFile(GetSampleManifest(), Case).IsOk()))
	{
		return false;
	}
	if (!TestEqual(TEXT("CONTROL: the sample declares two meshes"), Case.Meshes.Num(), 2))
	{
		return false;
	}

	/* == The boundaries mesh: four independent patches ======================= */
	const FCFDVizMesh* Boundaries = Case.Meshes.FindByPredicate(
		[](const FCFDVizMesh& Mesh) { return Mesh.Id == FName(TEXT("boundaries")); });
	if (!TestNotNull(TEXT("CONTROL: the boundaries mesh is declared"), Boundaries))
	{
		return false;
	}

	FString MeshPath;
	if (!TestTrue(TEXT("CONTROL: its path resolves"),
			Case.ResolveMeshPath(Boundaries->Id, MeshPath).IsOk()))
	{
		return false;
	}

	TArray<FFlowVizBoundaryPatchGeometry> Patches;
	const FCFDVizResult Built = FlowVizBoundary::BuildPatches(
		MeshPath, *Boundaries, CFDViz::MetersToUnrealCentimeters, Patches);
	if (!TestTrue(FString::Printf(TEXT("the boundary patches build (%s)"), *Built.ToString()),
			Built.IsOk()))
	{
		return false;
	}

	// The manifest declares inlet, outlet, sideWalls, topBottom (section 5.3
	// requires the first three by name). Every declared patch must come back
	// with real triangles.
	if (!TestEqual(FString::Printf(TEXT("every declared patch has geometry (got %d)"),
			Patches.Num()),
			Patches.Num(), Boundaries->Patches.Num()))
	{
		return false;
	}

	int32 TotalTriangles = 0;
	for (const FFlowVizBoundaryPatchGeometry& Patch : Patches)
	{
		TestTrue(FString::Printf(TEXT("patch '%s' has vertices"), *Patch.Name),
			Patch.Vertices.Num() >= 3);
		TestTrue(FString::Printf(TEXT("patch '%s' has whole triangles"), *Patch.Name),
			Patch.Indices.Num() >= 3 && Patch.Indices.Num() % 3 == 0);
		TotalTriangles += Patch.Indices.Num() / 3;

		// Indices index THIS patch's vertex list -- the per-patch welding that
		// makes sections independent. An index into a shared list would pass
		// rendering and break the moment one section is released.
		int32 OutOfRange = 0;
		for (const int32 Index : Patch.Indices)
		{
			if (Index < 0 || Index >= Patch.Vertices.Num())
			{
				++OutOfRange;
			}
		}
		TestEqual(FString::Printf(TEXT("patch '%s' indices stay inside its own vertex list"),
			*Patch.Name), OutOfRange, 0);
	}

	/*
	 * THE DOMAIN BOX CHECK, in Unreal space. The solver domain is 12 x 4 x 1 m;
	 * MakeSolverToUnrealTransform scales metres to centimetres and mirrors Y,
	 * so every boundary vertex must land in x [0,1200], y [-400,0], z [0,100].
	 * A vertex outside that box means the transform, the mirror, or the length
	 * unit went wrong -- each of which renders as a plausible box of the wrong
	 * size or handedness.
	 */
	int32 OutsideBox = 0;
	for (const FFlowVizBoundaryPatchGeometry& Patch : Patches)
	{
		for (const FVector& Vertex : Patch.Vertices)
		{
			if (Vertex.X < -1.0 || Vertex.X > 1201.0 || Vertex.Y < -401.0 || Vertex.Y > 1.0
				|| Vertex.Z < -1.0 || Vertex.Z > 101.0)
			{
				++OutsideBox;
			}
		}
	}
	TestEqual(TEXT("every vertex lands inside the Unreal-space domain box -- outside means "
				   "the unit scale or the Y mirror is wrong"),
		OutsideBox, 0);

	/* == DoD 13: patches are hideable INDEPENDENTLY ========================== */
	/*
	 * The structural fact: each patch owns disjoint geometry, so "hide inlet"
	 * cannot affect outlet's triangles. Asserted by construction -- no vertex
	 * sharing across patches is possible when each has its own arrays -- plus
	 * the distinctness below, which is what a UI's per-patch toggle needs to
	 * be MEANINGFUL (two patches with identical geometry would make the
	 * toggles look broken in a different way).
	 */
	{
		TSet<FString> Names;
		for (const FFlowVizBoundaryPatchGeometry& Patch : Patches)
		{
			Names.Add(Patch.Name);
		}
		TestEqual(TEXT("every patch has a distinct name for its visibility row"),
			Names.Num(), Patches.Num());

		const bool bInletPresent = Names.Contains(TEXT("inlet"));
		const bool bOutletPresent = Names.Contains(TEXT("outlet"));
		TestTrue(TEXT("inlet and outlet are separate patches -- hiding one leaves the "
					  "other's section untouched, which is DoD 13's requirement"),
			bInletPresent && bOutletPresent);
	}

	/* == The obstacle mesh: the cylinder wall ================================ */
	{
		const FCFDVizMesh* Obstacle = Case.Meshes.FindByPredicate(
			[](const FCFDVizMesh& Mesh) { return Mesh.Id == FName(TEXT("obstacle")); });
		if (!TestNotNull(TEXT("CONTROL: the obstacle mesh is declared"), Obstacle))
		{
			return false;
		}
		FString ObstaclePath;
		TestTrue(TEXT("its path resolves"),
			Case.ResolveMeshPath(Obstacle->Id, ObstaclePath).IsOk());

		TArray<FFlowVizBoundaryPatchGeometry> Cylinder;
		TestTrue(TEXT("the cylinder wall builds"),
			FlowVizBoundary::BuildPatches(
				ObstaclePath, *Obstacle, CFDViz::MetersToUnrealCentimeters, Cylinder).IsOk());
		TestEqual(TEXT("as one patch"), Cylinder.Num(), 1);
		if (Cylinder.Num() == 1)
		{
			TestEqual(TEXT("named cylinderWall, the name section 5.3 mandates"),
				Cylinder[0].Name, FString(TEXT("cylinderWall")));

			/*
			 * The cylinder's axis is at solver (4, 2). The COMMITTED sample's
			 * radius is 0.45 m, not MockCaseParameters' 0.3 default -- the
			 * generator scales it so the obstacle spans enough grid cells
			 * (mock.py's own comment: 0.3 m would be 2.4 cells across). The
			 * first draft of this test asserted 30 cm and failed on all 96
			 * vertices; the mesh was right and the expectation was a default
			 * that the sample does not use. Measured from the CVM: 45 cm.
			 */
			int32 OffCylinder = 0;
			for (const FVector& Vertex : Cylinder[0].Vertices)
			{
				const double DX = Vertex.X - 400.0;
				const double DY = Vertex.Y + 200.0;
				const double RadiusCm = FMath::Sqrt(DX * DX + DY * DY);
				if (!FMath::IsNearlyEqual(RadiusCm, 45.0, 1.0))
				{
					++OffCylinder;
				}
			}
			TestEqual(TEXT("every wall vertex sits on the sample's 45 cm cylinder -- the "
						   "geometry is the declared obstacle, not a plausible box"),
				OffCylinder, 0);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
