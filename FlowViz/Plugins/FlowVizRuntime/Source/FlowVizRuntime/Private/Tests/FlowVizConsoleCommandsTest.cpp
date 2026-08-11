// Copyright FlowViz contributors. All Rights Reserved.

#include "CFDViz/CFDVizManifest.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/StringOutputDevice.h"
#include "Playback/FlowVizCasePlayer.h"
#include "UI/FlowVizConsoleCommands.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "UI/FlowVizWorkspaceRegistry.h"
#include "UI/FlowVizWorkspaceTab.h"
#include "UI/SFlowVizWorkspace.h"
#include "Widgets/Docking/SDockTab.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED NAMESPACE, NOT ANONYMOUS -- FlowVizRuntime is a unity build and an
 * anonymous namespace merges with every other one in the same blob rather than
 * being file-local. That collision already shipped once in this repo (#37).
 */
namespace FlowVizConsoleCommandsTest
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

	FString GetSampleManifestPath()
	{
		return FPaths::Combine(GetSampleCaseDir(), TEXT("manifest.json"));
	}

	/*
	 * KNOWN VALUES OF THE COMMITTED SAMPLE, spelled out rather than read back
	 * from the same manifest the code under test read. A test that derived its
	 * expectation from its subject would agree with any manifest, including a
	 * broken one.
	 */
	constexpr int32 SampleFrameCount = 20;
	constexpr int32 SampleDeclaredFieldCount = 8; // includes validMask

	/** `case.name` as committed. The reload arm rewrites exactly this string. */
	const TCHAR* const SampleCaseName = TEXT("Mock Cylinder Wake (low resolution)");

	/**
	 * A SCALAR FIELD, NAMED ON BOTH SIDES.
	 *
	 * Not left to default: OpenCase's "first non-mask field" rule lands on `U`, a
	 * three-component vector, and a test that never names a field would silently
	 * be exercising a different path from the one it reads about.
	 */
	const FName SampleField(TEXT("speed"));

	/**
	 * A WRITABLE COPY of the sample, so a test can change the case on disk.
	 *
	 * @return false when the copy could not be made; the caller skips rather than
	 *         asserting against a directory that is not what it thinks.
	 */
	bool MakeMutableCaseCopy(const FString& DestDir)
	{
		IFileManager& Files = IFileManager::Get();
		Files.DeleteDirectory(*DestDir, /*RequireExists*/ false, /*Tree*/ true);

		const FString SourceDir = GetSampleCaseDir();
		if (SourceDir.IsEmpty() || !Files.DirectoryExists(*SourceDir))
		{
			return false;
		}

		/*
		 * CopyDirectoryTree, NOT IFileManager::Copy. Copy() returns a uint32 whose
		 * SUCCESS value is 0, so `!Copy(...)` reads as "it worked" -- an inversion
		 * that has already silently copied nothing once in this repo's tests.
		 */
		return FPlatformFileManager::Get().GetPlatformFile().CopyDirectoryTree(
			*DestDir, *SourceDir, /*bOverwriteAllExisting*/ true);
	}

	/**
	 * Rewrite `case.name` in a copied manifest.
	 *
	 * The one edit that changes what a re-read observes without changing anything
	 * a decode depends on: no frame file, no grid, no field is touched, so a
	 * reload that fails to see it has failed for exactly one reason.
	 */
	bool RewriteCaseName(const FString& CaseDir, const FString& NewName)
	{
		const FString ManifestPath = FPaths::Combine(CaseDir, TEXT("manifest.json"));

		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *ManifestPath))
		{
			return false;
		}

		/*
		 * MATCHED WITH ITS KEY, not as a bare string.
		 *
		 * The sample's case name occurs TWICE: once as `case.name`, and once again
		 * inside `provenance`, where the generator's parameters are stored as an
		 * ESCAPED JSON STRING -- so the second copy is spelled \"name\": \"...\".
		 * Replacing the bare name would rewrite both, and the assertion that
		 * exactly one site changed would fail against a fixture that is perfectly
		 * fine. Including the unescaped `"name": "` prefix distinguishes them.
		 */
		const FString Search = FString::Printf(TEXT("\"name\": \"%s\""), SampleCaseName);
		const FString Replacement = FString::Printf(TEXT("\"name\": \"%s\""), *NewName);

		// EXACTLY ONE OCCURRENCE, or the fixture is not what this test assumes and
		// a partial rewrite would leave a manifest whose failure mode is its own.
		if (Json.ReplaceInline(*Search, *Replacement, ESearchCase::CaseSensitive) != 1)
		{
			return false;
		}

		return FFileHelper::SaveStringToFile(Json, *ManifestPath);
	}

	/**
	 * Run a registered console command and capture what it printed.
	 *
	 * GOES THROUGH THE CONSOLE MANAGER, not through the FlowVizConsoleCommands
	 * functions directly. That is the entire point of this file: a user types a
	 * name, the manager looks it up, and something happens. Calling the
	 * implementation directly would pass on a build where nothing was ever
	 * registered -- the same shape as every panel test SNew-ing its own widget.
	 *
	 * @return False when no command of that name exists, or when the object
	 *         registered under that name is a cvar rather than a command.
	 */
	bool ExecuteCommand(const TCHAR* Name, const TArray<FString>& Args, FString& OutText)
	{
		OutText.Reset();

		IConsoleObject* Object = IConsoleManager::Get().FindConsoleObject(Name);
		if (Object == nullptr)
		{
			return false;
		}

		IConsoleCommand* Command = Object->AsCommand();
		if (Command == nullptr)
		{
			return false;
		}

		FStringOutputDevice Capture;
		// Each Logf becomes its own line. The default is false, which would run
		// every row of a multi-line report together and let a Contains() check
		// match across a boundary that is not there on screen.
		Capture.SetAutoEmitLineTerminator(true);

		const bool bExecuted = Command->Execute(Args, /*InWorld*/ nullptr, Capture);
		OutText = static_cast<const FString&>(Capture);
		return bExecuted;
	}

	/** No arguments. Spelled once so the call sites read as the console line does. */
	bool ExecuteCommand(const TCHAR* Name, FString& OutText)
	{
		return ExecuteCommand(Name, TArray<FString>(), OutText);
	}

	TArray<FString> MakeArgs(const FString& A)
	{
		TArray<FString> Args;
		Args.Add(A);
		return Args;
	}

	TArray<FString> MakeArgs(const FString& A, const FString& B)
	{
		TArray<FString> Args;
		Args.Add(A);
		Args.Add(B);
		return Args;
	}

	/**
	 * Drive one round of decode to completion, WITHOUT pretending to be playback.
	 *
	 * Three calls, and each is load-bearing. Tick starts the pending decodes --
	 * StartPendingLoads has exactly one call site and it is inside Tick, so a
	 * player that is never ticked never reads frame 0 off disk. The wait blocks
	 * on decodes ALREADY IN FLIGHT and returns instantly against an empty queue,
	 * so a wait without a preceding tick is a tautology (repo memory
	 * seek-is-not-load). The second tick drains the results and marks them
	 * complete, which is what makes them resident.
	 */
	void PumpUntilResident(FFlowVizCasePlayer& Player)
	{
		Player.Tick(0.0);
		Player.WaitForPendingLoads(30.0);
		Player.Tick(0.0);
	}
}

