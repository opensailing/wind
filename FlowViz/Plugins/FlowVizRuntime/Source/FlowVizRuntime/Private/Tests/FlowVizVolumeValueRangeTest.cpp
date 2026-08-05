// Copyright FlowViz contributors. All Rights Reserved.

#include "Scene/FlowVizVolumeComponent.h"

#include "CFDViz/CFDVizManifest.h"
#include "ConvexVolume.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PrimitiveSceneProxy.h"
#include "Render/FlowVizVolumeTexture.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * DOES THE VOLUME THE MARCHER IS HANDED CARRY A USABLE COLOUR DOMAIN?
 *
 * This exists because a volume can render, cover the screen, cost a full
 * dispatch, and still be a flat single-coloured block - and that block is the
 * hardest kind of wrong to notice, because a ray-marched volume of a smooth
 * field genuinely IS fairly uniform in places. "It rendered" and "it rendered
 * the data" are different claims and only the first has been testable.
 *
 * THE MECHANISM. FFlowVizVolumeTransform::MakeShaderParameters is a pure
 * function of a grid and a texture layout. Neither of those carries a field's
 * value range, so it wrote
 *
 *     Params.ValueRangeMin = 0.0f;
 *     Params.ValueRangeMax = 0.0f;
 *
 * as placeholders. The .usf normalises through
 *
 *     if (ValueRangeMax > ValueRangeMin) { T = (Value - Min) / (Max - Min); }
 *     T = saturate(T);
 *
 * so with min == max the guard never fires, T stays 0 for every voxel, and
 * every sample in the volume reads LUT entry 0. The range-classification blocks
 * at .usf:367 and :813 are guarded on the same comparison, so no UNDER_RANGE or
 * OVER_RANGE bit fires either - the reason channel agrees that everything is
 * fine. The volume is uniformly the colormap's darkest colour and nothing
 * anywhere reports a problem.
 *
 * WHY THE PIXEL TESTS CANNOT CATCH IT. FlowVizVolumeMarchTest and
 * FlowVizReasonChannelTest both drive the shader with parameters they build
 * THEMSELVES, and both set a real range explicitly (see
 * FlowVizVolumeMarchTest.cpp:554). They are testing the marcher, correctly, and
 * they are structurally incapable of observing what the production data path
 * puts in that field - the same shape of defect that let the ray-march seam sit
 * unwired while the suite was green.
 *
 * WHY A DEGENERATE RANGE DEFEATS THE CAPTURE VERDICT TOO. Tools/capture's
 * differential asks whether the marcher contributed pixels. A flat block
 * contributes plenty. Repo memory: degenerate data defeats assertions - a
 * uniform fixture makes a correct and a broken renderer produce the same image.
 * The range is the thing that makes the fixture non-degenerate, so it has to be
 * asserted here, on the CPU, where the numbers are readable.
 *
 * THE ASSERTIONS ARE DIFFERENTIAL, NOT "IS IT SET". A test that only checked
 * `Max > Min` would pass on [0, 1] hardcoded anywhere in the chain. So this
 * file asserts the range MATCHES THE MANIFEST's declared statistics for the
 * bound field, and separately that two DIFFERENT fields of the same case
 * produce DIFFERENT ranges - which no constant can satisfy.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizVolumeValueRangeTest,
	"FlowViz.Scene.VolumeValueRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

namespace FlowVizVolumeValueRangeTestHelpers
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
}

bool FFlowVizVolumeValueRangeTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizVolumeValueRangeTestHelpers;

	const FString CaseDir = GetSampleCaseDir();

	/*
	 * THE FIXTURE'S OWN RANGES, TRANSCRIBED FROM manifest.json BY HAND rather
	 * than read back through the loader this test is checking. `speed` is
	 * declared globalComponentMin 0.92919921875 / max 13.4921875;
	 * `passiveScalar` is 0.0 / 1.0. Reading them from the parsed manifest would
	 * make this assertion true of any self-consistent mistake, including the
	 * loader dropping statistics entirely and both sides seeing zeros.
	 *
	 * They are also chosen to be MUTUALLY DISTINCT and NOT [0,1]-shaped for
	 * `speed`, so a hardcoded unit domain fails the first comparison and a
	 * hardcoded anything fails the differential at the end.
	 */
	constexpr double SpeedMin = 0.92919921875;
	constexpr double SpeedMax = 13.4921875;
	constexpr double PassiveScalarMin = 0.0;
	constexpr double PassiveScalarMax = 1.0;

	/*
	 * `pressure` IS THE DISCRIMINATING CASE, and it is here deliberately.
	 *
	 * Its declared COMPONENT range is [-62.9375, 27.6875] and its declared
	 * MAGNITUDE range is [9.870529174804688e-05, 62.9375]. Those differ, and
	 * only one of them is right: FlowVizRayMarch::FillDefaults sets
	 * ComponentMode = Magnitude, and .usf FlowVizExtractScalar under that mode
	 * returns sqrt(x*x) = |x| for a one-component field. So the value being
	 * coloured is the magnitude, and the domain must be the magnitude's.
	 *
	 * Colouring |pressure| through the component domain would map the entire
	 * field into the upper half of [-62.94, 27.69] - every sample at or above
	 * the neutral point - which renders as a smooth, plausible, completely wrong
	 * picture with nothing on screen to say so. `speed` and `passiveScalar`
	 * cannot catch that, because they are non-negative and their two ranges
	 * coincide. This field is the only one here that can fail that way.
	 */
	constexpr double PressureMin = 9.870529174804688e-05;
	constexpr double PressureMax = 62.9375;

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

	/*
	 * THE MANIFEST MUST ACTUALLY DECLARE WHAT THIS TEST ASSUMES.
	 *
	 * Checked first and separately, because if the sample ever loses its
	 * statistics block then every assertion below would be testing the fallback
	 * path while appearing to test the real one. An absent statistic is not a
	 * zero (FCFDVizFieldStatistics's own comment), and this is where that
	 * distinction gets enforced rather than assumed.
	 */
	{
		FCFDVizCase Case;
		const FCFDVizResult LoadResult =
			FCFDVizCase::LoadFromFile(FPaths::Combine(CaseDir, TEXT("manifest.json")), Case);
		if (!TestTrue(
				FString::Printf(TEXT("the sample manifest parses: %s"), *LoadResult.ToString()),
				LoadResult.IsOk()))
		{
			return false;
		}

		const FCFDVizField* Speed = Case.FindField(FName(TEXT("speed")));
		if (!TestNotNull(TEXT("the sample declares a 'speed' field"), Speed))
		{
			return false;
		}

		TestTrue(TEXT("'speed' declares a component range in the manifest; without one this "
			"whole test would be exercising a fallback and reporting it as the real path"),
			Speed->Statistics.bHasComponentRange);

		// Through the shared statistics type, which is where the flag-gated
		// accessors live - FCFDVizFieldStatistics deliberately exposes only raw
		// arrays plus the flags, so that reading them without checking is not
		// convenient.
		FCFDVizStatistics Converted;
		double DeclaredMin = 0.0;
		double DeclaredMax = 0.0;
		if (TestTrue(TEXT("'speed' statistics convert to the shared type"),
				Speed->Statistics.TryMakeStatistics(1, Converted))
			&& TestTrue(TEXT("'speed' component 0 has a usable declared range"),
				Converted.TryGetComponentRange(0, DeclaredMin, DeclaredMax)))
		{
			// Pins the hand-transcribed constants above against the file, so a
			// change to the sample fails HERE with a clear message rather than
			// as a confusing mismatch further down.
			TestEqual(TEXT("the transcribed 'speed' minimum still matches the manifest"),
				DeclaredMin, SpeedMin, 1e-9);
			TestEqual(TEXT("the transcribed 'speed' maximum still matches the manifest"),
				DeclaredMax, SpeedMax, 1e-9);
		}
	}

	/*
	 * THE ACTUAL SUBJECT: what the shader parameter block carries after a real
	 * load and a real upload of a real frame.
	 */
	struct FFieldExpectation
	{
		const TCHAR* FieldId;
		double ExpectedMin;
		double ExpectedMax;
	};

	const FFieldExpectation Expectations[] = {
		{TEXT("speed"), SpeedMin, SpeedMax},
		{TEXT("passiveScalar"), PassiveScalarMin, PassiveScalarMax},
		// The one whose component and magnitude ranges disagree - see above.
		{TEXT("pressure"), PressureMin, PressureMax},
	};

	// Kept so the two fields can be compared against each other at the end. A
	// per-field assertion alone cannot catch a constant that happens to equal
	// one field's range.
	TArray<FVector2D> Observed;

	int32 Checked = 0;

	for (const FFieldExpectation& Expectation : Expectations)
	{
		UCFDVizVolumeComponent* Volume = NewObject<UCFDVizVolumeComponent>(World);
		if (!TestNotNull(TEXT("a volume component is constructed"), Volume))
		{
			return false;
		}
		Volume->RegisterComponentWithWorld(World);

		ON_SCOPE_EXIT
		{
			Volume->UnregisterComponent();
		};

		const FCFDVizResult LoadResult = Volume->LoadCase(CaseDir, FName(Expectation.FieldId));
		if (!TestTrue(
				FString::Printf(TEXT("the sample case loads bound to '%s': %s"),
					Expectation.FieldId, *LoadResult.ToString()),
				LoadResult.IsOk()))
		{
			continue;
		}

		/*
		 * UploadFrame is what fills UploadedScalarLayout, and without it
		 * TryMakeShaderParameters returns false - so a test that skipped the
		 * upload would observe "no parameters" and could not tell that from
		 * "parameters with a dead range". The upload is the precondition of the
		 * question, not a detail.
		 */
		const FCFDVizResult UploadResult = Volume->UploadFrame(0);
		if (!TestTrue(
				FString::Printf(TEXT("frame 0 of '%s' uploads: %s"),
					Expectation.FieldId, *UploadResult.ToString()),
				UploadResult.IsOk()))
		{
			continue;
		}

		FFlowVizVolumeShaderParameters Params;
		if (!TestTrue(
				FString::Printf(
					TEXT("shader parameters are producible for '%s' after an upload"),
					Expectation.FieldId),
				Volume->TryMakeShaderParameters(Params)))
		{
			continue;
		}

		/*
		 * THE HEADLINE ASSERTION. A zero-width domain makes the .usf's
		 * normalisation guard never fire, so every voxel reads LUT entry 0 and
		 * the volume renders as one flat colour with no reason bit raised.
		 */
		TestTrue(
			FString::Printf(
				TEXT("'%s' has a NON-DEGENERATE colour domain (got [%f, %f]); with max == min "
					 "the .usf normalisation guard never fires and every voxel reads LUT entry 0, "
					 "which renders a uniform block that no reason bit discloses"),
				Expectation.FieldId,
				Params.ValueRangeMin,
				Params.ValueRangeMax),
			Params.ValueRangeMax > Params.ValueRangeMin);

		// And it must be the field's OWN range, not merely some usable one.
		// [0,1] hardcoded anywhere would pass the check above.
		TestEqual(
			FString::Printf(TEXT("'%s' colour domain minimum comes from the manifest"),
				Expectation.FieldId),
			static_cast<double>(Params.ValueRangeMin), Expectation.ExpectedMin, 1e-5);
		TestEqual(
			FString::Printf(TEXT("'%s' colour domain maximum comes from the manifest"),
				Expectation.FieldId),
			static_cast<double>(Params.ValueRangeMax), Expectation.ExpectedMax, 1e-5);

		Observed.Add(FVector2D(Params.ValueRangeMin, Params.ValueRangeMax));
		++Checked;
	}

	/*
	 * COUNT WHAT WAS VERIFIED, NOT WHAT RETURNED GREEN. Every assertion above
	 * lives inside a loop that `continue`s on a failed precondition, so an
	 * empty or short match set would otherwise sail through as a pass - repo
	 * memory: an empty match set passes every check.
	 */
	TestEqual(TEXT("both fields were actually checked, so this result is not an empty match set "
		"reporting green"), Checked, static_cast<int32>(UE_ARRAY_COUNT(Expectations)));

	/*
	 * THE CHECK NO CONSTANT CAN SATISFY.
	 *
	 * Two different fields of the same case, on the same grid, with the same
	 * layout, must produce DIFFERENT colour domains. Any value hardcoded in
	 * MakeShaderParameters - 0/0, 0/1, or a range copied from the wrong place -
	 * gives the same answer twice and fails here even if it somehow satisfied
	 * an equality above.
	 */
	for (int32 I = 0; I < Observed.Num(); ++I)
	{
		for (int32 J = I + 1; J < Observed.Num(); ++J)
		{
			TestTrue(
				FString::Printf(
					TEXT("'%s' and '%s' produce DIFFERENT colour domains ([%f, %f] vs [%f, %f]); "
						 "an equal pair would mean the range is a constant rather than the bound "
						 "field's own statistics"),
					Expectations[I].FieldId, Expectations[J].FieldId,
					Observed[I].X, Observed[I].Y, Observed[J].X, Observed[J].Y),
				!Observed[I].Equals(Observed[J], 1e-6));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
