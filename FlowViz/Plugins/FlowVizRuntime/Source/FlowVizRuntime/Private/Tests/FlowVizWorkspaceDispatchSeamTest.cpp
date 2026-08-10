// Copyright FlowViz contributors. All Rights Reserved.

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Playback/FlowVizCasePlayer.h"
#include "Render/FlowVizVolumeTexture.h"
#include "Scene/FlowVizCaseActor.h"
#include "Scene/FlowVizVolumeComponent.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "UI/SFlowVizWorkspace.h"
#include "UObject/UObjectGlobals.h"

#include "RHIGlobals.h"
#include "RenderingThread.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED NAMESPACE, NOT ANONYMOUS -- FlowVizRuntime is a unity build and an
 * anonymous namespace merges with every other one in the same blob rather than
 * being file-local. That collision already shipped once in this repo (#37).
 */
namespace FlowVizWorkspaceDispatchSeamTest
{
	/** The committed low-resolution sample, beside Plugins/ rather than inside the plugin. */
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

	/**
	 * Scalar, and named on both sides. A vector field's bytes land in the slot's
	 * VECTOR texture, leaving ScalarTexture null, and DispatchVolumeRayMarch
	 * early-returns on an invalid FieldTexture without logging. The reasoning is
	 * spelled out in full at FlowViz.UI.Workspace.UploadSeam; it applies verbatim
	 * here and matters more, because this file's subject IS the dispatch.
	 */
	const FName FieldId(TEXT("speed"));

	/** Neither the first frame nor the last, so no default and no clamp lands on it. */
	constexpr int32 TargetFrame = 7;

	/**
	 * SEEKING IS NOT LOADING. WaitForPendingLoads returns true immediately when
	 * nothing has been queued, which is exactly what a bare SeekToFrame leaves
	 * behind; Tick starts the loads, the wait lets them finish, the next Tick
	 * drains them. Repo memory seek-is-not-load.
	 */
	bool PumpUntilDisplayed(FFlowVizCasePlayer& Player, int32 FrameIndex, double TimeoutSeconds = 60.0)
	{
		const double StartTime = FPlatformTime::Seconds();

		while (Player.GetDisplay().FrameA != FrameIndex)
		{
			if (FPlatformTime::Seconds() - StartTime > TimeoutSeconds)
			{
				return false;
			}

			Player.Tick(0.0);
			Player.WaitForPendingLoads(TimeoutSeconds);
		}

		return true;
	}
}

/**
 * DOES THE VOLUME PLAYBACK FEEDS ACTUALLY MARCH?
 *
 * FlowViz.UI.Workspace.UploadSeam (#66) closed the channel from the decoder to
 * the GPU and asserted the PRECONDITIONS of a march: a slot booked for the
 * displayed frame, the display pinned on the set, TryMakeShaderParameters
 * succeeding. It stops one step short of the pixels on purpose -- it runs under
 * the default -nullrhi suite, where no texture is ever really created.
 *
 * THE STEP IT STOPS SHORT OF IS THE ONE NOTHING ELSE TAKES EITHER.
 * FlowVizVolumeRayMarch::ClassifyDispatch and ClassifyDispatchAndPublish are the
 * gate: they decide, for one volume in one view, whether the ray-marcher runs.
 * Outside the scene proxy they have exactly three callers, all in
 * FlowVizVolumeComponentTest, and every one of them CONSTRUCTS ITS OWN
 * ARGUMENTS:
 *
 *     FFlowVizVolumeSlotTextures     PublishSlotA;        // default-constructed
 *     FFlowVizVolumeTextureSet       PublishTextureSet;   // never uploaded to
 *     FRecordingDispatcher           PublishDispatcher;   // not the real one
 *     FFlowVizVolumeProxyDynamicData PublishReady;
 *     PublishReady.bHasParameters = true;                 // ASSIGNED, not produced
 *
 * Those tests are correct and they are about the classifier's logic. What no
 * test asks is whether the four values PRODUCTION produces are jointly
 * sufficient -- which is the whole question, and is exactly the shape recorded
 * twice in repo memory: mocking-a-seam-hides-that-nothing-builds-it (every
 * FlowViz.Render.Wiring test installed its own dispatcher, so none could notice
 * that production installed none) and a-test-that-supplies-the-input-cannot-
 * find-the-gap. The comment above that very block names the hazard for
 * bHasParameters, and then the code below it sets bHasParameters by hand.
 *
 * SO EVERY INPUT BELOW IS TAKEN FROM PRODUCTION, NOT BUILT HERE:
 *
 *   dispatcher   FlowVizVolumeRayMarch::GetDispatcher(), installed by
 *                FlowVizVolumeRayMarchProduction::Register() at module startup.
 *   dynamic data Volume->MakeProxyDynamicData(), the same call the proxy makes.
 *   texture set  the component's own, filled by the PLAYER through the seam
 *                #66 wired -- nothing here calls UploadFrame or SetTextureSet.
 *   slot         TextureSet.FindSlotForFrame(FrameA), which additionally
 *                requires bHasContent -- written only in UploadOnRenderThread
 *                after a real RHI create AND update have both succeeded.
 *
 * That last one is why this file needs a device and #66's could not use it.
 * FindSlotForFrame is the residency question, and under -nullrhi its answer is
 * INDEX_NONE for reasons that have nothing to do with the wiring. Rather than
 * route around it with the game-thread booking (which is what #66 does, and
 * says so), this test asks the real question and SKIPS LOUDLY when it cannot.
 *
 * Run with:  RHI=1 ./FlowViz/Tools/run_tests.sh FlowViz.UI.Workspace.DispatchSeam
 *
 * WHY GetLastDispatchStatus IS NOT THE ASSERTION. It is written by the render
 * thread from inside GetDynamicMeshElements and reads NeverRendered until a
 * frame has actually been drawn. This world has a scene but nothing presents
 * it, so that channel stays at its initial value no matter how correct the
 * wiring is -- asserting on it would be asserting on the harness. ClassifyDispatch
 * is pure, takes no view and no collector, and is the same function the proxy
 * calls with the same four values. It is the strongest claim available without
 * a presented frame, and it is a real one: it is the gate itself.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceDispatchSeamTest,
	"FlowViz.UI.Workspace.DispatchSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceDispatchSeamTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceDispatchSeamTest;

	if (!GIsRHIInitialized || GUsingNullRHI)
	{
		// NOT A PASS. A texture set under -nullrhi never marks a slot resident,
		// so every assertion below would be false for a reason unrelated to its
		// subject. Skipping is honest; skipping SILENTLY would report coverage
		// that did not run (repo memory green-totals-can-hide-skips).
		AddInfo(TEXT("SKIPPED: no RHI device (running under -nullrhi), so no slot can ever "
			"report resident and NOTHING about the dispatch gate was verified. Re-run with: "
			"RHI=1 ./FlowViz/Tools/run_tests.sh FlowViz.UI.Workspace.DispatchSeam"));
		return true;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	UWorld* World = MakeWorld(WorldContext);
	if (!TestNotNull(TEXT("CONTROL: a test world was created"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		// The set's release path enqueues render commands; the teardown below
		// frees the component that owns it. Flushed so those run against a live
		// object rather than a freed one.
		FlushRenderingCommands();
		World->DestroyWorld(/*bInformEngineOfWorld*/ true);
		World->RemoveFromRoot();
		GEngine->DestroyWorldContext(World);
	};

	ACFDVizCaseActor* Actor = World->SpawnActor<ACFDVizCaseActor>();
	UCFDVizVolumeComponent* Volume = Actor != nullptr ? Actor->GetVolumeComponent() : nullptr;
	if (!TestNotNull(TEXT("CONTROL: the actor owns a volume component"), Volume))
	{
		return false;
	}
	if (!TestTrue(TEXT("CONTROL: the sample's scalar 'speed' field loads into the volume"),
			Actor->LoadCase(GetSampleCaseDir(), FieldId).IsOk()))
	{
		return false;
	}

	/* == The dispatcher must be PRODUCTION'S, and it must exist ============== */

	/*
	 * THE CONTROL WITHOUT WHICH THIS FILE MEASURES NOTHING. ClassifyDispatch
	 * returns NoDispatcher for a null one, so a run where module startup never
	 * installed the production dispatcher would fail every assertion below --
	 * and, far worse, a future edit that installed a TEST double would make them
	 * all pass while measuring the double. Asked before anything else so the
	 * failure names the real cause instead of surfacing as "the volume did not
	 * march".
	 */
	IFlowVizVolumeRayMarchDispatcher* const Dispatcher = FlowVizVolumeRayMarch::GetDispatcher();
	if (!TestNotNull(TEXT("CONTROL: module startup installed a ray-march dispatcher, so the gate "
						  "below is being asked about production's own and not about a null one"),
			Dispatcher))
	{
		return false;
	}

	TSharedPtr<SFlowVizWorkspace> Workspace = SNew(SFlowVizWorkspace);

	if (!TestTrue(TEXT("CONTROL: the workspace opens the same case on the same field, so the player "
					   "decodes the voxels this component's textures are shaped for"),
			Workspace->GetModel().OpenCase(GetSampleCaseDir(), FieldId).IsOk()))
	{
		return false;
	}

	FFlowVizCasePlayer& Player = Workspace->GetModel().Player;

	if (!TestTrue(FString::Printf(TEXT("CONTROL: the sample has more than %d frames, so the seek "
									   "below lands mid-timeline rather than clamping to the end"),
					  TargetFrame),
			Player.GetTimeline().GetFrameCount() > TargetFrame))
	{
		return false;
	}

	FFlowVizVolumeTextureSet& TextureSet = Volume->GetTextureSet();

	/* == THE IDENTITY CONTROL: not marching, before playback feeds it ======== */

	/*
	 * WITHOUT THIS THE HEADLINE ASSERTION CANNOT FAIL IN THE DIRECTION THAT
	 * MATTERS. `Dispatched` is one value of an enum whose other values are all
	 * blockers; a ClassifyDispatch that had been mutated to return Dispatched
	 * unconditionally -- or an enum whose zero value drifted onto Dispatched --
	 * would satisfy the assertion below over a volume with no case, no voxels
	 * and no parameters. This arm demands that the SAME call, over the SAME
	 * component, reports a blocker before playback has fed it anything.
	 *
	 * It deliberately does not name WHICH blocker. The component has loaded a
	 * case but has never uploaded, so both NoParameters and FrameNotResident are
	 * honest answers depending on which check runs first, and pinning one would
	 * make this arm a test of the classifier's statement order rather than of
	 * the fact that it refuses.
	 */
	{
		const FFlowVizVolumeProxyDynamicData BeforeData = Volume->MakeProxyDynamicData();
		const int32 BeforeSlotIndex = TextureSet.FindSlotForFrame(BeforeData.FrameSelection.FrameA);
		FFlowVizVolumeSlotTextures BeforeTextureSnapshot;
		const FFlowVizVolumeSlotTextures* BeforeTextures =
			TextureSet.GetSlotTextures(BeforeSlotIndex, BeforeTextureSnapshot)
				? &BeforeTextureSnapshot
				: nullptr;
		const FFlowVizDispatchStatus Before = FlowVizVolumeRayMarch::ClassifyDispatch(
			Dispatcher, BeforeData, &TextureSet, BeforeTextures);

		if (!TestFalse(
				FString::Printf(TEXT("IDENTITY CONTROL: before playback feeds it, the very same gate "
									 "over the very same volume refuses to march (reason %d) -- "
									 "without this, 'Dispatched' below could be what this call "
									 "returns for everything"),
					static_cast<int32>(Before.Reason)),
				Before.ShouldDispatch()))
		{
			return false;
		}
	}

	/* == The binding, and one real frame through the player ================== */

	Workspace->SetVolume(Volume);

	Player.SeekToFrame(TargetFrame);
	if (!TestTrue(TEXT("CONTROL: the seeked frame finishes decoding, so the assertions below are "
					   "about the dispatch gate rather than about an undecoded frame"),
			PumpUntilDisplayed(Player, TargetFrame)))
	{
		return false;
	}

	/*
	 * THE UPLOAD IS A RENDER COMMAND, and residency is set inside it. Without
	 * this flush FindSlotForFrame answers INDEX_NONE because the command has not
	 * run yet -- a race that would make this test fail intermittently and for
	 * the wrong reason.
	 */
	FlushRenderingCommands();

	/* == The four production values, and the gate they are handed to ========= */

	const FFlowVizVolumeProxyDynamicData DynamicData = Volume->MakeProxyDynamicData();

	TestEqual(TEXT("CONTROL: production's own dynamic data names the frame the player is "
				   "displaying, so the residency question below is asked about the right frame"),
		DynamicData.FrameSelection.FrameA, Player.GetDisplay().FrameA);

	/*
	 * RESIDENT, not merely booked. FindSlotForFrame requires bHasContent, which
	 * UploadOnRenderThread sets only after the RHI create AND the update both
	 * succeeded -- this is the assertion #66 could not make and the reason this
	 * file needs a device.
	 *
	 * Asserted separately rather than folded into the headline: GetSlotTextures
	 * returns false for INDEX_NONE, so an unasserted miss here would reach
	 * ClassifyDispatch as a null slot and be reported as FrameNotResident --
	 * a correct diagnosis of the wrong thing, blaming the gate for a failed
	 * upload.
	 */
	const int32 SlotIndex = TextureSet.FindSlotForFrame(DynamicData.FrameSelection.FrameA);
	if (!TestTrue(
			FString::Printf(TEXT("the frame playback decoded is RESIDENT on the device -- created, "
								 "uploaded and marked complete by the render thread (frame %d, "
								 "slot %d)"),
				DynamicData.FrameSelection.FrameA, SlotIndex),
			SlotIndex != INDEX_NONE))
	{
		return false;
	}

	FFlowVizVolumeSlotTextures SlotTextureSnapshot;
	const bool bHasSlotTextures = TextureSet.GetSlotTextures(SlotIndex, SlotTextureSnapshot);
	const FFlowVizVolumeSlotTextures* const SlotTextures =
		bHasSlotTextures ? &SlotTextureSnapshot : nullptr;
	if (!TestNotNull(TEXT("CONTROL: the resident slot's textures are readable, so the gate below is "
						  "handed a real slot rather than a null one"),
			SlotTextures))
	{
		return false;
	}

	/*
	 * THE ASSERTION THIS FILE EXISTS FOR.
	 *
	 * Every argument came from production: the dispatcher module startup
	 * installed, the payload MakeProxyDynamicData built, the texture set the
	 * PLAYER filled through the seam #66 wired, and the slot the render thread
	 * marked resident. Nothing in this test constructed a dynamic data struct,
	 * set bHasParameters, or installed a dispatcher.
	 *
	 * The reason is asserted rather than only the boolean, because the boolean
	 * alone cannot say WHICH of the four inputs was the one missing, and that is
	 * the actionable half of every failure this gate reports.
	 */
	const FFlowVizDispatchStatus Status = FlowVizVolumeRayMarch::ClassifyDispatch(
		Dispatcher, DynamicData, &TextureSet, SlotTextures);

	TestEqual(
		FString::Printf(TEXT("A VOLUME FED BY PLAYBACK MARCHES: the same gate the scene proxy uses, "
							 "over production's own dispatcher, payload, texture set and resident "
							 "slot, reports Dispatched (got '%s')"),
			*FlowVizVolumeRayMarch::DescribeDispatchReason(Status.Reason)),
		static_cast<int32>(Status.Reason),
		static_cast<int32>(EFlowVizDispatchReason::Dispatched));

	TestTrue(TEXT("and the boolean the proxy actually gates on agrees with the reason it reports"),
		Status.ShouldDispatch());

	/* == It keeps marching as the frame changes ============================== */

	/*
	 * A build that marched only the frame present at bind time would pass
	 * everything above. Scrubbing to a different frame and asking again is what
	 * separates "the wiring delivered once" from "the wiring is a channel" --
	 * and it is the case a user actually produces, every time they drag the
	 * timeline.
	 *
	 * Frame 0 is the component's documented no-source fallback, so this arm
	 * reads the RESIDENCY of the frame the player names rather than trusting the
	 * frame number alone: 0 would also be what a detached component reports.
	 */
	Player.SeekToFrame(0);
	if (TestTrue(TEXT("CONTROL: frame 0 finishes decoding"), PumpUntilDisplayed(Player, 0)))
	{
		FlushRenderingCommands();

		const FFlowVizVolumeProxyDynamicData ScrubbedData = Volume->MakeProxyDynamicData();
		const int32 ScrubbedSlot = TextureSet.FindSlotForFrame(ScrubbedData.FrameSelection.FrameA);

		if (TestTrue(FString::Printf(TEXT("CONTROL: the scrubbed-to frame is resident too (frame %d, "
										  "slot %d)"),
						 ScrubbedData.FrameSelection.FrameA, ScrubbedSlot),
				ScrubbedSlot != INDEX_NONE))
		{
			FFlowVizVolumeSlotTextures ScrubbedTextureSnapshot;
			const FFlowVizVolumeSlotTextures* ScrubbedTextures =
				TextureSet.GetSlotTextures(ScrubbedSlot, ScrubbedTextureSnapshot)
					? &ScrubbedTextureSnapshot
					: nullptr;
			const FFlowVizDispatchStatus ScrubbedStatus = FlowVizVolumeRayMarch::ClassifyDispatch(
				Dispatcher, ScrubbedData, &TextureSet, ScrubbedTextures);

			TestEqual(
				FString::Printf(TEXT("and it still marches after a scrub, so playback keeps the "
									 "renderer fed rather than having satisfied it once (got '%s')"),
					*FlowVizVolumeRayMarch::DescribeDispatchReason(ScrubbedStatus.Reason)),
				static_cast<int32>(ScrubbedStatus.Reason),
				static_cast<int32>(EFlowVizDispatchReason::Dispatched));
		}
	}

	// Dropped before the world teardown in the scope-exit, so the player is gone
	// while the component it borrowed from is still a valid object.
	Workspace->SetVolume(nullptr);
	Workspace.Reset();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
