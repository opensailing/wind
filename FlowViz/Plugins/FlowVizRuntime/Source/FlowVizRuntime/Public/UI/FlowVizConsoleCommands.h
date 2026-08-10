// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FCFDVizCase;

/**
 * The `FlowViz.*` console commands (plan.md section 17).
 *
 * WHY THIS IS A NAMED REGISTRATION AND NOT EIGHT FAutoConsoleCommand GLOBALS.
 * The one-liner form -- a file-scope `static FAutoConsoleCommand GFoo(...)` --
 * registers during static initialisation and is what most engine code does. It
 * is wrong here for two specific reasons:
 *
 *   1. This module loads at PostConfigInit and lives in a UNITY BUILD. A
 *      file-scope object's construction order relative to the rest of the
 *      module is unspecified, and the delegate it binds reaches into the
 *      workspace registry.
 *
 *   2. A test cannot observe a registration it cannot name. Static
 *      initialisation happens before any test runs, so an
 *      "is it registered" assertion would be true of a build where
 *      StartupModule was never called at all -- which is the same shape of
 *      un-falsifiable check this repo keeps finding. Registering from
 *      StartupModule, beside FlowVizWorkspaceTab::Register(), means
 *      FlowViz.UI.Console.Wiring is asking about the production startup path.
 *
 * WHAT THE COMMANDS ACT ON. Every one of them resolves its target through
 * FlowVizWorkspaceRegistry::GetActiveWorkspace(). There is no way to reach a
 * workspace otherwise: the tab spawner builds one and hands it to Slate without
 * retaining a reference. A command that constructed its own workspace instead
 * would report numbers about an object the user has never seen -- which is
 * worse than reporting nothing, because it looks like an answer.
 *
 * THREADING. Game thread only. The console executes there, and everything these
 * commands touch (Slate widgets, view models, the case player) is game-thread
 * bound.
 */
namespace FlowVizConsoleCommands
{
	/**
	 * The command names, shared between the registration and the test.
	 *
	 * Spelled once for the same reason FlowVizWorkspaceTab::TabId is: a test
	 * that hard-coded these strings would stay green while production registered
	 * different ones, which is a passing test for a command nobody can type.
	 */
	FLOWVIZRUNTIME_API extern const TCHAR* const LoadCaseName;
	FLOWVIZRUNTIME_API extern const TCHAR* const ReloadCaseName;
	FLOWVIZRUNTIME_API extern const TCHAR* const ClearCacheName;
	FLOWVIZRUNTIME_API extern const TCHAR* const ShowDiagnosticsName;
	FLOWVIZRUNTIME_API extern const TCHAR* const SetCpuCacheMBName;
	FLOWVIZRUNTIME_API extern const TCHAR* const SetGpuCacheMBName;
	FLOWVIZRUNTIME_API extern const TCHAR* const DumpCaseName;
	FLOWVIZRUNTIME_API extern const TCHAR* const BenchmarkName;

	/** All eight, in plan.md's order. What the wiring test iterates. */
	FLOWVIZRUNTIME_API TArray<const TCHAR*> GetCommandNames();

	/**
	 * Register every command with the console manager.
	 *
	 * Called from module startup. Idempotent: a module reload in a live editor
	 * re-runs startup, and re-registering an existing name would trip the console
	 * manager's own duplicate check.
	 */
	FLOWVIZRUNTIME_API void Register();

	/** Undo Register. Safe when Register never ran. */
	FLOWVIZRUNTIME_API void Unregister();

	/* --- The pure parts, exposed so they can be asserted directly ----------- */

	/**
	 * Build the console line used by UI and startup paths to open a case.
	 *
	 * The path is quoted because case directories routinely inherit spaces from a
	 * user's project or home directory. Leaving it bare makes the console manager
	 * split one path into several arguments and reports a misleading load failure.
	 */
	FLOWVIZRUNTIME_API FString MakeLoadCaseCommand(
		const FString& CasePath, const FString& FieldId = FString());

	/**
	 * Parse a cache-budget argument into bytes.
	 *
	 * SEPARATE FROM THE COMMAND because the interesting behaviour is the
	 * REFUSALS, and a refusal that only manifests as text on an output device is
	 * awkward to pin down exactly. "0" and "-1" and "banana" and an absent
	 * argument are four different mistakes a user makes, and each should be
	 * refused rather than clamped -- a budget silently coerced to 1 MB is a
	 * command that reports success and starves the cache.
	 *
	 * THE UPPER BOUND IS NOT DECORATION. MB * 1024 * 1024 overflows int64 above
	 * roughly 8.8e12, and a wrapped budget is NEGATIVE, which
	 * FFlowVizFrameCache::SetBudgets rejects with a message about a minimum --
	 * naming the wrong problem. Refused here, at the parse, where the number the
	 * user typed is still visible.
	 *
	 * @param Args     The command's arguments. Exactly one is expected.
	 * @param OutBytes Set only on success.
	 * @param OutError Set only on failure, phrased for a console reader.
	 * @return True when OutBytes holds a usable budget.
	 */
	FLOWVIZRUNTIME_API bool ParseCacheMegabytes(
		const TArray<FString>& Args, int64& OutBytes, FString& OutError);

	/**
	 * A human-readable description of a case: what `FlowViz.DumpCase` prints.
	 *
	 * Takes the case rather than the workspace so it can be asserted against a
	 * case loaded directly in a test, with no widget, no world and no player.
	 *
	 * @param Case         The case to describe.
	 * @param BoundFieldId The field the player is displaying, marked in the field
	 *                     list. NAME_None marks nothing rather than guessing.
	 */
	FLOWVIZRUNTIME_API FString FormatCaseSummary(const FCFDVizCase& Case, FName BoundFieldId);
}
