// Copyright FlowViz contributors. All Rights Reserved.

#include "FlowVizCaptureLibrary.h"

#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The scene-proxy diagnostic must actually track the render scene.
 *
 * This test exists because the previous version of this diagnostic did not. It
 * counted UPrimitiveComponents whose SceneProxy pointer was non-null, which
 * sounds equivalent and is not: destroyed actors are not garbage collected
 * immediately, so their components linger with stale pointers and the count
 * does not fall. That made it report a healthy 16 primitives for a scene that
 * provably rendered nothing, which is worse than having no diagnostic at all -
 * it actively argued against the correct diagnosis.
 *
 * So the requirement under test is not "returns a plausible number". It is
 * **the count must respond to the scene changing in both directions**. A
 * diagnostic that cannot fall cannot detect an empty scene, which is the single
 * thing it is for.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizSceneProxyCountTest,
	"FlowViz.Capture.SceneProxyCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizSceneProxyCountTest::RunTest(const FString& Parameters)
{
	// Follows Engine/Source/Developer/CQTest ActorTestSpawner. The world context
	// and InitializeActorsForPlay are not ceremony: without them components do
	// not register, so the scene stays empty and this test would "reproduce" a
	// bug that is only in its own setup.
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

	// A world with no real FScene cannot answer the question at all. Saying so
	// is the correct behaviour and is distinct from honestly reporting zero -
	// which is the whole contract, so assert it rather than only skipping.
	const int32 Baseline = UFlowVizCaptureLibrary::GetSceneProxyCount(World);

	if (World->Scene == nullptr || World->Scene->GetRenderScene() == nullptr)
	{
		TestEqual(TEXT("a world with no real render scene reports INDEX_NONE, not a false zero"),
			Baseline, INDEX_NONE);
		// The literal "SKIPPED" is load-bearing: Tools/run_tests.sh's skip
		// listing keys on it, and a message without it leaves this partial run
		// counted as a full green - the project's own "green totals can hide
		// skips" hazard.
		AddInfo(TEXT("SKIPPED (partially): no real FScene on this world - expected under "
			"-nullrhi, and also whenever GIsClient is false or FApp::CanEverRender() is "
			"false. The INDEX_NONE no-render-scene contract arm above DID run and passed; "
			"the differential arms (add/remove geometry moves the count) need a real "
			"scene and did NOT run. Run with RHI=1 to exercise them."));
		return true;
	}

	TestTrue(TEXT("baseline count is a real answer, not an error"), Baseline >= 0);

	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("engine cube mesh loaded"), Mesh))
	{
		return false;
	}

	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>();
	if (!TestNotNull(TEXT("static mesh actor spawned"), Actor))
	{
		return false;
	}

	// Mobility must be set before the mesh is assigned; a Static component in a
	// world that never runs a build cannot accept the change afterwards.
	Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
	Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);

	UFlowVizCaptureLibrary::FlushSceneUpdates(World);

	UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
	AddInfo(FString::Printf(
		TEXT("component: registered=%d visible=%d hasProxy=%d worldInitialized=%d"),
		Component->IsRegistered() ? 1 : 0,
		Component->IsVisible() ? 1 : 0,
		Component->SceneProxy != nullptr ? 1 : 0,
		World->bIsWorldInitialized ? 1 : 0));
	UFlowVizCaptureLibrary::LogPrimitiveBreakdown(World);

	const int32 WithActor = UFlowVizCaptureLibrary::GetSceneProxyCount(World);
	TestTrue(
		FString::Printf(TEXT("adding a mesh raises the count (%d -> %d)"), Baseline, WithActor),
		WithActor > Baseline);

	// The assertion the old implementation failed. Deleting geometry must be
	// observable, or the diagnostic cannot distinguish a populated scene from an
	// empty one - which is exactly the confusion it was built to resolve.
	World->DestroyActor(Actor);
	UFlowVizCaptureLibrary::FlushSceneUpdates(World);

	const int32 AfterRemoval = UFlowVizCaptureLibrary::GetSceneProxyCount(World);
	TestEqual(
		FString::Printf(TEXT("removing the mesh returns to baseline (%d -> %d)"), WithActor, AfterRemoval),
		AfterRemoval, Baseline);

	return true;
}

/**
 * Spawning a capture camera must be possible from script.
 *
 * This is the gap that blocked headless capture entirely. Every Blueprint
 * spawn entry point - `SpawnActorFromClass`, `BeginDeferredActorSpawnFromClass`,
 * `AddComponentByClass` - is marked `BlueprintInternalUseOnly`, so none of them
 * are exposed to Python. Constructing a `SceneCaptureComponent2D` in Python
 * does succeed, but the object lands in `/Engine/Transient` with no world, and
 * `RegisterComponent` is not a UFUNCTION, so it can never be attached to one.
 * Such a component captures nothing and reports no error - it silently renders
 * a black frame, which is indistinguishable from a broken GPU path.
 *
 * Hence the requirement: the returned actor must be **in the requested world
 * and registered**, not merely non-null. A test that only checked for non-null
 * would have passed against the orphaned-transient-component case that caused
 * the original failure.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizSpawnSceneCaptureTest,
	"FlowViz.Capture.SpawnSceneCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizSpawnSceneCaptureTest::RunTest(const FString& Parameters)
{
	// Logging an error on an unusable context is correct behaviour, but the
	// automation harness fails any test that emits one, so the probe below would
	// otherwise report a failure for doing exactly what it should.
	AddExpectedError(TEXT("could not resolve a world from the supplied context object"),
		EAutomationExpectedErrorFlags::Contains, 1);

	// A null world context must yield null. The caller has to be able to tell
	// "could not spawn" from "spawned but captures nothing"; returning a
	// dangling actor here would collapse that distinction.
	TestNull(TEXT("a null world context spawns nothing"),
		UFlowVizCaptureLibrary::SpawnSceneCapture2D(
			nullptr, FVector::ZeroVector, FRotator::ZeroRotator, 90.0f));

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

	const FVector Location(-400.0, 0.0, 150.0);
	const FRotator Rotation(-15.0, 0.0, 0.0);
	const float FOV = 75.0f;

	ASceneCapture2D* Capture =
		UFlowVizCaptureLibrary::SpawnSceneCapture2D(World, Location, Rotation, FOV);

	if (!TestNotNull(TEXT("scene capture actor spawned"), Capture))
	{
		return false;
	}

	TestEqual(TEXT("capture actor belongs to the requested world"), Capture->GetWorld(), World);

	USceneCaptureComponent2D* Component = Capture->GetCaptureComponent2D();
	if (!TestNotNull(TEXT("capture actor has a capture component"), Component))
	{
		return false;
	}

	// The orphaned-component failure mode in one assertion.
	TestTrue(TEXT("capture component is registered with a world"), Component->IsRegistered());
	TestEqual(TEXT("capture component belongs to the requested world"),
		Component->GetWorld(), World);

	TestEqual(TEXT("requested FOV was applied"), Component->FOVAngle, FOV);
	TestTrue(TEXT("requested location was applied"),
		Capture->GetActorLocation().Equals(Location));
	TestTrue(TEXT("requested rotation was applied"),
		Capture->GetActorRotation().Equals(Rotation));

	// Capturing on every tick would make results depend on when the capture is
	// read, which is untestable and unreproducible. Callers drive it explicitly.
	TestFalse(TEXT("capture is explicit, not every-frame"), Component->bCaptureEveryFrame);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
