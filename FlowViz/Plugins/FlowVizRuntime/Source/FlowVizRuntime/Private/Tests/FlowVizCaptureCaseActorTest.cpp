// Copyright FlowViz contributors. All Rights Reserved.

#include "FlowVizCaptureLibrary.h"

#include "CFDViz/CFDVizManifest.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Scene/FlowVizCaseActor.h"
#include "Scene/FlowVizVolumeComponent.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED NAMESPACE, NOT ANONYMOUS. FlowVizRuntime is a unity build, so an
 * anonymous namespace here would merge with every other anonymous namespace in
 * the same blob rather than being file-local. That collision already shipped
 * once in this repo.
 */
namespace FlowVizCaptureCaseActorTest
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

	/** A world actors can be spawned into and components can register with. */
	UWorld* MakeWorld(FWorldContext& OutContext)
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
		OutContext.SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());
		return World;
	}
}

/**
 * A CASE ACTOR PLACED FROM SCRIPT MUST BE READY TO MARCH, NOT MERELY PRESENT.
 *
 * The gap this closes is the one that makes the whole headless capture
 * meaningless if it is left open. `ACFDVizCaseActor::BeginPlay` calls LoadCase
 * and never calls UploadFrame, and `UploadFrame` is not a UFUNCTION, so Python
 * cannot call it either. A case actor spawned the obvious way therefore has a
 * scene proxy, a wireframe box and a solid hull - and NO shader parameters, so
 * `DynamicData.bHasParameters` is false and the proxy never reaches the
 * dispatcher at all. It renders an opaque box that photographs beautifully.
 *
 * So "the actor exists" is not the requirement under test and would pass
 * against exactly the broken state that motivated this. The requirements are:
 *
 *  1. The component reports a renderable volume (the case really loaded).
 *  2. `TryMakeShaderParameters` SUCCEEDS - which is only true after an upload,
 *     and is the precondition of the dispatch actually happening.
 *  3. The bound field is a SCALAR, because a vector field's bytes never reach
 *     `Slot.ScalarTexture` and `DispatchVolumeRayMarch` early-returns on the
 *     invalid FieldTexture, silently.
 *  4. Asking for a vector field FAILS rather than half-succeeding, and asking
 *     for NAME_None fails rather than defaulting to `U`, which is a vector.
 *
 * Requirement 4 is what keeps this from being a test that cannot fail: the
 * sample really does contain 3-component fields (`U`, `vorticity`), so the
 * negative cases are reachable with the shipped fixture rather than hypothetical.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizSpawnCaseActorTest,
	"FlowViz.Capture.SpawnCaseActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizSpawnCaseActorTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizCaptureCaseActorTest;

	const FString CaseDir = GetSampleCaseDir();
	if (!TestFalse(TEXT("the sample case directory resolves"), CaseDir.IsEmpty()))
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	UWorld* World = MakeWorld(WorldContext);
	if (!TestNotNull(TEXT("test world was created"), World))
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		World->DestroyWorld(/*bInformEngineOfWorld*/ true);
		World->RemoveFromRoot();
		GEngine->DestroyWorldContext(World);
	};

	const FVector Location(100.0, 200.0, 300.0);
	const FRotator Rotation(0.0, 45.0, 0.0);

	/* --- The happy path, which is the one the capture depends on. ---------- */

	FString Error;
	ACFDVizCaseActor* Actor = UFlowVizCaptureLibrary::SpawnCaseActor(
		World, CaseDir, FName(TEXT("speed")), Location, Rotation, Error,
		/*FrameIndex*/ 0, /*bDrawBoundingBox*/ false);

	if (!TestNotNull(
			FString::Printf(TEXT("a case actor spawns for a scalar field: %s"), *Error),
			Actor))
	{
		return false;
	}

	TestEqual(TEXT("the actor belongs to the requested world"), Actor->GetWorld(), World);
	TestTrue(TEXT("the requested location was applied"),
		Actor->GetActorLocation().Equals(Location));
	TestTrue(TEXT("the requested rotation was applied"),
		Actor->GetActorRotation().Equals(Rotation));

	UCFDVizVolumeComponent* Volume = Actor->GetVolumeComponent();
	if (!TestNotNull(TEXT("the actor owns a volume component"), Volume))
	{
		return false;
	}

	TestTrue(TEXT("the component is registered with the world, so it can reach the render scene"),
		Volume->IsRegistered());
	TestTrue(TEXT("the case loaded and the component has a renderable volume"),
		Volume->HasRenderableVolume());
	TestEqual(TEXT("the requested field was bound, not the manifest's default"),
		Volume->GetCaseBinding().FieldId, FName(TEXT("speed")));
	TestFalse(TEXT("the wireframe box flag was applied as requested"),
		Volume->bDrawBoundingBox);

	/*
	 * THE HEADLINE ASSERTION. This is false for a spawned-but-not-uploaded
	 * actor, which is precisely the state BeginPlay leaves behind, and it is
	 * what gates the dispatch in FFlowVizVolumeSceneProxy::GetDynamicMeshElements.
	 */
	FFlowVizVolumeShaderParameters Params;
	TestTrue(
		TEXT("shader parameters are producible, so a frame was actually uploaded; without "
			 "this the proxy's bHasParameters is false and it never calls the dispatcher, "
			 "leaving an opaque hull box that looks exactly like a rendered volume"),
		Volume->TryMakeShaderParameters(Params));

	// The degenerate-domain defect this suite already fixed once, asserted here
	// too: at the seam Python actually uses, not only at the component's.
	TestTrue(
		FString::Printf(TEXT("the uploaded field has a non-degenerate colour domain [%f, %f]"),
			Params.ValueRangeMin, Params.ValueRangeMax),
		Params.ValueRangeMax > Params.ValueRangeMin);

	// The bound field must really be 1-component, or the dispatcher early-returns
	// on an invalid FieldTexture and nothing marches.
	const FCFDVizField* const BoundField =
		Volume->GetCaseBinding().Case.FindField(Volume->GetCaseBinding().FieldId);
	if (TestNotNull(TEXT("the bound field is present in the loaded manifest"), BoundField))
	{
		TestEqual(
			TEXT("the bound field is a scalar; a vector field leaves Slot.ScalarTexture null "
				 "and DispatchVolumeRayMarch early-returns without marching anything"),
			BoundField->ComponentCount, 1);
	}

	/* --- The negative cases, which are what make the check able to fail. --- */

	/*
	 * Each refusal below logs an Error, which is correct behaviour - a silent
	 * refusal would reproduce the very ambiguity this helper exists to remove -
	 * but the automation harness fails any test that emits one. Declaring them
	 * expected keeps the diagnostics loud in production while letting the test
	 * assert on them.
	 *
	 * DECLARED WITH EXACT COUNTS, NOT AS A BLANKET SUPPRESSION. A catch-all
	 * would also swallow an unexpected error from the happy path above, which is
	 * the case that actually matters; a count of 1 each means an extra failure
	 * still surfaces, and a MISSING one fails too - so these double as
	 * assertions that each refusal really fired.
	 */
	AddExpectedError(TEXT("has 3 components"),
		EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("no field was named"),
		EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("could not load '/nonexistent/NoSuchCase.cfdviz'"),
		EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("frame 100000 is outside the timeline"),
		EAutomationExpectedErrorFlags::Contains, 1);

	// A vector field must be REFUSED. The sample's `U` really is 3-component,
	// so this case is reachable rather than hypothetical.
	FString VectorError;
	TestNull(
		TEXT("a 3-component field is refused rather than silently producing a volume that "
			 "cannot march"),
		UFlowVizCaptureLibrary::SpawnCaseActor(
			World, CaseDir, FName(TEXT("U")), Location, Rotation, VectorError));
	TestTrue(TEXT("refusing a vector field explains why, naming the component count"),
		VectorError.Contains(TEXT("component")));

	// NAME_None must be refused rather than defaulted, because the default IS
	// a vector - the trap this whole helper exists to close.
	FString DefaultError;
	TestNull(
		TEXT("NAME_None is refused rather than defaulting to the manifest's first field, "
			 "which is the vector `U`"),
		UFlowVizCaptureLibrary::SpawnCaseActor(
			World, CaseDir, NAME_None, Location, Rotation, DefaultError));
	TestFalse(TEXT("refusing NAME_None explains why"), DefaultError.IsEmpty());

	// A bad path must fail with a reason, not spawn a blank actor.
	FString PathError;
	TestNull(TEXT("a nonexistent case directory spawns nothing"),
		UFlowVizCaptureLibrary::SpawnCaseActor(
			World, TEXT("/nonexistent/NoSuchCase.cfdviz"), FName(TEXT("speed")),
			Location, Rotation, PathError));
	TestFalse(TEXT("a failed load explains why"), PathError.IsEmpty());

	// A frame index outside the timeline must fail at the upload rather than
	// leaving a parameterless actor behind.
	FString FrameError;
	TestNull(TEXT("an out-of-range frame index spawns nothing"),
		UFlowVizCaptureLibrary::SpawnCaseActor(
			World, CaseDir, FName(TEXT("speed")), Location, Rotation, FrameError,
			/*FrameIndex*/ 100000));
	TestFalse(TEXT("a failed upload explains why"), FrameError.IsEmpty());

	return true;
}

