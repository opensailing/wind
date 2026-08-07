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
#include "Scene/FlowVizSurfaceMeshComponent.h"
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

	/* -- The obstacle rides the load (renderer overhaul P2) ----------------- */
	//
	// LoadCase queues a worker build of the boundary patches; the apply lands
	// on the game thread via AsyncTask. In this headless test that hop is a
	// game-thread task we can pump to completion deterministically.
	{
		UCFDVizSurfaceMeshComponent* Obstacle = Actor->GetObstacleComponent();
		if (TestNotNull(TEXT("the actor owns an obstacle component"), Obstacle))
		{
			const double Deadline = FPlatformTime::Seconds() + 5.0;
			while (Obstacle->GetSectionCount() == 0 && FPlatformTime::Seconds() < Deadline)
			{
				FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
				FPlatformProcess::Sleep(0.01f);
			}

			// The sample declares 4 patches across two meshes: cylinderWall,
			// inlet, outlet, sideWalls. Every one becomes a section.
			TestEqual(TEXT("every declared boundary patch became a mesh section"),
				Obstacle->GetSectionCount(), 4);

			// The manifest's defaultVisible is applied, not merely recorded:
			// sideWalls (patch 3) declares false and must arrive hidden.
			int32 SideWallsIndex = INDEX_NONE;
			int32 WallIndex = INDEX_NONE;
			for (int32 Index = 0; Index < Obstacle->GetSectionCount(); ++Index)
			{
				if (Obstacle->GetSectionId(Index) == 3u) { SideWallsIndex = Index; }
				if (Obstacle->GetSectionId(Index) == 4u) { WallIndex = Index; }
			}
			if (TestTrue(TEXT("the sideWalls patch is addressable by id"),
					SideWallsIndex != INDEX_NONE))
			{
				TestFalse(TEXT("sideWalls arrives hidden -- defaultVisible applied"),
					Obstacle->IsSectionVisible(SideWallsIndex));
			}
			if (TestTrue(TEXT("the cylinder wall is addressable by id"),
					WallIndex != INDEX_NONE))
			{
				TestTrue(TEXT("the cylinder wall arrives visible"),
					Obstacle->IsSectionVisible(WallIndex));
			}

			// Opaque and shadowed: the compositing rules as component state.
			TestTrue(TEXT("the obstacle casts shadows -- it is real scene geometry"),
				Obstacle->CastShadow != 0);
		}
	}

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

