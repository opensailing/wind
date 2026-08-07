// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class SFlowVizWorkspace;

/**
 * Which workspaces currently exist (plan.md section 17).
 *
 * WHY ANYTHING NEEDS THIS. FlowVizWorkspaceTab::SpawnWorkspaceTab builds an
 * SFlowVizWorkspace, hands it to an SDockTab and keeps no reference. That is
 * correct for a tab -- Slate owns the widget -- but it means a console command
 * typed by a user has NO WAY TO NAME the workspace it is supposed to act on.
 * Without this list, `FlowViz.ShowDiagnostics` could only either construct a
 * second, empty workspace and report on that (numbers describing an object the
 * user has never seen) or report nothing at all.
 *
 * A LIST, NOT A SINGLETON, because more than one can exist: a docked tab plus a
 * second nomad tab, or a test's own widget alongside the real one. Pretending
 * there is exactly one would make whichever arrived second invisible.
 *
 * WEAK, AND THAT IS THE WHOLE OWNERSHIP STORY. The registry holds no reference
 * that keeps a workspace alive; Slate's widget tree does. A strong list here
 * would keep every closed tab's workspace -- and its case player, its cache and
 * its texture-set pointer -- alive for the life of the process. Entries whose
 * widget has been destroyed are compacted on the next read, and the destructor
 * removes its own entry so that compaction is not the only mechanism.
 *
 * THREADING. Game thread only. Slate widgets are constructed and destroyed
 * there, console commands execute there, and the array is unsynchronised.
 */
namespace FlowVizWorkspaceRegistry
{
	/**
	 * Add a workspace to the list. Called from SFlowVizWorkspace::Construct.
	 *
	 * Takes the widget rather than a shared pointer because Construct is where
	 * the call has to happen -- earlier there is no widget and later there is no
	 * single place every construction path passes through -- and the registry
	 * derives its own weak reference from it.
	 */
	FLOWVIZRUNTIME_API void Register(SFlowVizWorkspace& Workspace);

	/** Remove a workspace. Called from ~SFlowVizWorkspace. Safe when Register never ran. */
	FLOWVIZRUNTIME_API void Unregister(SFlowVizWorkspace& Workspace);

	/**
	 * Every workspace that still exists, oldest first.
	 *
	 * Shared rather than raw: a console command that resolves a target and then
	 * does work with it -- Benchmark pumps the player for seconds -- must not
	 * have that target destroyed underneath it, and the returned pointers are
	 * what prevent it.
	 */
	FLOWVIZRUNTIME_API TArray<TSharedPtr<SFlowVizWorkspace>> GetLiveWorkspaces();

	/**
	 * The workspace a console command should act on, or null when none exists.
	 *
	 * THE MOST RECENTLY CONSTRUCTED ONE, which is the opposite of the rule
	 * FlowVizWorkspaceTab::FindVolumeToDrive uses for case actors, and the
	 * difference is deliberate. Case actors are placed in a level and their order
	 * is arbitrary, so "the first one" is as good an arbitrary choice as any and
	 * the warning is what matters. Workspaces are OPENED, one after another, by a
	 * person -- and the one they just opened is the one they mean. Taking the
	 * oldest would send every command to a tab that has been sitting in the
	 * background since the session started.
	 *
	 * It also makes this resolvable in a test without depending on what other
	 * tests left behind: a freshly constructed workspace is always the answer.
	 */
	FLOWVIZRUNTIME_API TSharedPtr<SFlowVizWorkspace> GetActiveWorkspace();
}
