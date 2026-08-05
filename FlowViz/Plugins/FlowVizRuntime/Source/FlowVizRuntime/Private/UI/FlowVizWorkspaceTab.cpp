// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizWorkspaceTab.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/CoreDelegates.h"
#include "UI/SFlowVizWorkspace.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "FlowVizWorkspaceTab"

namespace FlowVizWorkspaceTab
{
	const FName TabId(TEXT("FlowVizWorkspace"));

	namespace
	{
		TSharedRef<SDockTab> SpawnWorkspaceTab(const FSpawnTabArgs& Args)
		{
			return SNew(SDockTab)
				.TabRole(ETabRole::NomadTab)
				.Label(LOCTEXT("WorkspaceTabLabel", "FlowViz"))
				[
					// THE PRODUCTION CONSTRUCTION SITE. This is the line that makes
					// the widgets a feature rather than a definition.
					SNew(SFlowVizWorkspace)
				];
		}
	}

	namespace
	{
		/** Set once we have subscribed to post-engine-init, so we subscribe once. */
		bool bDeferredRegistrationPending = false;
	}

	void Register()
	{
		/*
		 * THIS MODULE LOADS AT PostConfigInit - BEFORE SLATE EXISTS.
		 *
		 * That is set in FlowVizRuntime.uplugin and it is correct for the rest of
		 * the module (the shader path mapping has to be in place before shaders
		 * compile). But it means an `if (!FSlateApplication::IsInitialized())
		 * return;` here is not a headless guard, it is an unconditional early
		 * return in the editor too: at StartupModule there is no Slate yet, so the
		 * tab would NEVER be registered.
		 *
		 * That is not hypothetical - it is what this code did on its first run,
		 * and FlowViz.UI.Workspace.Wiring failed on exactly that. It is worth
		 * noting how quiet the failure was: the guard looks like ordinary
		 * defensive coding, the module starts cleanly, no warning is logged, and
		 * every panel test stays green because each builds its own widget. The
		 * only visible symptom is a menu entry that does not exist.
		 *
		 * So: register NOW if Slate is already up (module reload in a live
		 * editor), otherwise defer to post-engine-init, which runs after Slate is
		 * initialised in an editor or game and never runs at all in a commandlet -
		 * which is the headless behaviour the original guard was reaching for.
		 */
		if (!FSlateApplication::IsInitialized())
		{
			if (!bDeferredRegistrationPending)
			{
				bDeferredRegistrationPending = true;
				FCoreDelegates::GetOnPostEngineInit().AddStatic(&Register);
			}
			return;
		}

		const TSharedRef<FGlobalTabmanager> TabManager = FGlobalTabmanager::Get();

		// IDEMPOTENT. Module reload during a live editor session would otherwise
		// hit the tab manager's own duplicate-registration ensure.
		if (TabManager->HasTabSpawner(TabId))
		{
			return;
		}

		TabManager
			->RegisterNomadTabSpawner(TabId, FOnSpawnTab::CreateStatic(&SpawnWorkspaceTab))
			.SetDisplayName(LOCTEXT("WorkspaceMenuName", "FlowViz Workspace"))
			.SetTooltipText(LOCTEXT("WorkspaceMenuTip",
				"Open the FlowViz workspace: transport, colour and opacity, clipping, slices "
				"and probes."))
			.SetMenuType(ETabSpawnerMenuType::Enabled);
	}

	void Unregister()
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}

		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
	}
}

#undef LOCTEXT_NAMESPACE