/**
 * Paths with spaces remain one console argument.
 *
 * Both -case= startup and the workspace's Open button route through a textual
 * console line. Direct IConsoleCommand tests supply an argument array and cannot
 * see a missing pair of quotes, so this pins the exact tokenizer that receives
 * those production strings.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizLoadCaseCommandTest,
	"FlowViz.UI.Console.LoadCaseCommand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizLoadCaseCommandTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizConsoleCommandsTest;

	const FString CasePath(TEXT("/Users/Example/Flow Cases/Cylinder Wake.cfdviz"));
	const FString Command =
		FlowVizConsoleCommands::MakeLoadCaseCommand(CasePath, SampleField.ToString());
	const TCHAR* Cursor = *Command;

	FString ParsedCommand;
	FString ParsedPath;
	FString ParsedField;
	FString Extra;
	TestTrue(TEXT("CONTROL: the command name token exists"),
		FParse::Token(Cursor, ParsedCommand, /*bUseEscape*/ false));
	TestTrue(TEXT("CONTROL: the case path token exists"),
		FParse::Token(Cursor, ParsedPath, /*bUseEscape*/ false));
	TestTrue(TEXT("CONTROL: the optional field token exists"),
		FParse::Token(Cursor, ParsedField, /*bUseEscape*/ false));

	TestEqual(TEXT("the generated line names the registered load command"),
		ParsedCommand, FString(FlowVizConsoleCommands::LoadCaseName));
	TestEqual(TEXT("a path containing spaces reaches the command as one exact argument"),
		ParsedPath, CasePath);
	TestEqual(TEXT("the field stays a separate third argument"),
		ParsedField, SampleField.ToString());
	TestFalse(TEXT("and no path fragment leaks into a fourth argument"),
		FParse::Token(Cursor, Extra, /*bUseEscape*/ false));

	return true;
}

/* ========================================================================== */
/* Wiring: what did module startup leave behind?                               */
/* ========================================================================== */

/**
 * CAN A USER ACTUALLY TYPE THESE?
 *
 * Modelled on FlowVizWorkspaceWiringTest, which exists because 49 rendering
 * tests were green while a volume in a real map drew nothing: every one of them
 * installed its own dispatcher as a precondition, so not one could observe that
 * production installed none.
 *
 * A console command has the identical hole and a nastier version of it. The
 * implementation can be complete, correct and covered by direct unit tests of
 * its helpers, and if nothing calls RegisterConsoleCommand the feature does not
 * exist -- typing the name gets "Command not recognized". So this test
 * registers NOTHING itself and asks the console manager what startup did.
 *
 * AND IT ASKS AsCommand(), NOT MERELY "does an object exist". IConsoleObject
 * covers cvars too, and a cvar accidentally registered under a command's name
 * would satisfy a null check while doing nothing when executed. AsCommand()
 * returns null for everything that is not an IConsoleCommand, which is the only
 * available way to tell the two apart.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizConsoleWiringTest,
	"FlowViz.UI.Console.Wiring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizConsoleWiringTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizConsoleCommandsTest;

	const TArray<const TCHAR*> Names = FlowVizConsoleCommands::GetCommandNames();

	// EIGHT, SPELLED HERE. plan.md section 17 lists eight; a GetCommandNames that
	// quietly returned six would otherwise make the loop below pass by iterating
	// less.
	if (!TestEqual(
			TEXT("FlowVizConsoleCommands::GetCommandNames offers all eight commands plan.md "
				 "section 17 asks for"),
			Names.Num(), 8))
	{
		return false;
	}

	for (const TCHAR* Name : Names)
	{
		IConsoleObject* Object = IConsoleManager::Get().FindConsoleObject(Name);

		if (!TestNotNull(
				*FString::Printf(
					TEXT("module startup registers '%s' with the console manager; without the "
						 "registration the implementation behind it is unreachable from the "
						 "running application and no direct test of its helpers can tell"),
					Name),
				Object))
		{
			continue;
		}

		TestNotNull(
			*FString::Printf(
				TEXT("'%s' is registered as a COMMAND rather than as a console variable, so "
					 "typing it executes something"),
				Name),
			Object->AsCommand());
	}

	/*
	 * THE NO-WORKSPACE BRANCH. The contract CHANGED with the DoD 4 fix: with no
	 * workspace alive, ResolveTarget now INVOKES the FlowViz tab itself and
	 * retries -- in the packaged app a console command is the only door, and
	 * the old refusal pointed at a menu (Window > FlowViz) that only the editor
	 * has. So the assertion is no longer "it says no workspace"; it is "the
	 * command self-serves: afterwards a workspace EXISTS and the command acted
	 * on it".
	 *
	 * GATED AND DISCLOSED rather than asserted unconditionally. Automation test
	 * order is not guaranteed and another UI test's workspace may still be alive,
	 * in which case the self-serve arm cannot be distinguished from an ordinary
	 * resolve -- and a silently skipped arm inside a green test is exactly the
	 * "reported success having verified nothing" shape this repo's runner
	 * exists to surface.
	 */
	if (FlowVizWorkspaceRegistry::GetActiveWorkspace().IsValid())
	{
		AddInfo(TEXT("SKIPPED (the no-workspace arm only): another test's workspace is still "
					 "alive, so there was already a target to resolve."));
	}
	else
	{
		FString Output;
		TestTrue(TEXT("FlowViz.ShowDiagnostics executes with no workspace open"),
			ExecuteCommand(FlowVizConsoleCommands::ShowDiagnosticsName, Output));
		TestTrue(
			TEXT("with no workspace open the command OPENS one (the packaged app has no "
				 "Window menu, so the console is the only door) -- afterwards a workspace "
				 "exists"),
			FlowVizWorkspaceRegistry::GetActiveWorkspace().IsValid());
		TestFalse(
			*FString::Printf(
				TEXT("and the command did not refuse -- it acted on the workspace it "
					 "opened. It printed: '%s'"),
				*Output),
			Output.Contains(TEXT("no workspace")));

		TSharedPtr<SDockTab> OpenedTab =
			FGlobalTabmanager::Get()->FindExistingLiveTab(FlowVizWorkspaceTab::TabId);
		if (TestTrue(TEXT("CONTROL: the command self-served through the registered workspace tab, "
						   "so this test can close exactly what it opened"),
				OpenedTab.IsValid()))
		{
			OpenedTab->RequestCloseTab();
			OpenedTab.Reset();
		}

		TestFalse(
			TEXT("the self-served command closes the workspace it opened for this arm, so a "
				 "test of the no-workspace path does not leave a target that makes later tests "
				 "silently skip their own no-workspace paths"),
			FlowVizWorkspaceRegistry::GetActiveWorkspace().IsValid());
	}

	return true;
}

