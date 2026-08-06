// Copyright FlowViz contributors. All Rights Reserved.

#include "Containers/Ticker.h"
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

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED NAMESPACE, NOT ANONYMOUS -- FlowVizRuntime is a unity build and an
 * anonymous namespace merges with every other one in the same blob rather than
 * being file-local. That collision already shipped once in this repo (#37).
 */
namespace FlowVizWorkspaceUploadSeamTest
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
	 * NAMED ON BOTH SIDES RATHER THAN DEFAULTED, and it must be a SCALAR.
	 *
	 * Both FFlowVizWorkspaceModel::OpenCase and UCFDVizVolumeComponent::LoadCase
	 * pick "the first field that is not a mask" when handed NAME_None. Those are
	 * two independent copies of the same rule over the same manifest, so today
	 * they agree -- on `U`, which is a 3-component velocity field. Relying on that
	 * agreement would make this test's subject "do two duplicated loops still
	 * match", which is not what it is for, and it would silently start playing one
	 * field into another field's textures the day either loop changes.
	 *
	 * `speed` is the magnitude of that velocity: one float16 component on the same
	 * grid. Scalar matters because a vector field's bytes land in the slot's
	 * VECTOR texture, and DispatchVolumeRayMarch early-returns on an invalid
	 * FieldTexture without logging -- so a vector field would still exercise the
	 * upload seam under test while quietly failing to be the configuration anyone
	 * can see on screen. FlowViz.Capture.SpawnCaseActor makes the same choice for
	 * the same reason.
	 */
	const FName FieldId(TEXT("speed"));

	/**
	 * A frame that is neither 0 nor the last, so no default and no clamp lands on
	 * it. Asserted against the sample's real length below rather than assumed.
	 */
	constexpr int32 TargetFrame = 7;

	/**
	 * SEEKING IS NOT LOADING, and WaitForPendingLoads alone does not close that
	 * gap -- it returns true IMMEDIATELY when nothing has been queued yet, which
	 * is exactly the state a bare SeekToFrame leaves behind. Tick starts the
	 * loads; the wait lets them finish; the next Tick drains them into Display.
	 * Repo memory seek-is-not-load.
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

	/**
	 * The slot holding this frame according to the set's GAME-THREAD bookkeeping,
	 * or INDEX_NONE.
	 *
	 * DELIBERATELY NOT FFlowVizVolumeTextureSet::FindSlotForFrame, which is the
	 * obvious call and is the wrong one here. That function additionally requires
	 * Slots[i].bHasContent, which is written in UploadOnRenderThread only after a
	 * real RHI create and update have both succeeded. The automation suite's
	 * default run is -nullrhi, so an assertion routed through it would answer "not
	 * resident" for reasons that have nothing to do with the wiring under test,
	 * and the honest version of this file would then have to skip itself on the
	 * default suite -- reporting coverage that never ran (repo memory
	 * green-totals-can-hide-skips, nullrhi-and-rhi-are-different-suites). The one
	 * device-side assertion this project has on that path is
	 * FlowViz.Render.VolumeDevice, which runs under RHI=1 and says so when it does
	 * not.
	 *
	 * SlotStates[i].FrameIndex is written SYNCHRONOUSLY inside EnqueueUpload,
	 * before the render command is queued, and is still strictly downstream of the
	 * upload: it is set nowhere else, so a non-INDEX_NONE entry means a validated
	 * payload for that frame was accepted by this set. That is the claim this file
	 * needs and it is device-independent.
	 */
	int32 FindSlotBookedForFrame(const FFlowVizVolumeTextureSet& Set, int32 FrameIndex)
	{
		const TArrayView<const FFlowVizVolumeSlotState> States = Set.GetSlotStates();
		for (int32 Index = 0; Index < States.Num(); ++Index)
		{
			if (States[Index].FrameIndex == FrameIndex)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	/** How many slots hold any frame at all. The "before" control reads 0. */
	int32 CountBookedSlots(const FFlowVizVolumeTextureSet& Set)
	{
		int32 Count = 0;
		for (const FFlowVizVolumeSlotState& State : Set.GetSlotStates())
		{
			if (State.FrameIndex != INDEX_NONE)
			{
				++Count;
			}
		}
		return Count;
	}
}

/**
 * DOES PLAYBACK PUT VOXELS ON THE GPU, or only move the label on them?
 *
 * FFlowVizCasePlayer::SetTextureSet has NO PRODUCTION CALLER -- the whole plugin
 * outside tests contains its declaration and its definition and nothing else. So
 * the player's TextureSet member is permanently null and both of its users take
 * their null branch:
 *
 *   DrainCompletedLoads       skips TextureSet->EnqueueUpload, so a decoded
 *                             frame is marked complete and never uploaded
 *   ApplyDisplayToTextureSet  returns immediately, so no set is ever told which
 *                             pair is on screen
 *
 * The other upload path is real but is not playback's: UCFDVizVolumeComponent::
 * UploadFrame does a full synchronous read-build-enqueue, and its only production
 * caller is FlowVizCaptureLibrary -- the headless still-capture tool. Repo memory
 * one-green-caller-implies-none, except that the one caller here is a QA tool, so
 * the function looks covered while the interactive path it appears to serve has
 * never run it.
 *
 * WHY EVERY EXISTING TEST PASSES OVER THIS. FlowViz.UI.Workspace.ClockSeam (#65)
 * asserts the display selection advances, and it genuinely does; FlowViz.UI.
 * Workspace.FrameSeam (#57) asserts the component follows the player's display,
 * and it genuinely does. Both are true of a build that uploads nothing -- the
 * selection is an integer pair, and it advances whether or not any texture behind
 * it was written. The two seams are one short of the pixels (repo memory
 * a-test-that-supplies-the-input-cannot-find-the-gap).
 *
 * AND ASSERTING "THE DISPLAY ADVANCED" HERE WOULD BE WORSE THAN USELESS, because
 * on the FIXED build it becomes a proxy for the upload and on the broken one it
 * does not. DrainCompletedLoads only calls Cache.MarkComplete after EnqueueUpload
 * returns Ok -- but that whole block sits inside `if (TextureSet != nullptr)`, so
 * with no set attached the frame is marked complete unconditionally. The display
 * therefore advances identically in both worlds. Every assertion below reads the
 * TEXTURE SET.
 *
 * THE SECOND GAP, which wiring SetTextureSet alone does not close. A volume only
 * ray-marches when FFlowVizVolumeProxyDynamicData::bHasParameters is true, and
 * that is TryMakeShaderParameters, which requires UploadedScalarLayout.IsValid().
 * The ONLY writer of that field is the component's own UploadFrame -- the QA path
 * again. Route playback through the player and the voxels arrive, the layout does
 * not, bHasParameters stays false, and ClassifyDispatch reports NoParameters: a
 * black volume, with the diagnostic naming a blocker that is now a symptom rather
 * than the cause. Arm 4 is that gap, and it is why closing this task takes more
 * than one line in SetVolume.
 *
 * THE LIFETIME DIRECTION IS THE OPPOSITE OF #57'S, and it is the dangerous one.
 * The frame source is held BY the component, so a component that dies takes its
 * reference with it and the workspace is never left pointing at anything. A
 * texture set is held BY THE PLAYER, as a raw pointer INTO a UObject the
 * workspace only tracks weakly. A component collected while bound therefore
 * leaves the player writing decoded frames into freed memory on its next tick --
 * and TickClock ticks the player unconditionally, by design, precisely so that a
 * scrub with no volume bound still resolves. Arm 5 covers it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceUploadSeamTest,
	"FlowViz.UI.Workspace.UploadSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceUploadSeamTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceUploadSeamTest;

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	UWorld* World = MakeWorld(WorldContext);
	if (!TestNotNull(TEXT("CONTROL: a test world was created"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		// The set's release path enqueues render commands; the world teardown
		// below frees the component that owns it. Flushed so those commands run
		// against a live object rather than a freed one.
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

	// A TSharedPtr rather than a TSharedRef: the last arm destroys the workspace
	// while the volume is still alive and bound, which is what closing the tab does.
	TSharedPtr<SFlowVizWorkspace> Workspace = SNew(SFlowVizWorkspace);

	if (!TestTrue(TEXT("CONTROL: the workspace opens the same case on the same field, so the "
					   "player decodes the voxels this component's textures are shaped for"),
			Workspace->GetModel().OpenCase(GetSampleCaseDir(), FieldId).IsOk()))
	{
		return false;
	}

	FFlowVizCasePlayer& Player = Workspace->GetModel().Player;

	if (!TestTrue(FString::Printf(TEXT("CONTROL: the sample has more than %d frames, so the seek "
									   "below lands on a middle frame rather than clamping to the "
									   "end -- which a build that ignored the seek could pass"),
					  TargetFrame),
			Player.GetTimeline().GetFrameCount() > TargetFrame))
	{
		return false;
	}

	FFlowVizVolumeTextureSet& TextureSet = Volume->GetTextureSet();

	/* == Before the binding ================================================== */

	/*
	 * THE CONTROL THAT KEEPS ARM 3 FROM BEING VACUOUS. The component initialises
	 * its set in its constructor, so the slots exist from the start; what they do
	 * not have is any frame booked into them. Without this reading zero, "a slot
	 * holds frame 7" could be true of a set that was pre-populated by the
	 * component's own QA path and say nothing about playback.
	 */
	if (!TestEqual(TEXT("CONTROL: the volume's texture set has slots but no frame booked into any "
						"of them before playback is bound"),
			CountBookedSlots(TextureSet), 0))
	{
		return false;
	}

	TestEqual(TEXT("CONTROL: and the player has no texture set attached, which is the state this "
				   "file exists to change"),
		Player.GetTextureSet(), (FFlowVizVolumeTextureSet*)nullptr);

	/* == The binding ========================================================= */

	Workspace->SetVolume(Volume);

	Player.SeekToFrame(TargetFrame);
	if (!TestTrue(TEXT("CONTROL: the seeked frame finishes decoding, so the player's DISPLAY "
					   "advances -- an undecoded frame would leave every assertion below false "
					   "for a reason unrelated to the upload wiring"),
			PumpUntilDisplayed(Player, TargetFrame)))
	{
		return false;
	}

	TestEqual(TEXT("CONTROL: the player's own display reached the target frame"),
		Player.GetDisplay().FrameA, TargetFrame);

	/*
	 * THE ASSERTION THIS FILE EXISTS FOR, and it names the frame rather than
	 * counting uploads. "EnqueueUpload happened" would be satisfied by a build
	 * that uploads frame 0 forever; the requirement is that the voxels behind the
	 * frame the display NAMES are the ones that were sent.
	 */
	const int32 TargetSlot = FindSlotBookedForFrame(TextureSet, TargetFrame);
	TestTrue(
		FString::Printf(TEXT("and the VOLUME'S TEXTURE SET took delivery of frame %d -- the "
							 "channel from the decoder to the GPU, with nothing in this test "
							 "calling UploadFrame or SetTextureSet itself (slot %d)"),
			TargetFrame, TargetSlot),
		TargetSlot != INDEX_NONE);

	/*
	 * AND THE SET WAS TOLD WHICH PAIR IS ON SCREEN. Separate from the arm above
	 * and not implied by it: EnqueueUpload books a slot, SetDisplayFrames pins
	 * it. A build that uploaded without ever pinning would pass the previous
	 * assertion and then let a prefetch evict the very frame being sampled, which
	 * presents as a torn or stale image under scrubbing and reads as a decode bug.
	 */
	TestEqual(TEXT("and the set knows which frame is displayed, so eviction cannot take the frame "
				   "the shader is about to read"),
		TextureSet.GetDisplayFrameA(), Player.GetDisplay().FrameA);

	/* == The layout, which the upload alone does not carry =================== */

	/*
	 * ARM 4: THE SECOND GAP. Uploading voxels is necessary and not sufficient.
	 * The proxy refuses to march unless bHasParameters is true, and that is this
	 * call -- gated on UploadedScalarLayout, whose only writer is the component's
	 * QA-path UploadFrame. Wire the player's upload and leave this alone and the
	 * volume is still black, now reported as NoParameters.
	 *
	 * Asserted through the component's own public accessor rather than through
	 * the dispatch status, because GetLastDispatchStatus is written by the render
	 * thread after a frame has actually been rendered and reads NeverRendered
	 * until then -- correct behaviour, and unusable as a game-thread assertion in
	 * a world that never renders.
	 */
	FFlowVizVolumeShaderParameters Params;
	TestTrue(TEXT("and the component can build shader parameters, without which "
				  "FFlowVizVolumeProxyDynamicData::bHasParameters is false and the proxy reports "
				  "NoParameters and marches nothing -- the uploaded voxels would never be sampled"),
		Volume->TryMakeShaderParameters(Params));

	/* == It tracks, rather than delivering once at bind time ================= */

	Player.SeekToFrame(0);
	if (TestTrue(TEXT("CONTROL: frame 0 finishes decoding"), PumpUntilDisplayed(Player, 0)))
	{
		TestEqual(TEXT("CONTROL: the player's display went back to frame 0"),
			Player.GetDisplay().FrameA, 0);

		/*
		 * A build that handed the set over at bind time and then stopped feeding
		 * it would pass every assertion above and fail here. Frame 0 is the
		 * component's documented no-source fallback, so this arm deliberately
		 * does NOT read the frame selection -- it reads the set's own booking,
		 * where 0 means "these voxels arrived" rather than "nothing is attached".
		 */
		TestTrue(TEXT("and a later scrub delivers too, so the player keeps feeding the set "
					  "rather than having been introduced to it once"),
			FindSlotBookedForFrame(TextureSet, 0) != INDEX_NONE);
	}

	/* == The set is BORROWED, and the player must not outlive it ============= */

	Workspace->SetVolume(nullptr);

	/*
	 * The player holds the set as a RAW POINTER INTO A UOBJECT. This is the
	 * mirror image of #57's lifetime arm and the more dangerous half: there, the
	 * component held the frame source, so a dying component took its own
	 * reference with it. Here the survivor holds the pointer, and the workspace
	 * only tracks the component weakly -- so nothing about the component's death
	 * reaches the player unless someone makes it.
	 */
	TestEqual(TEXT("unbinding releases the texture set, so the player does not write decoded "
				   "frames into a component the workspace no longer drives"),
		Player.GetTextureSet(), (FFlowVizVolumeTextureSet*)nullptr);

	/* == Collected while still bound, which is what closing a level does ===== */

	Workspace->SetVolume(Volume);
	if (!TestNotEqual(TEXT("CONTROL: re-binding restores the channel, so the assertion below is "
						   "about the component's death rather than about the re-bind failing"),
			Player.GetTextureSet(), (FFlowVizVolumeTextureSet*)nullptr))
	{
		return false;
	}

	/*
	 * THE PATH THAT ACTUALLY HAPPENS, and the one no explicit unbind covers.
	 * Nothing in the codebase calls SetVolume(nullptr); what happens in practice
	 * is the level going away under a docked tab. The workspace's Volume is a
	 * TWeakObjectPtr, so it reads null the moment the actor is destroyed -- but
	 * the PLAYER's pointer is raw and reads whatever used to be there.
	 *
	 * TickClock is where this has to be caught, because TickClock is what runs
	 * afterwards: it ticks the player UNCONDITIONALLY (deliberately -- a paused
	 * player still drains decodes, which is what makes scrubbing land), and
	 * DrainCompletedLoads dereferences the texture set. So the guard cannot live
	 * behind the `if (Volume.Get())` that the frame-publishing code sits in.
	 *
	 * The assertion is on the released pointer rather than on the absence of a
	 * crash: the memory is not freed until a garbage collection actually runs, so
	 * a build without the guard would very likely still not crash HERE -- it
	 * would crash in the field, later, in someone else's session. Reading the
	 * pointer is what makes the guard observable now.
	 */
	Actor->Destroy();

	TestNull(TEXT("CONTROL: the workspace's weak pointer drops the destroyed component, which is "
				  "the signal the guard has to act on"),
		Workspace->GetVolume());

	// One engine frame, through the object the engine loop actually drives.
	FTSTicker::GetCoreTicker().Tick(1.0f / 30.0f);

	TestEqual(TEXT("a component destroyed while bound releases the player's texture set on the "
				   "next tick, so playback does not upload into freed memory"),
		Player.GetTextureSet(), (FFlowVizVolumeTextureSet*)nullptr);

	// Dropped before the world teardown in the scope-exit, so the player is gone
	// while the component it borrowed from is still a valid object.
	Workspace.Reset();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
