// Copyright FlowViz contributors. All Rights Reserved.

#include "Scene/FlowVizVolumeComponent.h"

#include "Scene/FlowVizCaseActor.h"

#include "CFDViz/CFDVizManifest.h"
/*
 * ConvexVolume.h and PrimitiveSceneProxy.h are included for types this file
 * uses BY VALUE and DELETES, not merely by pointer. Both resolved for a long
 * time only because this module is a unity build and some other .cpp in the
 * blob included them first. Compiling this file alone (-singlefile) failed with
 * "variable has incomplete type 'FConvexVolume'" and, worse, "deleting pointer
 * to incomplete type 'FPrimitiveSceneProxy'" - the latter is real undefined
 * behaviour, since that destructor is virtual and deleting through an
 * incomplete type skips it. Do not drop these because the module still links.
 */
#include "ConvexVolume.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PrimitiveSceneProxy.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The scene-graph layer: placement, extent and winding (plan.md section 5.E).
 *
 * WHY THESE ASSERTIONS AND NOT OTHERS. Every defect this layer can produce
 * still renders something. A missing Y mirror renders a mirrored wake, which is
 * physically plausible (ADR 004 opens with exactly that). Unreversed winding
 * renders a box whose faces point inward, which reads as "the data did not
 * load". Bounds that are too small render a volume that vanishes at oblique
 * angles, which reads as a streaming bug. None crashes; none is visible without
 * knowing the answer in advance. So:
 *
 *  1. **Expected values are hand-computed from the sample manifest, not read
 *     back from the code under test.** The mock domain is 56 x 28 x 6 cells at
 *     spacing (0.21428571428571427, 0.14285714285714285, 0.16666666666666666),
 *     which is exactly 12 m x 4 m x 1 m - so at 100 Unreal units per metre the
 *     expected world extent is 1200 x 400 x 100 cm, arithmetic done outside this
 *     file. Asserting CalcBounds against GetPhysicalSize() * 100 would restate
 *     the implementation and pass against any self-consistent mistake.
 *
 *  2. **Every assertion has a stated way to fail, and the ones that matter are
 *     differential.** A winding test that only checks "the box has 12 triangles"
 *     cannot distinguish reversed from unreversed. So the winding block asserts
 *     the two orderings DIFFER, and separately that each is outward-facing in
 *     the space it is destined for - computed by taking an actual cross product
 *     and dotting it against the actual outward direction. A bounds test that
 *     only checks "the extent is positive" cannot catch bounds that are too
 *     small, so the bounds block asserts an exact extent AND asserts that a
 *     deliberately shrunken box fails the same containment check the real one
 *     passes.
 *
 *  3. **The fixture is asymmetric on every axis, and off-origin in Y.** ADR 004
 *     is explicit that a Y-flip test needs a point with non-zero Y, and that a
 *     cube domain passes under a dimension swap. The mock domain is 12 x 4 x 1
 *     with three different spacings, and the placement block additionally uses a
 *     grid origin of (3, 5, 7) - no two equal, none zero - so a transform that
 *     drops the origin, transposes an axis or omits the mirror produces a
 *     different number here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizVolumeComponentTest,
	"FlowViz.Scene.VolumeComponent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

namespace FlowVizVolumeComponentTestHelpers
{
	/** The committed low-resolution sample, which lives beside Plugins/, not inside the plugin. */
	FString GetSampleCaseDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FlowVizRuntime"));
		if (!Plugin.IsValid())
		{
			return FString();
		}
		const FString ProjectDir = FPaths::GetPath(FPaths::GetPath(Plugin->GetBaseDir()));
		return FPaths::Combine(ProjectDir, TEXT("Samples"), TEXT("MockCylinderWake.cfdviz"));
	}

	/**
	 * The sample's grid, spelled out here rather than read back from the manifest
	 * the code under test also reads. Anisotropic on every axis by design: a
	 * bounds or transform bug that assumes cubic voxels shows up against it.
	 */
	FCFDVizGrid MakeSampleGrid()
	{
		FCFDVizGrid Grid;
		Grid.Dimensions = FIntVector(56, 28, 6);
		Grid.Origin = FVector::ZeroVector;
		Grid.Spacing = FVector(
			0.21428571428571427,
			0.14285714285714285,
			0.16666666666666666);
		return Grid;
	}

	/**
	 * A frame source that reports one fixed selection.
	 *
	 * Stands in for the case player, which owns real frame selection. This is not
	 * a stub pretending to be a player - it exists so the component's OWN
	 * behaviour (what it pins, what it publishes) can be tested against a
	 * selection chosen by the test rather than by a peer's timeline.
	 */
	class FFixedFrameSource final : public IFlowVizVolumeFrameSource
	{
	public:
		FFixedFrameSource(int32 InFrameA, int32 InFrameB, float InAlpha)
			: Selection{InFrameA, InFrameB, InAlpha}
		{
		}

		virtual FFlowVizVolumeFrameSelection GetFrameSelection() const override
		{
			return Selection;
		}

	private:
		FFlowVizVolumeFrameSelection Selection;
	};

	/**
	 * Outward local normal of face F of a [0,Size] box, in the same face order
	 * FlowVizVolumeBox::MakeBoxGeometry emits: -X, +X, -Y, +Y, -Z, +Z.
	 *
	 * Written here independently of the implementation so the winding assertion
	 * has something to compare against that did not come out of the code it is
	 * checking.
	 */
	FVector3f ExpectedFaceNormal(int32 FaceIndex)
	{
		switch (FaceIndex)
		{
		case 0: return FVector3f(-1.0f, 0.0f, 0.0f);
		case 1: return FVector3f(1.0f, 0.0f, 0.0f);
		case 2: return FVector3f(0.0f, -1.0f, 0.0f);
		case 3: return FVector3f(0.0f, 1.0f, 0.0f);
		case 4: return FVector3f(0.0f, 0.0f, -1.0f);
		default: return FVector3f(0.0f, 0.0f, 1.0f);
		}
	}

	/**
	 * Does every triangle of this geometry face outward once transformed by M?
	 *
	 * This is the whole winding question, asked the way the rasterizer asks it:
	 * take the cross product of two transformed edges and dot it against the
	 * transformed outward direction. A positive dot on every triangle means the
	 * surface is outward-facing in M's space.
	 *
	 * Note the normal is transformed as a DIRECTION and the cross product is
	 * taken AFTER transformation, on purpose: that is what makes this sensitive
	 * to the reflection. Computing the cross product first and transforming the
	 * result would be the pseudovector path (ADR 004 section 4) and would report
	 * the same answer for both windings, i.e. it could not fail.
	 */
	bool IsOutwardFacingUnder(const FFlowVizVolumeBoxGeometry& Geometry, const FMatrix& M)
	{
		if (Geometry.Indices.Num() != FlowVizVolumeBox::IndexCount)
		{
			return false;
		}

		for (int32 Triangle = 0; Triangle < FlowVizVolumeBox::TriangleCount; ++Triangle)
		{
			const uint32 I0 = Geometry.Indices[Triangle * 3 + 0];
			const uint32 I1 = Geometry.Indices[Triangle * 3 + 1];
			const uint32 I2 = Geometry.Indices[Triangle * 3 + 2];

			const FVector P0 = M.TransformPosition(FVector(Geometry.Positions[I0]));
			const FVector P1 = M.TransformPosition(FVector(Geometry.Positions[I1]));
			const FVector P2 = M.TransformPosition(FVector(Geometry.Positions[I2]));

			// Unreal is left-handed and rasterizes counter-clockwise-front by
			// default, so the geometric front face normal of (P0,P1,P2) is
			// (P1-P0) x (P2-P0) in a left-handed reading, which is the same
			// arithmetic FVector::CrossProduct performs.
			const FVector FaceNormal = FVector::CrossProduct(P1 - P0, P2 - P0);
			const FVector OutwardDirection = M.TransformVector(FVector(Geometry.Normals[I0]));

			if (FVector::DotProduct(FaceNormal, OutwardDirection) <= 0.0)
			{
				return false;
			}
		}
		return true;
	}
}

bool FFlowVizVolumeComponentTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizVolumeComponentTestHelpers;

	/* == Placement: the mirror, the origin, and the double precision ========= */
	{
		// Off-origin on every axis, no two components equal and none zero, so a
		// transform that drops the origin translation or transposes an axis
		// produces a different number here. ADR 004: a Y-flip test needs
		// non-zero Y.
		FFlowVizVolumeTransform VolumeTransform;
		VolumeTransform.Grid = MakeSampleGrid();
		VolumeTransform.Grid.Origin = FVector(3.0, 5.0, 7.0);
		VolumeTransform.Association = ECFDVizAssociation::Cell;

		const FMatrix LocalToUnreal = VolumeTransform.GetLocalToUnrealTransform();

		// THE ASSERTION THE WHOLE WINDING PROBLEM RESTS ON. If this ever comes
		// out positive the mirror has been lost, every consequence below is
		// moot, and the volume renders as a plausible mirrored wake.
		TestTrue(TEXT("the placement matrix has a negative determinant - Y is mirrored"),
			LocalToUnreal.Determinant() < 0.0);
		TestTrue(TEXT("and TransformReversesWinding agrees, so callers need not rediscover it"),
			TransformReversesWinding(LocalToUnreal));

		// Local origin -> the grid origin in Unreal space. Hand-computed:
		// (3, 5, 7) m -> (300, -500, 700) cm. The Y sign is the mirror; the
		// factor of 100 is metres to centimetres.
		const FVector PlacedOrigin = LocalToUnreal.TransformPosition(FVector::ZeroVector);
		TestEqual(TEXT("local origin lands on the grid origin, X"), PlacedOrigin.X, 300.0, 1e-6);
		TestEqual(TEXT("local origin lands on the grid origin, Y IS MIRRORED"), PlacedOrigin.Y, -500.0, 1e-6);
		TestEqual(TEXT("local origin lands on the grid origin, Z"), PlacedOrigin.Z, 700.0, 1e-6);

		// The far corner: (3+12, 5+4, 7+1) m -> (1500, -900, 800) cm. This is
		// the assertion that catches an extent applied before the mirror instead
		// of after, which moves the corner to the wrong side of the origin.
		const FVector FarCorner = LocalToUnreal.TransformPosition(FVector(12.0, 4.0, 1.0));
		TestEqual(TEXT("far corner X"), FarCorner.X, 1500.0, 1e-6);
		TestEqual(TEXT("far corner Y is more negative than the origin, not less"), FarCorner.Y, -900.0, 1e-6);
		TestEqual(TEXT("far corner Z"), FarCorner.Z, 800.0, 1e-6);

		// A kilometre-scale origin is where float32 placement fails, and it is
		// why the cbuffer's GridOrigin must never be used for placement.
		FFlowVizVolumeTransform FarTransform = VolumeTransform;
		FarTransform.Grid.Origin = FVector(12345678.9, 0.0, 0.0);
		TestTrue(TEXT("a kilometre-scale origin reports a non-zero narrowing error"),
			FarTransform.GetOriginNarrowingError() > 0.0);

		// The placement matrix is double precision and must not have absorbed
		// that error. 12345678.9 m -> 1234567890.0 cm exactly; the float32
		// round-trip of the origin is off by ~0.1 m = 10 cm, which is far larger
		// than this tolerance, so a float-narrowed placement fails here.
		const FVector FarPlacement =
			FarTransform.GetLocalToUnrealTransform().TransformPosition(FVector::ZeroVector);
		TestEqual(TEXT("placement stays double precision at kilometre scale"),
			FarPlacement.X, 1234567890.0, 1e-3);
	}

	/* == Winding: reversed and unreversed must actually DIFFER =============== */
	{
		const FVector LocalSize(12.0, 4.0, 1.0);

		FFlowVizVolumeBoxGeometry Unreversed;
		FFlowVizVolumeBoxGeometry Reversed;
		FlowVizVolumeBox::MakeBoxGeometry(LocalSize, /*bReverseWinding*/ false, Unreversed);
		FlowVizVolumeBox::MakeBoxGeometry(LocalSize, /*bReverseWinding*/ true, Reversed);

		TestEqual(TEXT("the box has 24 vertices - four per face, so each face keeps its own normal"),
			Unreversed.Positions.Num(), FlowVizVolumeBox::VertexCount);
		TestEqual(TEXT("the box has 36 indices - 12 triangles"),
			Unreversed.Indices.Num(), FlowVizVolumeBox::IndexCount);
		TestEqual(TEXT("normals are parallel to positions"),
			Unreversed.Normals.Num(), FlowVizVolumeBox::VertexCount);
		TestEqual(TEXT("local UVWs are parallel to positions"),
			Unreversed.LocalUVWs.Num(), FlowVizVolumeBox::VertexCount);

		// THE ASSERTION THAT MAKES THIS TEST ABLE TO FAIL. If the flag is
		// ignored - the single most likely implementation mistake, and one that
		// leaves a perfectly plausible box - these two index buffers are equal
		// and this fails. Without it, every other winding assertion below would
		// pass for a function that returns the same geometry both ways.
		TestFalse(TEXT("reversing the winding actually changes the index buffer"),
			Unreversed.Indices == Reversed.Indices);

		// Reversal is a permutation, not a different mesh: same vertices, same
		// triangles, opposite orientation. This catches a "reversal" implemented
		// by dropping or duplicating triangles.
		TArray<uint32> SortedUnreversed = Unreversed.Indices;
		TArray<uint32> SortedReversed = Reversed.Indices;
		SortedUnreversed.Sort();
		SortedReversed.Sort();
		TestTrue(TEXT("reversal is a permutation - the same triangles, oppositely wound"),
			SortedUnreversed == SortedReversed);

		// Positions are untouched by the flag; only order changes.
		TestTrue(TEXT("reversal does not move a vertex"),
			Unreversed.Positions == Reversed.Positions);

		// The box spans exactly [0, LocalSize]: local space is [0, PhysicalSize]
		// with the grid's minimum corner at the origin, NOT a centred box. A
		// centred box would place the volume half a domain off, which renders
		// plausibly.
		FVector3f Minimum(TNumericLimits<float>::Max());
		FVector3f Maximum(-TNumericLimits<float>::Max());
		for (const FVector3f& Position : Unreversed.Positions)
		{
			Minimum = FVector3f(FMath::Min(Minimum.X, Position.X), FMath::Min(Minimum.Y, Position.Y), FMath::Min(Minimum.Z, Position.Z));
			Maximum = FVector3f(FMath::Max(Maximum.X, Position.X), FMath::Max(Maximum.Y, Position.Y), FMath::Max(Maximum.Z, Position.Z));
		}
		TestEqual(TEXT("the box's minimum corner is the local origin, X"), Minimum.X, 0.0f, 1e-4f);
		TestEqual(TEXT("the box's minimum corner is the local origin, Y"), Minimum.Y, 0.0f, 1e-4f);
		TestEqual(TEXT("the box's minimum corner is the local origin, Z"), Minimum.Z, 0.0f, 1e-4f);
		TestEqual(TEXT("the box spans the full physical extent, X"), Maximum.X, 12.0f, 1e-3f);
		TestEqual(TEXT("the box spans the full physical extent, Y"), Maximum.Y, 4.0f, 1e-3f);
		TestEqual(TEXT("the box spans the full physical extent, Z - not a cube"), Maximum.Z, 1.0f, 1e-3f);

		// Anisotropy is carried, not assumed: the three extents must differ.
		TestTrue(TEXT("the hull is anisotropic, so a cubic shortcut is visible here"),
			!FMath::IsNearlyEqual(Maximum.X, Maximum.Y) && !FMath::IsNearlyEqual(Maximum.Y, Maximum.Z));

		// Every normal is an outward axis direction, and each of the six faces
		// appears exactly four times.
		int32 FaceHits[6] = {0, 0, 0, 0, 0, 0};
		for (const FVector3f& Normal : Unreversed.Normals)
		{
			for (int32 Face = 0; Face < 6; ++Face)
			{
				if (Normal.Equals(ExpectedFaceNormal(Face), 1e-4f))
				{
					++FaceHits[Face];
				}
			}
		}
		for (int32 Face = 0; Face < 6; ++Face)
		{
			TestEqual(
				FString::Printf(TEXT("face %d carries four outward-normal vertices"), Face),
				FaceHits[Face], 4);
		}

		// THE DIFFERENTIAL PAIR. Under the identity - no mirror - the unreversed
		// box is the outward-facing one. Under the real placement matrix, which
		// mirrors Y, the REVERSED box is. Both halves are asserted, and each
		// asserts the other ordering is wrong, so neither can pass vacuously:
		// a MakeBoxGeometry that ignored its flag would fail two of these four.
		const FMatrix Identity = FMatrix::Identity;
		TestTrue(TEXT("under a non-mirroring transform, the UNREVERSED box faces outward"),
			IsOutwardFacingUnder(Unreversed, Identity));
		TestFalse(TEXT("...and the reversed one does not"),
			IsOutwardFacingUnder(Reversed, Identity));

		FFlowVizVolumeTransform VolumeTransform;
		VolumeTransform.Grid = MakeSampleGrid();
		VolumeTransform.Association = ECFDVizAssociation::Cell;
		const FMatrix Mirroring = VolumeTransform.GetLocalToUnrealTransform();
		TestTrue(TEXT("the fixture matrix really is mirroring, or the next two assertions are vacuous"),
			TransformReversesWinding(Mirroring));

		TestTrue(TEXT("under the MIRRORING placement matrix, the REVERSED box faces outward"),
			IsOutwardFacingUnder(Reversed, Mirroring));
		TestFalse(TEXT("...and the unreversed one is inside-out, which renders as an empty box"),
			IsOutwardFacingUnder(Unreversed, Mirroring));

		// A degenerate extent must produce no geometry rather than a hull with
		// zero-area faces, which rasterizes as nothing and looks like a load
		// failure.
		FFlowVizVolumeBoxGeometry Degenerate;
		FlowVizVolumeBox::MakeBoxGeometry(FVector(12.0, 0.0, 1.0), true, Degenerate);
		TestEqual(TEXT("a zero-extent axis yields no geometry, not a degenerate hull"),
			Degenerate.Positions.Num(), 0);

		FFlowVizVolumeBoxGeometry NotFinite;
		FlowVizVolumeBox::MakeBoxGeometry(
			FVector(12.0, std::numeric_limits<double>::quiet_NaN(), 1.0), true, NotFinite);
		TestEqual(TEXT("a non-finite extent yields no geometry"), NotFinite.Positions.Num(), 0);
	}

	/* == The component: bounds, placement and the case load ================== */
	{
		const FName WorldName = MakeUniqueObjectName(
			nullptr, UWorld::StaticClass(), NAME_None, EUniqueObjectNameOptions::GloballyUnique);

		FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
		UWorld* World = UWorld::CreateWorld(
			EWorldType::Game, /*bInformEngineOfWorld*/ false, WorldName, GetTransientPackage());

		if (!TestNotNull(TEXT("test world was created"), World))
		{
			return false;
		}

		World->AddToRoot();
		WorldContext.SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());

		ON_SCOPE_EXIT
		{
			World->DestroyWorld(/*bInformEngineOfWorld*/ true);
			World->RemoveFromRoot();
			GEngine->DestroyWorldContext(World);
		};

		ACFDVizCaseActor* Actor = World->SpawnActor<ACFDVizCaseActor>();
		if (!TestNotNull(TEXT("a CFDViz case actor spawns"), Actor))
		{
			return false;
		}

		UCFDVizVolumeComponent* Volume = Actor->GetVolumeComponent();
		if (!TestNotNull(TEXT("the actor owns a volume component"), Volume))
		{
			return false;
		}

		// An unloaded component must not claim a renderable volume. Otherwise
		// "nothing loaded" and "loaded and empty" are indistinguishable, which
		// is the diagnosis-destroying failure FlowVizCaptureLibrary's comment
		// describes.
		TestFalse(TEXT("an unloaded component has nothing to render"), Volume->HasRenderableVolume());
		TestEqual(TEXT("an unloaded component has zero physical size"),
			Volume->GetPhysicalSize().Size(), 0.0, 1e-9);

		const FString CaseDir = GetSampleCaseDir();
		const FCFDVizResult LoadResult = Actor->LoadCase(CaseDir);
		if (!TestTrue(
			FString::Printf(TEXT("the sample case loads: %s"), *LoadResult.ToString()),
			LoadResult.IsOk()))
		{
			return false;
		}

		TestTrue(TEXT("a loaded component has a renderable volume"), Volume->HasRenderableVolume());

		// Hand-computed from the manifest, outside this file: 56 cells at
		// 0.21428571428571427 m is exactly 12 m, 28 at 0.14285714285714285 is
		// 4 m, 6 at 0.16666666666666666 is 1 m. Reading these back from
		// GetPhysicalSize()'s own inputs would be a restatement.
		const FVector PhysicalSize = Volume->GetPhysicalSize();
		TestEqual(TEXT("physical size X is 12 m"), PhysicalSize.X, 12.0, 1e-9);
		TestEqual(TEXT("physical size Y is 4 m"), PhysicalSize.Y, 4.0, 1e-9);
		TestEqual(TEXT("physical size Z is 1 m - anisotropic, and not a guess"), PhysicalSize.Z, 1.0, 1e-9);

		// The mask field must not be what a default load displays: it is a 0/1
		// volume and would render as a solid block, which reads as a broken
		// transfer function rather than as the wrong field.
		TestNotEqual(TEXT("a default load does not bind the grid's mask field"),
			Volume->GetCaseBinding().FieldId, FName(TEXT("validMask")));

		/*
		 * THE OTHER HALF OF THAT RULE, AND THE REASON IT IS HERE.
		 *
		 * The line above covers one branch: the mask must not be DISPLAYED as if
		 * it were data. The second branch is that the mask must still be FOUND
		 * and forwarded, so masked cells reach the status texture instead of
		 * being rendered as real values (plan.md section 4 rule 10). LoadFrame
		 * does that via FindGridForField(...)->HasMaskField(), and nothing in
		 * this file asserted the lookup resolves - so a change that returned
		 * null there would keep every other assertion green while silently
		 * dropping the mask, and invalid cells would render as data.
		 *
		 * Found by auditing my own comments for two-state hazards and checking
		 * that BOTH named branches have an assertion; this one had the first
		 * only. The mask decision is a pure manifest lookup and runs BEFORE
		 * EnqueueUpload, so it is checkable without an RHI - testing the branch
		 * through LoadFrame itself would self-skip under -nullrhi and cover
		 * nothing (repo memory green-totals-can-hide-skips).
		 *
		 * The fixture can fail: MockCylinderWake's grid really does declare
		 * maskField "validMask", so a broken lookup makes these red rather than
		 * vacuous.
		 */
		const FCFDVizCase& LoadedCase = Volume->GetCaseBinding().Case;
		const FCFDVizField* const BoundField = LoadedCase.FindField(Volume->GetCaseBinding().FieldId);
		if (TestNotNull(TEXT("the bound field is present in the loaded case"), BoundField))
		{
			const FCFDVizGridDescriptor* const Grid = LoadedCase.FindGridForField(*BoundField);
			if (TestNotNull(TEXT("the bound field resolves to a grid - the mask lookup's first step"), Grid))
			{
				TestTrue(
					TEXT("that grid DECLARES a mask field, so masked cells can reach the status "
						 "texture rather than rendering as data"),
					Grid->HasMaskField());
				TestEqual(TEXT("and it is the sample's validMask"),
					Grid->MaskFieldId, FName(TEXT("validMask")));
			}
		}

		/* -- Bounds ---------------------------------------------------------- */

		// The component sits at the world origin with an identity transform, so
		// the bounds are the placement matrix's image of [0, PhysicalSize]:
		// X in [0, 1200], Y in [-400, 0], Z in [0, 100] centimetres. Extent is
		// half of each, and TransformBy takes the absolute value of each matrix
		// row, so the mirror shows up in the ORIGIN's sign, not the extent's.
		const FBoxSphereBounds Bounds = Volume->CalcBounds(FTransform::Identity);

		TestEqual(TEXT("bounds extent X is half of 1200 cm"), Bounds.BoxExtent.X, 600.0, 1e-4);
		TestEqual(TEXT("bounds extent Y is half of 400 cm"), Bounds.BoxExtent.Y, 200.0, 1e-4);
		TestEqual(TEXT("bounds extent Z is half of 100 cm - the thin axis, not a cube"),
			Bounds.BoxExtent.Z, 50.0, 1e-4);

		TestEqual(TEXT("bounds origin X"), Bounds.Origin.X, 600.0, 1e-4);
		TestEqual(TEXT("bounds origin Y IS NEGATIVE - the domain is mirrored into -Y"),
			Bounds.Origin.Y, -200.0, 1e-4);
		TestEqual(TEXT("bounds origin Z"), Bounds.Origin.Z, 50.0, 1e-4);

		// THE ASSERTION THAT CATCHES BOUNDS THAT ARE TOO SMALL. Every corner of
		// the actual drawn hull must lie inside the reported box. This is the
		// containment culling uses, and it is what a "close enough" bounds fails.
		const FBox WorldBox = Bounds.GetBox();
		const FMatrix LocalToWorld = Volume->GetRenderMatrix();
		bool bEveryCornerContained = true;
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector LocalCorner(
				(Corner & 1) ? PhysicalSize.X : 0.0,
				(Corner & 2) ? PhysicalSize.Y : 0.0,
				(Corner & 4) ? PhysicalSize.Z : 0.0);
			const FVector WorldCorner = LocalToWorld.TransformPosition(LocalCorner);
			if (!WorldBox.ExpandBy(1e-3).IsInside(WorldCorner))
			{
				bEveryCornerContained = false;
			}
		}
		TestTrue(TEXT("every corner of the drawn hull is inside the reported bounds"),
			bEveryCornerContained);

		// PROOF THE CONTAINMENT CHECK ABOVE CAN FAIL. A bounds box shrunk by one
		// per cent must reject at least one corner; if it does not, the check is
		// vacuous and would pass for bounds computed from a unit cube.
		{
			const FBox ShrunkBox = FBox(
				Bounds.Origin - Bounds.BoxExtent * 0.99,
				Bounds.Origin + Bounds.BoxExtent * 0.99);
			bool bShrunkRejectsACorner = false;
			for (int32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector LocalCorner(
					(Corner & 1) ? PhysicalSize.X : 0.0,
					(Corner & 2) ? PhysicalSize.Y : 0.0,
					(Corner & 4) ? PhysicalSize.Z : 0.0);
				if (!ShrunkBox.IsInside(LocalToWorld.TransformPosition(LocalCorner)))
				{
					bShrunkRejectsACorner = true;
				}
			}
			TestTrue(TEXT("a one-per-cent-smaller box rejects a corner, so the containment check is not vacuous"),
				bShrunkRejectsACorner);
		}

		// A unit cube scaled by the actor transform - the tempting wrong answer -
		// would give an extent of 50 cm on every axis. Asserting the real extent
		// differs on two axes states that difference rather than implying it.
		TestTrue(TEXT("the extent is not a uniform cube, which a unit-cube bounds would be"),
			!FMath::IsNearlyEqual(Bounds.BoxExtent.X, Bounds.BoxExtent.Y)
				&& !FMath::IsNearlyEqual(Bounds.BoxExtent.Y, Bounds.BoxExtent.Z));

		/* -- Bounds follow the actor ----------------------------------------- */

		// Culling is against the world bounds, so a moved and scaled actor must
		// move and scale them. Hand-computed: translate by (1000, 2000, 3000) and
		// scale by 2, and the origin becomes 1000 + 2*600 = 2200 in X and
		// 2000 + 2*(-200) = 1600 in Y, with the extent doubled.
		const FTransform Moved(
			FRotator::ZeroRotator,
			FVector(1000.0, 2000.0, 3000.0),
			FVector(2.0, 2.0, 2.0));
		const FBoxSphereBounds MovedBounds = Volume->CalcBounds(Moved);

		TestEqual(TEXT("a moved, scaled actor moves the bounds origin X"), MovedBounds.Origin.X, 2200.0, 1e-3);
		TestEqual(TEXT("a moved, scaled actor moves the bounds origin Y"), MovedBounds.Origin.Y, 1600.0, 1e-3);
		TestEqual(TEXT("a moved, scaled actor moves the bounds origin Z"), MovedBounds.Origin.Z, 3100.0, 1e-3);
		TestEqual(TEXT("a scaled actor scales the bounds extent X"), MovedBounds.BoxExtent.X, 1200.0, 1e-3);
		TestEqual(TEXT("a scaled actor scales the bounds extent Y"), MovedBounds.BoxExtent.Y, 400.0, 1e-3);
		TestEqual(TEXT("a scaled actor scales the bounds extent Z"), MovedBounds.BoxExtent.Z, 100.0, 1e-3);

		// The sphere must contain the box, or the coarse cull rejects volumes the
		// fine cull would have kept. Hand-computed diagonal of the half-extent:
		// sqrt(600^2 + 200^2 + 50^2) = 634.43 cm.
		TestTrue(TEXT("the bounding sphere contains the bounding box"),
			Bounds.SphereRadius >= Bounds.BoxExtent.Size() - 1e-3);
		TestEqual(TEXT("and is exactly the half-extent diagonal, not a padded guess"),
			Bounds.SphereRadius, 634.4288770224761, 1e-3);

		/* -- Culling: visible when on screen, culled when off ----------------- */

		// The bounds' whole job. A frustum aimed at the volume must accept it and
		// the same frustum aimed away must reject it - both halves, because a
		// bounds box large enough to never be culled passes the first assertion
		// alone.
		{
			// A 90-degree perspective frustum looking down +X from behind the
			// domain, which spans X in [0, 1200] and Y in [-400, 0].
			const FMatrix LookAtVolume = FLookFromMatrix(
				FVector(-1000.0, -200.0, 50.0), FVector(1.0, 0.0, 0.0), FVector(0.0, 0.0, 1.0))
				* FPerspectiveMatrix(PI / 4.0f, 1.0f, 1.0f, 10.0f, 100000.0f);
			FConvexVolume FrustumAtVolume;
			GetViewFrustumBounds(FrustumAtVolume, LookAtVolume, /*bUseNearPlane*/ true);
			TestTrue(TEXT("a frustum aimed at the volume accepts its bounds"),
				FrustumAtVolume.IntersectBox(Bounds.Origin, Bounds.BoxExtent));

			// Same camera, turned around. If this also passes, the bounds are so
			// large that culling can never reject them and the assertion above
			// proves nothing.
			const FMatrix LookAway = FLookFromMatrix(
				FVector(-1000.0, -200.0, 50.0), FVector(-1.0, 0.0, 0.0), FVector(0.0, 0.0, 1.0))
				* FPerspectiveMatrix(PI / 4.0f, 1.0f, 1.0f, 10.0f, 100000.0f);
			FConvexVolume FrustumAway;
			GetViewFrustumBounds(FrustumAway, LookAway, /*bUseNearPlane*/ true);
			TestFalse(TEXT("the same frustum turned around REJECTS them - so culling is real"),
				FrustumAway.IntersectBox(Bounds.Origin, Bounds.BoxExtent));
		}

		/* -- Frame selection -------------------------------------------------- */

		// No player attached is a display policy - frame 0 - not a stub
		// pretending to be a player.
		const FFlowVizVolumeFrameSelection DefaultSelection = Volume->GetFrameSelection();
		TestEqual(TEXT("with no frame source the component holds frame 0"), DefaultSelection.FrameA, 0);
		TestEqual(TEXT("and is not interpolating"), DefaultSelection.FrameB, INDEX_NONE);
		TestFalse(TEXT("so nothing claims to be an interpolated frame"), DefaultSelection.IsInterpolated());

		/* -- What counts as an interpolated frame ----------------------------- */

		// VISUAL_QA section 1 rule 5: "a temporally interpolated frame says so on
		// screen." The predicate that answers it must be true exactly when the
		// displayed frame was SYNTHESIZED - i.e. is not any stored frame.
		//
		// Both failure directions are real and neither is visible on screen:
		// disclosing a blend that did not happen tells a scientist a stored
		// measurement is derived, and failing to disclose one that did tells them
		// synthesized data is measured. The second is the direction rule 5 exists
		// to prevent, so the boundaries are asserted rather than assumed.
		{
			FFlowVizVolumeFrameSelection Selection;

			// No second frame: whatever alpha says, there is nothing to blend with.
			Selection.FrameA = 3;
			Selection.FrameB = INDEX_NONE;
			Selection.Alpha = 0.5f;
			TestFalse(TEXT("with no second frame, nothing is interpolated whatever alpha says"),
				Selection.IsInterpolated());

			// A genuine blend, the case the rule is about.
			Selection.FrameB = 4;
			Selection.Alpha = 0.5f;
			TestTrue(TEXT("a half-way blend of two distinct frames IS interpolated"),
				Selection.IsInterpolated());

			// ALPHA 0 AND ALPHA 1 ARE BOTH EXACT LANDINGS. At alpha 0 the displayed
			// frame is stored frame A; at alpha 1 it is stored frame B. Neither was
			// synthesized, so neither may claim to be.
			Selection.Alpha = 0.0f;
			TestFalse(TEXT("alpha 0 is exactly stored frame A, not a blend"),
				Selection.IsInterpolated());

			Selection.Alpha = 1.0f;
			TestFalse(TEXT("alpha 1 is exactly stored frame B, not a blend"),
				Selection.IsInterpolated());

			// Just inside each boundary must still count, or a scrub that stops one
			// float from a stored frame would hide a real blend.
			Selection.Alpha = 1.0f - UE_KINDA_SMALL_NUMBER;
			TestTrue(TEXT("just short of alpha 1 is still a blend"), Selection.IsInterpolated());
			Selection.Alpha = UE_KINDA_SMALL_NUMBER;
			TestTrue(TEXT("just past alpha 0 is still a blend"), Selection.IsInterpolated());

			// Blending a frame with ITSELF is the identity at every alpha - the
			// result is that stored frame exactly, so there is nothing to disclose.
			Selection.FrameA = 7;
			Selection.FrameB = 7;
			Selection.Alpha = 0.5f;
			TestFalse(TEXT("blending a frame with itself yields that frame, so it is not interpolated"),
				Selection.IsInterpolated());
		}

		/* -- Displayed frames are pinned against eviction --------------------- */

		// The proxy samples BOTH display frames every frame during an
		// interpolated blend. FFlowVizVolumeTextureSet's eviction policy refuses
		// to overwrite a slot holding a pinned frame - but it only knows what is
		// pinned because someone called SetDisplayFrames.
		//
		// If the component never pins, ChooseUploadSlot is free to evict a slot
		// the shader is about to read, and the next prefetch takes it. The result
		// is a torn or stale frame under scrubbing only - the case where a
		// prefetch is in flight while two frames are displayed - so it is
		// invisible in a paused screenshot and looks like a decode bug rather
		// than an eviction bug.
		{
			FFlowVizVolumeTextureSet& Textures = Volume->GetTextureSet();

			// Frame 0 is what GetFrameSelection reports with no player attached,
			// so it is what the proxy would sample right now.
			Volume->PublishDisplayFrames();

			TestEqual(TEXT("the component pins the frame it displays, so eviction cannot take it"),
				Textures.GetDisplayFrameA(), 0);
			TestEqual(TEXT("and pins no second frame when it is not interpolating"),
				Textures.GetDisplayFrameB(), INDEX_NONE);

			// THE HALF THAT ACTUALLY DISCRIMINATES. The assertions above cannot
			// tell "pins both frames" from "pins only A", because B is INDEX_NONE
			// either way when nothing is interpolating - so on their own they
			// would pass for a component that leaves every blended-toward frame
			// evictable. A frame source that IS interpolating separates them.
			const TSharedPtr<FFixedFrameSource> Source = MakeShared<FFixedFrameSource>(3, 4, 0.5f);
			Volume->SetFrameSource(Source);
			Volume->PublishDisplayFrames();

			TestEqual(TEXT("while blending, frame A is pinned"), Textures.GetDisplayFrameA(), 3);
			TestEqual(TEXT("AND SO IS FRAME B - the shader reads it just as often"),
				Textures.GetDisplayFrameB(), 4);

			// Detaching must release the pins, or a scrubbed-away frame stays
			// resident forever and the ring silently loses a slot.
			Volume->SetFrameSource(nullptr);
			Volume->PublishDisplayFrames();
			TestEqual(TEXT("detaching the source returns to the held frame"),
				Textures.GetDisplayFrameA(), 0);
			TestEqual(TEXT("and releases the second pin, so the ring keeps its slot"),
				Textures.GetDisplayFrameB(), INDEX_NONE);
		}

		/* -- The scene proxy -------------------------------------------------- */

		// The component must actually reach the renderer. A proxy that is never
		// created is the difference between "the volume is placed correctly" and
		// "the volume is placed correctly and nothing draws it".
		FPrimitiveSceneProxy* Proxy = Volume->CreateSceneProxy();
		TestNotNull(TEXT("a loaded component creates a scene proxy"), Proxy);
		if (Proxy != nullptr)
		{
			delete Proxy;
		}

		/* -- The hull's material must be DECLARED, not merely used ------------ */
		//
		// FPrimitiveSceneProxy::VerifyUsedMaterial (PrimitiveSceneProxy.cpp:1756)
		// checks every batch's material against GetUsedMaterials, and a batch
		// whose material is missing is ensured on and DROPPED. This is not a
		// cosmetic warning: the first completed headless capture of this volume
		// contained the template floor and sphere and NO VOLUME AT ALL, while
		// the hull geometry covered 62% of the frame. The batch was built, the
		// proxy was gathered, and the renderer threw the mesh away.
		//
		// The proxy is constructed only when this list is filled, so a component
		// that draws with a material it does not declare renders nothing and
		// says nothing. Asserted against the SAME expression BuildHullMeshBatch
		// uses, fallback included, so the two cannot drift apart silently.
		{
			TArray<UMaterialInterface*> UsedMaterials;
			Volume->GetUsedMaterials(UsedMaterials, /*bGetDebugMaterials*/ true);

			UMaterialInterface* const HullMaterial =
				(GEngine != nullptr && GEngine->DebugMeshMaterial != nullptr)
					? ToRawPtr(GEngine->DebugMeshMaterial)
					: static_cast<UMaterialInterface*>(UMaterial::GetDefaultMaterial(MD_Surface));

			TestTrue(
				TEXT("the hull's material is declared, or every hull batch is dropped and the volume renders as nothing"),
				UsedMaterials.Contains(HullMaterial));

			// The renderer asks with bGetDebugMaterials TRUE (PrimitiveSceneProxy.cpp:556),
			// which is the only call that matters; this half pins the material as
			// a DEBUG one so a later move out of that category is a test failure
			// rather than a silently reintroduced dropped batch.
			TArray<UMaterialInterface*> NonDebugMaterials;
			Volume->GetUsedMaterials(NonDebugMaterials, /*bGetDebugMaterials*/ false);
			TestEqual(
				TEXT("and it is reported as a debug material, which is what the hull actually uses"),
				NonDebugMaterials.Num(), 0);
		}

		// ...and an unloaded one must not, or the scene carries a primitive with
		// no volume in it and the proxy count diagnostic reports health that is
		// not there.
		UCFDVizVolumeComponent* Empty = NewObject<UCFDVizVolumeComponent>(Actor);
		FPrimitiveSceneProxy* EmptyProxy = Empty->CreateSceneProxy();
		TestNull(TEXT("a component with no case creates NO proxy"), EmptyProxy);
		if (EmptyProxy != nullptr)
		{
			delete EmptyProxy;
		}

		/* -- Failure keeps the previous binding ------------------------------- */

		const FCFDVizResult BadLoad = Volume->LoadCase(TEXT("/nonexistent/NoSuchCase.cfdviz"));
		TestFalse(TEXT("a nonexistent case fails to load"), BadLoad.IsOk());
		TestTrue(TEXT("and the previously loaded case is still bound, not blanked"),
			Volume->HasRenderableVolume());
		TestEqual(TEXT("with its physical size intact"), Volume->GetPhysicalSize().X, 12.0, 1e-9);

		Volume->ClearCase();
		TestFalse(TEXT("ClearCase removes the volume"), Volume->HasRenderableVolume());
	}

	return true;
}