/**
 * A COMPOSITE MODE SELECTED FROM SCRIPT MUST REACH THE PLACED VOLUME.
 *
 * The last link in a chain that is verified everywhere else. Below this point
 * everything is now mutation-verified end to end:
 *
 *   component -> payload ....... FlowViz.Scene.ProxySettings
 *   payload -> context ......... FlowViz.Scene.DispatchContext
 *   context -> shader params ... FlowViz.Render.SettingsSeam
 *
 * And ABOVE it, nothing. UCFDVizVolumeComponent::SetRenderSettings had exactly
 * one caller in the whole module and it was a test. So the sixteen parameters
 * that #39 unfroze were reachable from C++ and from nowhere a user or a capture
 * script could stand: every shipped frame still composited Alpha, unlit.
 *
 * That is the same defect shape three times running -- a verified producer, a
 * verified consumer, and no test on the join -- which is why this asserts the
 * JOIN and not the halves. Reading back through SetVolumeCompositeMode's own
 * getter would pass against a library that stored the value in a static and
 * never touched the component, so every assertion here reads the COMPONENT, and
 * the last one reads the marshalled payload the render thread actually gets.
 *
 * WHY int32 AND NOT THE ENUM: EFlowVizCompositeMode is a plain enum class, not
 * a UENUM, because its values are pinned to the FLOWVIZ_MODE_* defines in the
 * .usf and a UENUM would add a second place for them to drift. A UFUNCTION
 * cannot take it, so the raw value is validated by SetCompositeModeByValue,
 * which refuses anything outside the enum rather than passing it to a shader
 * that would switch to a default branch nobody selected.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizCaptureRenderSettingsTest,
	"FlowViz.Capture.RenderSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizCaptureRenderSettingsTest::RunTest(const FString& Parameters)
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

	FString Error;
	ACFDVizCaseActor* Actor = UFlowVizCaptureLibrary::SpawnCaseActor(
		World, CaseDir, FName(TEXT("speed")), FVector::ZeroVector, FRotator::ZeroRotator,
		Error, /*FrameIndex*/ 0, /*bDrawBoundingBox*/ false);

	if (!TestNotNull(
			FString::Printf(TEXT("a case actor spawns for a scalar field: %s"), *Error), Actor))
	{
		return false;
	}

	UCFDVizVolumeComponent* Volume = Actor->GetVolumeComponent();
	if (!TestNotNull(TEXT("the actor owns a volume component"), Volume))
	{
		return false;
	}

	/* == THE IDENTITY CONTROL, FIRST ========================================= */
	//
	// A spawned actor that nobody has configured must still composite Alpha,
	// unlit. Without this, an implementation that forced some mode of its own at
	// spawn would satisfy every assertion below while changing every existing
	// capture -- and the shipped reference images would silently stop matching.
	{
		TestEqual(
			TEXT("a freshly spawned case composites Alpha, so adding this control changed no "
				 "existing capture"),
			static_cast<int32>(Volume->GetRenderSettings().GetCompositeMode()),
			static_cast<int32>(EFlowVizCompositeMode::Alpha));
		TestFalse(TEXT("and renders unlit, which is the Scientific profile's default"),
			Volume->GetRenderSettings().IsLightingEnabled());
	}

	/* == THE HEADLINE: the mode reaches the component and its payload ======== */
	{
		const bool bSet = UFlowVizCaptureLibrary::SetVolumeCompositeMode(
			Actor, static_cast<int32>(EFlowVizCompositeMode::IsoSurface), /*IsoValue*/ 2.5f);

		TestTrue(TEXT("a valid composite mode is accepted"), bSet);

		// Read the COMPONENT, not the library. A library that kept the value in a
		// static of its own would pass a round-trip through its own getter.
		TestEqual(
			TEXT("the mode reaches the placed component, which is the object the renderer reads"),
			static_cast<int32>(Volume->GetRenderSettings().GetCompositeMode()),
			static_cast<int32>(EFlowVizCompositeMode::IsoSurface));
		TestEqual(TEXT("so does the iso value the mode is meaningless without"),
			Volume->GetRenderSettings().GetIsoValue(), 2.5f);

		// And through to the render-thread payload, which is what a frame is
		// actually composited from. Two links in one assertion is deliberate:
		// this is the seam where the whole chain was severed before.
		const FFlowVizVolumeProxyDynamicData Data = Volume->MakeProxyDynamicData();
		TestEqual(TEXT("and reaches the marshalled payload the render thread receives"),
			static_cast<int32>(Data.RenderSettings.GetCompositeMode()),
			static_cast<int32>(EFlowVizCompositeMode::IsoSurface));
	}

	/* == Lighting is a SEPARATE control, not a rider on the mode ============= */
	//
	// Setting the mode must not disturb it, and setting it must not disturb the
	// mode. A single "apply settings" call that rebuilt the view model from
	// scratch would pass each of the two blocks above in isolation and fail here.
	{
		TestTrue(TEXT("lighting can be turned on"),
			UFlowVizCaptureLibrary::SetVolumeLightingEnabled(Actor, true));
		TestTrue(TEXT("the component reports lighting on"),
			Volume->GetRenderSettings().IsLightingEnabled());
		TestEqual(TEXT("and the composite mode set earlier SURVIVED the lighting change"),
			static_cast<int32>(Volume->GetRenderSettings().GetCompositeMode()),
			static_cast<int32>(EFlowVizCompositeMode::IsoSurface));

		TestTrue(TEXT("a second mode change is accepted"),
			UFlowVizCaptureLibrary::SetVolumeCompositeMode(
				Actor, static_cast<int32>(EFlowVizCompositeMode::Maximum), /*IsoValue*/ 2.5f));
		TestEqual(TEXT("the new mode took"),
			static_cast<int32>(Volume->GetRenderSettings().GetCompositeMode()),
			static_cast<int32>(EFlowVizCompositeMode::Maximum));
		TestTrue(TEXT("and lighting SURVIVED the mode change"),
			Volume->GetRenderSettings().IsLightingEnabled());
	}

	/* == Refusal, and it must keep the previous value ======================== */
	//
	// The direction that makes the acceptances above mean something. An
	// out-of-range value handed to the shader falls through to a default branch
	// and renders as a mode nobody selected -- a broken-shader picture produced
	// by a rejected input. Asserting the mode is UNCHANGED, not merely that the
	// call returned false, is what distinguishes "refused" from "refused and
	// also cleared".
	{
		AddExpectedError(TEXT("is not a composite mode"),
			EAutomationExpectedErrorFlags::Contains, 2);

		TestFalse(TEXT("a value outside the enum is refused"),
			UFlowVizCaptureLibrary::SetVolumeCompositeMode(Actor, 99, /*IsoValue*/ 2.5f));
		TestEqual(TEXT("and the previous mode is kept rather than cleared"),
			static_cast<int32>(Volume->GetRenderSettings().GetCompositeMode()),
			static_cast<int32>(EFlowVizCompositeMode::Maximum));

		/*
		 * NEGATIVE, WHICH NOTHING HERE USED TO PASS.
		 *
		 * Found by mutation: deleting the `CompositeMode < 0` half of the guard
		 * SURVIVED this suite, because every rejection case above was positive.
		 * A survivor cannot distinguish "the guard is redundant" from "the guard
		 * is untested", and those want opposite responses.
		 *
		 * MEASURED, and the guard IS redundant for correctness: static_cast to
		 * uint32 sends -1 to 4294967295, -2 to 4294967294 and INT32_MIN to
		 * 2147483648, and SetCompositeModeByValue's exhaustive switch refuses
		 * every one -- no negative int32 can alias a mode. The mutant is
		 * equivalent, and this assertion is therefore expected to stay green
		 * whether the guard is there or not.
		 *
		 * BOTH ARE KEPT ANYWAY, for different reasons. The guard stays because
		 * its redundancy depends entirely on the switch being exhaustive: the
		 * moment someone adds a range check "for simplicity", -1 becomes a huge
		 * value under whatever `<=` bound they wrote, and the guard is what
		 * still refuses it. The assertion stays because it pins the BEHAVIOUR
		 * (negatives are refused, the mode is kept) rather than the guard, so it
		 * goes red on that day regardless of which of the two is edited.
		 */
		TestFalse(TEXT("a negative value is refused too -- it casts to a huge uint32, not to a mode"),
			UFlowVizCaptureLibrary::SetVolumeCompositeMode(Actor, -1, /*IsoValue*/ 2.5f));
		TestEqual(TEXT("and it also keeps the previous mode"),
			static_cast<int32>(Volume->GetRenderSettings().GetCompositeMode()),
			static_cast<int32>(EFlowVizCompositeMode::Maximum));
	}

	/* == A non-finite ISO VALUE is refused, and refused by this caller ======== */
	//
	// FOUND BY MUTATION, and it was hidden twice over. The arm that discards
	// SetIsoValue's return -- applying the value but ignoring the refusal --
	// first scored INVALID (it was written as unreachable code, which does not
	// compile here), and an INVALID reads in a summary exactly like an arm
	// nobody wrote. Rewritten so it compiles, it then SURVIVED: nothing in this
	// suite asserted that this function propagates the refusal.
	//
	// WHY THE PRIMITIVE'S OWN TEST IS NOT ENOUGH. FlowViz.UI.RenderSettings
	// already proves SetIsoValue refuses NaN. That is the same two-verified-
	// halves-with-an-unverified-join shape that left the render settings
	// unwired: the setter refuses, and separately this function is supposed to
	// act on that refusal, and nothing checked the second part.
	//
	// WHAT A DROPPED REFUSAL LOOKS LIKE: an infinite or NaN threshold is never
	// crossed, so the iso-surface renders EMPTY -- which is indistinguishable
	// on screen from a correctly configured surface whose data is out of range.
	// The user is sent to check their case file for a defect in their input
	// handling.
	{
		AddExpectedError(TEXT("iso value"), EAutomationExpectedErrorFlags::Contains, 2);

		// Establish a known-good state first, so a refusal that also CLEARS is
		// distinguishable from one that leaves things alone.
		TestTrue(TEXT("a finite iso value with a valid mode is accepted"),
			UFlowVizCaptureLibrary::SetVolumeCompositeMode(
				Actor, static_cast<int32>(EFlowVizCompositeMode::IsoSurface), /*IsoValue*/ 2.5f));
		TestEqual(TEXT("and it reaches the volume"),
			Volume->GetRenderSettings().GetIsoValue(), 2.5f);

		// THE ARM THE MUTANT SURVIVED. NaN via sqrt(-1) rather than a literal,
		// matching how FlowViz.UI.RenderSettings builds one.
		TestFalse(TEXT("a NaN iso value is refused by the capture entry point, not just by the setter"),
			UFlowVizCaptureLibrary::SetVolumeCompositeMode(
				Actor, static_cast<int32>(EFlowVizCompositeMode::IsoSurface), FMath::Sqrt(-1.0f)));
		TestEqual(TEXT("and the previous iso value survives the refusal"),
			Volume->GetRenderSettings().GetIsoValue(), 2.5f);

		// Infinity too: SetIsoValue tests IsFinite rather than !IsNaN, and an
		// infinite threshold is never crossed either. Without this case, a guard
		// weakened to a NaN-only check passes everything above.
		TestFalse(TEXT("an infinite iso value is refused as well -- finite, not merely non-NaN"),
			UFlowVizCaptureLibrary::SetVolumeCompositeMode(
				Actor, static_cast<int32>(EFlowVizCompositeMode::IsoSurface),
				TNumericLimits<float>::Max() * 2.0f));
		TestEqual(TEXT("and that refusal keeps the previous value too"),
			Volume->GetRenderSettings().GetIsoValue(), 2.5f);
	}

	/* == A null actor is refused, not dereferenced =========================== */
	{
		AddExpectedError(TEXT("no case actor"),
			EAutomationExpectedErrorFlags::Contains, 2);

		TestFalse(TEXT("a null actor is refused by the mode setter"),
			UFlowVizCaptureLibrary::SetVolumeCompositeMode(
				nullptr, static_cast<int32>(EFlowVizCompositeMode::Alpha), /*IsoValue*/ 0.0f));
		TestFalse(TEXT("and by the lighting setter"),
			UFlowVizCaptureLibrary::SetVolumeLightingEnabled(nullptr, false));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
