// Copyright FlowViz contributors. All Rights Reserved.

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Scene/FlowVizCaseActor.h"
#include "Scene/FlowVizVolumeComponent.h"
#include "UI/FlowVizSession.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "UI/SFlowVizClipPanel.h"
#include "UI/SFlowVizPipelinePanel.h"
#include "UI/SFlowVizProbePanel.h"
#include "UI/SFlowVizWorkspace.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED NAMESPACE, NOT ANONYMOUS -- FlowVizRuntime is a unity build and an
 * anonymous namespace merges with every other one in the same blob rather than
 * being file-local. That collision already shipped once in this repo (#37).
 */
namespace FlowVizWorkspaceSessionSeamTest
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
		// GLOBALLY UNIQUE NAME, matching the sibling seam tests. Two tests that
		// both create an unnamed world in the transient package get the same
		// object name, and the second one is the one that fails -- for a reason
		// that has nothing to do with what it is testing.
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
	 * Per-test directory under Saved/. Removed by the test that creates it.
	 *
	 * PER TEST rather than one shared directory, for the reason
	 * FlowVizWorkspaceModelTest gives: each test deletes its directory on the way
	 * out whether it passed or failed, so a shared one would let a failing test
	 * remove the file a later test was about to read.
	 */
	FString GetSessionDir(const TCHAR* TestName)
	{
		return FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("FlowVizWorkspaceSessionSeamTest"), TestName);
	}

	/* --- The values the session carries ------------------------------------ */

	/*
	 * NONE OF THESE IS A DEFAULT, AND NONE IS THE POISON BELOW.
	 *
	 * Three distinct populations are in play at once here and a collision
	 * between any two makes a failure unreadable:
	 *
	 *     DEFAULT      what a fresh view model holds (viridis, [0,1], no planes)
	 *     SAVED        what the session captured -- these constants
	 *     POISON       what the model and the volume hold just before the load
	 *
	 * If SAVED equalled DEFAULT, a load that restored nothing would still read
	 * correct. If SAVED equalled POISON, a load that pushed nothing would too.
	 * Repo memory degenerate-data-defeats-assertions.
	 */
	constexpr ECFDVizColorMap SavedMap = ECFDVizColorMap::Inferno;
	constexpr float SavedRangeMin = -3.25f;
	constexpr float SavedRangeMax = 11.75f;

	constexpr ECFDVizColorMap PoisonMap = ECFDVizColorMap::Turbo;
	constexpr float PoisonRangeMin = -101.5f;
	constexpr float PoisonRangeMax = -7.25f;

	/**
	 * A plane distance inside the sample's domain that no default produces.
	 *
	 * The sample is 12 x 4 x 1 m (hand-computed and asserted in
	 * FlowVizVolumeComponentTest), so 0.375 sits inside it on Z and is neither
	 * the midpoint, the extent, nor zero.
	 */
	constexpr double SavedPlaneDistance = 0.375;

	/** Configure a workspace's models the way a user would, checking every step. */
	bool ConfigureSaved(FFlowVizWorkspaceModel& Model, FAutomationTestBase& Test)
	{
		if (!Test.TestTrue(TEXT("CONTROL: the fixture's colour map was accepted"),
				Model.TransferFunction.SetColorMap(SavedMap).IsOk()))
		{
			return false;
		}
		if (!Test.TestTrue(TEXT("CONTROL: the fixture's manual range was accepted"),
				Model.TransferFunction.SetManualRange(SavedRangeMin, SavedRangeMax).IsOk()))
		{
			return false;
		}

		FFlowVizClipPlane Plane;
		Plane.Normal = FVector(0.0, 0.0, 1.0);
		Plane.Distance = SavedPlaneDistance;
		Plane.bEnabled = true;
		if (!Test.TestTrue(TEXT("CONTROL: the fixture's clip plane was accepted"),
				Model.Clip.AddPlane(Plane).IsOk()))
		{
			return false;
		}
		if (!Test.TestTrue(TEXT("CONTROL: the fixture's probe was accepted"),
				Model.Probes.AddProbeAtSolverPosition(
					FVector(1.25, 0.75, 0.25), TEXT("session probe")).IsValid()))
		{
			return false;
		}

		// READ BACK, not assumed from the results above. A setter that returned Ok
		// and stored nothing would pass all three checks.
		return Test.TestEqual(TEXT("CONTROL: the fixture really holds the chosen map"),
				   static_cast<int32>(Model.TransferFunction.GetColorMap()),
				   static_cast<int32>(SavedMap))
			&& Test.TestEqual(TEXT("CONTROL: and the chosen plane"), Model.Clip.GetPlaneCount(), 1)
			&& Test.TestEqual(TEXT("CONTROL: and the chosen probe"), Model.Probes.GetProbes().Num(), 1);
	}

	/**
	 * Overwrite the model AND the volume with a recognisable wrong scene.
	 *
	 * BOTH SIDES, because they answer different questions. Poisoning the MODEL
	 * makes "the session was applied" falsifiable; poisoning the VOLUME makes
	 * "the application was pushed" falsifiable. Poisoning only the model would
	 * leave the volume holding whatever the last push put there -- which, in a
	 * test that pushed the saved values before saving, is the very thing the
	 * assertions look for. Repo memory a-defaults-helper-can-supply-the-observed
	 * -value: ask what wrote the value you are reading.
	 */
	bool Poison(
		FFlowVizWorkspaceModel& Model, UCFDVizVolumeComponent* Volume, FAutomationTestBase& Test)
	{
		if (!Test.TestTrue(TEXT("CONTROL: the poison colour map was accepted, so a later reading "
								"of the saved map means the load overwrote it rather than that "
								"the poison never landed"),
				Model.TransferFunction.SetColorMap(PoisonMap).IsOk()))
		{
			return false;
		}
		if (!Test.TestTrue(TEXT("CONTROL: the poison range was accepted"),
				Model.TransferFunction.SetManualRange(PoisonRangeMin, PoisonRangeMax).IsOk()))
		{
			return false;
		}

		Model.Clip.RemoveAllPlanes();
		if (!Test.TestEqual(TEXT("CONTROL: the model's planes were cleared, so a plane read back "
								 "after the load is one the session restored"),
				Model.Clip.GetPlaneCount(), 0))
		{
			return false;
		}

		// THE VOLUME IS POISONED THROUGH THE PRODUCTION PUSH, not by reaching into
		// it -- the same call the code under test uses. A direct SetTransferFunction
		// here would let this helper diverge from what a real push can express.
		if (!Test.TestTrue(TEXT("CONTROL: the poisoned model reached the volume, so what the "
								"volume holds at the load is the poison and not a default"),
				FFlowVizWorkspaceModel::PushTransferFunctionToVolume(
					Model.TransferFunction, Volume)))
		{
			return false;
		}
		if (!Test.TestTrue(TEXT("CONTROL: and the cleared clip model reached it"),
				FFlowVizWorkspaceModel::PushClipToVolume(Model.Clip, Volume)))
		{
			return false;
		}

		return Test.TestEqual(TEXT("CONTROL: the volume really holds the poison map"),
				   static_cast<int32>(Volume->GetTransferFunction().GetColorMap()),
				   static_cast<int32>(PoisonMap))
			&& Test.TestEqual(TEXT("CONTROL: and really holds no planes"),
				   Volume->GetClip().GetPlaneCount(), 0);
	}
}