/* ========================================================================== */
/* Target: which workspace does a typed command act on?                        */
/* ========================================================================== */

/**
 * THE COMMANDS NEED SOMETHING TO POINT AT, AND NOTHING USED TO KEEP ONE.
 *
 * FlowVizWorkspaceTab::SpawnWorkspaceTab builds an SFlowVizWorkspace, hands it
 * to an SDockTab and retains no reference. Correct for a tab -- Slate owns the
 * widget -- and it leaves a console command with no way to NAME its target. The
 * two available wrong answers are both worse than failing: construct a second,
 * empty workspace and report numbers about an object the user has never seen,
 * or report nothing at all forever, which reads as "the UI is not open".
 *
 * This is the arm that fails if the FlowVizWorkspaceRegistry::Register call in
 * SFlowVizWorkspace::Construct is removed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizConsoleTargetTest,
	"FlowViz.UI.Console.Target",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizConsoleTargetTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizConsoleCommandsTest;

	const int32 BeforeCount = FlowVizWorkspaceRegistry::GetLiveWorkspaces().Num();

	// Kept as an opaque address so the post-destruction check can compare
	// identity without dereferencing a widget that no longer exists.
	const void* Address = nullptr;

	{
		const TSharedRef<SFlowVizWorkspace> Workspace = SNew(SFlowVizWorkspace);
		Address = &Workspace.Get();

		const TArray<TSharedPtr<SFlowVizWorkspace>> Live =
			FlowVizWorkspaceRegistry::GetLiveWorkspaces();

		if (!TestEqual(
				TEXT("constructing a workspace adds exactly one entry to the registry, so a "
					 "Construct that registered twice is not mistaken for two open tabs"),
				Live.Num(), BeforeCount + 1))
		{
			return false;
		}

		const TSharedPtr<SFlowVizWorkspace> Active = FlowVizWorkspaceRegistry::GetActiveWorkspace();

		if (!TestTrue(
				TEXT("a live workspace resolves as the active one; without this the eight "
					 "FlowViz.* commands are registered, documented and inert"),
				Active.IsValid()))
		{
			return false;
		}

		TestTrue(
			TEXT("the MOST RECENTLY constructed workspace is the one a typed command resolves, "
				 "so a command means the tab the user just opened rather than one that has been "
				 "sitting in the background since the session started"),
			Active.Get() == &Workspace.Get());

		// IDEMPOTENT. Construct runs once per widget, but a second Register must
		// not produce a second entry -- "how many workspaces are open" would then
		// be wrong by one for the rest of the session.
		FlowVizWorkspaceRegistry::Register(Workspace.Get());
		TestEqual(
			TEXT("registering the same workspace twice leaves one entry"),
			FlowVizWorkspaceRegistry::GetLiveWorkspaces().Num(), BeforeCount + 1);
	}

	/*
	 * AND IT MUST STOP RESOLVING. A registry that only ever grew would hand a
	 * console command a workspace whose player has already released its texture
	 * set -- a use-after-free reached by typing a command name.
	 */
	const TArray<TSharedPtr<SFlowVizWorkspace>> After =
		FlowVizWorkspaceRegistry::GetLiveWorkspaces();

	TestEqual(
		TEXT("destroying the workspace removes its registry entry"), After.Num(), BeforeCount);

	bool bStillListed = false;
	for (const TSharedPtr<SFlowVizWorkspace>& Entry : After)
	{
		if (static_cast<const void*>(Entry.Get()) == Address)
		{
			bStillListed = true;
		}
	}
	TestFalse(
		TEXT("the destroyed workspace is not still listed, so no console command can pin one "
			 "mid-teardown"),
		bStillListed);

	return true;
}

/* ========================================================================== */
/* The cache-budget parse                                                      */
/* ========================================================================== */