/*
 * THE "IS ANYTHING WIRED IN PRODUCTION?" TEST DELIBERATELY DOES NOT LIVE HERE.
 *
 * It is FlowViz.Render.Wiring, in FlowVizRenderWiringTest.cpp. I wrote a version
 * of it in this file first and deleted it: a third agent had independently
 * written a stronger one, and two tests asserting the same thing means the
 * weaker one eventually gets "fixed" to match a mock and stops checking.
 *
 * It cannot live in THIS file for a structural reason. Every other test here
 * installs a dispatcher double as a precondition, and automation tests in one
 * file share a global that those tests mutate. A wiring test's whole subject is
 * what module startup left in that global, so it has to be somewhere nothing
 * sets it first. Keeping it out of this file is what keeps it honest.
 */

/**
 * The seam onto the ray-marcher, tested for the one property that matters
 * before the marcher exists: **the component must be able to say that nothing
 * ray-marched it.**
 *
 * "No marcher is registered" and "the marcher ran and drew nothing" are the same
 * black screen and have nothing in common as fixes. VISUAL_QA section 3 rule 6
 * forbids reporting an unrendered feature as working, and this is the hook that
 * makes that reportable rather than a judgement call.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizVolumeRayMarchSeamTest,
	"FlowViz.Scene.RayMarchSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

namespace FlowVizVolumeComponentTestHelpers
{
	/** Records that it was called and what it was handed. Owned by the test, never installed permanently. */
	class FRecordingDispatcher final : public IFlowVizVolumeRayMarchDispatcher
	{
	public:
		mutable int32 CallCount = 0;
		mutable FMatrix SeenLocalToWorld = FMatrix::Identity;
		mutable FFlowVizVolumeShaderParameters SeenParameters;
		mutable bool bSeenInterpolationDegraded = false;

		virtual void DispatchVolumeRayMarch(const FFlowVizVolumeRayMarchContext& Context) const override
		{
			++CallCount;
			SeenLocalToWorld = Context.LocalToWorld;
			SeenParameters = Context.Parameters;
			bSeenInterpolationDegraded = Context.bInterpolationDegraded;
		}
	};

}

bool FFlowVizVolumeRayMarchSeamTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizVolumeComponentTestHelpers;

	/*
	 * SAVE AND RESTORE WHATEVER IS INSTALLED - DO NOT ASSUME IT IS NULL.
	 *
	 * This test used to say "nothing is installed by default" and read the
	 * global expecting null. That was true only while production installed no
	 * dispatcher at all, which was itself the bug (FlowViz.Render.Wiring). Once
	 * module startup registers a real one, an assertion that the global starts
	 * null is asserting the DEFECT, and it would have gone red on the commit
	 * that fixed it - a test failing because the product got fixed.
	 *
	 * So: capture whatever is there, restore it on every exit path, and make
	 * this test's own preconditions explicit by setting null deliberately rather
	 * than inheriting it. The round-trip assertions below are unchanged and
	 * still check what they always did - install, read back, uninstall - they
	 * just no longer depend on the starting state of a global that production
	 * now owns.
	 */
	IFlowVizVolumeRayMarchDispatcher* const Previous = FlowVizVolumeRayMarch::GetDispatcher();

	ON_SCOPE_EXIT
	{
		FlowVizVolumeRayMarch::SetDispatcher(Previous);
	};

	// Explicitly cleared by this test, not assumed. This is the one place the
	// null is a precondition we establish rather than a claim about production.
	FlowVizVolumeRayMarch::SetDispatcher(nullptr);
	TestNull(TEXT("after this test clears it, there is no ray-march dispatcher"),
		FlowVizVolumeRayMarch::GetDispatcher());

	FRecordingDispatcher Dispatcher;
	FlowVizVolumeRayMarch::SetDispatcher(&Dispatcher);
	TestEqual(TEXT("an installed dispatcher is the one returned"),
		FlowVizVolumeRayMarch::GetDispatcher(),
		static_cast<IFlowVizVolumeRayMarchDispatcher*>(&Dispatcher));

	FlowVizVolumeRayMarch::SetDispatcher(nullptr);
	TestNull(TEXT("and it can be uninstalled, so a test cannot leak one into the next"),
		FlowVizVolumeRayMarch::GetDispatcher());

	/* == A blend that lost its second half must SAY so ======================= */
	{
		// The context defaults to "not degraded", so a dispatcher that never
		// looks at the flag sees the honest answer for the common case.
		const FFlowVizVolumeRayMarchContext Default;
		TestFalse(TEXT("a fresh context does not claim a degraded blend"),
			Default.bInterpolationDegraded);

		// Asked for a real blend, frame B is resident: nothing degraded.
		FFlowVizVolumeFrameSelection Blend;
		Blend.FrameA = 3;
		Blend.FrameB = 4;
		Blend.Alpha = 0.5f;
		TestTrue(TEXT("a mid-blend selection is genuinely interpolated"), Blend.IsInterpolated());
		TestFalse(TEXT("and with frame B resident it is not degraded"),
			FlowVizVolumeRayMarch::IsInterpolationDegraded(Blend, /*bSlotBResident=*/true));

		// THE CASE THIS EXISTS FOR. Same request, frame B evicted or not yet
		// uploaded. The render falls back to frame A alone, which is pixel-wise
		// identical to a genuine non-interpolated frame - so if this is not
		// reported, a held frame is indistinguishable from measured data at that
		// timestep.
		TestTrue(TEXT("but a blend whose frame B is missing IS degraded"),
			FlowVizVolumeRayMarch::IsInterpolationDegraded(Blend, /*bSlotBResident=*/false));

		// Not a blend to begin with: a missing frame B is the normal state and
		// degradation would be a false alarm. Without this case the rule
		// "report whenever SlotB is null" would pass every other assertion here
		// while crying interpolation on every single-frame display.
		FFlowVizVolumeFrameSelection Single;
		Single.FrameA = 3;
		Single.FrameB = INDEX_NONE;
		Single.Alpha = 0.0f;
		TestFalse(TEXT("a single-frame display is not interpolated"), Single.IsInterpolated());
		TestFalse(TEXT("so its absent frame B is not a degraded blend"),
			FlowVizVolumeRayMarch::IsInterpolationDegraded(Single, /*bSlotBResident=*/false));

		// An exact landing on a stored frame carries FrameB but is not a blend,
		// so losing B costs nothing and must not be reported either.
		FFlowVizVolumeFrameSelection Landed;
		Landed.FrameA = 3;
		Landed.FrameB = 4;
		Landed.Alpha = 0.0f;
		TestFalse(TEXT("an exact landing is not interpolated"), Landed.IsInterpolated());
		TestFalse(TEXT("so it is not degraded when frame B is missing"),
			FlowVizVolumeRayMarch::IsInterpolationDegraded(Landed, /*bSlotBResident=*/false));
	}

	return true;
}