/**
 * A SESSION LOAD MOVES THE IMAGE, NOT JUST THE PANELS.
 *
 * THE GAP THIS EXISTS FOR. FFlowVizWorkspaceModel::LoadSession restores the
 * transfer function and the clip planes into the VIEW MODELS. Nothing copied
 * them into the bound UCFDVizVolumeComponent afterwards, because every existing
 * push is triggered by a panel's change delegate and a session load changes the
 * models underneath the panels without any of them announcing anything.
 *
 * The symptom is the one #50 had one level down: the user loads a session, the
 * controls all read correctly, and the render is still the previous session's
 * until they nudge something. A screenshot of the UI at that moment looks
 * entirely correct, which is what makes it worth a test rather than a glance.
 *
 * WHY THE MISSING-CASE ARM IS NOT AN EXTRA. LoadSession returns NON-Ok when the
 * session's case has moved, and still applies every panel -- that is plan.md
 * section 14's relink contract, asserted in FlowViz.UI.WorkspaceModel.Session
 * MissingCase. So an entry point that pushed only on success would leave the
 * render disagreeing with the panels in exactly the flow the relink prompt
 * exists to serve. The obvious implementation has that bug and passes the happy
 * arm, so the happy arm alone would not have found it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceSessionSeamTest,
	"FlowViz.UI.Workspace.SessionSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceSessionSeamTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceSessionSeamTest;

	const FString CaseDir = GetSampleCaseDir();
	if (!TestTrue(TEXT("CONTROL: the sample case is on disk -- without it the volume has no "
					   "domain, the clip push is correctly refused, and every clip assertion "
					   "below would be vacuous"),
			!CaseDir.IsEmpty() && FPaths::DirectoryExists(CaseDir)))
	{
		return false;
	}

	const FString SessionDir = GetSessionDir(TEXT("Seam"));
	const FString SessionPath = FPaths::Combine(SessionDir, TEXT("seam.cfdvizsession"));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*SessionDir, /*RequireExists=*/false, /*Tree=*/true);
	};

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
	if (!TestNotNull(TEXT("CONTROL: a case actor spawns"), Actor))
	{
		return false;
	}
	UCFDVizVolumeComponent* Volume = Actor->GetVolumeComponent();
	if (!TestNotNull(TEXT("CONTROL: the actor owns a volume component"), Volume))
	{
		return false;
	}
	if (!TestTrue(TEXT("CONTROL: the sample case loads into the volume, giving it the domain "
					   "every clip push is normalised against"),
			Actor->LoadCase(CaseDir).IsOk()))
	{
		return false;
	}

	/* == A session is written by a real workspace ============================ */

	{
		const TSharedRef<SFlowVizWorkspace> Author = SNew(SFlowVizWorkspace);
		if (!TestTrue(TEXT("CONTROL: the authoring workspace opens the sample case, so the "
						   "session it writes names a case that exists"),
				Author->GetModel().OpenCase(CaseDir).IsOk()))
		{
			return false;
		}
		if (!ConfigureSaved(Author->GetModel(), *this))
		{
			return false;
		}
		if (!TestTrue(TEXT("CONTROL: the session was written"),
				Author->GetModel().SaveSession(SessionPath).IsOk()))
		{
			return false;
		}
	}

	/* == The workspace that loads it ========================================= */

	const TSharedRef<SFlowVizWorkspace> Workspace = SNew(SFlowVizWorkspace);
	Workspace->SetVolume(Volume);
	if (!TestTrue(TEXT("CONTROL: the workspace is bound to the volume this test reads, so a "
					   "reading below that never changed means the push did not happen rather "
					   "than that it went somewhere else"),
			Workspace->GetVolume() == Volume))
	{
		return false;
	}

	if (!Poison(Workspace->GetModel(), Volume, *this))
	{
		return false;
	}

	const FCFDVizResult Loaded = Workspace->LoadSession(SessionPath);
	TestTrue(*FString::Printf(TEXT("the load reports success (%s)"), *Loaded.ToString()),
		Loaded.IsOk());

	/* --- The models, so a push failure is distinguishable from a load one --- */

	TestEqual(TEXT("the session's colour map reached the view model"),
		static_cast<int32>(Workspace->GetModel().TransferFunction.GetColorMap()),
		static_cast<int32>(SavedMap));

	/* --- Explicit-refresh panels: the session changed their row populations. -- */
	TestEqual(TEXT("the clip panel rebuilt rows for the restored planes"),
		Workspace->GetClipPanel()->GetPlaneRowCount(),
		Workspace->GetModel().Clip.GetPlaneCount());
	TestEqual(TEXT("the probe panel rebuilt rows for the restored probes"),
		Workspace->GetProbePanel()->GetProbeRowCount(),
		Workspace->GetModel().Probes.GetProbes().Num());
	TestEqual(TEXT("the pipeline panel rebuilt fields for the restored case"),
		Workspace->GetPipelinePanel()->GetFieldRowCount(),
		Workspace->GetModel().GetVolumeFieldIds().Num());

	/* --- The volume: the link this test exists for -------------------------- */

	TestEqual(TEXT("and the session's colour map reached the VOLUME, so the image the user is "
				   "looking at agrees with the panel that describes it"),
		static_cast<int32>(Volume->GetTransferFunction().GetColorMap()),
		static_cast<int32>(SavedMap));

	TestEqual(TEXT("the session's value range reached the volume as well -- the map alone would "
				   "pass on a push that copied one field"),
		Volume->GetTransferFunction().GetRangeMax(), SavedRangeMax);

	TestEqual(TEXT("and the session's clip plane reached the volume, which is the channel with "
				   "the domain precondition and so the one that fails separately"),
		Volume->GetClip().GetPlaneCount(), 1);

	return true;
}

