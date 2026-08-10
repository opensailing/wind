// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizWorkspaceTab.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "FlowVizRuntime.h"
#include "Misc/CoreDelegates.h"
#include "Scene/FlowVizCaseActor.h"
#include "Scene/FlowVizVolumeComponent.h"
#include "UI/SFlowVizWorkspace.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "FlowVizWorkspaceTab"

namespace FlowVizWorkspaceTab
{
	const FName TabId(TEXT("FlowVizWorkspace"));

	namespace
	{
		/**
		 * The volume component a freshly-opened workspace should drive.
		 *
		 * WHY THE TAB DOES THIS AND NOT THE WORKSPACE. The workspace widget knows
		 * about view models and panels; giving it a world to search would make it
		 * unconstructible in the places that build it without one. The tab is
		 * already the seam between "the engine has a session" and "the UI exists",
		 * so the lookup belongs here.
		 *
		 * FIRST MATCH, NOT AN ARBITRATION. A level with two case actors is not a
		 * supported arrangement yet - the workspace drives one volume - and
		 * picking silently among several would be a decision made where nobody
		 * could see it. When multi-case lands this becomes a selector, and the
		 * warning below is what will point at it.
		 */
		UCFDVizVolumeComponent* FindVolumeToDrive()
		{
			if (GEngine == nullptr)
			{
				return nullptr;
			}

			UCFDVizVolumeComponent* Found = nullptr;
			int32 CandidateCount = 0;

			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				// PIE AND GAME ONLY. An Editor-type context is the level being
				// authored; driving its component from a nomad tab would edit the
				// asset rather than the session, and during PIE both contexts exist
				// at once - taking either would be a coin flip.
				if (Context.WorldType != EWorldType::PIE && Context.WorldType != EWorldType::Game)
				{
					continue;
				}

				UWorld* World = Context.World();
				if (World == nullptr)
				{
					continue;
				}

				for (TActorIterator<ACFDVizCaseActor> It(World); It; ++It)
				{
					UCFDVizVolumeComponent* Volume = It->GetVolumeComponent();
					if (Volume == nullptr)
					{
						continue;
					}

					++CandidateCount;
					if (Found == nullptr)
					{
						Found = Volume;
					}
				}
			}

			if (CandidateCount > 1)
			{
				// SAID OUT LOUD. Otherwise a user with two cases open would find
				// the panels driving one of them for no visible reason, and would
				// reasonably conclude the controls were broken.
				UE_LOG(LogFlowViz, Warning,
					TEXT("FlowViz workspace: %d case actors are present; the panels will drive "
						 "the first one found. Multi-case selection is not implemented."),
					CandidateCount);
			}

			return Found;
		}

		TSharedRef<SDockTab> SpawnWorkspaceTab(const FSpawnTabArgs& Args)
		{
			// THE PRODUCTION CONSTRUCTION SITE. This is the line that makes the
			// widgets a feature rather than a definition.
			const TSharedRef<SFlowVizWorkspace> Workspace = SNew(SFlowVizWorkspace);

			/*
			 * AND THE LINE THAT MAKES THE PANELS DRIVE A RENDERER.
			 *
			 * Without it the workspace is built, every panel works against its view
			 * model, and nothing reaches a pixel - which is exactly the state the
			 * clip panel's advisory used to describe permanently. Null is a normal
			 * outcome (no case actor in the level yet); the workspace stays
			 * operable and the advisory stays up to say why.
			 */
			Workspace->SetVolume(FindVolumeToDrive());

			return SNew(SDockTab)
				.TabRole(ETabRole::NomadTab)
				.Label(LOCTEXT("WorkspaceTabLabel", "FlowViz"))
				[
					Workspace
				];
		}
	}

	namespace
	{
		/** The callback is removed after success and on module shutdown. */
		FDelegateHandle DeferredRegistrationHandle;

		void ClearDeferredRegistration()
		{
			if (DeferredRegistrationHandle.IsValid())
			{
				FCoreDelegates::GetOnPostEngineInit().Remove(DeferredRegistrationHandle);
				DeferredRegistrationHandle.Reset();
			}
		}
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
			if (!DeferredRegistrationHandle.IsValid())
			{
				DeferredRegistrationHandle =
					FCoreDelegates::GetOnPostEngineInit().AddStatic(&Register);
			}
			return;
		}

		// The deferred callback has served its only purpose. Keeping it bound would
		// leave a global delegate pointing into this module until shutdown/reload.
		ClearDeferredRegistration();

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
		ClearDeferredRegistration();

		if (!FSlateApplication::IsInitialized())
		{
			return;
		}

		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
	}

#if WITH_DEV_AUTOMATION_TESTS
	bool IsDeferredRegistrationPendingForTesting()
	{
		return DeferredRegistrationHandle.IsValid();
	}
#endif
}

#undef LOCTEXT_NAMESPACE
