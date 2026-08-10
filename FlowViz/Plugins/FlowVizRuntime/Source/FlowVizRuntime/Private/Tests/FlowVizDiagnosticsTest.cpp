// Copyright FlowViz contributors. All Rights Reserved.

#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RenderTimer.h"
#include "Scene/FlowVizCaseActor.h"
#include "Scene/FlowVizVolumeComponent.h"
#include "UI/FlowVizDiagnostics.h"
#include "UI/FlowVizWorkspaceModel.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED NAMESPACE, NOT ANONYMOUS -- FlowVizRuntime is a unity build and an
 * anonymous namespace merges with every other one in the same blob rather than
 * being file-local. That collision already shipped once in this repo (#37).
 */
namespace FlowVizDiagnosticsTest
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

	/*
	 * KNOWN VALUES OF THE COMMITTED SAMPLE. Asserted rather than read back from
	 * the same manifest the code under test read, so a manifest that changes
	 * shape fails here instead of silently redefining what the test proves.
	 */
	/*
	 * SEVEN, NOT THE MANIFEST'S EIGHT. The sample declares eight fields, one of
	 * which is `validMask`; GetVolumeFieldIds excludes mask fields because
	 * offering one in a picker invites selecting a field that renders as a solid
	 * block. The overlay lists what a user can actually display, so the mask is
	 * excluded here too -- and the count is spelled 7 rather than
	 * Fields.Num() - 1 so that a change in the exclusion rule fails here.
	 */
	constexpr int32 SampleFieldCount = 7;
	const FIntVector SampleDimensions(56, 28, 6);

	/** Frame spacing in the sample's timeline, in solver units. */
	constexpr double SampleFrameSpacing = 0.05;

	/**
	 * RESTORES WHAT IT MUTATES. This test drives three engine globals to known
	 * values. They are process-wide: leaving GGameThreadTime at a fabricated
	 * number would corrupt any later test that reads it, and the corruption
	 * would surface as an unrelated failure in an unrelated file.
	 */
	struct FEngineTimingScope
	{
		double SavedDelta;
		uint32 SavedGameThreadTime;
		uint32 SavedRenderThreadTime;

		FEngineTimingScope()
			: SavedDelta(FApp::GetDeltaTime())
			, SavedGameThreadTime(GGameThreadTime)
			, SavedRenderThreadTime(GRenderThreadTime)
		{
		}

		~FEngineTimingScope()
		{
			FApp::SetDeltaTime(SavedDelta);
			GGameThreadTime = SavedGameThreadTime;
			GRenderThreadTime = SavedRenderThreadTime;
		}
	};

	/** Cycles that convert to a given millisecond count on this platform. */
	uint32 CyclesForMs(double Milliseconds)
	{
		return static_cast<uint32>(Milliseconds / 1000.0 / FPlatformTime::GetSecondsPerCycle());
	}

	/**
	 * SEEKING IS NOT LOADING, and LOADING IS NOT UPLOADING. A bare SeekToFrame
	 * queues nothing, so WaitForPendingLoads returns true immediately and the
	 * texture set stays empty; Tick starts the decode, the wait finishes it, the
	 * next Tick drains it into the set. Repo memory seek-is-not-load.
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
			Player.WaitForPendingLoads(1.0);
		}
		return true;
	}

	/**
	 * A copy of the sample whose frame 0 is CORRUPT, for the failure counter.
	 *
	 * WHY A WHOLE CASE AND NOT A MISSING FILE. LoadsFailed can only be asserted
	 * at zero against the committed sample, and a row hardcoded to 0 passes that
	 * -- the same un-falsifiable shape this file exists to refuse. Driving it
	 * non-zero needs a decode that STARTS and then fails, which means the
	 * manifest must resolve the path (so the request is made) while the file
	 * itself is unreadable (so the task reports a failure rather than the
	 * request being dropped before it counts).
	 *
	 * The corruption is the CVF magic. It is caught in the reader's first
	 * header check, so the failure is deterministic and does not depend on
	 * zlib's behaviour over arbitrary bytes.
	 *
	 * @return false if the fixture could not be built; the caller skips rather
	 *         than asserting against a case that is not what it thinks.
	 */
	bool MakeCaseWithCorruptFrame(const FString& DestDir, int32 CorruptFrame)
	{
		IFileManager& Files = IFileManager::Get();
		Files.DeleteDirectory(*DestDir, /*RequireExists*/ false, /*Tree*/ true);

		const FString SourceDir = GetSampleCaseDir();
		if (SourceDir.IsEmpty() || !Files.DirectoryExists(*SourceDir))
		{
			return false;
		}

		/*
		 * CopyDirectoryTree, NOT IFileManager::Copy. Copy() returns a uint32
		 * EFileOperationResult whose SUCCESS value is 0, so `!Copy(...)` reads as
		 * "it worked" and a naive bool test inverts the result -- which is how an
		 * earlier draft of this helper silently copied nothing and then reported
		 * the missing frame file as a corruption failure.
		 */
		if (!FPlatformFileManager::Get().GetPlatformFile().CopyDirectoryTree(
				*DestDir, *SourceDir, /*bOverwriteAllExisting*/ true))
		{
			return false;
		}

		const FString FramePath = FPaths::Combine(
			DestDir, TEXT("frames"), FString::Printf(TEXT("%06d"), CorruptFrame), TEXT("speed.cvf"));

		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *FramePath) || Bytes.Num() < 8)
		{
			return false;
		}

		// Overwrite the 8-byte magic with something that is not it. The rest of
		// the file is left intact so the failure is attributable to one cause.
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Bytes[Index] = static_cast<uint8>('X');
		}
		return FFileHelper::SaveArrayToFile(Bytes, *FramePath);
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
}