/* -------------------------------------------------------------------------- */
/* Do the render settings actually LEAVE the component?                         */
/* -------------------------------------------------------------------------- */

/**
 * The link above the mutation-verified dispatcher seam.
 *
 * WHAT WAS WRONG. FlowViz.Render.SettingsSeam proves the dispatcher applies
 * whatever Context.RenderSettings holds -- deleting that call is KILLED, with an
 * identity control that survives. It was being handed nothing. A grep of every
 * production (non-Tests) reference to RenderSettings found a DECLARATION on the
 * context struct and a READ in the dispatcher, and no write anywhere: the scene
 * proxy built its context without ever assigning the field, so every shipped
 * frame composited with a default-constructed view model.
 *
 * WHY BOTH GREEN SUITES MISSED IT, which is the part worth keeping:
 *
 *  - The seam test builds its own context and sets RenderSettings itself. A test
 *    that supplies the input cannot discover that nothing else supplies it --
 *    the same defect shape as the original frozen-parameter finding, one layer up.
 *  - check_frozen_params.sh exits 0 either way. MEASURED: deleting the seam call
 *    from the dispatcher entirely still exits 0, because it scans for a writer's
 *    existence and reference, not for a call that runs.
 *
 * So a verified seam sat under an unwired one, and the two green artifacts
 * between them covered every part of the path except the join.
 *
 * WHY IT ASSERTS ON MakeProxyDynamicData AND NOT ON A RENDERED FRAME. The
 * assignment lives in GetDynamicMeshElements -- render thread, needs a live
 * scene, an RHI and a collector -- so a test that drove it would self-skip
 * under the default -nullrhi suite and cover nothing (repo memory
 * green-totals-can-hide-skips). The payload builder is the same code the proxy
 * consumes, is const, and needs none of that. Testing the primitive directly is
 * only meaningful because the primitive IS the production path: both writers,
 * the constructor seed and the per-frame push, call this one function.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizProxySettingsTest,
	"FlowViz.Scene.ProxySettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizProxySettingsTest::RunTest(const FString& Parameters)
{
	UCFDVizVolumeComponent* Volume = NewObject<UCFDVizVolumeComponent>();
	if (!TestNotNull(TEXT("a volume component was created"), Volume))
	{
		return false;
	}

	Volume->AddToRoot();
	ON_SCOPE_EXIT
	{
		Volume->RemoveFromRoot();
	};

	/* == THE IDENTITY CONTROL, FIRST ========================================= */
	//
	// A fresh component must marshal settings that are an identity over
	// FillDefaults, or every assertion below is measuring a component that
	// changed the picture merely by existing. This is also the assertion that
	// keeps the wiring honest in the other direction: it fails if someone gives
	// the component a non-default starting mode to "make the test interesting".
	{
		const FFlowVizVolumeProxyDynamicData Fresh = Volume->MakeProxyDynamicData();

		TestEqual(TEXT("a fresh component marshals the default composite mode, so wiring this changed no picture"),
			static_cast<int32>(Fresh.RenderSettings.GetCompositeMode()),
			static_cast<int32>(EFlowVizCompositeMode::Alpha));
		TestFalse(TEXT("and lighting off, which is the Scientific profile's default"),
			Fresh.RenderSettings.IsLightingEnabled());
	}

	/* == The setting must survive the trip into the payload ================== */
	//
	// IsoSurface, not Maximum: it is the mode whose value is read alongside a
	// SECOND parameter (IsoValue), so a payload that carried the mode but
	// dropped everything else would still fail here. Every value below is
	// distinct from the default, or the assertion cannot fail.
	{
		FFlowVizRenderSettingsViewModel Settings;
		Settings.SetCompositeMode(EFlowVizCompositeMode::IsoSurface);
		TestTrue(TEXT("the fixture's iso value was accepted, so the assertion below is about marshalling"),
			Settings.SetIsoValue(3.5f));
		Settings.SetLightingEnabled(true);
		Settings.SetMaxSteps(777u);

		Volume->SetRenderSettings(Settings);

		TestEqual(TEXT("the component reports back the mode it was given"),
			static_cast<int32>(Volume->GetRenderSettings().GetCompositeMode()),
			static_cast<int32>(EFlowVizCompositeMode::IsoSurface));

		/*
		 * THE HEADLINE ASSERTION. This is what had no production writer. It
		 * fails if MakeProxyDynamicData drops the field, and that is not a
		 * hypothetical form of the bug -- it is precisely the state the code was
		 * in: the payload struct carried Parameters and FrameSelection and the
		 * settings simply were not in it.
		 */
		const FFlowVizVolumeProxyDynamicData Data = Volume->MakeProxyDynamicData();

		TestEqual(TEXT("the composite mode reaches the render-thread payload"),
			static_cast<int32>(Data.RenderSettings.GetCompositeMode()),
			static_cast<int32>(EFlowVizCompositeMode::IsoSurface));
		TestEqual(TEXT("so does the iso value the mode is meaningless without"),
			Data.RenderSettings.GetIsoValue(), 3.5f);
		TestTrue(TEXT("and the lighting flag, which is a separate frozen parameter"),
			Data.RenderSettings.IsLightingEnabled());
		TestEqual(TEXT("and MaxSteps, so this is not one field wired by hand"),
			static_cast<int32>(Data.RenderSettings.GetMaxSteps()), 777);
	}

	/* == A COPY, not a reference ============================================= */
	//
	// The payload crosses to the render thread and outlives the call. If it
	// aliased the component's field, a settings edit during a frame would mutate
	// state the render thread is reading -- a data race whose symptom is an
	// occasional wrong composite mode, not a crash.
	{
		const FFlowVizVolumeProxyDynamicData Captured = Volume->MakeProxyDynamicData();

		FFlowVizRenderSettingsViewModel Changed;
		Changed.SetCompositeMode(EFlowVizCompositeMode::Minimum);
		Volume->SetRenderSettings(Changed);

		TestEqual(TEXT("a payload already built is unaffected by a later edit, so it is a copy"),
			static_cast<int32>(Captured.RenderSettings.GetCompositeMode()),
			static_cast<int32>(EFlowVizCompositeMode::IsoSurface));
		TestEqual(TEXT("while a payload built after the edit carries the new mode"),
			static_cast<int32>(Volume->MakeProxyDynamicData().RenderSettings.GetCompositeMode()),
			static_cast<int32>(EFlowVizCompositeMode::Minimum));
	}

	return true;
}