/**
 * THE MARCHER TOGGLE MUST ACTUALLY MOVE THE GLOBAL, IN BOTH DIRECTIONS.
 *
 * This toggle is the single variable the capture verdict moves, so if it does
 * not work the verdict silently becomes "two identical captures differ: no" -
 * a FAIL that looks like a real finding about the renderer while actually being
 * a finding about the test harness.
 *
 * A one-directional check would not catch that. `SetVolumeRayMarcherEnabled(false)`
 * followed by an is-it-null assertion passes just as well if the function is
 * `SetDispatcher(nullptr)` unconditionally, ignoring its argument entirely -
 * and that implementation makes both capture states marcher-free, so the
 * scene and the control match and the run reports the marcher rendering
 * nothing. Hence: off, then on, then off again, asserting the transition each
 * time, and asserting that re-enabling restores the SAME pointer the module
 * installed rather than merely some non-null one.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizMarcherToggleTest,
	"FlowViz.Capture.MarcherToggle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizMarcherToggleTest::RunTest(const FString& Parameters)
{
	/*
	 * What module startup installed. Captured BEFORE anything is touched, and
	 * restored on exit: this global is shared with the whole engine, and leaving
	 * it null would make FlowViz.Render.Wiring - the one test that asserts
	 * production installs a dispatcher - fail for a reason that has nothing to
	 * do with production.
	 */
	IFlowVizVolumeRayMarchDispatcher* const Original = FlowVizVolumeRayMarch::GetDispatcher();

	ON_SCOPE_EXIT
	{
		FlowVizVolumeRayMarch::SetDispatcher(Original);
	};

	if (!TestNotNull(
			TEXT("module startup installed a dispatcher; without one this toggle has nothing "
				 "to restore and the capture control state is indistinguishable from the "
				 "scene state"),
			Original))
	{
		return false;
	}

	TestTrue(TEXT("the marcher reports as enabled to begin with"),
		UFlowVizCaptureLibrary::IsVolumeRayMarcherEnabled());

	// Off.
	const bool bAfterDisable = UFlowVizCaptureLibrary::SetVolumeRayMarcherEnabled(false);
	TestFalse(TEXT("disabling reports the marcher is no longer installed"), bAfterDisable);
	TestFalse(TEXT("disabling really uninstalls it"),
		UFlowVizCaptureLibrary::IsVolumeRayMarcherEnabled());
	TestNull(TEXT("the global itself is null, not merely reported as such"),
		FlowVizVolumeRayMarch::GetDispatcher());

	// On. The half that a hardcoded SetDispatcher(nullptr) would fail.
	const bool bAfterEnable = UFlowVizCaptureLibrary::SetVolumeRayMarcherEnabled(true);
	TestTrue(TEXT("enabling reports the marcher is installed"), bAfterEnable);
	TestTrue(TEXT("enabling really installs it"),
		UFlowVizCaptureLibrary::IsVolumeRayMarcherEnabled());

	// And it must be the production dispatcher, not any non-null pointer. A
	// freshly allocated one would leak and would not be what a placed volume
	// renders through.
	TestEqual(
		TEXT("re-enabling restores the same dispatcher module startup installed, rather than "
			 "some other non-null pointer"),
		FlowVizVolumeRayMarch::GetDispatcher(), Original);

	// Off again, so the toggle is shown to be repeatable rather than a
	// one-shot that happens to work in the order the first two cases used.
	TestFalse(TEXT("the toggle is repeatable: disabling a second time works"),
		UFlowVizCaptureLibrary::SetVolumeRayMarcherEnabled(false));
	TestNull(TEXT("the global is null again"), FlowVizVolumeRayMarch::GetDispatcher());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