/**
 * THE RELINK FLOW: a load that REPORTS A FAILURE still moves the image.
 *
 * Split from the arm above rather than folded into it because the two differ in
 * what LoadSession RETURNS, and an entry point written as
 *
 *     if (Model->LoadSession(Path).IsOk()) { PushToVolume(); }
 *
 * passes the arm above and fails this one. That is the whole reason this test
 * is here: the guard reads as prudent, and it is precisely backwards. LoadSession
 * documents that a non-Ok result "does NOT mean nothing happened", and a missing
 * case is the case where the most has happened -- every panel applied, the case
 * absent, a relink about to be offered.
 *
 * THE SESSION IS WRITTEN BY A REAL SAVE and then its case is moved on disk,
 * rather than hand-editing a JSON file. A hand-written session could omit a key
 * the real capture always writes, and the test would then be about a shape
 * production never produces.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceSessionSeamMissingCaseTest,
	"FlowViz.UI.Workspace.SessionSeamMissingCase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceSessionSeamMissingCaseTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceSessionSeamTest;

	const FString CaseDir = GetSampleCaseDir();
	if (!TestTrue(TEXT("CONTROL: the sample case is on disk"),
			!CaseDir.IsEmpty() && FPaths::DirectoryExists(CaseDir)))
	{
		return false;
	}

	const FString SessionDir = GetSessionDir(TEXT("SeamMissingCase"));
	const FString SessionPath = FPaths::Combine(SessionDir, TEXT("seam.cfdvizsession"));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*SessionDir, /*RequireExists=*/false, /*Tree=*/true);
	};

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
	if (!TestNotNull(TEXT("CONTROL: a case actor spawns"), Actor))
	{
		return false;
	}
	UCFDVizVolumeComponent* Volume = Actor->GetVolumeComponent();
	if (!TestNotNull(TEXT("CONTROL: the actor owns a volume component"), Volume))
	{
		return false;
	}
	if (!TestTrue(TEXT("CONTROL: the sample case loads into the volume"),
			Actor->LoadCase(CaseDir).IsOk()))
	{
		return false;
	}

	/* == A real session, then a real parse ==================================== */

	{
		const TSharedRef<SFlowVizWorkspace> Author = SNew(SFlowVizWorkspace);
		if (!TestTrue(TEXT("CONTROL: the authoring workspace opens the sample case"),
				Author->GetModel().OpenCase(CaseDir).IsOk()))
		{
			return false;
		}
		if (!ConfigureSaved(Author->GetModel(), *this))
		{
			return false;
		}
		if (!TestTrue(TEXT("CONTROL: the session was written"),
				Author->GetModel().SaveSession(SessionPath).IsOk()))
		{
			return false;
		}
	}

	FFlowVizSessionState State;
	if (!TestTrue(TEXT("CONTROL: the session parses"),
			FlowVizSession::LoadFromFile(SessionPath, State).IsOk()))
	{
		return false;
	}
	if (!TestTrue(TEXT("CONTROL: the parse FOUND the case, so repointing it below is a change "
					   "rather than a restatement of what the parse already produced"),
			State.bCaseFound))
	{
		return false;
	}

	const FString MovedAway = FPaths::Combine(SessionDir, TEXT("gone"), TEXT("manifest.json"));
	if (!TestFalse(*FString::Printf(TEXT("CONTROL: the path the case is moved to genuinely does "
										 "not exist, so the load below really takes the "
										 "missing-case branch: '%s'"),
					   *MovedAway),
			FPaths::FileExists(MovedAway)))
	{
		return false;
	}
	State.ResolvedCasePath = MovedAway;
	State.bCaseFound = false;

	/* == The workspace that applies it ======================================= */

	const TSharedRef<SFlowVizWorkspace> Workspace = SNew(SFlowVizWorkspace);
	Workspace->SetVolume(Volume);

	if (!Poison(Workspace->GetModel(), Volume, *this))
	{
		return false;
	}

	const FCFDVizResult Applied = Workspace->LoadState(State);

	// ASSERTED, NOT ASSUMED. If this ever came back Ok the arm would silently
	// become a duplicate of the happy-path test above -- green, and testing
	// nothing this file was written for.
	if (!TestFalse(TEXT("CONTROL: the load REPORTS the missing case, which is the condition "
						"under which the push below must still have happened"),
			Applied.IsOk()))
	{
		return false;
	}
	TestEqual(TEXT("CONTROL: and reports it as a missing FILE rather than as some other failure "
				   "that would make this arm about the wrong branch"),
		static_cast<int32>(Applied.Error), static_cast<int32>(ECFDVizError::FileNotFound));

	TestEqual(TEXT("the session's colour map reached the VOLUME even though the load reported a "
				   "failure -- the relink prompt is offered against a render that already shows "
				   "the restored setup"),
		static_cast<int32>(Volume->GetTransferFunction().GetColorMap()),
		static_cast<int32>(SavedMap));

	TestEqual(TEXT("and the session's clip plane reached the volume on that same reported failure"),
		Volume->GetClip().GetPlaneCount(), 1);

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