/**
 * THE INTERESTING BEHAVIOUR IS THE REFUSALS.
 *
 * `FlowViz.SetCpuCacheMB 0` must not be read as "one megabyte, clamped": a
 * budget silently coerced upward is a command that reports success and starves
 * the cache, and the user has no way to see the difference. Each mistake below
 * is a different typo and each has to be refused rather than repaired.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizConsoleParseCacheTest,
	"FlowViz.UI.Console.ParseCacheMB",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizConsoleParseCacheTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizConsoleCommandsTest;

	auto Refuses = [this](const TArray<FString>& Args, const TCHAR* Why)
	{
		int64 Bytes = -12345;
		FString Error;
		const bool bParsed = FlowVizConsoleCommands::ParseCacheMegabytes(Args, Bytes, Error);

		TestFalse(*FString::Printf(TEXT("refused: %s"), Why), bParsed);
		TestFalse(
			*FString::Printf(TEXT("the refusal of %s carries a message for the console reader"), Why),
			Error.IsEmpty());
		// UNTOUCHED ON REFUSAL. A caller that ignored the return value would
		// otherwise set a budget from a number the parse rejected.
		TestEqual(
			*FString::Printf(TEXT("%s leaves the out-parameter untouched"), Why), Bytes, int64(-12345));
	};

	auto Accepts = [this](const TArray<FString>& Args, int64 Expected, const TCHAR* What)
	{
		int64 Bytes = -1;
		FString Error;
		const bool bParsed = FlowVizConsoleCommands::ParseCacheMegabytes(Args, Bytes, Error);

		TestTrue(*FString::Printf(TEXT("accepted: %s"), What), bParsed);
		TestEqual(*FString::Printf(TEXT("%s converts to bytes"), What), Bytes, Expected);
		TestTrue(*FString::Printf(TEXT("%s reports no error"), What), Error.IsEmpty());
	};

	Accepts(MakeArgs(TEXT("1")), 1ll * 1024 * 1024, TEXT("one megabyte"));
	Accepts(MakeArgs(TEXT("512")), 512ll * 1024 * 1024, TEXT("the default CPU budget"));
	Accepts(MakeArgs(TEXT(" 256 ")), 256ll * 1024 * 1024, TEXT("surrounding whitespace"));

	Refuses(TArray<FString>(), TEXT("no argument at all"));
	Refuses(MakeArgs(TEXT("0")), TEXT("zero, which would be a cache that can hold nothing"));
	Refuses(MakeArgs(TEXT("-1")), TEXT("a negative budget"));
	Refuses(MakeArgs(TEXT("banana")), TEXT("a word"));
	Refuses(MakeArgs(TEXT("1.5")), TEXT("a fraction of a megabyte"));
	Refuses(MakeArgs(TEXT("512"), TEXT("MB")), TEXT("a stray second argument"));

	/*
	 * THE OVERFLOW BOUND IS NOT DECORATION. MB * 1024 * 1024 wraps int64 above
	 * roughly 8.8e12, and a wrapped budget is NEGATIVE -- which
	 * FFlowVizFrameCache::SetBudgets then rejects with a message about a MINIMUM,
	 * naming the wrong problem entirely. Refused here, where the number the user
	 * typed is still visible.
	 */
	Refuses(MakeArgs(TEXT("9000000000000")), TEXT("a megabyte count that overflows int64 bytes"));
	Refuses(
		MakeArgs(TEXT("99999999999999999999999")),
		TEXT("a digit string too long to convert at all"));

	return true;
}

/* ========================================================================== */
/* The case summary                                                            */
/* ========================================================================== */