/**
 * EVERY OVERLAY ROW IS A MEASUREMENT, AND THIS IS WHAT MAKES IT ONE.
 *
 * The hazard this test exists for: a diagnostics overlay is the easiest place in
 * a codebase to ship a lie. Every row is a number beside a label, and a hardcoded
 * zero is indistinguishable from a real reading of zero -- worse than an omitted
 * row, because a reader takes it as evidence. "The overlay rendered" and "the
 * field is present" both pass against a struct of constants.
 *
 * So each field below is DRIVEN to a known non-zero value through production's
 * own writers, and the assertion names that exact value. A collector that
 * returned a default-constructed snapshot fails every arm.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizDiagnosticsCollectTest,
	"FlowViz.UI.Diagnostics.Collect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizDiagnosticsCollectTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizDiagnosticsTest;

	FEngineTimingScope TimingScope;

	FFlowVizWorkspaceModel Model;

	/* == Before a case: the collector must say so, not report zeros ========== */

	const FFlowVizDiagnosticsSnapshot Empty = FlowVizDiagnostics::Collect(Model);

	if (!TestFalse(
			TEXT("with no case open, bCaseOpen is false -- so a reader can tell "
				 "'nothing loaded' from 'loaded and measuring zero'"),
			Empty.bCaseOpen))
	{
		return false;
	}

	TestTrue(
		TEXT("with no selection yet made, the cache hit rate is NEGATIVE rather than 0.0 -- "
			 "a rate of zero means every selection missed, which is a different and real state"),
		Empty.CacheHitRate < 0.0);

	/* == Engine timing: driven to exact values, asserted exactly ============= */

	// 60 fps exactly. Chosen so the reciprocal is exact in binary and the
	// assertion needs no tolerance argument.
	FApp::SetDeltaTime(1.0 / 60.0);
	GGameThreadTime = CyclesForMs(4.0);
	GRenderThreadTime = CyclesForMs(7.0);

	{
		const FFlowVizDiagnosticsSnapshot Timing = FlowVizDiagnostics::Collect(Model);

		TestEqual(
			TEXT("FPS is DERIVED from the engine's frame delta: a 1/60 s delta reports 60 fps"),
			Timing.FramesPerSecond, 60.0, 1e-9);

		TestEqual(
			TEXT("the raw frame delta is carried through, so the derivation is auditable"),
			Timing.FrameDeltaSeconds, 1.0 / 60.0, 1e-12);

		TestEqual(
			TEXT("game thread ms converts GGameThreadTime cycles -- 4 ms of cycles reads as 4 ms"),
			Timing.GameThreadMs, 4.0, 0.01);

		TestEqual(
			TEXT("render thread ms converts GRenderThreadTime, and is NOT a copy of the game "
				 "thread's -- 7 ms and 4 ms are distinct so a swapped source is visible"),
			Timing.RenderThreadMs, 7.0, 0.01);
	}

	/* == Open the sample and measure a real case ============================= */

	const FString CaseDir = GetSampleCaseDir();
	if (!TestTrue(TEXT("the committed sample case opens"), Model.OpenCase(CaseDir, FName(TEXT("speed"))).IsOk()))
	{
		return false;
	}

	// A frame that is neither the first nor the last, so an off-by-one or a
	// reset-to-zero is visible rather than coincidentally correct.
	constexpr int32 TargetFrame = 7;
	Model.Player.SeekToFrame(TargetFrame);

	{
		const FFlowVizDiagnosticsSnapshot Open = FlowVizDiagnostics::Collect(Model);

		TestTrue(TEXT("with a case open, bCaseOpen is true"), Open.bCaseOpen);

		TestEqual(
			TEXT("the current frame row reports the frame the playhead was SEEKED to, "
				 "not the first frame and not zero"),
			Open.FrameA, TargetFrame);

		TestEqual(
			TEXT("physical time is the playhead in SOLVER units -- frame 7 of a case spaced "
				 "0.05 apart is t=0.35, not the frame index and not wall-clock seconds"),
			Open.PhysicalTime, TargetFrame * SampleFrameSpacing, 1e-9);

		TestTrue(
			TEXT("the frame count is the sample's whole timeline, non-zero"),
			Open.FrameCount > 1);

		TestEqual(
			TEXT("every volume field the sample declares is listed -- the count is the "
				 "manifest's, so a collector returning an empty array fails here"),
			Open.LoadedFields.Num(), SampleFieldCount);

		TestTrue(
			TEXT("the field ids are the case's own, not placeholders: 'speed' is among them"),
			Open.LoadedFields.Contains(FName(TEXT("speed"))));

		TestFalse(
			TEXT("and the mask field is NOT listed -- the overlay reports what a user can "
				 "display, so a collector reading Case->Fields raw is visible here rather "
				 "than only in the count"),
			Open.LoadedFields.Contains(FName(TEXT("validMask"))));

		/* -- Cache budgets: set to a known non-default, read back ------------ */

		// Deliberately not the 512/256 MB defaults, so a collector reporting the
		// constants instead of the live cache is caught.
		constexpr int64 CpuBudget = 300 * 1024 * 1024;
		constexpr int64 GpuBudget = 150 * 1024 * 1024;
		Model.Player.SetMemoryBudgets(CpuBudget, GpuBudget);

		const FFlowVizDiagnosticsSnapshot Budgeted = FlowVizDiagnostics::Collect(Model);

		TestEqual(
			TEXT("the CPU cache budget row reads the LIVE budget just set, not the default"),
			Budgeted.Cache.CpuBudgetBytes, CpuBudget);

		TestEqual(
			TEXT("the GPU cache budget row reads the live GPU budget, and is not a copy of "
				 "the CPU one -- the two values differ so a crossed wire is visible"),
			Budgeted.Cache.GpuBudgetBytes, GpuBudget);

		/* -- Clip planes: two defined, one enabled --------------------------- */

		// The clip view model IS a member of the workspace model, so these rows
		// come from the same object the clip panel edits.
		FFlowVizClipPlane PlaneA;
		PlaneA.bEnabled = true;
		FFlowVizClipPlane PlaneB;
		PlaneB.bEnabled = true;

		Model.Clip.AddPlane(PlaneA);
		Model.Clip.AddPlane(PlaneB);
		Model.Clip.SetPlaneEnabled(1, false);

		const FFlowVizDiagnosticsSnapshot Clipped = FlowVizDiagnostics::Collect(Model);

		TestEqual(
			TEXT("the clipping row counts the planes actually defined"),
			Clipped.ClipPlaneCount, 2);

		TestEqual(
			TEXT("and counts ENABLED planes separately -- two defined with one switched off "
				 "reports 1, so a row copying the total is visible"),
			Clipped.EnabledClipPlaneCount, 1);
	}

	/* == The formatted form must carry the values, not just labels =========== */

	/*
	 * A FORMATTER THAT PRINTS LABELS BESIDE BLANKS is the same failure as a
	 * collector that returns constants, one layer further out: the overlay looks
	 * complete and reports nothing. Built from a hand-made snapshot rather than a
	 * collected one so the expected strings are known exactly and this arm fails
	 * for formatting reasons only.
	 */
	{
		FFlowVizDiagnosticsSnapshot Snapshot;
		Snapshot.bCaseOpen = true;
		Snapshot.FramesPerSecond = 59.94;
		Snapshot.FrameA = 12;
		Snapshot.CacheHitRate = 0.75;
		Snapshot.VolumeDimensions = SampleDimensions;

		const FString Text = FlowVizDiagnostics::Format(Snapshot);

		TestTrue(
			TEXT("the formatted overlay contains the FPS VALUE, not merely the word 'FPS'"),
			Text.Contains(TEXT("59.9")));

		TestTrue(
			TEXT("the formatted overlay contains the current frame's value"),
			Text.Contains(TEXT("12")));

		TestTrue(
			TEXT("the cache hit rate is rendered as a percentage a reader can act on"),
			Text.Contains(TEXT("75")));

		TestTrue(
			TEXT("the volume resolution is rendered with all three dimensions"),
			Text.Contains(TEXT("56")) && Text.Contains(TEXT("28")) && Text.Contains(TEXT("6")));
	}

	Model.CloseCase();
	return true;
}

