// Copyright FlowViz contributors. All Rights Reserved.

#include "Flow/FlowVizIsoSurface.h"
#include "Misc/AutomationTest.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizIsoSurfaceTest
{
	using FIsoGrid = FlowVizIsoSurface::FIsoGrid;

	/** A grid filled by a callable of the solver-space position. */
	template <typename FillType>
	FIsoGrid MakeGrid(const FIntVector& Counts, const FVector& Spacing, FillType Fill)
	{
		FIsoGrid Grid;
		Grid.Counts = Counts;
		Grid.Spacing = Spacing;
		Grid.Values.SetNum(Counts.X * Counts.Y * Counts.Z);
		for (int32 Z = 0; Z < Counts.Z; ++Z)
		{
			for (int32 Y = 0; Y < Counts.Y; ++Y)
			{
				for (int32 X = 0; X < Counts.X; ++X)
				{
					const FVector Position(X * Spacing.X, Y * Spacing.Y, Z * Spacing.Z);
					Grid.Values[(Z * Counts.Y + Y) * Counts.X + X] = Fill(Position);
				}
			}
		}
		return Grid;
	}

	/** Solver -> Unreal for a POSITION, the transform the extractor must apply. */
	FVector ToUnreal(const FVector& Solver)
	{
		return FVector(Solver.X * 100.0, -Solver.Y * 100.0, Solver.Z * 100.0);
	}
}