/**
 * What `FlowViz.DumpCase` prints, asserted against a case loaded directly.
 *
 * Takes the case rather than a workspace so there is no widget, no world and no
 * player between the assertion and its subject.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizConsoleCaseSummaryTest,
	"FlowViz.UI.Console.CaseSummary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizConsoleCaseSummaryTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizConsoleCommandsTest;

	FCFDVizCase Case;
	if (!TestTrue(
			TEXT("CONTROL: the committed sample manifest loads, without which every assertion "
				 "below would be about an empty case"),
			FCFDVizCase::LoadFromFile(GetSampleManifestPath(), Case).IsOk()))
	{
		return false;
	}

	if (!TestEqual(
			TEXT("CONTROL: the sample declares the frame count this test expects"),
			Case.Timeline.FrameCount, SampleFrameCount))
	{
		return false;
	}

	const FString Summary = FlowVizConsoleCommands::FormatCaseSummary(Case, SampleField);

	TestTrue(TEXT("the summary names the case"), Summary.Contains(SampleCaseName));
	TestTrue(TEXT("the summary names the case's frame count"),
		Summary.Contains(TEXT("20 frames")));
	TestTrue(TEXT("the summary names the grid resolution"),
		Summary.Contains(TEXT("56 x 28 x 6")));
	FCFDVizCase SolverWithoutIdentity = Case;
	SolverWithoutIdentity.Metadata.Solver.Name.Reset();
	SolverWithoutIdentity.Metadata.Solver.Version.Reset();
	TestTrue(TEXT("CONTROL: the formatter fixture still has method-only solver evidence"),
		!SolverWithoutIdentity.Metadata.Solver.Method.IsEmpty());
	const FString MethodOnlySummary = FlowVizConsoleCommands::FormatCaseSummary(
		SolverWithoutIdentity, SampleField);
	TArray<FString> SummaryLines;
	MethodOnlySummary.ParseIntoArrayLines(SummaryLines);
	const bool bHasBlankSolverRow = SummaryLines.ContainsByPredicate(
		[](const FString& Line) { return Line.TrimEnd() == TEXT("  Solver"); });
	TestFalse(TEXT("an omitted solver identity does not produce a blank summary row"),
		bHasBlankSolverRow);
	TestTrue(TEXT("method-only solver evidence is still disclosed"),
		MethodOnlySummary.Contains(SolverWithoutIdentity.Metadata.Solver.Method));

	/*
	 * THE CODEC IS ASSERTED ON EACH FIELD'S OWN ROW, not as "the word zlib appears
	 * somewhere in the report".
	 *
	 * Every field in this sample is zlib-compressed, so a whole-string Contains()
	 * is satisfied by ONE row printing its codec -- and asserting it once per
	 * field in a loop is the same check written eight times, not a stronger one.
	 * An implementation that dropped the column from all but the first row would
	 * pass either form. The codec is why this column exists -- a case whose bricks
	 * this build cannot decode should be visible from a dump rather than from the
	 * first brick that fails -- and that is a per-row property.
	 */
	auto RowFor = [&Summary](const FString& FieldId) -> FString
	{
		TArray<FString> Lines;
		Summary.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			// The field rows are the indented ones; the marker column is two
			// characters wide and blank when the field is not bound.
			FString Trimmed = Line.TrimStartAndEnd();
			if (Trimmed.StartsWith(TEXT("->")))
			{
				Trimmed = Trimmed.RightChop(2).TrimStart();
			}
			FString First;
			if (!Trimmed.Split(TEXT(" "), &First, nullptr))
			{
				First = Trimmed;
			}
			if (First == FieldId)
			{
				return Line;
			}
		}
		return FString();
	};

	for (const FCFDVizField& Field : Case.Fields)
	{
		const FString Id = Field.Id.ToString();
		const FString Row = RowFor(Id);

		if (!TestFalse(
				*FString::Printf(TEXT("the summary has a row of its own for field '%s'"), *Id),
				Row.IsEmpty()))
		{
			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("field '%s's row names its storage codec (%s), so a case this build cannot "
					 "decode is visible before the first brick fails. The row was: '%s'"),
				*Id, CodecToString(Field.Storage.Codec), *Row),
			Row.Contains(CodecToString(Field.Storage.Codec)));

		TestTrue(
			*FString::Printf(
				TEXT("field '%s's row names its data type (%s), which is what says whether this "
					 "build can read the samples at all. The row was: '%s'"),
				*Id, DataTypeToString(Field.DataType), *Row),
			Row.Contains(DataTypeToString(Field.DataType)));

		TestTrue(
			*FString::Printf(
				TEXT("field '%s's row names its component count (%d), which distinguishes a "
					 "scalar a user can display from a vector they cannot. The row was: '%s'"),
				*Id, Field.ComponentCount, *Row),
			Row.Contains(FString::Printf(TEXT("%d comp"), Field.ComponentCount)));
	}

	/*
	 * EVERY DECLARED FIELD, INCLUDING THE MASK. This differs deliberately from
	 * the diagnostics overlay, which lists only what a user can DISPLAY. DumpCase
	 * describes the file: a mask field silently omitted from a dump would make a
	 * manifest and its dump disagree about what is in the case, which is the one
	 * question this command exists to answer.
	 */
	for (const FCFDVizField& Field : Case.Fields)
	{
		TestTrue(
			*FString::Printf(TEXT("the summary lists field '%s'"), *Field.Id.ToString()),
			Summary.Contains(Field.Id.ToString()));
	}
	TestEqual(TEXT("CONTROL: the sample declares eight fields, one of them a mask"),
		Case.Fields.Num(), SampleDeclaredFieldCount);

	/*
	 * THE BOUND FIELD IS MARKED, AND THE MARK MOVES.
	 *
	 * ASSERTED ON THE MARKED ROW, NOT ON WHOLE-STRING INEQUALITY.
	 *
	 * The obvious form -- format the case twice and TestNotEqual the results --
	 * is what this test had first, and it cannot fail for the reason it claims.
	 * FormatCaseSummary ends with a "Displaying <field>" line that names the
	 * binding independently of the marker, so two summaries differ in that line
	 * whatever the field loop does. Marking the first row unconditionally, or
	 * marking nothing at all, both leave every TestNotEqual green.
	 *
	 * So the marked row is extracted and the FIELD ON IT is named. That is the
	 * claim: `->` sits beside the field the player is displaying, which on this
	 * sample matters because the first row is `validMask` -- the one field a user
	 * can never be looking at.
	 */
	auto MarkedField = [](const FString& Text) -> FString
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			const FString Trimmed = Line.TrimStartAndEnd();
			if (Trimmed.StartsWith(TEXT("->")))
			{
				// "-> speed  1 comp  f32 ..." -> "speed"
				FString Rest = Trimmed.RightChop(2).TrimStart();
				FString FieldName;
				Rest.Split(TEXT(" "), &FieldName, nullptr);
				return FieldName.IsEmpty() ? Rest : FieldName;
			}
		}
		return FString();
	};

	// CONTROL: the sample's first field is NOT the one under test, so "marks the
	// first row unconditionally" and "marks the bound row" give different answers.
	if (!TestNotEqual(
			TEXT("CONTROL: the bound field is not the sample's first declared field, without "
				 "which marking the first row unconditionally would be indistinguishable from "
				 "marking the bound one"),
			Case.Fields[0].Id, SampleField))
	{
		return false;
	}

	TestEqual(
		TEXT("the marker sits beside the BOUND field's row rather than always landing on the "
			 "first one, which on this sample is the mask a user can never be displaying"),
		MarkedField(Summary), SampleField.ToString());

	const FString PressureSummary =
		FlowVizConsoleCommands::FormatCaseSummary(Case, FName(TEXT("pressure")));
	TestEqual(
		TEXT("binding a different field moves the marker to that field's row"),
		MarkedField(PressureSummary), FString(TEXT("pressure")));

	const FString UnboundSummary = FlowVizConsoleCommands::FormatCaseSummary(Case, NAME_None);
	TestEqual(
		TEXT("NAME_None marks NO row rather than guessing at a field the player is not showing"),
		MarkedField(UnboundSummary), FString());

	/*
	 * AND A FIELD THAT IS NOT IN THE CASE MUST NOT SILENTLY LOOK LIKE NO BINDING.
	 * A workspace bound to a field the manifest was edited out from under is a
	 * real state, and a dump that rendered it identically to "nothing bound"
	 * would hide it. Asserted on the trailing line's TEXT, because that is the
	 * only place the two states can differ -- neither marks a row.
	 */
	const FString MissingSummary =
		FlowVizConsoleCommands::FormatCaseSummary(Case, FName(TEXT("notAField")));
	TestEqual(TEXT("CONTROL: a bound field the case does not declare marks no row either, so the "
				   "assertion below is the only thing separating it from no binding at all"),
		MarkedField(MissingSummary), FString());
	TestTrue(
		*FString::Printf(
			TEXT("a bound field the case does not declare is named AND called out, rather than "
				 "reading as 'nothing bound'. It printed: '%s'"),
			*MissingSummary),
		MissingSummary.Contains(TEXT("notAField"))
			&& MissingSummary.Contains(TEXT("DOES NOT DECLARE")));
	TestFalse(
		TEXT("...and the no-binding wording is not ALSO printed for it, which would let a reader "
			 "skim past the case the line exists to surface"),
		MissingSummary.Contains(TEXT("(nothing bound)")));

	/*
	 * THE MASK IS LISTED. DumpCase describes the FILE, which is deliberately
	 * different from the diagnostics overlay's list of what a user can DISPLAY.
	 * Named explicitly rather than left to the loop above: `validMask` is the one
	 * field an implementation would plausibly filter out by copying the overlay's
	 * rule, and the loop would still pass if the mask were the row it skipped
	 * only because the loop asserts per field rather than on the total.
	 */
	TestTrue(TEXT("the summary lists the mask field, so a manifest and its dump cannot disagree "
				  "about what is in the case"),
		Summary.Contains(TEXT("validMask")));

	/*
	 * CFDViz 1.1 REPRESENTATIVE-DATA EVIDENCE MUST REACH THE USER.
	 *
	 * The committed sample deliberately remains a synthetic 1.0 correctness
	 * fixture, so construct the additive metadata here rather than relabelling it
	 * as external CFD. Formatting takes a case by value and does not validate it;
	 * this is exactly the seam a console reader uses after a validated load.
	 */
	FCFDVizCase Representative = Case;
	Representative.Metadata.Quality = TEXT("external-solver-sample");
	Representative.Metadata.Solver.Name = TEXT("OpenFOAM");
	Representative.Metadata.Solver.Version = TEXT("13");
	Representative.Metadata.Solver.Method = TEXT("finite-volume LES");
	Representative.Metadata.Solver.Commit = TEXT("foam-commit-abc");
	Representative.Metadata.Solver.Configuration = TEXT("motorBike-transient");

	FCFDVizTimelineSampling Sampling;
	Sampling.SourceTimeStep = 0.0025;
	Sampling.StoredStepStride = 4;
	Sampling.MaxFeatureDisplacementCells = 0.8;
	Representative.Timeline.Sampling = Sampling;

	Representative.Provenance.SourceType = TEXT("external-solver");
	Representative.Provenance.SourceRevision = TEXT("case-revision-def");
	Representative.Provenance.ExportCommand = TEXT("postProcess -func sample");

	FCFDVizQualityMetrics Quality;
	Quality.GridId = Representative.Grids[0].Id;
	Quality.ActiveCellCount = 1234;
	Quality.ActiveDimensions = FIntVector(56, 28, 6);
	Quality.EffectiveSpatialDimensions = 3;
	Quality.VelocityFieldId = FName(TEXT("U"));
	Quality.VelocityComponentRms = FVector(2.0, 0.5, 0.25);
	Quality.SpanwiseGradientRms = 0.125;
	Quality.TemporalFrameCount = Representative.Timeline.FrameCount;
	Representative.QualityMetrics = Quality;

	FCFDVizField* PhaseField = Representative.Fields.FindByPredicate(
		[](const FCFDVizField& Field) { return Field.Id == FName(TEXT("alphaWater")); });
	if (PhaseField == nullptr)
	{
		// The synthetic fixture has no liquid field. Reuse pressure only to exercise
		// the formatter; no production manifest is changed or misclassified.
		PhaseField = Representative.Fields.FindByPredicate(
			[](const FCFDVizField& Field) { return Field.Id == FName(TEXT("pressure")); });
	}
	if (TestNotNull(TEXT("CONTROL: a scalar field is available for phase formatting"), PhaseField))
	{
		FCFDVizPhaseInterpretation Phase;
		Phase.Representation = ECFDVizPhaseRepresentation::VolumeFraction;
		Phase.PrimaryPhase = TEXT("water");
		Phase.SecondaryPhase = TEXT("air");
		Phase.InterfaceValue = 0.5;
		Phase.Inside = ECFDVizPhaseInside::GreaterThanInterface;
		PhaseField->Phase = Phase;
	}

	const FString RepresentativeSummary =
		FlowVizConsoleCommands::FormatCaseSummary(Representative, SampleField);
	TestTrue(TEXT("the summary prints the solver method"),
		RepresentativeSummary.Contains(TEXT("finite-volume LES")));
	TestTrue(TEXT("the summary prints the solver revision"),
		RepresentativeSummary.Contains(TEXT("foam-commit-abc")));
	TestTrue(TEXT("the summary prints the solver configuration"),
		RepresentativeSummary.Contains(TEXT("motorBike-transient")));
	TestTrue(TEXT("the summary names external-solver provenance on its own row"),
		RepresentativeSummary.Contains(TEXT("  Provenance        external-solver\n")));
	TestTrue(TEXT("the summary prints the source revision"),
		RepresentativeSummary.Contains(TEXT("case-revision-def")));
	TestTrue(TEXT("the summary prints the export command"),
		RepresentativeSummary.Contains(TEXT("postProcess -func sample")));
	TestTrue(TEXT("the summary prints the stored-step stride"),
		RepresentativeSummary.Contains(TEXT("stride 4")));
	TestTrue(TEXT("the summary prints the feature-displacement gate"),
		RepresentativeSummary.Contains(TEXT("0.8 cells/snapshot")));
	TestTrue(TEXT("the summary prints effective three-dimensionality"),
		RepresentativeSummary.Contains(TEXT("effective 3D")));
	TestTrue(TEXT("the summary prints the quality velocity field"),
		RepresentativeSummary.Contains(TEXT("velocity 'U'")));
	TestTrue(TEXT("the summary prints spanwise variation evidence"),
		RepresentativeSummary.Contains(TEXT("spanwise gradient RMS 0.125")));
	TestTrue(TEXT("the phase field row names the representation"),
		RepresentativeSummary.Contains(TEXT("volume-fraction")));
	TestTrue(TEXT("the phase field row prints the interface and inside convention"),
		RepresentativeSummary.Contains(TEXT("interface 0.5"))
			&& RepresentativeSummary.Contains(TEXT("greater-than-interface")));

	return true;
}

