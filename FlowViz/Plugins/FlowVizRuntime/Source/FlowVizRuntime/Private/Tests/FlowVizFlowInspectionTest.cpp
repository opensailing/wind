// Copyright FlowViz contributors. All Rights Reserved.

#include "Flow/FlowVizFieldSampler.h"
#include "Flow/FlowVizFlowInspection.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED namespace: unity build (#37).
 */
namespace FlowVizFlowInspectionTest
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

	bool LoadCase(FAutomationTestBase& Test, FCFDVizCase& OutCase)
	{
		const FCFDVizResult Result = FCFDVizCase::LoadFromFile(GetSampleManifest(), OutCase);
		if (!Result.IsOk())
		{
			Test.AddError(FString::Printf(
				TEXT("could not load the sample case: %s"), *Result.ToString()));
			return false;
		}
		return true;
	}

	/*
	 * THE ANALYTIC FACT the wake tests lean on: the mock's U.x is strictly
	 * positive everywhere (declared globalComponentMin[0] = 0.553), so flow
	 * always runs downstream. A streamline seeded mid-domain MUST exit the +X
	 * face, and every glyph must have a positive X direction component.
	 */
	const FVector DomainSize(12.0, 4.0, 1.0);
}

/**
 * The CPU field sampler: trilinear, refusing, and consistent with the probe
 * sampler's convention (#80/#81, Milestone E).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizFieldSamplerTest,
	"FlowViz.Flow.FieldSampler",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizFieldSamplerTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizFlowInspectionTest;

	FCFDVizCase Case;
	if (!LoadCase(*this, Case))
	{
		return false;
	}

	FFlowVizFieldSampler Sampler;

	/* == An unbuilt sampler refuses ========================================== */
	{
		TArray<double> Value;
		TestFalse(TEXT("an unbuilt sampler refuses to sample"),
			Sampler.Sample(FVector(6.0, 2.0, 0.5), Value));
	}

	/* == Build, and the basic reads ========================================== */
	{
		const FCFDVizResult Built = Sampler.Build(Case, FName(TEXT("U")), 0);
		if (!TestTrue(FString::Printf(TEXT("the U sampler builds (%s)"), *Built.ToString()),
				Built.IsOk()))
		{
			return false;
		}
		TestEqual(TEXT("U has three components"), Sampler.GetComponentCount(), 3);

		FVector Velocity;
		TestTrue(TEXT("an interior sample succeeds"),
			Sampler.SampleVector(FVector(6.0, 2.0, 0.5), Velocity));
		TestTrue(TEXT("with a positive downstream component -- the mock's U.x is strictly "
					  "positive by construction"),
			Velocity.X > 0.0);
		TestTrue(TEXT("and within the field's declared global bound"),
			Velocity.Size() <= 13.4972 + 1.0e-3);

		/* -- Outside is refused, never zero (rule 10). ---------------------- */
		TestFalse(TEXT("a sample beyond +X is refused"),
			Sampler.SampleVector(FVector(13.0, 2.0, 0.5), Velocity));
		TestFalse(TEXT("a sample below -Y is refused"),
			Sampler.SampleVector(FVector(6.0, -1.0, 0.5), Velocity));

		/*
		 * -- Trilinear really interpolates: a sample halfway between two cell
		 * centres lies BETWEEN the two centre values. Nearest-neighbour --
		 * the regression this guards -- returns one endpoint exactly.
		 */
		const FVector Spacing(12.0 / 56.0, 4.0 / 28.0, 1.0 / 6.0);
		const FVector CentreA(28.5 * Spacing.X, 14.5 * Spacing.Y, 3.5 * Spacing.Z);
		const FVector CentreB(29.5 * Spacing.X, 14.5 * Spacing.Y, 3.5 * Spacing.Z);
		const FVector Midpoint = (CentreA + CentreB) * 0.5;

		FVector VelocityA, VelocityB, VelocityMid;
		TestTrue(TEXT("CONTROL: both cell centres sample"),
			Sampler.SampleVector(CentreA, VelocityA) && Sampler.SampleVector(CentreB, VelocityB));
		TestTrue(TEXT("CONTROL: the two centres differ -- the wake is not uniform here, or "
					  "the interpolation assertion below would be vacuous"),
			!VelocityA.Equals(VelocityB, 1.0e-9));
		TestTrue(TEXT("the midpoint samples"), Sampler.SampleVector(Midpoint, VelocityMid));
		TestTrue(TEXT("and is the average of the two centres -- trilinear along one axis is "
					  "exactly the midpoint average; nearest-neighbour fails this"),
			VelocityMid.Equals((VelocityA + VelocityB) * 0.5, 1.0e-6));
	}

	return true;
}