/**
 * THE TEN ROWS THE FIRST TEST DOES NOT NAME.
 *
 * WHY THIS FILE NEEDED A THIRD TEST. Two deliberate mutations of the collector
 * (a crossed thread-timing source, a hardcoded resolution) were both caught by
 * the tests above, and that is exactly what made the file look finished. An
 * audit of the snapshot field by field found ten of its twenty-six members that
 * no assertion in this file mentions at all:
 *
 *     FrameB, InterpolationAlpha, DisplayFrameA, DisplayFrameB,
 *     LoadsInFlight, LoadsStarted, LoadsCompleted, LoadsFailed,
 *     CacheHits, CacheMisses
 *
 * Every one could have been deleted, crossed with its neighbour, or replaced
 * with a constant, and both tests above would still have passed. Two of them --
 * CacheHits and CacheMisses -- are the inputs to the cache hit rate, the row
 * task #53 singled out as the archetype of a number that is read as a
 * measurement. The rate itself IS asserted, which is what made the cache section
 * scan as covered; the asserted arm proves only the negative sentinel, and says
 * nothing about the arithmetic above it.
 *
 * These counters are DEVICE-INDEPENDENT: the decode path increments them all
 * before it reaches the texture set, and with no texture set bound the upload is
 * skipped entirely (FlowVizCasePlayer.cpp, DrainCompletedLoads). So this test
 * runs under -nullrhi with nothing skipped, unlike the resolution row below.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizDiagnosticsCountersTest,
	"FlowViz.UI.Diagnostics.Counters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizDiagnosticsCountersTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizDiagnosticsTest;

	FFlowVizWorkspaceModel Model;

	const FString CaseDir = GetSampleCaseDir();
	if (!TestTrue(TEXT("the committed sample case opens"), Model.OpenCase(CaseDir, FName(TEXT("speed"))).IsOk()))
	{
		return false;
	}
	ON_SCOPE_EXIT { Model.CloseCase(); };

	/* == A fresh case reports zeros that are REAL zeros ====================== */

	/*
	 * The baseline matters as much as the driven values. Without it, a collector
	 * that reported a constant 3 for LoadsStarted would pass the "greater than
	 * zero" arms below. Open() resets every counter, so these zeros are the
	 * player's own, and the arms after this one measure a CHANGE from them.
	 */
	{
		const FFlowVizDiagnosticsSnapshot Fresh = FlowVizDiagnostics::Collect(Model);

		TestEqual(TEXT("a freshly-opened case has started no loads"), Fresh.LoadsStarted, (int64)0);
		TestEqual(TEXT("...completed none"), Fresh.LoadsCompleted, (int64)0);
		TestEqual(TEXT("...failed none"), Fresh.LoadsFailed, (int64)0);
		TestEqual(TEXT("...has none in flight"), Fresh.LoadsInFlight, 0);
		TestEqual(TEXT("...and has neither hit nor missed the cache"), Fresh.CacheHits, (int64)0);
		TestEqual(TEXT("...(misses too)"), Fresh.CacheMisses, (int64)0);
	}

	/* == Interpolation: a playhead BETWEEN two stored frames ================= */

	/*
	 * Seeking to a frame INDEX can never move FrameB off FrameA -- TryBracket
	 * returns alpha 0 with A == B at an exact stored time, which is why the test
	 * above could assert FrameA and learn nothing about FrameB. The sample
	 * stores frame 2 at t=0.10 and frame 3 at t=0.15, so t=0.12 sits two fifths
	 * of the way between them.
	 *
	 * 0.4 RATHER THAN 0.5, deliberately, and this arm was written at 0.5 first
	 * by mistake: the midpoint is the one alpha a collector could hardcode and
	 * still look right, and it is also the exact >= 0.5 threshold SelectFrames
	 * uses to pick NearestFrame, so a midpoint fixture straddles a branch
	 * boundary as well as being unfalsifiable.
	 */
	constexpr double BetweenTime = 0.12;
	constexpr double BetweenAlpha = 0.4;
	Model.Player.SeekToTime(BetweenTime);

	{
		const FFlowVizDiagnosticsSnapshot Between = FlowVizDiagnostics::Collect(Model);

		TestEqual(TEXT("the playhead brackets from frame 2..."), Between.FrameA, 2);

		TestEqual(
			TEXT("...to frame 3 -- FrameB is a SEPARATE reading, so a collector copying "
				 "FrameA into it (which is correct at every stored frame) fails here"),
			Between.FrameB, 3);

		TestEqual(
			TEXT("and the blend between them is 0.4 -- not 0.5, which is the value a "
				 "hardcoded midpoint would report, and not 0.0"),
			Between.InterpolationAlpha, BetweenAlpha, 1e-9);

		TestEqual(
			TEXT("physical time is the playhead's own position, BETWEEN the two frames' "
				 "stored times rather than snapped to either"),
			Between.PhysicalTime, BetweenTime, 1e-9);
	}

	/* == Display lags selection while the frames are still decoding ========== */

	/*
	 * DisplayFrameA/B are what the renderer may sample; FrameA/B are what the
	 * playhead wants. They differ during a stall, and that difference is the
	 * whole reason both pairs are on the overlay -- a collector that filled the
	 * display rows from the selection would print a frame whose voxels have not
	 * been written, which is the "never display a partial frame" rule inverted.
	 *
	 * Nothing has been decoded yet, so the display is still the invalid pair
	 * Open() installed while the selection names frames 2 and 3.
	 */
	{
		const FFlowVizDiagnosticsSnapshot Stalled = FlowVizDiagnostics::Collect(Model);

		TestEqual(
			TEXT("with nothing decoded, the DISPLAY frame is INDEX_NONE even though the "
				 "playhead has selected frame 2 -- so the two pairs are separately sourced"),
			Stalled.DisplayFrameA, (int32)INDEX_NONE);

		TestEqual(
			TEXT("...and so is the display's B"),
			Stalled.DisplayFrameB, (int32)INDEX_NONE);
	}

	/* == Ticking starts real decodes ========================================= */

	/*
	 * Tick is production's own driver: it drains, starts pending loads, and
	 * resolves the display. Nothing here reaches into the player to set a
	 * counter.
	 */
	Model.Player.Tick(0.0);

	{
		const FFlowVizDiagnosticsSnapshot Started = FlowVizDiagnostics::Collect(Model);

		TestTrue(
			TEXT("a tick over a non-resident selection STARTS decodes -- the row is a "
				 "count of real work, so a collector returning 0 fails here"),
			Started.LoadsStarted > 0);

		TestEqual(
			TEXT("every started decode is charged as a cache MISS, since none was "
				 "resident -- the two counters agree because the same requests drove both"),
			Started.CacheMisses, Started.LoadsStarted);

		TestEqual(
			TEXT("and no request found a resident frame, so hits are still zero -- "
				 "asserted rather than assumed, because a collector crossing hits with "
				 "misses would otherwise pass every arm in this block"),
			Started.CacheHits, (int64)0);

		TestTrue(
			TEXT("the in-flight row counts decodes running RIGHT NOW: non-zero between "
				 "the tick that started them and the tick that drains them"),
			Started.LoadsInFlight > 0);

		TestEqual(
			TEXT("in flight is bounded by the concurrency limit the player reports"),
			Started.LoadsInFlight, Model.Player.GetMaxConcurrentLoads());

		TestEqual(
			TEXT("nothing has finished yet, so completed is still zero -- this is what "
				 "separates 'started' from 'completed' rather than one number twice"),
			Started.LoadsCompleted, (int64)0);
	}

	/* == Draining completes them ============================================= */

	/*
	 * PUMPED TO QUIESCENCE, NOT TICKED ONCE. A single drain does not empty the
	 * pipeline: the same Tick that drains a result also calls StartPendingLoads,
	 * which spends the freed concurrency on the preload frames around the
	 * selection. So in-flight is legitimately non-zero after one tick, and this
	 * loop was written asserting otherwise -- it failed against correct code.
	 *
	 * Bounded by a tick count rather than wall-clock, so a player that genuinely
	 * leaked in-flight work exits the loop and fails the assertion below instead
	 * of hanging until the suite times out.
	 */
	constexpr int32 MaxDrainTicks = 64;
	int32 DrainTicks = 0;
	while (DrainTicks < MaxDrainTicks)
	{
		if (!TestTrue(TEXT("CONTROL: the started decodes finish"), Model.Player.WaitForPendingLoads(60.0)))
		{
			return false;
		}
		Model.Player.Tick(0.0);
		++DrainTicks;

		if (Model.Player.GetDiagnostics().LoadsInFlight == 0)
		{
			break;
		}
	}

	int64 CompletedAfterDrain = 0;
	{
		const FFlowVizDiagnosticsSnapshot Drained = FlowVizDiagnostics::Collect(Model);
		CompletedAfterDrain = Drained.LoadsCompleted;

		TestTrue(
			TEXT("the drain moves decodes into COMPLETED -- a separate counter from "
				 "started, incremented at a different point in the pipeline"),
			Drained.LoadsCompleted > 0);

		TestEqual(
			TEXT("and the in-flight row empties once the preload settles: a row that "
				 "stayed non-zero with no work outstanding would be reporting a leak "
				 "that is not happening"),
			Drained.LoadsInFlight, 0);

		TestTrue(
			TEXT("CONTROL: the pipeline drained within the tick budget, so the zero above "
				 "means 'quiesced' and not 'gave up'"),
			DrainTicks < MaxDrainTicks);

		TestEqual(
			TEXT("the sample's frames all decode, so nothing FAILED -- the failure row is "
				 "read as 'the data is intact', and a row wired to the wrong counter "
				 "would report otherwise here"),
			Drained.LoadsFailed, (int64)0);

		TestEqual(
			TEXT("the display has caught up to the selection's A"),
			Drained.DisplayFrameA, 2);

		TestEqual(
			TEXT("...and to its B, so the two rows track the pair independently"),
			Drained.DisplayFrameB, 3);
	}

	/* == Re-requesting a RESIDENT frame is a hit, not a miss ================= */

	/*
	 * The arithmetic behind the hit-rate row, which until now had only its
	 * not-yet-measured sentinel asserted. Seeking back to a frame that is still
	 * cached takes RequestFrame's resident branch: hits move, misses do not, and
	 * the rate follows from both.
	 */
	{
		const FFlowVizDiagnosticsSnapshot Before = FlowVizDiagnostics::Collect(Model);

		Model.Player.SeekToTime(BetweenTime);
		Model.Player.Tick(0.0);

		const FFlowVizDiagnosticsSnapshot After = FlowVizDiagnostics::Collect(Model);

		TestTrue(
			TEXT("re-selecting frames that are already resident registers cache HITS"),
			After.CacheHits > Before.CacheHits);

		TestEqual(
			TEXT("and starts no new decode -- so the hit is a hit, not a miss counted "
				 "twice under two labels"),
			After.LoadsStarted, Before.LoadsStarted);

		TestEqual(
			TEXT("misses do not move on a resident selection"),
			After.CacheMisses, Before.CacheMisses);

		TestEqual(
			TEXT("completed does not move either: nothing new was decoded"),
			After.LoadsCompleted, CompletedAfterDrain);

		/* -- The rate is derived from those two, not reported independently -- */

		const int64 Selections = After.CacheHits + After.CacheMisses;
		if (TestTrue(TEXT("CONTROL: selections have been made, so a rate exists"), Selections > 0))
		{
			TestEqual(
				TEXT("the hit rate is hits over hits-plus-misses, computed from the same "
					 "two counters this test drove -- so the row cannot be a constant that "
					 "happens to look plausible"),
				After.CacheHitRate,
				static_cast<double>(After.CacheHits) / static_cast<double>(Selections),
				1e-12);

			TestTrue(
				TEXT("and it is now a real measurement in [0,1], no longer the negative "
					 "not-yet-measured sentinel"),
				After.CacheHitRate >= 0.0 && After.CacheHitRate <= 1.0);

			TestTrue(
				TEXT("with at least one hit and at least one miss recorded, the rate is "
					 "STRICTLY between the ends -- a rate pinned at 0 or 1 is what a row "
					 "reading only one of the two counters would report"),
				After.CacheHitRate > 0.0 && After.CacheHitRate < 1.0);
		}
	}

	return true;
}

