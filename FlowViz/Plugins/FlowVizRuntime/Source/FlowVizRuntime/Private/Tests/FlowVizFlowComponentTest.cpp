// Copyright FlowViz contributors. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Scene/FlowVizFlowComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizFlowComponentTest
{
	/** Two glyphs, one at half the other's speed, along different axes. */
	TArray<FFlowVizGlyph> MakeGlyphs()
	{
		TArray<FFlowVizGlyph> Glyphs;
		FFlowVizGlyph& Fast = Glyphs.AddDefaulted_GetRef();
		Fast.Position = FVector(6.0, 2.0, 0.5);
		Fast.Direction = FVector(1.0, 0.0, 0.0);
		Fast.Magnitude = 10.0;

		FFlowVizGlyph& Slow = Glyphs.AddDefaulted_GetRef();
		Slow.Position = FVector(3.0, 1.0, 0.5);
		Slow.Direction = FVector(0.0, 1.0, 0.0);
		Slow.Magnitude = 5.0;
		return Glyphs;
	}
}

/**
 * The flow component's transform maths (#86 / DoD 9 and 10's viewport hop).
 *
 * The glyph/streamline BUILDERS are tested against the analytic wake; what
 * this pins is the hop they feed: solver positions through the position
 * adapter, directions through the DIRECTION adapter (they mirror
 * differently -- the vortex-spin hazard CFDVizTypes warns about), length
 * proportional to magnitude against the batch's own maximum, and empty
 * lines staying empty.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizFlowComponentTest,
	"FlowViz.Scene.FlowComponent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizFlowComponentTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizFlowComponentTest;

	/* == Glyph transforms ==================================================== */
	{
		TArray<FTransform> Transforms;
		double MaxMagnitude = 0.0;
		UCFDVizFlowComponent::MakeGlyphTransforms(
			MakeGlyphs(), CFDViz::MetersToUnrealCentimeters, Transforms, MaxMagnitude);

		if (!TestEqual(TEXT("one transform per glyph"), Transforms.Num(), 2))
		{
			return false;
		}
		TestEqual(TEXT("the batch maximum is the fast glyph's"), MaxMagnitude, 10.0);

		// Position through the POSITION adapter: metres to centimetres with the
		// Y mirror -- solver (6, 2, 0.5) lands at Unreal (600, -200, 50).
		TestTrue(TEXT("the position is adapted, mirror included"),
			Transforms[0].GetLocation().Equals(FVector(600.0, -200.0, 50.0), 1e-4));

		// The fast glyph's scaled height is the full MaxGlyphLength; the slow
		// one's is half -- length IS the data, radial thickness is styling.
		const double FastLength = Transforms[0].GetScale3D().Z * 100.0;
		const double SlowLength = Transforms[1].GetScale3D().Z * 100.0;
		TestTrue(TEXT("the fastest glyph draws at the full glyph length"),
			FMath::IsNearlyEqual(FastLength, UCFDVizFlowComponent::MaxGlyphLength, 1e-6));
		TestTrue(TEXT("and lengths are proportional to magnitude -- the ratio between "
					  "arrows is the data"),
			FMath::IsNearlyEqual(SlowLength / FastLength, 0.5, 1e-6));

		// Direction through the DIRECTION adapter: the cone's +Z must land on
		// the mirrored flow direction. Solver +X is Unreal +X (unaffected by
		// the Y mirror), so rotating the cone's +Z by the transform's rotation
		// must give +X.
		const FVector FastAxis = Transforms[0].GetRotation().RotateVector(FVector::ZAxisVector);
		TestTrue(TEXT("the fast glyph points along Unreal +X"),
			FastAxis.Equals(FVector::XAxisVector, 1e-6));

		// Solver +Y MIRRORS to Unreal -Y. Using the position adapter for the
		// direction would also scale it; using no mirror would point it +Y.
		const FVector SlowAxis = Transforms[1].GetRotation().RotateVector(FVector::ZAxisVector);
		TestTrue(TEXT("the slow glyph points along Unreal -Y -- the direction is mirrored, "
					  "not merely copied"),
			SlowAxis.Equals(-FVector::YAxisVector, 1e-6));
	}

	/* == Streamline batches ================================================== */
	{
		TArray<FFlowVizStreamline> Lines;
		FFlowVizStreamline& Real = Lines.AddDefaulted_GetRef();
		Real.Points = { FVector(1.0, 2.0, 0.5), FVector(2.0, 2.0, 0.5), FVector(3.0, 2.0, 0.5) };
		Real.Magnitudes = { 2.0, 4.0, 8.0 };

		// The empty line a seed outside the domain produces.
		Lines.AddDefaulted();

		TArray<UCFDVizFlowComponent::FStreamlineBatch> Batches;
		UCFDVizFlowComponent::MakeStreamlineBatches(
			Lines, CFDViz::MetersToUnrealCentimeters, Batches);

		if (!TestEqual(TEXT("the empty line yields no batch -- nothing to draw is nothing "
							"drawn, not a zero-length artifact"),
				Batches.Num(), 1))
		{
			return false;
		}
		TestEqual(TEXT("points survive count-for-count"), Batches[0].Points.Num(), 3);
		TestEqual(TEXT("with a colour per point"), Batches[0].Colors.Num(), 3);
		TestTrue(TEXT("positions are adapted, mirror included"),
			Batches[0].Points[0].Equals(FVector(100.0, -200.0, 50.0), 1e-4));
		TestFalse(TEXT("the slow end and the fast end are DIFFERENT colours -- the ramp "
					   "carries the magnitude, so a constant-colour line means the "
					   "normalisation collapsed"),
			Batches[0].Colors[0].Equals(Batches[0].Colors[2]));
	}

	/* == The component instances what the transforms describe ================ */
	{
		UCFDVizFlowComponent* Component = NewObject<UCFDVizFlowComponent>();
		Component->AddToRoot();

		TestEqual(TEXT("CONTROL: a fresh component draws nothing"),
			Component->GetGlyphInstanceCount(), 0);

		Component->SetFlowData(
			MakeGlyphs(), TArray<FFlowVizStreamline>(), CFDViz::MetersToUnrealCentimeters);
		TestEqual(TEXT("SetFlowData instances one glyph per input"),
			Component->GetGlyphInstanceCount(), 2);

		Component->ClearFlowData();
		TestEqual(TEXT("ClearFlowData removes them"), Component->GetGlyphInstanceCount(), 0);

		Component->RemoveFromRoot();
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