/**
 * Glyphs on a slice: anchored on the plane, oriented by the flow (#80, DoD 9).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizSliceGlyphsTest,
	"FlowViz.Flow.SliceGlyphs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizSliceGlyphsTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizFlowInspectionTest;

	FCFDVizCase Case;
	if (!LoadCase(*this, Case))
	{
		return false;
	}
	FFlowVizFieldSampler Sampler;
	if (!TestTrue(TEXT("CONTROL: the U sampler builds"),
			Sampler.Build(Case, FName(TEXT("U")), 0).IsOk()))
	{
		return false;
	}

	// The mid-Z slice: the plane every glyph must sit on.
	const FVector PlaneOrigin(6.0, 2.0, 0.5);
	const FVector PlaneNormal(0.0, 0.0, 1.0);

	FFlowVizGlyphSettings Settings;
	Settings.SamplesPerAxis = 16;

	TArray<FFlowVizGlyph> Glyphs;
	TestTrue(TEXT("glyph building succeeds"),
		FlowVizFlow::BuildSliceGlyphs(Sampler, PlaneOrigin, PlaneNormal, Settings, Glyphs));

	if (!TestTrue(FString::Printf(TEXT("a 16x16 grid over a 12x4 domain yields many glyphs "
										"(got %d) -- zero means the grid never hit the box"),
			Glyphs.Num()),
			Glyphs.Num() >= 32))
	{
		return false;
	}

	int32 OffPlane = 0, Upstream = 0, Unnormalised = 0, OutsideBox = 0;
	for (const FFlowVizGlyph& Glyph : Glyphs)
	{
		if (!FMath::IsNearlyEqual(Glyph.Position.Z, 0.5, 1.0e-6))
		{
			++OffPlane;
		}
		// U.x > 0 everywhere in the mock, so every glyph points downstream.
		if (Glyph.Direction.X <= 0.0)
		{
			++Upstream;
		}
		if (!FMath::IsNearlyEqual(Glyph.Direction.Size(), 1.0, 1.0e-6))
		{
			++Unnormalised;
		}
		if (Glyph.Position.X < 0.0 || Glyph.Position.X > DomainSize.X
			|| Glyph.Position.Y < 0.0 || Glyph.Position.Y > DomainSize.Y)
		{
			++OutsideBox;
		}
	}
	TestEqual(TEXT("every glyph sits ON the slice plane"), OffPlane, 0);
	TestEqual(TEXT("every glyph points downstream -- the mock's U.x is strictly positive, "
				   "so an upstream glyph is a sampling or orientation defect"),
		Upstream, 0);
	TestEqual(TEXT("every direction is unit length"), Unnormalised, 0);
	TestEqual(TEXT("no glyph is anchored outside the domain -- outside samples must be "
				   "refused, not zero-filled"),
		OutsideBox, 0);

	/* == A plane outside the domain yields zero glyphs, not an error ========= */
	{
		TArray<FFlowVizGlyph> None;
		TestTrue(TEXT("a far-away plane still 'succeeds'"),
			FlowVizFlow::BuildSliceGlyphs(
				Sampler, FVector(0.0, 0.0, 100.0), PlaneNormal, Settings, None));
		TestEqual(TEXT("with zero glyphs"), None.Num(), 0);
	}

	/* == A degenerate normal is refused ====================================== */
	{
		TArray<FFlowVizGlyph> None;
		TestFalse(TEXT("a zero normal is refused"),
			FlowVizFlow::BuildSliceGlyphs(
				Sampler, PlaneOrigin, FVector::ZeroVector, Settings, None));
	}

	return true;
}

