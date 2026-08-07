// Copyright FlowViz contributors. All Rights Reserved.

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Playback/FlowVizCasePlayer.h"
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
namespace FlowVizWorkspaceFrameSeamTest
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
	 * A frame that is neither 0 nor the last, so no default and no clamp lands on it.
	 *
	 * The sample has 20 frames (asserted below rather than assumed, because a
	 * shorter sample would silently turn this into "seek past the end", which
	 * clamps to the last frame and would make the assertion pass on a component
	 * that ignored the seek entirely).
	 */
	constexpr int32 TargetFrame = 7;

	/**
	 * SEEKING IS NOT LOADING, and WaitForPendingLoads alone does not close that
	 * gap -- it returns true IMMEDIATELY when nothing has been queued yet, which
	 * is exactly the state a bare SeekToFrame leaves behind.
	 *
	 * SeekToFrame moves the playhead and resolves the WANTED selection. It is
	 * Tick that starts the loads and drains the completed ones into Display, and
	 * the seam reads Display (FlowVizCaseSeam.cpp: what is complete and resident,
	 * not what the playhead wants). So a seek-then-wait leaves Display at
	 * INDEX_NONE and the volume at 0 -- indistinguishable from the missing wiring
	 * this file exists to detect.
	 *
	 * That is the direction that matters: an un-ticked player produces the SAME
	 * reading as a broken seam, so without this the test would have gone green on
	 * the fix while still being unable to fail for the right reason.
	 *
	 * Returns false rather than asserting, so the caller states what the failure
	 * means in its own words.
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

			// Tick STARTS the pending loads; the wait lets them finish; the next
			// iteration's Tick drains them into Display. All three are needed, and
			// a single Tick would spin here forever.
			Player.Tick(0.0);
			Player.WaitForPendingLoads(TimeoutSeconds);
		}

		return true;
	}
}

/**
 * DOES THE TIMELINE REACH THE RENDERER, or is the frame source merely available?
 *
 * This is #50 again, one seam over, and it was found while writing the parity
 * doc: UCFDVizVolumeComponent::SetFrameSource and FlowVizPlayback::MakeFrameSource
 * have NO PRODUCTION CALLER. Both appear only in tests. So the transport bar
 * scrubs a view model, the player decodes the right frames, the component
 * renders a frame selection nobody set -- and every test on both sides passes,
 * because each installs the seam it is testing. Repo memory
 * mocking-a-seam-hides-that-nothing-builds-it; the same defect took 49 green
 * rendering tests to notice last time.
 *
 * WHAT MAKES A COMPONENT WITH NO SOURCE INDISTINGUISHABLE, and why this test
 * cannot use frame 0. GetFrameSelection's documented fallback for a component
 * with no frame source is FrameA = 0 -- a deliberate display policy, not a stub.
 * So asserting "the volume shows frame 0" would pass with the seam wired, with
 * it unwired, and with SetFrameSource deleted outright. Every arm below drives
 * the player to a frame that is NOT 0 first, and asserts the volume followed.
 * Repo memory degenerate-data-defeats-assertions.
 *
 * WHY THE LIFETIME ARM IS NOT OPTIONAL. FCasePlayerFrameSource holds the player
 * by RAW REFERENCE (FlowVizCaseSeam.cpp), and the player lives in the
 * workspace's TUniquePtr<FFlowVizWorkspaceModel> while the component lives in
 * the world. Neither owns the other, and a component outliving the workspace is
 * the ordinary case: close the tab, the level keeps rendering. If SetVolume does
 * not clear the source on unbind, the volume calls through a dangling reference
 * on its next tick. That crash reproduces on teardown only.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceFrameSeamTest,
	"FlowViz.UI.Workspace.FrameSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceFrameSeamTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceFrameSeamTest;

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	UWorld* World = MakeWorld(WorldContext);
	if (!TestNotNull(TEXT("CONTROL: a test world was created"), World))
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
	UCFDVizVolumeComponent* Volume = Actor != nullptr ? Actor->GetVolumeComponent() : nullptr;
	if (!TestNotNull(TEXT("CONTROL: the actor owns a volume component"), Volume))
	{
		return false;
	}
	if (!TestTrue(TEXT("CONTROL: the sample case loads into the volume"),
			Actor->LoadCase(GetSampleCaseDir()).IsOk()))
	{
		return false;
	}

	/*
	 * A TSharedPtr RATHER THAN A TSharedRef, because the last arm destroys the
	 * workspace while the volume is still alive and bound. That is not a
	 * contrived teardown -- it is what closing the tab does.
	 */
	TSharedPtr<SFlowVizWorkspace> Workspace = SNew(SFlowVizWorkspace);

	if (!TestTrue(TEXT("CONTROL: the workspace opens the sample case, which is what gives its "
					   "player a frame count to seek within"),
			Workspace->GetModel().OpenCase(GetSampleCaseDir()).IsOk()))
	{
		return false;
	}

	FFlowVizCasePlayer& Player = Workspace->GetModel().Player;

	/*
	 * ASSERTED, NOT ASSUMED. If the sample ever shrinks below TargetFrame + 1,
	 * SeekToFrame clamps to the last frame and this test quietly becomes "does
	 * seeking to the end work" -- which a component ignoring the seam could
	 * still pass by luck.
	 */
	if (!TestTrue(FString::Printf(TEXT("CONTROL: the sample has more than %d frames, so the seek "
									   "below lands on a middle frame rather than clamping"),
					  TargetFrame),
			Player.GetTimeline().GetFrameCount() > TargetFrame))
	{
		return false;
	}

	/* == Before the binding ================================================== */

	TestEqual(TEXT("CONTROL: a volume with no frame source falls back to frame 0, which is why "
				   "every arm below targets a frame that is not 0"),
		Volume->GetFrameSelection().FrameA, 0);

	/* == The binding ========================================================= */

	Workspace->SetVolume(Volume);

	Player.SeekToFrame(TargetFrame);
	if (!TestTrue(TEXT("CONTROL: the seeked frame finishes loading, so the player's DISPLAY "
					   "advances -- the seam reads what is resident, not what is wanted, so an "
					   "unloaded frame would leave the display on 0 for a reason unrelated to "
					   "the wiring"),
			PumpUntilDisplayed(Player, TargetFrame)))
	{
		return false;
	}

	TestEqual(TEXT("CONTROL: the player's own display reached the target frame"),
		Player.GetDisplay().FrameA, TargetFrame);

	/*
	 * THE ASSERTION THIS FILE EXISTS FOR. Nothing here called SetFrameSource: the
	 * workspace was told which volume to drive, and the player was scrubbed. If
	 * SetVolume does not install a frame source, the player sits on frame 7 while
	 * the volume renders frame 0 -- which is exactly the state the codebase is in
	 * as this test is written.
	 */
	TestEqual(TEXT("and the VOLUME followed the player to that frame, with nothing in this test "
				   "installing a frame source -- the channel from timeline to renderer"),
		Volume->GetFrameSelection().FrameA, TargetFrame);

	/* == It tracks, rather than latching once at bind time =================== */

	Player.SeekToFrame(0);
	if (TestTrue(TEXT("CONTROL: frame 0 finishes loading"), PumpUntilDisplayed(Player, 0)))
	{
		TestEqual(TEXT("CONTROL: the player's display went back to frame 0"),
			Player.GetDisplay().FrameA, 0);

		/*
		 * A source that captured the selection at bind time rather than reading
		 * it live would pass the arm above and fail here. This is the difference
		 * between a wire and a snapshot, and only one of them is a timeline.
		 */
		TestEqual(TEXT("and the volume followed back, so the source READS the player rather than "
					   "having copied it once at bind time"),
			Volume->GetFrameSelection().FrameA, 0);
	}

	/* == The player is BORROWED, and must not outlive its binding ============ */

	Workspace->SetVolume(nullptr);

	/*
	 * The frame source holds the workspace's player by raw reference. A component
	 * that keeps it after unbinding calls through that reference on its next tick,
	 * and the workspace is free to die first -- closing the tab while the level
	 * keeps rendering is the ordinary case, not the exotic one.
	 *
	 * Asserting the FALLBACK (frame 0) rather than "not TargetFrame" is deliberate:
	 * it distinguishes a cleared source from a source that merely stopped tracking.
	 */
	Player.SeekToFrame(TargetFrame);
	Player.WaitForPendingLoads(60.0);

	TestEqual(TEXT("unbinding the volume clears its frame source, so a later scrub does not read "
				   "through a reference to a player the workspace may already have destroyed"),
		Volume->GetFrameSelection().FrameA, 0);

	/* == Destroyed while still bound, which is what closing the tab does ====== */

	/*
	 * THE PATH THAT ACTUALLY HAPPENS. The arm above requires someone to call
	 * SetVolume(nullptr) on the way out; nothing in the codebase does. The real
	 * teardown is the widget's last reference going away with a volume still
	 * bound -- close the tab, the level keeps rendering -- and that path runs the
	 * DESTRUCTOR, not SetVolume.
	 *
	 * So this arm re-binds and then simply drops the workspace. A destructor that
	 * does not release the source leaves the component holding a reference to a
	 * player inside a freed FFlowVizWorkspaceModel. Asserting the fallback here is
	 * what makes "released" observable; a use-after-free is not something a test
	 * can assert on directly, and under ASan it would abort the whole run rather
	 * than fail this one check.
	 */
	Workspace->SetVolume(Volume);
	if (!TestEqual(TEXT("CONTROL: re-binding restores the channel, so the assertion below is about "
						"the destructor rather than about the re-bind failing"),
			Volume->GetFrameSelection().FrameA, TargetFrame))
	{
		return false;
	}

	Workspace.Reset();

	TestEqual(TEXT("destroying the workspace with a volume still bound releases the frame source, "
				   "so the component does not read through a reference into the freed model"),
		Volume->GetFrameSelection().FrameA, 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