/**
 * The marching-cubes extractor (renderer overhaul P4): edge interpolation,
 * watertight sharing, NaN-cube skipping, gradient normals -- the topology
 * logic that rots silently, pinned analytically.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizIsoSurfaceTest,
	"FlowViz.Flow.IsoSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizIsoSurfaceTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizIsoSurfaceTest;

	/* == Exact edge interpolation on a linear ramp ========================== */
	//
	// f(p) = x over x in [0,4]: the iso surface at 1.7 is EXACTLY the plane
	// x = 1.7 -- any midpoint shortcut lands at 1.5 or 2.0 and fails loudly.
	{
		const FIsoGrid Ramp = MakeGrid(FIntVector(5, 3, 3), FVector(1.0),
			[](const FVector& P) { return P.X; });

		FFlowVizMeshSection Section;
		if (!TestTrue(TEXT("the ramp extracts"),
				FlowVizIsoSurface::ExtractIsoSurface(Ramp, 1.7, Section)))
		{
			return false;
		}
		TestTrue(TEXT("it has triangles"), Section.Indices.Num() >= 6);

		bool bAllOnPlane = true;
		for (const FVector& Vertex : Section.Vertices)
		{
			// Unreal space: solver x=1.7 -> 170.
			bAllOnPlane &= FMath::IsNearlyEqual(Vertex.X, 170.0, 1e-6);
		}
		TestTrue(TEXT("every vertex sits EXACTLY at x=170 -- edge interpolation, "
					  "not midpoints"), bAllOnPlane);

		// The gradient of f=x is +X; the normal must point toward DECREASING
		// field (outward of the high side), i.e. Unreal -X.
		bool bAllOutward = true;
		for (const FVector& Normal : Section.Normals)
		{
			bAllOutward &= Normal.X < -0.99;
		}
		TestTrue(TEXT("normals point down-gradient (Unreal -X)"), bAllOutward);
	}

	/* == Watertight sharing on a sphere ===================================== */
	{
		const FVector Center(4.0, 4.0, 4.0);
		const FIsoGrid Sphere = MakeGrid(FIntVector(9, 9, 9), FVector(1.0),
			[Center](const FVector& P) { return (P - Center).Size(); });

		FFlowVizMeshSection Section;
		if (!TestTrue(TEXT("the sphere extracts at r=2.5"),
				FlowVizIsoSurface::ExtractIsoSurface(Sphere, 2.5, Section)))
		{
			return false;
		}

		// SHARED vertices: a triangle-soup extractor emits 3 vertices per
		// triangle; welding makes vertex count far smaller.
		TestTrue(TEXT("vertices are shared, not soup"),
			Section.Vertices.Num() < Section.Indices.Num() / 2);

		// WATERTIGHT: every edge is used exactly twice (once per winding
		// direction) on a closed surface. This is THE topology assertion --
		// wrong MC case tables tear exactly here.
		TMap<TPair<int32, int32>, int32> EdgeUse;
		for (int32 Index = 0; Index + 2 < Section.Indices.Num(); Index += 3)
		{
			const int32 A = Section.Indices[Index];
			const int32 B = Section.Indices[Index + 1];
			const int32 C = Section.Indices[Index + 2];
			for (const TPair<int32, int32>& Edge :
				{ TPair<int32, int32>(A, B), TPair<int32, int32>(B, C),
				  TPair<int32, int32>(C, A) })
			{
				const TPair<int32, int32> Key(
					FMath::Min(Edge.Key, Edge.Value), FMath::Max(Edge.Key, Edge.Value));
				EdgeUse.FindOrAdd(Key)++;
			}
		}
		int32 OpenEdges = 0;
		for (const TPair<TPair<int32, int32>, int32>& Use : EdgeUse)
		{
			if (Use.Value != 2)
			{
				++OpenEdges;
			}
		}
		TestEqual(TEXT("the sphere is watertight -- every edge shared by exactly two "
					   "triangles"), OpenEdges, 0);

		// Radius: every vertex within half a cell of r=2.5 (Unreal: 250).
		bool bOnSphere = true;
		const FVector UnrealCenter = ToUnreal(Center);
		for (const FVector& Vertex : Section.Vertices)
		{
			bOnSphere &= FMath::Abs(FVector::Dist(Vertex, UnrealCenter) - 250.0) < 50.0;
		}
		TestTrue(TEXT("vertices lie on the iso sphere"), bOnSphere);
	}

	/* == NaN cubes are skipped -- the P1 mask policy ======================== */
	{
		// The ramp again, with a NaN pocket at x index 1, mid y/z: cubes
		// touching it must vanish, others must survive.
		FIsoGrid Masked = MakeGrid(FIntVector(5, 4, 4), FVector(1.0),
			[](const FVector& P) { return P.X; });
		const FIntVector Pocket(1, 1, 1);
		Masked.Values[(Pocket.Z * 4 + Pocket.Y) * 5 + Pocket.X] =
			std::numeric_limits<double>::quiet_NaN();

		FFlowVizMeshSection Full, Holed;
		FIsoGrid Clean = MakeGrid(FIntVector(5, 4, 4), FVector(1.0),
			[](const FVector& P) { return P.X; });
		if (TestTrue(TEXT("clean grid extracts"),
				FlowVizIsoSurface::ExtractIsoSurface(Clean, 1.5, Full))
			&& TestTrue(TEXT("masked grid still extracts"),
				FlowVizIsoSurface::ExtractIsoSurface(Masked, 1.5, Holed)))
		{
			// x=1.5 crosses cubes whose low corner is x index 1 -- the pocket
			// removes some of them, so the holed surface is strictly smaller.
			TestTrue(TEXT("NaN-corner cubes are skipped -- the surface has a hole"),
				Holed.Indices.Num() < Full.Indices.Num());
			TestTrue(TEXT("but only locally -- most of the surface survives"),
				Holed.Indices.Num() > Full.Indices.Num() / 2);

			// And no NaN leaked into geometry.
			bool bFinite = true;
			for (const FVector& Vertex : Holed.Vertices)
			{
				bFinite &= FMath::IsFinite(Vertex.X) && FMath::IsFinite(Vertex.Y)
					&& FMath::IsFinite(Vertex.Z);
			}
			for (const FVector& Normal : Holed.Normals)
			{
				bFinite &= FMath::IsFinite(Normal.X) && FMath::IsFinite(Normal.Y)
					&& FMath::IsFinite(Normal.Z);
			}
			TestTrue(TEXT("no NaN reached vertices or normals"), bFinite);
		}
	}

	/* == Empty and invalid refusals ========================================= */
	{
		FFlowVizMeshSection Section;
		const FIsoGrid Flat = MakeGrid(FIntVector(4, 4, 4), FVector(1.0),
			[](const FVector&) { return 1.0; });
		TestFalse(TEXT("a level with no crossing refuses -- empty, not a degenerate "
					   "sliver"),
			FlowVizIsoSurface::ExtractIsoSurface(Flat, 5.0, Section));

		FIsoGrid Invalid;
		TestFalse(TEXT("an invalid grid refuses"),
			FlowVizIsoSurface::ExtractIsoSurface(Invalid, 0.5, Section));

		FIsoGrid OverflowingCounts;
		OverflowingCounts.Counts = FIntVector(65536, 65536, 2);
		TestFalse(TEXT("overflowing dimensions cannot wrap to the empty array's size"),
			OverflowingCounts.IsValid());
	}

	/* == The percentile default ============================================= */
	{
		// 1..100 with negatives and NaNs mixed in: P90 of the POSITIVE values.
		TArray<double> Values;
		for (int32 Index = 1; Index <= 100; ++Index)
		{
			Values.Add(static_cast<double>(Index));
		}
		Values.Add(-50.0);
		Values.Add(std::numeric_limits<double>::quiet_NaN());

		double Iso = 0.0;
		if (TestTrue(TEXT("the percentile computes"),
				FlowVizIsoSurface::PercentilePositiveIsoValue(Values, 90.0, Iso)))
		{
			TestTrue(FString::Printf(TEXT("P90 of 1..100 is ~90 (got %f) -- negatives "
										  "and NaN excluded"), Iso),
				Iso >= 89.0 && Iso <= 91.0);
		}

		const TArray<double> AllNegative = { -1.0, -2.0, -3.0 };
		double Refused = 0.0;
		TestFalse(TEXT("no positive values, no default -- refuse, never invent"),
			FlowVizIsoSurface::PercentilePositiveIsoValue(AllNegative, 90.0, Refused));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