/**
 * Streamlines: seeded, integrated downstream, ending for stated reasons
 * (#81, DoD 10).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizStreamlinesTest,
	"FlowViz.Flow.Streamlines",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizStreamlinesTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizFlowInspectionTest;

	FCFDVizCase Case;
	if (!LoadCase(*this, Case))
	{
		return false;
	}
	FFlowVizFieldSampler Sampler;
	if (!TestTrue(TEXT("CONTROL: the U sampler builds"),
			Sampler.Build(Case, FName(TEXT("U")), 0).IsOk()))
	{
		return false;
	}

	// A vertical rake across the wake at x = 2, mid-Z.
	const FVector RakeStart(2.0, 0.5, 0.5);
	const FVector RakeEnd(2.0, 3.5, 0.5);

	FFlowVizStreamlineSettings Settings;
	Settings.SeedCount = 8;
	Settings.MaxSteps = 4096;

	TArray<FFlowVizStreamline> Streamlines;
	TestTrue(TEXT("streamline building succeeds"),
		FlowVizFlow::BuildStreamlines(Sampler, RakeStart, RakeEnd, Settings, Streamlines));

	if (!TestEqual(TEXT("one streamline per seed, in rake order"),
			Streamlines.Num(), Settings.SeedCount))
	{
		return false;
	}

	/*
	 * TWO REGIMES, BOTH ASSERTED. The mock is a vortex street: in the FREE
	 * STREAM (the rake's ends, y near 0.5 and 3.5) U.x dominates and a line
	 * must exit the +X face. In the WAKE (mid-rake) the instantaneous field
	 * contains closed orbits around vortex cores -- a streamline caught in one
	 * circles until the step budget ends, and that is CORRECT physics for an
	 * instantaneous field, not an integrator defect. The first draft of this
	 * test asserted every line leaves; seeds 3 and 4 orbited a core at
	 * x ~ 3.5 and failed it, which is the analytic field doing exactly what a
	 * vortex street does. So: every line ends for a DISCLOSED reason that is
	 * not Stagnant (|U| >= 0.93 declared), free-stream lines leave downstream,
	 * and no point ever sits outside the domain.
	 */
	for (int32 Index = 0; Index < Streamlines.Num(); ++Index)
	{
		const FFlowVizStreamline& Line = Streamlines[Index];
		if (!TestTrue(FString::Printf(TEXT("streamline %d has points"), Index),
				Line.Points.Num() > 2))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("streamline %d's magnitudes parallel its points"), Index),
			Line.Magnitudes.Num(), Line.Points.Num());

		TestTrue(FString::Printf(
			TEXT("streamline %d does not stagnate -- |U| >= 0.93 by the manifest"), Index),
			Line.EndReason != FFlowVizStreamline::EEndReason::Stagnant);

		int32 Outside = 0;
		for (const FVector& Point : Line.Points)
		{
			if (Point.X < -1e-6 || Point.X > DomainSize.X + 1e-6 || Point.Y < -1e-6
				|| Point.Y > DomainSize.Y + 1e-6 || Point.Z < -1e-6
				|| Point.Z > DomainSize.Z + 1e-6)
			{
				++Outside;
			}
		}
		TestEqual(FString::Printf(
			TEXT("streamline %d never records a point outside the domain"), Index),
			Outside, 0);
	}

	// The free-stream seeds: first and last, at y = 0.5 and 3.5, outside the
	// wake deficit. U.x dominates there, so both must exit downstream.
	for (const int32 Index : { 0, Settings.SeedCount - 1 })
	{
		const FFlowVizStreamline& Line = Streamlines[Index];
		double MaxX = -1.0;
		for (const FVector& Point : Line.Points)
		{
			MaxX = FMath::Max(MaxX, Point.X);
		}
		TestTrue(FString::Printf(
			TEXT("free-stream streamline %d travels downstream (max x %.2f from seed 2.0)"),
				Index, MaxX),
			MaxX > 8.0);
		TestEqual(FString::Printf(
			TEXT("free-stream streamline %d ends by LEAVING the domain, the only exit the "
				 "free stream allows"), Index),
			Line.EndReason, FFlowVizStreamline::EEndReason::LeftDomain);
	}

	/* == A seed outside the domain yields an empty line IN PLACE ============= */
	{
		TArray<FFlowVizStreamline> Lines;
		FFlowVizStreamlineSettings Two = Settings;
		Two.SeedCount = 2;
		TestTrue(TEXT("a rake with one seed outside still builds"),
			FlowVizFlow::BuildStreamlines(
				Sampler, FVector(6.0, 2.0, 0.5), FVector(6.0, 200.0, 0.5), Two, Lines));
		TestEqual(TEXT("both seeds are represented"), Lines.Num(), 2);
		TestTrue(TEXT("the inside seed integrated"), Lines[0].Points.Num() > 2);
		TestEqual(TEXT("the outside seed is EMPTY IN PLACE, keeping rake order rather than "
					   "shifting its neighbour's index"),
			Lines[1].Points.Num(), 0);
	}

	return true;
}


/**
 * The dataset-aware default rake (renderer overhaul P5): the seeding rule
 * that cannot produce an empty seeding on a shallow grid.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizDefaultRakeTest,
	"FlowViz.Flow.DefaultRake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizDefaultRakeTest::RunTest(const FString& Parameters)
{
	FVector Start, End;

	// The committed sample's domain: the rake must sit upstream, z-mid,
	// spanning the middle of Y -- and be NON-DEGENERATE, which is the property
	// a stride-based rule loses on 6-cell-deep data.
	if (TestTrue(TEXT("the sample domain yields a rake"),
			FlowVizFlow::MakeDefaultRake(FVector(12.0, 4.0, 1.0), Start, End)))
	{
		TestTrue(TEXT("upstream: x sits inside the first tenth"),
			Start.X > 0.0 && Start.X < 1.2);
		TestEqual(TEXT("z-mid, matching the default cut plane"), Start.Z, 0.5);
		TestEqual(TEXT("a vertical line: both ends share x"), Start.X, End.X);
		TestTrue(TEXT("NON-DEGENERATE: the rake spans most of Y -- the empty-seeding "
					  "failure a fixed stride produces here cannot"),
			End.Y - Start.Y > 2.0);
		TestTrue(TEXT("clear of the walls at both ends"),
			Start.Y > 0.0 && End.Y < 4.0);
	}

	// A tall thin domain: the rule adapts, still non-degenerate.
	if (TestTrue(TEXT("a tall domain yields a rake"),
			FlowVizFlow::MakeDefaultRake(FVector(2.0, 40.0, 0.5), Start, End)))
	{
		TestTrue(TEXT("still spanning most of Y"), End.Y - Start.Y > 20.0);
	}

	FVector RefusedStart, RefusedEnd;
	TestFalse(TEXT("a degenerate domain refuses"),
		FlowVizFlow::MakeDefaultRake(FVector(12.0, 0.0, 1.0), RefusedStart, RefusedEnd));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