/* ========================================================================== */
/* Execution: does typing the command actually change anything?                */
/* ========================================================================== */

/**
 * THE ARM THAT MAKES THE OTHERS MEAN SOMETHING.
 *
 * Everything above proves a command is registered, or that a helper computes the
 * right value. Neither proves the two are connected: a command whose delegate
 * body was deleted would still be found by the wiring test, would still answer
 * AsCommand(), would still execute successfully, and would do nothing.
 *
 * So this test drives real commands through IConsoleCommand::Execute and asserts
 * on STATE THE COMMAND CHANGED -- a case that was not open becoming open, a
 * budget moving to a number no default could produce, a populated cache becoming
 * empty. Each assertion is preceded by a control that establishes the before
 * state, because "the budget is 7 MB" proves nothing if 7 MB is where it started.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizConsoleExecuteTest,
	"FlowViz.UI.Console.Execute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizConsoleExecuteTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizConsoleCommandsTest;

	/*
	 * A WRITABLE COPY, not the committed sample, because the reload arm has to
	 * change the case on disk to prove the manifest was read a second time.
	 */
	const FString CaseDir = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("FlowVizConsoleExecuteTest"), TEXT("Case.cfdviz"));

	if (!TestTrue(TEXT("CONTROL: a writable copy of the sample case was made"),
			MakeMutableCaseCopy(CaseDir)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*CaseDir, /*RequireExists*/ false, /*Tree*/ true);
	};

	const TSharedRef<SFlowVizWorkspace> Workspace = SNew(SFlowVizWorkspace);
	FFlowVizWorkspaceModel& Model = Workspace->GetModel();

	if (!TestTrue(
			TEXT("CONTROL: the freshly built workspace is the command target"),
			FlowVizWorkspaceRegistry::GetActiveWorkspace().Get() == &Workspace.Get()))
	{
		return false;
	}

	FString Output;

	/* --- With no case open, every command that needs one says so ---------- */

	if (!TestFalse(TEXT("CONTROL: no case is open yet"), Model.IsCaseOpen()))
	{
		return false;
	}

	const TArray<const TCHAR*> NeedACase = {
		FlowVizConsoleCommands::ReloadCaseName,
		FlowVizConsoleCommands::DumpCaseName,
		FlowVizConsoleCommands::BenchmarkName,
	};
	for (const TCHAR* Name : NeedACase)
	{
		TestTrue(*FString::Printf(TEXT("%s executes with no case open"), Name),
			ExecuteCommand(Name, Output));
		TestTrue(
			*FString::Printf(
				TEXT("%s reports that no case is open rather than printing a plausible-looking "
					 "report about nothing. It printed: '%s'"),
				Name, *Output),
			Output.Contains(TEXT("no case")));
	}

	/* --- LoadCase --------------------------------------------------------- */

	if (!TestTrue(TEXT("FlowViz.LoadCase executes"),
			ExecuteCommand(FlowVizConsoleCommands::LoadCaseName,
				MakeArgs(CaseDir, SampleField.ToString()), Output)))
	{
		return false;
	}

	if (!TestTrue(
			*FString::Printf(
				TEXT("FlowViz.LoadCase opens the case in the workspace the user is looking at. "
					 "It printed: '%s'"),
				*Output),
			Model.IsCaseOpen()))
	{
		return false;
	}

	TestEqual(
		TEXT("FlowViz.LoadCase binds the field named on the command line rather than defaulting "
			 "to the first non-mask field"),
		Model.Player.GetFieldId(), SampleField);

	TestEqual(TEXT("the opened case is the sample's twenty frames"),
		Model.GetCase() != nullptr ? Model.GetCase()->Timeline.FrameCount : -1, SampleFrameCount);

	// A REFUSAL IS REPORTED, NOT SWALLOWED. A bad path that printed nothing would
	// leave the previous case on screen and look like the command did nothing.
	TestTrue(TEXT("FlowViz.LoadCase executes for a path that does not exist"),
		ExecuteCommand(FlowVizConsoleCommands::LoadCaseName,
			MakeArgs(TEXT("/no/such/case.cfdviz")), Output));
	TestTrue(
		*FString::Printf(TEXT("a failed load says so. It printed: '%s'"), *Output),
		Output.Contains(TEXT("failed")));
	TestTrue(
		TEXT("a failed load leaves the previously open case in place rather than closing it, so "
			 "a typo does not silently blank the viewport"),
		Model.IsCaseOpen());

	/* --- ShowDiagnostics -------------------------------------------------- */

	TestTrue(TEXT("FlowViz.ShowDiagnostics executes"),
		ExecuteCommand(FlowVizConsoleCommands::ShowDiagnosticsName, Output));
	TestTrue(
		*FString::Printf(
			TEXT("FlowViz.ShowDiagnostics reports on the OPEN case rather than on a workspace it "
				 "built itself. It printed: '%s'"),
			*Output),
		Output.Contains(SampleField.ToString()));
	TestTrue(TEXT("the diagnostics name the case's frame count"),
		Output.Contains(FString::Printf(TEXT("of %d"), SampleFrameCount)));

	/* --- DumpCase --------------------------------------------------------- */

	TestTrue(TEXT("FlowViz.DumpCase executes with a case open"),
		ExecuteCommand(FlowVizConsoleCommands::DumpCaseName, Output));
	TestTrue(
		*FString::Printf(TEXT("FlowViz.DumpCase describes the open case. It printed: '%s'"), *Output),
		Output.Contains(TEXT("56 x 28 x 6")));

	/* --- SetCpuCacheMB / SetGpuCacheMB ------------------------------------ */

	// 7 AND 5 ARE NOT DEFAULTS. The defaults are 512 and 256, so an assertion
	// against these cannot be satisfied by a command that did nothing.
	constexpr int64 CpuTarget = 7ll * 1024 * 1024;
	constexpr int64 GpuTarget = 5ll * 1024 * 1024;

	if (!TestNotEqual(TEXT("CONTROL: the CPU budget does not already hold the target value"),
			Model.Player.GetCache().GetStats().CpuBudgetBytes, CpuTarget))
	{
		return false;
	}

	TestTrue(TEXT("FlowViz.SetCpuCacheMB executes"),
		ExecuteCommand(FlowVizConsoleCommands::SetCpuCacheMBName, MakeArgs(TEXT("7")), Output));
	TestEqual(TEXT("FlowViz.SetCpuCacheMB moves the player's CPU budget"),
		Model.Player.GetCache().GetStats().CpuBudgetBytes, CpuTarget);

	TestTrue(TEXT("FlowViz.SetGpuCacheMB executes"),
		ExecuteCommand(FlowVizConsoleCommands::SetGpuCacheMBName, MakeArgs(TEXT("5")), Output));
	TestEqual(TEXT("FlowViz.SetGpuCacheMB moves the player's GPU budget"),
		Model.Player.GetCache().GetStats().GpuBudgetBytes, GpuTarget);

	/*
	 * AND IT LEAVES THE OTHER ONE ALONE. SetMemoryBudgets takes BOTH budgets, so
	 * a command that passed a default for the budget it is not setting would
	 * silently reset the CPU budget every time the GPU budget was changed -- and
	 * a test that only checked the budget it just set would never see it.
	 */
	TestEqual(
		TEXT("setting the GPU budget does not disturb the CPU budget"),
		Model.Player.GetCache().GetStats().CpuBudgetBytes, CpuTarget);

	TestTrue(TEXT("FlowViz.SetCpuCacheMB executes for a rejected argument"),
		ExecuteCommand(FlowVizConsoleCommands::SetCpuCacheMBName, MakeArgs(TEXT("0")), Output));
	TestEqual(
		TEXT("a refused budget leaves the previous one in place rather than clamping to a "
			 "number the user did not ask for"),
		Model.Player.GetCache().GetStats().CpuBudgetBytes, CpuTarget);
	TestFalse(TEXT("a refused budget prints an explanation"), Output.IsEmpty());

	/* --- ClearCache ------------------------------------------------------- */

	// Restore budgets large enough to hold frames before populating the cache:
	// 7 MB would make the control below fail for a reason that is not the cache.
	ExecuteCommand(FlowVizConsoleCommands::SetCpuCacheMBName, MakeArgs(TEXT("512")), Output);
	ExecuteCommand(FlowVizConsoleCommands::SetGpuCacheMBName, MakeArgs(TEXT("256")), Output);

	PumpUntilResident(Model.Player);

	if (!TestTrue(
			TEXT("CONTROL: pumping the player populates the cache, without which ClearCache "
				 "would be asserted against an already-empty cache and could not fail"),
			Model.Player.GetCache().GetStats().EntryCount > 0))
	{
		return false;
	}

	TestTrue(TEXT("FlowViz.ClearCache executes"),
		ExecuteCommand(FlowVizConsoleCommands::ClearCacheName, Output));
	TestEqual(TEXT("FlowViz.ClearCache empties the frame cache"),
		Model.Player.GetCache().GetStats().EntryCount, 0);
	TestEqual(TEXT("FlowViz.ClearCache releases the CPU bytes those entries held"),
		Model.Player.GetCache().GetStats().CpuBytes, int64(0));

	/* --- ReloadCase ------------------------------------------------------- */

	/*
	 * A RELOAD THAT RE-READ NOTHING IS NOT A RELOAD, and pointer identity cannot
	 * prove it: OpenCase frees the previous shared case BEFORE allocating the new
	 * one, so the allocator is free to hand back the same address and a
	 * `GetCase() != Before` assertion would fail against correct code.
	 *
	 * So the manifest is CHANGED ON DISK first. The command exists so a user who
	 * has just rewritten a case can see the new data, and the only assertion that
	 * means that is one only a genuine re-read can satisfy.
	 */
	const FString ReloadedName(TEXT("Rewritten Between Load And Reload"));

	if (!TestTrue(TEXT("CONTROL: the copied manifest was rewritten on disk"),
			RewriteCaseName(CaseDir, ReloadedName)))
	{
		return false;
	}

	if (!TestEqual(
			TEXT("CONTROL: the OPEN case still holds the original name, so the assertion below "
				 "is about the reload rather than about the rewrite"),
			Model.GetCase() != nullptr ? Model.GetCase()->Metadata.Name : FString(),
			FString(SampleCaseName)))
	{
		return false;
	}

	TestTrue(TEXT("FlowViz.ReloadCase executes"),
		ExecuteCommand(FlowVizConsoleCommands::ReloadCaseName, Output));
	TestTrue(TEXT("FlowViz.ReloadCase leaves a case open"), Model.IsCaseOpen());

	TestEqual(
		TEXT("FlowViz.ReloadCase re-reads the manifest from disk, so a case rewritten by the "
			 "solver since it was opened is what the user now sees"),
		Model.GetCase() != nullptr ? Model.GetCase()->Metadata.Name : FString(), ReloadedName);

	TestEqual(TEXT("FlowViz.ReloadCase keeps the bound field rather than reverting to the "
				   "first non-mask one"),
		Model.Player.GetFieldId(), SampleField);

	/* --- Benchmark -------------------------------------------------------- */

	const double PlayheadBeforeBenchmark = Model.Player.GetPhysicalTime();

	TestTrue(TEXT("FlowViz.Benchmark executes"),
		ExecuteCommand(FlowVizConsoleCommands::BenchmarkName, MakeArgs(TEXT("3")), Output));

	const FString HealthyRun = Output;

	/*
	 * plan.md: "Do not claim performance that was not measured." So the report has
	 * to say how many frames it actually RESOLVED, not merely how many it was
	 * asked for.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("FlowViz.Benchmark reports how many of the requested frames it resolved. It "
				 "printed: '%s'"),
			*HealthyRun),
		HealthyRun.Contains(TEXT("3 of 3")));
	TestTrue(TEXT("FlowViz.Benchmark reports a measured duration in milliseconds"),
		HealthyRun.Contains(TEXT("ms")));

	// A DIAGNOSTIC COMMAND MUST NOT SCRUB THE USER'S VIEW. The benchmark seeks
	// across the timeline to sample it; where it leaves the playhead is not the
	// user's business and silently moving it would be a command with a side
	// effect nothing announces.
	TestEqual(TEXT("FlowViz.Benchmark restores the playhead it moved to take its samples"),
		Model.Player.GetPhysicalTime(), PlayheadBeforeBenchmark);

	/*
	 * AND THE COUNT HAS TO BE ABLE TO SAY SOMETHING ELSE.
	 *
	 * "3 of 3" is what a healthy run prints -- and it is also what a benchmark
	 * that counted the frames it REQUESTED rather than the ones that RESOLVED
	 * would print, on every case, including one whose frames are all missing. The
	 * assertion above cannot tell those apart, so the fixture is broken on purpose
	 * and the same command is run again: the bound field's payload is deleted from
	 * every frame directory, and the report must now differ.
	 *
	 * THE FIELD'S FILES, NOT THE WHOLE FRAME DIRECTORY. Deleting a frame outright
	 * risks the case failing to resolve for some earlier reason -- the assertion
	 * would then be about a missing directory rather than about a decode that did
	 * not produce a resident frame.
	 */
	/*
	 * RequireExists = TRUE, and the count is the control.
	 *
	 * IFileManager::Delete returns TRUE for a file that was never there when
	 * RequireExists is false -- it reports "the file is now absent", not "this
	 * call removed something". A loop written that way counts 20 against a
	 * misspelled path, and the control then certifies a fixture nothing touched
	 * while the benchmark below reads a perfectly intact case. Requiring existence
	 * makes the count mean deletions.
	 */
	int32 Deleted = 0;
	for (int32 FrameIndex = 0; FrameIndex < SampleFrameCount; ++FrameIndex)
	{
		const FString Payload = FPaths::Combine(CaseDir, TEXT("frames"),
			FString::Printf(TEXT("%06d"), FrameIndex),
			FString::Printf(TEXT("%s.cvf"), *SampleField.ToString()));
		if (IFileManager::Get().Delete(*Payload, /*RequireExists*/ true))
		{
			++Deleted;
		}
	}

	if (!TestEqual(
			TEXT("CONTROL: every frame's payload for the bound field was deleted, without which "
				 "the assertion below would be about a case that still decodes fine"),
			Deleted, SampleFrameCount))
	{
		return false;
	}

	// The cache still holds what it decoded a moment ago, and a benchmark reading
	// those would measure a TMap lookup rather than the missing files. The command
	// empties the cache itself, which is exactly what makes this arm reach disk.
	TestTrue(TEXT("FlowViz.Benchmark executes against a case whose payloads are gone"),
		ExecuteCommand(FlowVizConsoleCommands::BenchmarkName, MakeArgs(TEXT("3")), Output));

	TestNotEqual(
		*FString::Printf(
			TEXT("a benchmark over frames that cannot be read does not print the same report as "
				 "one where every frame loaded -- otherwise the count is of frames REQUESTED and "
				 "would read as a healthy run on any broken case. Healthy: '%s' Broken: '%s'"),
			*HealthyRun, *Output),
		Output, HealthyRun);

	TestFalse(
		*FString::Printf(
			TEXT("...and specifically it does not still claim it resolved all three. It printed: "
				 "'%s'"),
			*Output),
		Output.Contains(TEXT("3 of 3")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
