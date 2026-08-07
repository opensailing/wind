// Copyright FlowViz contributors. All Rights Reserved.

#include "Flow/FlowVizFieldMask.h"
#include "Flow/FlowVizFieldSampler.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizFieldMaskTest
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

	/*
	 * Case facts the assertions lean on (measured, not assumed -- the radius
	 * one bit us before): cylinder axis at solver (4, 2), radius 0.45 m;
	 * spacing (12/56, 4/28, 1/6); vorticityMagnitude frame 0 carries NaN in
	 * exactly 132 of 9408 voxels -- the masked cylinder interior.
	 */
	constexpr int32 MaskedVoxelCount = 132;
	const FVector CylinderCenter(4.0, 2.0, 0.5);
	const FVector FreeStream(1.0, 3.0, 0.5);
}

/**
 * The mask policy (renderer overhaul P1): NaN = solid, formalized once.
 *
 * Every downstream consumer -- marching cubes (skip NaN-corner cubes), cut
 * planes (hard-edged mask cells), streamlines (terminate on entry), Q
 * stencils (one-sided at boundaries) -- asks THIS type, so the policy cannot
 * drift per consumer.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizFieldMaskTest,
	"FlowViz.Flow.FieldMask",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizFieldMaskTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizFieldMaskTest;

	FCFDVizCase Case;
	{
		const FCFDVizResult Result = FCFDVizCase::LoadFromFile(GetSampleManifest(), Case);
		if (!TestTrue(TEXT("the sample case loads"), Result.IsOk()))
		{
			return false;
		}
	}

	FFlowVizFieldSampler Scalar;
	if (!TestTrue(TEXT("the scalar sampler builds"),
			Scalar.Build(Case, TEXT("vorticityMagnitude"), 0).IsOk()))
	{
		return false;
	}

	/* == Building and counting ============================================== */

	FFlowVizFieldMask Mask;
	TestFalse(TEXT("CONTROL: a fresh mask is not built"), Mask.IsBuilt());
	Mask.Build(Scalar);
	if (!TestTrue(TEXT("the mask builds from a built sampler"), Mask.IsBuilt()))
	{
		return false;
	}

	// The exact count is the identity control: a mask that flags nothing (or
	// everything) would pass any per-voxel spot check somewhere.
	TestEqual(TEXT("exactly the cylinder's voxels are masked"),
		Mask.CountMasked(), MaskedVoxelCount);

	/* == The mask is the GEOMETRY's, not one field's ======================== */
	{
		// U is a 3-component field over the same grid; a voxel is masked when
		// ANY component is non-finite. The solid is the same solid.
		FFlowVizFieldSampler Vector;
		if (TestTrue(TEXT("the vector sampler builds"),
				Vector.Build(Case, TEXT("U"), 0).IsOk()))
		{
			FFlowVizFieldMask VectorMask;
			VectorMask.Build(Vector);
			TestEqual(TEXT("the vector field's mask has the same count -- the mask "
						   "is the obstacle, not a per-field accident"),
				VectorMask.CountMasked(), MaskedVoxelCount);
		}
	}

	/* == Voxel and position queries ========================================= */

	// The voxel containing the cylinder axis. Spacing is (12/56, 4/28, 1/6);
	// cell i covers [i*dx, (i+1)*dx).
	const FIntVector CylinderVoxel(
		FMath::FloorToInt32(CylinderCenter.X / (12.0 / 56.0)),
		FMath::FloorToInt32(CylinderCenter.Y / (4.0 / 28.0)),
		FMath::FloorToInt32(CylinderCenter.Z / (1.0 / 6.0)));

	TestTrue(TEXT("the cylinder-axis voxel is masked"), Mask.IsVoxelMasked(CylinderVoxel));
	TestFalse(TEXT("CONTROL: a free-stream voxel is not"),
		Mask.IsVoxelMasked(FIntVector(4, 21, 3)));

	// OUT OF BOUNDS IS MASKED. "Not fluid" is one answer, whether the reason
	// is an obstacle or the end of the world: every consumer (MC corner test,
	// streamline termination) wants exactly that collapse.
	TestTrue(TEXT("an out-of-bounds voxel reports masked"),
		Mask.IsVoxelMasked(FIntVector(-1, 0, 0)));
	TestTrue(TEXT("...on the far side too"),
		Mask.IsVoxelMasked(FIntVector(56, 0, 0)));

	TestTrue(TEXT("a position inside the cylinder is masked"),
		Mask.IsMaskedAt(CylinderCenter));
	TestFalse(TEXT("a free-stream position is not"), Mask.IsMaskedAt(FreeStream));
	TestTrue(TEXT("a position outside the domain is masked"),
		Mask.IsMaskedAt(FVector(-1.0, 2.0, 0.5)));

	/* == One-sided gradient stencils ======================================== */
	//
	// THE HAZARD THIS EXISTS FOR: central differences at a mask boundary read
	// a NaN neighbour and poison the derivative -- which is how Q-criterion
	// and MC normals rot silently at exactly the surface the eye looks at.

	// Find a fluid voxel whose +X neighbour is masked (the cylinder's upstream
	// face guarantees one exists); the test LOCATES it rather than hardcoding
	// an index, so a regenerated sample cannot silently invalidate the fixture.
	FIntVector BoundaryVoxel(INDEX_NONE, INDEX_NONE, INDEX_NONE);
	const FIntVector Counts = Mask.GetValueCounts();
	for (int32 Z = 0; Z < Counts.Z && BoundaryVoxel.X == INDEX_NONE; ++Z)
	{
		for (int32 Y = 0; Y < Counts.Y && BoundaryVoxel.X == INDEX_NONE; ++Y)
		{
			for (int32 X = 1; X < Counts.X - 1; ++X)
			{
				const FIntVector Voxel(X, Y, Z);
				if (!Mask.IsVoxelMasked(Voxel)
					&& Mask.IsVoxelMasked(FIntVector(X + 1, Y, Z))
					&& !Mask.IsVoxelMasked(FIntVector(X - 1, Y, Z)))
				{
					BoundaryVoxel = Voxel;
					break;
				}
			}
		}
	}
	if (!TestTrue(TEXT("CONTROL: a fluid voxel with a masked +X neighbour exists"),
			BoundaryVoxel.X != INDEX_NONE))
	{
		return false;
	}

	FVector BoundaryGradient;
	if (TestTrue(TEXT("the gradient at a mask boundary succeeds"),
			FlowVizMaskStencil::OneSidedGradient(
				Scalar, Mask, BoundaryVoxel, /*Component*/ 0, BoundaryGradient)))
	{
		TestTrue(TEXT("and is finite -- the masked neighbour did not poison it"),
			FMath::IsFinite(BoundaryGradient.X) && FMath::IsFinite(BoundaryGradient.Y)
				&& FMath::IsFinite(BoundaryGradient.Z));

		// DIFFERENTIAL: the X component must be the BACKWARD difference -- the
		// only stencil that avoids the masked +X neighbour -- computed here
		// from the voxel values themselves.
		TArray<double> Here, Behind;
		if (TestTrue(TEXT("voxel values are readable"),
				Scalar.GetVoxelValue(BoundaryVoxel, Here)
					&& Scalar.GetVoxelValue(
						BoundaryVoxel - FIntVector(1, 0, 0), Behind)))
		{
			const double SpacingX = Scalar.GetGrid().Spacing.X;
			const double Expected = (Here[0] - Behind[0]) / SpacingX;
			TestTrue(TEXT("the X component IS the backward difference"),
				FMath::IsNearlyEqual(BoundaryGradient.X, Expected, 1e-9));
		}
	}

	// Deep free stream: all six neighbours fluid, so the stencil must be the
	// plain CENTRAL difference -- one-sidedness is a boundary behaviour, not a
	// global downgrade of accuracy.
	{
		const FIntVector Interior(4, 21, 3);
		FVector InteriorGradient;
		if (TestTrue(TEXT("the interior gradient succeeds"),
				FlowVizMaskStencil::OneSidedGradient(
					Scalar, Mask, Interior, 0, InteriorGradient)))
		{
			TArray<double> XP, XN;
			Scalar.GetVoxelValue(Interior + FIntVector(1, 0, 0), XP);
			Scalar.GetVoxelValue(Interior - FIntVector(1, 0, 0), XN);
			const double Expected = (XP[0] - XN[0]) / (2.0 * Scalar.GetGrid().Spacing.X);
			TestTrue(TEXT("the interior X component IS the central difference"),
				FMath::IsNearlyEqual(InteriorGradient.X, Expected, 1e-9));
		}
	}

	// A masked voxel has no derivative: fail, never a zero-filled vector that
	// downstream code would happily normalize (rule 10 in stencil form).
	{
		FVector Refused;
		TestFalse(TEXT("the gradient AT a masked voxel refuses"),
			FlowVizMaskStencil::OneSidedGradient(
				Scalar, Mask, CylinderVoxel, 0, Refused));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