/**
 * The failure row, driven NON-ZERO.
 *
 * WHY IT IS NOT IN THE TEST ABOVE. Every arm there runs against the committed
 * sample, where the only thing that can be said about LoadsFailed is that it
 * stays 0 -- and a row hardcoded to 0 passes that assertion forever. The
 * failure counter is the one row on the overlay whose healthy reading is the
 * same as its broken reading, so it needs a case that actually breaks.
 *
 * SEPARATE TEST because it writes a fixture to Saved/ and deletes it again. A
 * fixture that failed to build would otherwise look like a diagnostics failure.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizDiagnosticsFailureTest,
	"FlowViz.UI.Diagnostics.Failures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizDiagnosticsFailureTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizDiagnosticsTest;

	const FString CaseDir = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("FlowVizDiagnosticsFailureTest"), TEXT("CorruptCase.cfdviz"));

	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(
			*FPaths::GetPath(CaseDir), /*RequireExists*/ false, /*Tree*/ true);
	};

	// Frame 0, because the playhead starts there: opening the case and ticking
	// once is enough to request it, with no seek to get wrong.
	constexpr int32 CorruptFrame = 0;
	if (!TestTrue(TEXT("CONTROL: the corrupt-frame fixture was built"),
			MakeCaseWithCorruptFrame(CaseDir, CorruptFrame)))
	{
		return false;
	}

	FFlowVizWorkspaceModel Model;

	/*
	 * THE CASE ITSELF MUST STILL OPEN. If corrupting the frame also broke the
	 * manifest, no decode would ever be requested and LoadsFailed would stay 0
	 * for a reason that has nothing to do with the counter -- the test would
	 * pass its fixture and prove nothing. This asserts the corruption is where
	 * it was aimed.
	 */
	if (!TestTrue(TEXT("CONTROL: the case opens -- only the FRAME is corrupt, not the manifest"),
			Model.OpenCase(CaseDir, FName(TEXT("speed"))).IsOk()))
	{
		return false;
	}
	ON_SCOPE_EXIT { Model.CloseCase(); };

	Model.Player.SeekToFrame(CorruptFrame);
	Model.Player.Tick(0.0);

	{
		const FFlowVizDiagnosticsSnapshot Started = FlowVizDiagnostics::Collect(Model);

		if (!TestTrue(TEXT("CONTROL: a decode was actually started for the corrupt frame"),
				Started.LoadsStarted > 0))
		{
			return false;
		}

		TestEqual(
			TEXT("the failure has not been counted yet -- it is counted when the RESULT is "
				 "drained, not when the request is made"),
			Started.LoadsFailed, (int64)0);
	}

	if (!TestTrue(TEXT("CONTROL: the decode finishes (as a failure)"),
			Model.Player.WaitForPendingLoads(60.0)))
	{
		return false;
	}
	Model.Player.Tick(0.0);

	{
		const FFlowVizDiagnosticsSnapshot Failed = FlowVizDiagnostics::Collect(Model);

		TestTrue(
			TEXT("the failure row reports the decode that FAILED -- driven non-zero, so a "
				 "row hardcoded to 0 (which passes against every healthy case) is visible"),
			Failed.LoadsFailed > 0);

		/*
		 * NOT "COMPLETED IS ZERO". Only frame 0 is corrupt; the preload around it
		 * requests frame 1 from the same (otherwise intact) copy of the sample,
		 * and that one decodes fine. An earlier draft asserted zero here and
		 * failed against correct code -- the counter was reporting a real
		 * success, not double-counting the failure.
		 *
		 * The claim that actually distinguishes "the rows are separately sourced"
		 * from "one number under two labels" is that they DISAGREE: a failure
		 * moved one and not the other.
		 */
		TestEqual(
			TEXT("exactly one frame was corrupted, and exactly one failure is reported -- "
				 "so the row is a COUNT of failures rather than a flag that something "
				 "went wrong"),
			Failed.LoadsFailed, (int64)1);

		TestTrue(
			TEXT("meanwhile the INTACT preload frames completed normally: the two rows "
				 "moved independently in the same run, which is the claim a single "
				 "counter printed under two labels could not satisfy"),
			Failed.LoadsCompleted > 0);

		TestEqual(
			TEXT("nothing is left in flight after a failure: the failing decode is "
				 "retired, not leaked"),
			Failed.LoadsInFlight, 0);

		TestEqual(
			TEXT("and the frame never reaches the display, because a frame that failed to "
				 "decode has no voxels to show"),
			Failed.DisplayFrameA, (int32)INDEX_NONE);
	}

	return true;
}