/* -------------------------------------------------------------------------- */
/* The join between the payload and the dispatcher                              */
/* -------------------------------------------------------------------------- */

/**
 * Does the marshalled payload actually reach the context the dispatcher reads?
 *
 * WHY THIS EXISTS AS A SEPARATE TEST, and it is the whole lesson of this task:
 * the path has three links, and having verified two of them proved nothing
 * about the third.
 *
 *   1. component -> payload ....... FlowViz.Scene.ProxySettings
 *   2. payload -> context ......... THIS TEST -- previously nothing
 *   3. context -> shader params ... FlowViz.Render.SettingsSeam
 *
 * MEASURED, not assumed. With link 2 deleted -- `Context.RenderSettings =
 * DynamicData.RenderSettings` removed from the proxy's dispatch site -- the
 * full suite passed 90/90, ProxySettings and SettingsSeam included. Link 1
 * proves the payload carries the settings; link 3 proves the dispatcher applies
 * whatever the context holds; neither notices that nothing copies one into the
 * other. That is the same defect shape as the original frozen-parameter finding
 * and as the unwired view model above it: a verified producer, a verified
 * consumer, and no test on the join.
 *
 * The assembly had to be extracted from GetDynamicMeshElements to be reachable
 * at all -- see FlowVizVolumeRayMarch::MakeDispatchContext. This test calls that
 * production function; it does not rebuild the context, which would only assert
 * that its own copy works.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizDispatchContextTest,
	"FlowViz.Scene.DispatchContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizDispatchContextTest::RunTest(const FString& Parameters)
{
	// Distinct, non-default values throughout. A payload field that matches the
	// context's own default cannot show that it was copied.
	FFlowVizVolumeProxyDynamicData Data;
	Data.bHasParameters = true;
	Data.Parameters.ValueRangeMin = -7.5f;
	Data.Parameters.ValueRangeMax = 21.25f;
	Data.FrameSelection.FrameA = 3;
	Data.FrameSelection.FrameB = 4;
	Data.FrameSelection.Alpha = 0.25f;
	Data.RenderSettings.SetCompositeMode(EFlowVizCompositeMode::Average);
	Data.RenderSettings.SetLightingEnabled(true);

	const FMatrix LocalToWorld = FMatrix(
		FPlane(2.0, 0.0, 0.0, 0.0),
		FPlane(0.0, 3.0, 0.0, 0.0),
		FPlane(0.0, 0.0, 5.0, 0.0),
		FPlane(11.0, 13.0, 17.0, 1.0));

	// Distinct addresses are all this needs: MakeDispatchContext stores the
	// pointers and never dereferences them. Constructing real slot textures
	// would require an RHI and would test the texture set, not this join.
	FFlowVizVolumeSlotTextures SlotA;
	FFlowVizVolumeSlotTextures SlotB;

	/* == A resident frame B: everything is carried, nothing is degraded ====== */
	{
		const FFlowVizVolumeRayMarchContext Context =
			FlowVizVolumeRayMarch::MakeDispatchContext(
				/*View=*/nullptr, LocalToWorld, Data, &SlotA, &SlotB);

		/*
		 * THE HEADLINE ASSERTION -- the link that was missing. Average, not
		 * Maximum: it is neither the enum's zero nor the value any other case
		 * here uses, so a context that kept its own default fails, and so does
		 * one that copied some other test's leftover.
		 */
		TestEqual(TEXT("the payload's composite mode reaches the dispatcher's context"),
			static_cast<int32>(Context.RenderSettings.GetCompositeMode()),
			static_cast<int32>(EFlowVizCompositeMode::Average));
		TestTrue(TEXT("and its lighting flag, which is a separately frozen parameter"),
			Context.RenderSettings.IsLightingEnabled());

		// The fields that were already carried. Asserted so a refactor of this
		// assembly cannot fix the settings while dropping something else -- the
		// exact trade the inline version made invisible.
		TestEqual(TEXT("the cbuffer block is carried"),
			Context.Parameters.ValueRangeMax, 21.25f);
		TestTrue(TEXT("the placement matrix is carried"),
			Context.LocalToWorld.Equals(LocalToWorld, 1e-9));
		TestEqual(TEXT("slot A is carried"), Context.SlotA, static_cast<const FFlowVizVolumeSlotTextures*>(&SlotA));
		TestEqual(TEXT("slot B is carried"), Context.SlotB, static_cast<const FFlowVizVolumeSlotTextures*>(&SlotB));
		TestEqual(TEXT("with frame B resident the blend weight is the requested one"),
			Context.Alpha, 0.25f);
		TestFalse(TEXT("and a fully resident blend is not degraded"),
			Context.bInterpolationDegraded);
	}

	/* == Frame B missing: alpha is forced to zero AND the loss is disclosed == */
	//
	// Both halves matter and they fail differently. Keeping alpha at 0.25 with
	// no slot B blends toward a texture that is not there; keeping
	// bInterpolationDegraded false lets a held frame pass as measured data at
	// that timestep (VISUAL_QA section 1 rule 5).
	{
		const FFlowVizVolumeRayMarchContext Context =
			FlowVizVolumeRayMarch::MakeDispatchContext(
				/*View=*/nullptr, LocalToWorld, Data, &SlotA, /*SlotBTextures=*/nullptr);

		TestEqual(TEXT("a missing frame B forces the blend weight to zero"), Context.Alpha, 0.0f);
		TestTrue(TEXT("and the lost half of the blend is disclosed"),
			Context.bInterpolationDegraded);

		// The settings must survive the degraded path too. A guard clause that
		// returned early here would pass every assertion above.
		TestEqual(TEXT("the composite mode still reaches the context on the degraded path"),
			static_cast<int32>(Context.RenderSettings.GetCompositeMode()),
			static_cast<int32>(EFlowVizCompositeMode::Average));
	}

	/* == A single-frame display is NOT degraded ============================== */
	//
	// The control for the assertion above. Without it, "always report degraded
	// when slot B is null" passes both cases while crying interpolation on every
	// non-interpolated frame -- a false disclosure is also a lie about provenance.
	{
		FFlowVizVolumeProxyDynamicData Single = Data;
		Single.FrameSelection.FrameB = INDEX_NONE;
		Single.FrameSelection.Alpha = 0.0f;

		const FFlowVizVolumeRayMarchContext Context =
			FlowVizVolumeRayMarch::MakeDispatchContext(
				/*View=*/nullptr, LocalToWorld, Single, &SlotA, /*SlotBTextures=*/nullptr);

		TestFalse(TEXT("a single-frame display is not reported as a degraded blend"),
			Context.bInterpolationDegraded);
	}

	/* == THE IDENTITY CONTROL =============================================== */
	//
	// A default payload must produce a context that renders as it always did.
	// Without this, every assertion above could be satisfied by an assembly that
	// forced some non-default mode of its own.
	{
		const FFlowVizVolumeProxyDynamicData Fresh;
		const FFlowVizVolumeRayMarchContext Context =
			FlowVizVolumeRayMarch::MakeDispatchContext(
				/*View=*/nullptr, FMatrix::Identity, Fresh, /*SlotATextures=*/nullptr, /*SlotBTextures=*/nullptr);

		TestEqual(TEXT("a default payload yields the default composite mode, so wiring this changed no picture"),
			static_cast<int32>(Context.RenderSettings.GetCompositeMode()),
			static_cast<int32>(EFlowVizCompositeMode::Alpha));
		TestFalse(TEXT("and lighting stays off"), Context.RenderSettings.IsLightingEnabled());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
