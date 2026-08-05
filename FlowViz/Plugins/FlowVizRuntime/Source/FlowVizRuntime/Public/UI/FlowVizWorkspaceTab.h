// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The workspace's registration with the application (plan.md section F).
 *
 * WHY THIS EXISTS SEPARATELY FROM THE WIDGETS. A Slate panel that nothing ever
 * constructs is not a feature, it is a definition. The rendering layer already
 * shipped that exact failure here: the marcher and the volume component were
 * both complete and both thoroughly tested, and a volume in a real map drew
 * nothing because no production code installed the dispatcher between them
 * (see FlowVizRenderWiringTest.cpp and FlowVizRuntime.cpp:43). Every rendering
 * test installed its own dispatcher first, so none could see the gap.
 *
 * The UI has the same shape. Every panel test `SNew`s the widget it asserts
 * about, which means all of them would stay green if the running application
 * never built one. So the construction site is named, public, and covered by a
 * test that constructs nothing itself: FlowViz.UI.Workspace.Wiring asks the
 * global tab manager whether startup registered this tab and then invokes it.
 *
 * THREADING. Game thread only. Slate is not thread-safe and neither is the tab
 * manager.
 */
namespace FlowVizWorkspaceTab
{
	/**
	 * The tab id the workspace registers under.
	 *
	 * Shared between the registration and the test rather than spelled twice: a
	 * test that hard-coded the string would pass while production registered
	 * under a different name, which is a green test for a tab nobody can open.
	 */
	FLOWVIZRUNTIME_API extern const FName TabId;

	/**
	 * Register the workspace tab spawner with the global tab manager.
	 *
	 * Called from module startup. Safe to call when no Slate application exists
	 * (a commandlet, a cook): it registers nothing in that case rather than
	 * crashing, and the wiring test runs in the editor where one does.
	 */
	FLOWVIZRUNTIME_API void Register();

	/** Undo Register. Safe when Register never ran. */
	FLOWVIZRUNTIME_API void Unregister();
}