/**
 * The rows that live on the VOLUME COMPONENT rather than the workspace model.
 *
 * Ray step, step ceiling and uploaded resolution are the component's, because
 * that is where production keeps them: FFlowVizWorkspaceModel has no render
 * settings member, and inventing one for the overlay's convenience would give
 * the diagnostics a source no renderer reads. So the collector takes an optional
 * component, and this test supplies a real one from a real actor.
 *
 * SEPARATE TEST, because this one needs a world and the model-only rows do not.
 * Folding them together would make a world-construction failure look like a
 * diagnostics failure.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizDiagnosticsVolumeTest,
	"FlowViz.UI.Diagnostics.Volume",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizDiagnosticsVolumeTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizDiagnosticsTest;

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	UWorld* World = MakeWorld(WorldContext);
	if (!TestNotNull(TEXT("CONTROL: a test world was created"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
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
			Actor->LoadCase(GetSampleCaseDir(), FName(TEXT("speed"))).IsOk()))
	{
		return false;
	}

	FFlowVizWorkspaceModel Model;
	if (!TestTrue(TEXT("CONTROL: the workspace model opens the same sample"),
			Model.OpenCase(GetSampleCaseDir(), FName(TEXT("speed"))).IsOk()))
	{
		return false;
	}

	/* == Ray step: driven through the component's own setter ================= */

	// Neither value is the default (0.5 voxels, 2048 steps), so a collector
	// reporting the constants instead of the live settings is caught.
	constexpr float TargetStepVoxels = 0.25f;
	constexpr uint32 TargetMaxSteps = 777u;

	FFlowVizRenderSettingsViewModel RenderSettings = Volume->GetRenderSettings();
	RenderSettings.SetStepVoxels(TargetStepVoxels);
	RenderSettings.SetMaxSteps(TargetMaxSteps);
	Volume->SetRenderSettings(RenderSettings);

	const FFlowVizDiagnosticsSnapshot Snapshot = FlowVizDiagnostics::Collect(Model, Volume);

	TestEqual(
		TEXT("the ray step row reports the step the COMPONENT holds, not the 0.5 default"),
		Snapshot.StepVoxels, TargetStepVoxels);

	TestEqual(
		TEXT("the step ceiling row reports the live MaxSteps, not the 2048 default"),
		Snapshot.MaxSteps, TargetMaxSteps);

	/* == Resolution: the sample's real grid, not a guess ===================== */

	/*
	 * RESOLUTION COMES FROM THE UPLOADED LAYOUT, which is written when a frame
	 * reaches the texture set. Under -nullrhi no upload completes, so this row
	 * would read zero for a reason that has nothing to do with the collector
	 * being wrong -- and asserting the sample's dimensions there would fail an
	 * honest implementation. Asked only where it can be answered.
	 */
	if (GIsRHIInitialized && !GUsingNullRHI)
	{
		/*
		 * AND AN UPLOAD MUST ACTUALLY HAPPEN. LoadCase binds the case and
		 * validates the grid; it decodes no frame and fills no texture. So the
		 * layout is still empty here, and asserting the sample's dimensions
		 * against a freshly-loaded component would fail a CORRECT collector.
		 * The frame has to be driven through the player's own seam -- the same
		 * path FlowViz.UI.Workspace.UploadSeam wired -- before the row exists.
		 */
		Model.Player.SetTextureSet(&Volume->GetTextureSet());
		Model.Player.SeekToFrame(3);

		if (TestTrue(TEXT("CONTROL: a frame decodes and reaches the display"),
				PumpUntilDisplayed(Model.Player, 3)))
		{
			// The upload is completed by the render thread; without this the
			// layout may still be empty for timing reasons alone.
			FlushRenderingCommands();

			const FFlowVizDiagnosticsSnapshot Uploaded = FlowVizDiagnostics::Collect(Model, Volume);

			TestEqual(
				TEXT("the resolution row reports the SAMPLE's own 56x28x6 grid, so a row "
					 "hardcoding a plausible resolution is visible"),
				Uploaded.VolumeDimensions, SampleDimensions);
		}
	}
	else
	{
		/*
		 * ONE ASSERTION SKIPPED, NOT THE TEST. The ray step and step ceiling arms
		 * above ran and are device-independent; only the resolution row needs an
		 * upload. Worded so the run's skip list cannot be read as "this file
		 * verified nothing" -- it verified two of its three subjects.
		 */
		AddInfo(TEXT("SKIPPED (the resolution assertion only; the ray step and step ceiling "
					 "arms above DID run): the volume resolution row needs a real device, as "
					 "no upload completes under -nullrhi and the uploaded layout stays empty. "
					 "Re-run with: RHI=1 ./FlowViz/Tools/run_tests.sh FlowViz.UI.Diagnostics.Volume"));
	}

	Model.CloseCase();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
