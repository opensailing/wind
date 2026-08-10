// Copyright FlowViz contributors. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Scene/FlowVizBoundaryMesh.h"
#include "Scene/FlowVizMeshPayload.h"
#include "Scene/FlowVizSurfaceMeshComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizSurfaceMeshComponentTest
{
	/** Two sections: a visible triangle and a hidden quad, with scalars on the second. */
	FFlowVizMeshPayload MakePayload()
	{
		FFlowVizMeshPayload Payload;

		FFlowVizMeshSection& Wall = Payload.Sections.AddDefaulted_GetRef();
		Wall.SectionId = 4;
		Wall.Name = TEXT("cylinderWall");
		Wall.bDefaultVisible = true;
		Wall.Vertices = { FVector(0, 0, 0), FVector(100, 0, 0), FVector(0, 100, 0) };
		Wall.Indices = { 0, 1, 2 };
		Wall.Normals = { FVector::ZAxisVector, FVector::ZAxisVector, FVector::ZAxisVector };

		FFlowVizMeshSection& Inlet = Payload.Sections.AddDefaulted_GetRef();
		Inlet.SectionId = 1;
		Inlet.Name = TEXT("inlet");
		Inlet.bDefaultVisible = false;
		Inlet.Vertices = {
			FVector(0, 0, 0), FVector(0, -100, 0), FVector(0, -100, 100), FVector(0, 0, 100)
		};
		Inlet.Indices = { 0, 1, 2, 0, 2, 3 };
		Inlet.Normals = {
			FVector::XAxisVector, FVector::XAxisVector,
			FVector::XAxisVector, FVector::XAxisVector
		};
		Inlet.ScalarUVs = { 0.0f, 0.25f, 0.75f, 1.0f };
		return Payload;
	}
}

/**
 * The surface mesh applier (renderer overhaul P2): payloads in, PMC
 * sections out, per-section visibility honoured.
 *
 * The GEOMETRY is tested where it is built (FlowVizBoundaryMeshTest and,
 * later, the cut-plane/MC tests); what this pins is the HOP: section
 * count/identity, defaultVisible actually hiding, scalars landing in UV0,
 * and clearing being total.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizSurfaceMeshComponentTest,
	"FlowViz.Scene.SurfaceMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizSurfaceMeshComponentTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizSurfaceMeshComponentTest;

	UCFDVizSurfaceMeshComponent* Component = NewObject<UCFDVizSurfaceMeshComponent>();
	Component->AddToRoot();

	TestEqual(TEXT("CONTROL: a fresh component has no sections"),
		Component->GetSectionCount(), 0);
	TestFalse(TEXT("the inherited procedural-mesh default leaves surfaces unticked"),
		Component->PrimaryComponentTick.bCanEverTick);

	/* == Applying ============================================================ */

	Component->SetSurfaceData(MakePayload());

	if (!TestEqual(TEXT("one mesh section per payload section"),
			Component->GetSectionCount(), 2))
	{
		Component->RemoveFromRoot();
		return false;
	}

	// defaultVisible is honoured AT APPLY: an inlet that arrives hidden is the
	// manifest's stated intent, not a state the user must discover and fix.
	TestTrue(TEXT("the wall section is visible"), Component->IsSectionVisible(0));
	TestFalse(TEXT("the inlet section arrives HIDDEN -- defaultVisible is honoured"),
		Component->IsSectionVisible(1));

	// Identity survives the hop, so UI toggles can address sections by the
	// manifest's patch id rather than by fragile ordinal.
	TestEqual(TEXT("section 0 keeps its patch id"), Component->GetSectionId(0), 4u);
	TestEqual(TEXT("section 1 keeps its patch id"), Component->GetSectionId(1), 1u);
	TestEqual(TEXT("and its name"), Component->GetSectionName(1), FString(TEXT("inlet")));

	/* == Geometry and scalars reach the PMC ================================== */
	{
		const FProcMeshSection* Wall = Component->GetProcMeshSection(0);
		if (TestNotNull(TEXT("the wall's PMC section exists"), Wall))
		{
			TestEqual(TEXT("with its vertices"), Wall->ProcVertexBuffer.Num(), 3);
			TestEqual(TEXT("and its indices"), Wall->ProcIndexBuffer.Num(), 3);
		}

		const FProcMeshSection* Inlet = Component->GetProcMeshSection(1);
		if (TestNotNull(TEXT("the inlet's PMC section exists"), Inlet))
		{
			TestEqual(TEXT("quad vertices survive"), Inlet->ProcVertexBuffer.Num(), 4);
			if (Inlet->ProcVertexBuffer.Num() == 4)
			{
				// The scalar rides UV0.x -- float precision, so the colormap
				// material can sample the LUT without 8-bit posterization.
				TestEqual(TEXT("scalar 0 lands in UV0.x"),
					static_cast<float>(Inlet->ProcVertexBuffer[0].UV0.X), 0.0f);
				TestEqual(TEXT("scalar 3 lands in UV0.x"),
					static_cast<float>(Inlet->ProcVertexBuffer[3].UV0.X), 1.0f);
			}
		}
	}

	/* == Toggling and clearing ============================================== */

	Component->SetSectionVisible(1, true);
	TestTrue(TEXT("a hidden section can be shown"), Component->IsSectionVisible(1));

	Component->ClearSurfaceData();
	TestEqual(TEXT("clearing removes every section"), Component->GetSectionCount(), 0);

	/* == Re-apply after clear (the per-frame path P4 will lean on) ========== */

	Component->SetSurfaceData(MakePayload());
	TestEqual(TEXT("a second apply rebuilds cleanly"), Component->GetSectionCount(), 2);

	Component->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
