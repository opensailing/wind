// Copyright FlowViz contributors. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Scene/FlowVizCaseActor.h"
#include "Scene/FlowVizStudioRig.h"
#include "UI/FlowVizWorkspaceModel.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED namespace: unity build. An anonymous one here would merge with every
 * sibling test's rather than being file-local (#37).
 */
namespace FlowVizProfileTest
{
	UWorld* MakeWorld()
	{
		const FName WorldName = MakeUniqueObjectName(
			nullptr, UWorld::StaticClass(), NAME_None, EUniqueObjectNameOptions::GloballyUnique);

		UWorld* World = UWorld::CreateWorld(
			EWorldType::Game, /*bInformEngineOfWorld*/ false, WorldName, GetTransientPackage());
		if (World == nullptr)
		{
			return nullptr;
		}

		World->AddToRoot();
		FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
		WorldContext.SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());
		return World;
	}
}

/**
 * The Scientific/Presentation profile toggle (#83 / Milestone F).
 *
 * What it honestly does: owns the session's bPresentationMode and applies a
 * preset bundle on switch. What it does NOT do -- VISUAL_QA section 2's
 * film-grade bar -- is stated in its header rather than implied by the name.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizProfileTest,
	"FlowViz.UI.WorkspaceModel.Profiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizProfileTest::RunTest(const FString& Parameters)
{
	FFlowVizWorkspaceModel Model;

	/* == Scientific is the default -- the mode whose fidelity is stated ====== */
	TestFalse(TEXT("the default profile is Scientific (VISUAL_QA: Presentation must never "
				   "be the default for quantitative work)"),
		Model.IsPresentationMode());
	TestFalse(TEXT("and its render state is unlit"),
		Model.RenderSettings.IsLightingEnabled());

	/* == Entering Presentation applies the bundle ============================ */
	Model.SetPresentationMode(true);
	TestTrue(TEXT("the flag follows"), Model.IsPresentationMode());
	TestTrue(TEXT("presentation lights the volume"),
		Model.RenderSettings.IsLightingEnabled());
	TestTrue(TEXT("and enables jitter"), Model.RenderSettings.IsJitterEnabled());

	/* == The user may then adjust freely -- the profile applies ONCE ========= */
	Model.RenderSettings.SetLightingEnabled(false);
	TestTrue(TEXT("hand-disabling lighting does not flip the profile -- the profile is a "
				  "selection, not a constraint that fights the render panel"),
		Model.IsPresentationMode());

	/* == Entering Scientific SETS the honest state =========================== */
	Model.RenderSettings.SetLightingEnabled(true);
	Model.SetPresentationMode(false);
	TestFalse(TEXT("scientific turns lighting off -- rule 1: lighting must not modulate "
				   "apparent scalar value, whatever was tuned before"),
		Model.RenderSettings.IsLightingEnabled());
	TestFalse(TEXT("and jitter off (ADR 002)"), Model.RenderSettings.IsJitterEnabled());

	return true;
}


/**
 * The studio rig (renderer overhaul P7): the three-point grammar as a pure
 * description, and the apply/remove hop on a real actor.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizStudioRigTest,
	"FlowViz.Scene.StudioRig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizStudioRigTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizProfileTest;

	/* == The grammar, pinned pure =========================================== */
	TArray<FlowVizStudioRig::FLightDescription> Lights;
	FlowVizStudioRig::Describe(FVector(1200.0, 400.0, 100.0), Lights);
	if (!TestEqual(TEXT("three lights: key, fill, rim"), Lights.Num(), 3))
	{
		return false;
	}
	const FlowVizStudioRig::FLightDescription& Key = Lights[0];
	const FlowVizStudioRig::FLightDescription& Fill = Lights[1];
	const FlowVizStudioRig::FLightDescription& Rim = Lights[2];

	TestTrue(TEXT("the key is the brightest light -- the three-point grammar's "
				  "first rule"),
		Key.IntensityLux > Fill.IntensityLux && Key.IntensityLux > Rim.IntensityLux);
	TestTrue(TEXT("the fill is the dimmest"),
		Fill.IntensityLux < Key.IntensityLux && Fill.IntensityLux < Rim.IntensityLux);
	// Cool fill vs warm key: blue channel dominance flips between them.
	TestTrue(TEXT("the fill is COOLER than the key (higher blue-to-red ratio)"),
		Fill.Color.B / FMath::Max(Fill.Color.R, 0.01f)
			> Key.Color.B / FMath::Max(Key.Color.R, 0.01f));
	TestTrue(TEXT("the rim points UPSTREAM (-X) -- from behind the wake, so tube "
				  "silhouettes glow in the standard 3/4 view"),
		Rim.Direction.X < -0.5);
	TestTrue(TEXT("key and fill oppose across Y -- across the flow axis"),
		Key.Direction.Y * Fill.Direction.Y < 0.0);
	for (const FlowVizStudioRig::FLightDescription& Light : Lights)
	{
		TestTrue(TEXT("every light points DOWN (grazes, never uplights)"),
			Light.Direction.Z < 0.0);
	}

	/* == Apply / remove on a real actor ===================================== */
	UWorld* World = MakeWorld();
	if (!TestNotNull(TEXT("a world exists"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		World->DestroyWorld(/*bInformEngineOfWorld*/ true);
		World->RemoveFromRoot();
		GEngine->DestroyWorldContext(World);
	};

	ACFDVizCaseActor* Actor = World->SpawnActor<ACFDVizCaseActor>();
	if (TestNotNull(TEXT("an actor spawns"), Actor))
	{
		TestFalse(TEXT("CONTROL: no rig on a fresh actor"),
			FlowVizStudioRig::IsApplied(*Actor));

		FlowVizStudioRig::Apply(*Actor);
		TestTrue(TEXT("Apply installs the rig"), FlowVizStudioRig::IsApplied(*Actor));

		int32 RigLightCount = 0;
		for (UActorComponent* Component : Actor->GetComponents())
		{
			if (Cast<UDirectionalLightComponent>(Component) != nullptr)
			{
				++RigLightCount;
			}
		}
		TestEqual(TEXT("three directional lights exist"), RigLightCount, 3);

		FlowVizStudioRig::Apply(*Actor);
		int32 AfterSecondApply = 0;
		for (UActorComponent* Component : Actor->GetComponents())
		{
			if (Cast<UDirectionalLightComponent>(Component) != nullptr)
			{
				++AfterSecondApply;
			}
		}
		TestEqual(TEXT("Apply is idempotent -- no duplicate rigs"), AfterSecondApply, 3);

		FlowVizStudioRig::Remove(*Actor);
		TestFalse(TEXT("Remove strips it"), FlowVizStudioRig::IsApplied(*Actor));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
