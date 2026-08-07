// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizConsoleCommands.h"

#include "CFDViz/CFDVizManifest.h"
#include "CFDViz/CFDVizTypes.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/OutputDevice.h"
#include "Playback/FlowVizCasePlayer.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Docking/TabManager.h"
#include "Scene/FlowVizCaseActor.h"
#include "UI/SFlowVizPipelinePanel.h"
#include "Scene/FlowVizVolumeComponent.h"
#include "UI/FlowVizDiagnostics.h"
#include "UI/FlowVizWorkspaceTab.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "UI/FlowVizWorkspaceRegistry.h"
#include "UI/SFlowVizWorkspace.h"

namespace FlowVizConsoleCommands
{
	const TCHAR* const LoadCaseName = TEXT("FlowViz.LoadCase");
	const TCHAR* const ReloadCaseName = TEXT("FlowViz.ReloadCase");
	const TCHAR* const ClearCacheName = TEXT("FlowViz.ClearCache");
	const TCHAR* const ShowDiagnosticsName = TEXT("FlowViz.ShowDiagnostics");
	const TCHAR* const SetCpuCacheMBName = TEXT("FlowViz.SetCpuCacheMB");
	const TCHAR* const SetGpuCacheMBName = TEXT("FlowViz.SetGpuCacheMB");
	const TCHAR* const DumpCaseName = TEXT("FlowViz.DumpCase");
	const TCHAR* const BenchmarkName = TEXT("FlowViz.Benchmark");

	TArray<const TCHAR*> GetCommandNames()
	{
		return {
			LoadCaseName,
			ReloadCaseName,
			ClearCacheName,
			ShowDiagnosticsName,
			SetCpuCacheMBName,
			SetGpuCacheMBName,
			DumpCaseName,
			BenchmarkName,
		};
	}

	/* ====================================================================== */
	/* Shared plumbing                                                         */
	/* ====================================================================== */

	namespace Local
	{
		/**
		 * Every command's first two lines, spelled once.
		 *
		 * TWO SEPARATE REFUSALS, because they are two different situations and a
		 * user can act on exactly one of them. "No workspace" means open the
		 * FlowViz tab; "no case" means load one. Collapsing them into a single
		 * "nothing to report" tells the user neither.
		 *
		 * @param bRequireCase When true, a workspace with no case open is refused
		 *                     too. ClearCache and SetCpuCacheMB do not need one --
		 *                     they act on the player, which exists either way.
		 * @return The workspace, or null having already printed why.
		 */
		TSharedPtr<SFlowVizWorkspace> ResolveTarget(FOutputDevice& Ar, bool bRequireCase)
		{
			TSharedPtr<SFlowVizWorkspace> Workspace =
				FlowVizWorkspaceRegistry::GetActiveWorkspace();

			if (!Workspace.IsValid())
			{
				/*
				 * OPEN THE TAB OURSELVES, then re-resolve. In the editor a user
				 * reaches Window > FlowViz; in the PACKAGED app nothing invokes
				 * the tab, so before this a console command was the only door
				 * and it refused with directions to a menu that does not exist
				 * there (found by the DoD 4 smoke test: the packaged app booted
				 * and LoadCase declined). Invoking is idempotent -- an open tab
				 * is focused, not duplicated -- so the editor path is unchanged.
				 */
				FGlobalTabmanager::Get()->TryInvokeTab(FTabId(FlowVizWorkspaceTab::TabId));
				Workspace = FlowVizWorkspaceRegistry::GetActiveWorkspace();
			}

			if (!Workspace.IsValid())
			{
				Ar.Logf(TEXT("FlowViz: no workspace is open and the FlowViz tab could not be "
							 "opened. In the editor: Window > FlowViz."));
				return nullptr;
			}

			if (bRequireCase && !Workspace->GetModel().IsCaseOpen())
			{
				Ar.Logf(TEXT("FlowViz: no case is open. Load one with '%s <path.cfdviz>'."),
					LoadCaseName);
				return nullptr;
			}

			return Workspace;
		}

		/** Bytes as megabytes, for the readouts. Matches the diagnostics overlay's units. */
		double BytesToMB(int64 Bytes)
		{
			return static_cast<double>(Bytes) / (1024.0 * 1024.0);
		}

		/**
		 * Set one budget without disturbing the other.
		 *
		 * FFlowVizCasePlayer::SetMemoryBudgets takes BOTH, so the budget this
		 * command is not setting has to be read back and passed through. Defaulting
		 * it instead would make `FlowViz.SetGpuCacheMB 64` silently reset the CPU
		 * budget to 512 -- a side effect nothing on screen would show.
		 */
		void SetOneBudget(const TArray<FString>& Args, FOutputDevice& Ar, bool bCpu)
		{
			const TSharedPtr<SFlowVizWorkspace> Workspace = ResolveTarget(Ar, /*bRequireCase*/ false);
			if (!Workspace.IsValid())
			{
				return;
			}

			int64 Bytes = 0;
			FString Error;
			if (!ParseCacheMegabytes(Args, Bytes, Error))
			{
				Ar.Logf(TEXT("FlowViz: %s"), *Error);
				return;
			}

			FFlowVizCasePlayer& Player = Workspace->GetModel().Player;
			const FFlowVizFrameCache& Cache = Player.GetCache();

			const int64 CpuBytes = bCpu ? Bytes : Cache.GetCpuBudgetBytes();
			const int64 GpuBytes = bCpu ? Cache.GetGpuBudgetBytes() : Bytes;

			const FCFDVizResult Result = Player.SetMemoryBudgets(CpuBytes, GpuBytes);
			if (!Result.IsOk())
			{
				Ar.Logf(TEXT("FlowViz: budget refused: %s"), *Result.ToString());
				return;
			}

			Ar.Logf(TEXT("FlowViz: %s cache budget is now %.1f MB (%d entries resident, %.1f MB used)"),
				bCpu ? TEXT("CPU") : TEXT("GPU"), BytesToMB(Bytes), Cache.Num(),
				BytesToMB(bCpu ? Cache.GetCpuBytes() : Cache.GetGpuBytes()));
		}
	}

	/* ====================================================================== */
	/* The parse                                                               */
	/* ====================================================================== */

	bool ParseCacheMegabytes(const TArray<FString>& Args, int64& OutBytes, FString& OutError)
	{
		if (Args.Num() != 1)
		{
			OutError = FString::Printf(
				TEXT("expected exactly one argument, a whole number of megabytes; got %d"),
				Args.Num());
			return false;
		}

		const FString Trimmed = Args[0].TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			OutError = TEXT("expected a whole number of megabytes; got an empty argument");
			return false;
		}

		/*
		 * DIGITS ONLY -- no sign, no decimal point.
		 *
		 * This is what refuses "-1" and "1.5" and "banana" by the same rule rather
		 * than by three special cases, and it is why the accumulator below never
		 * has to consider a negative. A budget is a size; "-1 MB" is not a small
		 * budget, it is a typo, and clamping it to something legal would report
		 * success for a number the user did not ask for.
		 */
		for (const TCHAR Character : Trimmed)
		{
			if (Character < TEXT('0') || Character > TEXT('9'))
			{
				OutError = FString::Printf(
					TEXT("'%s' is not a whole number of megabytes"), *Trimmed);
				return false;
			}
		}

		/*
		 * ACCUMULATED WITH THE BOUND CHECKED PER DIGIT, rather than parsed and then
		 * range-checked. A digit string longer than int64 can hold would have
		 * already wrapped by the time a post-hoc check ran, and a wrapped budget is
		 * NEGATIVE -- which FFlowVizFrameCache::SetBudgets then rejects with a
		 * message about a MINIMUM, naming the wrong problem entirely. Refused here,
		 * where the number the user typed is still visible.
		 */
		constexpr int64 BytesPerMegabyte = 1024 * 1024;
		constexpr int64 MaxMegabytes = TNumericLimits<int64>::Max() / BytesPerMegabyte;

		int64 Megabytes = 0;
		for (const TCHAR Character : Trimmed)
		{
			const int64 Digit = static_cast<int64>(Character - TEXT('0'));
			if (Megabytes > (MaxMegabytes - Digit) / 10)
			{
				OutError = FString::Printf(
					TEXT("%s megabytes is more memory than this machine can address; the largest "
						 "budget is %lld MB"),
					*Trimmed, MaxMegabytes);
				return false;
			}
			Megabytes = Megabytes * 10 + Digit;
		}

		if (Megabytes <= 0)
		{
			// NOT CLAMPED TO ONE. A cache that can hold nothing is a real
			// configuration mistake, and silently repairing it produces a viewer
			// that re-reads every frame from disk while reporting success.
			OutError = TEXT("a cache budget of 0 MB would hold no frames at all; pass 1 or more");
			return false;
		}

		OutBytes = Megabytes * BytesPerMegabyte;
		return true;
	}

	/* ====================================================================== */
	/* The case summary                                                        */
	/* ====================================================================== */

	FString FormatCaseSummary(const FCFDVizCase& Case, FName BoundFieldId)
	{
		TStringBuilder<2048> B;

		B.Appendf(TEXT("FlowViz case\n"));
		B.Appendf(TEXT("  Name              %s\n"), *Case.Metadata.Name);
		if (!Case.Metadata.Id.IsEmpty())
		{
			B.Appendf(TEXT("  Id                %s\n"), *Case.Metadata.Id);
		}

		/*
		 * THE QUALITY CLASSIFICATION IS PRINTED, NOT DROPPED. It is how a viewer
		 * tells a user that synthetic demonstration data is not validation-grade
		 * CFD, and a dump that omitted it would let someone quote a number off a
		 * mock case as a result.
		 */
		if (!Case.Metadata.Quality.IsEmpty())
		{
			B.Appendf(TEXT("  Quality           %s\n"), *Case.Metadata.Quality);
		}
		B.Appendf(TEXT("  Solver            %s %s\n"),
			*Case.Metadata.Solver.Name, *Case.Metadata.Solver.Version);
		B.Appendf(TEXT("  Format            %s %s\n"), *Case.Format, *Case.FormatVersion);
		B.Appendf(TEXT("  Root              %s\n"),
			Case.CaseRootDir.IsEmpty() ? TEXT("(unknown)") : *Case.CaseRootDir);

		/* --- Timeline ------------------------------------------------------ */

		B.Appendf(TEXT("  Timeline          %d frames"), Case.Timeline.FrameCount);
		if (Case.Timeline.Times.Num() > 0)
		{
			B.Appendf(TEXT(", t = %.6g .. %.6g %s"),
				Case.Timeline.Times[0], Case.Timeline.Times.Last(), *Case.Units.Time);
		}
		B.Appendf(TEXT("\n"));

		/* --- Grids --------------------------------------------------------- */

		for (const FCFDVizGridDescriptor& Grid : Case.Grids)
		{
			B.Appendf(TEXT("  Grid '%s'         %d x %d x %d cells, spacing %.6g x %.6g x %.6g %s\n"),
				*Grid.Id.ToString(),
				Grid.Geometry.Dimensions.X, Grid.Geometry.Dimensions.Y, Grid.Geometry.Dimensions.Z,
				Grid.Geometry.Spacing.X, Grid.Geometry.Spacing.Y, Grid.Geometry.Spacing.Z,
				*Case.Units.Length);
			if (Grid.HasMaskField())
			{
				B.Appendf(TEXT("                    mask field '%s'\n"), *Grid.MaskFieldId.ToString());
			}
		}

		/* --- Fields -------------------------------------------------------- */

		/*
		 * EVERY DECLARED FIELD, INCLUDING THE MASK, which is deliberately different
		 * from the diagnostics overlay's list of what a user can DISPLAY. This
		 * command describes the FILE. A mask silently omitted here would make a
		 * manifest and its dump disagree about what is in the case, which is the
		 * one question this command exists to answer.
		 */
		B.Appendf(TEXT("  Fields            %d\n"), Case.Fields.Num());
		for (const FCFDVizField& Field : Case.Fields)
		{
			// The marker is what makes two summaries of the same case differ when
			// the binding differs -- without it, "which field am I looking at" is
			// unanswerable from a dump.
			const bool bBound = !BoundFieldId.IsNone() && Field.Id == BoundFieldId;

			B.Appendf(TEXT("    %s %-20s %d comp  %-8s %-6s %s"),
				bBound ? TEXT("->") : TEXT("  "),
				*Field.Id.ToString(),
				Field.ComponentCount,
				DataTypeToString(Field.DataType),
				AssociationToString(Field.Association),
				CodecToString(Field.Storage.Codec));

			if (!Field.Unit.IsEmpty())
			{
				B.Appendf(TEXT("  [%s]"), *Field.Unit);
			}
			B.Appendf(TEXT("\n"));
		}

		/* --- What is bound ------------------------------------------------- */

		if (BoundFieldId.IsNone())
		{
			B.Appendf(TEXT("  Displaying        (nothing bound)\n"));
		}
		else if (Case.FindField(BoundFieldId) != nullptr)
		{
			B.Appendf(TEXT("  Displaying        %s\n"), *BoundFieldId.ToString());
		}
		else
		{
			/*
			 * A REAL STATE, AND IT MUST NOT READ AS "nothing bound". A workspace
			 * bound to a field the manifest was edited out from under is exactly
			 * the situation a dump is run to diagnose, and rendering it identically
			 * to an unbound workspace would hide it.
			 */
			B.Appendf(TEXT("  Displaying        %s -- WHICH THIS CASE DOES NOT DECLARE\n"),
				*BoundFieldId.ToString());
		}

		if (Case.Meshes.Num() > 0)
		{
			B.Appendf(TEXT("  Meshes            %d\n"), Case.Meshes.Num());
		}
		if (Case.DerivedFields.Num() > 0)
		{
			B.Appendf(TEXT("  Derived fields    %d\n"), Case.DerivedFields.Num());
		}

		return B.ToString();
	}

	/* ====================================================================== */
	/* The command bodies                                                      */
	/* ====================================================================== */

	namespace Local
	{
		void ExecLoadCase(const TArray<FString>& Args, FOutputDevice& Ar)
		{
			const TSharedPtr<SFlowVizWorkspace> Workspace = ResolveTarget(Ar, /*bRequireCase*/ false);
			if (!Workspace.IsValid())
			{
				return;
			}

			if (Args.Num() < 1)
			{
				Ar.Logf(TEXT("usage: %s <path-to-case.cfdviz> [fieldId]"), LoadCaseName);
				return;
			}

			const FString Path = Args[0];
			const FName FieldId = Args.Num() >= 2 ? FName(*Args[1]) : NAME_None;

			const FCFDVizResult Result = Workspace->GetModel().OpenCase(Path, FieldId);
			if (!Result.IsOk())
			{
				/*
				 * SAID OUT LOUD. FFlowVizWorkspaceModel::OpenCase parses into a
				 * local and leaves the PREVIOUS case in place on failure, which is
				 * the right behaviour and is also indistinguishable from "the
				 * command did nothing" unless the refusal is printed.
				 */
				Ar.Logf(TEXT("FlowViz: load failed: %s"), *Result.ToString());
				return;
			}

			/*
			 * THE VIEWPORT HALF (#88). The model's OpenCase feeds the PANELS;
			 * pixels come from a UCFDVizVolumeComponent, which lives on an
			 * ACFDVizCaseActor in a world -- and nothing guaranteed one
			 * existed. Found by a user running the demo: the workspace opened,
			 * the console said "opened ... 20 frames", and the viewport stayed
			 * black, because LoadCase had nowhere to put voxels.
			 *
			 * So: no bound volume means find-or-spawn a case actor (PIE/Game
			 * world preferred, the editor world otherwise -- TRANSIENT, so a
			 * demo spawn never dirties the level asset), and EITHER WAY the
			 * case is loaded into the volume component too. The workspace's
			 * player decodes for the panels; the component's binding is what
			 * the ray marcher samples. Loading one without the other is the
			 * black-viewport state this comment exists to prevent.
			 */
			if (Workspace->GetVolume() == nullptr && GEngine != nullptr)
			{
				UWorld* TargetWorld = nullptr;
				for (const FWorldContext& Context : GEngine->GetWorldContexts())
				{
					if (Context.WorldType == EWorldType::PIE
						|| Context.WorldType == EWorldType::Game)
					{
						TargetWorld = Context.World();
						break;
					}
					if (TargetWorld == nullptr && Context.WorldType == EWorldType::Editor)
					{
						TargetWorld = Context.World();
					}
				}
				if (TargetWorld != nullptr)
				{
					FActorSpawnParameters SpawnParameters;
					SpawnParameters.ObjectFlags |= RF_Transient;
					if (ACFDVizCaseActor* Spawned =
							TargetWorld->SpawnActor<ACFDVizCaseActor>(SpawnParameters))
					{
						Workspace->SetVolume(Spawned->GetVolumeComponent());
						Ar.Logf(TEXT("FlowViz: spawned a transient case actor to display "
									 "the volume."));
					}
				}
			}

			if (UCFDVizVolumeComponent* Volume = Workspace->GetVolume())
			{
				const FCFDVizResult VolumeLoad = Volume->LoadCase(Path, FieldId);
				if (!VolumeLoad.IsOk())
				{
					Ar.Logf(TEXT("FlowViz: the panels are live but the volume could not "
								 "load the case: %s"),
						*VolumeLoad.ToString());
				}
			}
			else
			{
				Ar.Logf(TEXT("FlowViz: no world to spawn a case actor in; the panels are "
							 "live but nothing will render until a case actor exists."));
			}

			// The model changed underneath the panels and no panel announced it, so
			// nothing else will make the render follow. Same obligation
			// SFlowVizWorkspace::LoadState documents for a session load.
			Workspace->PushToVolume();

			// The field list changed with the case; the rows are explicit-refresh.
			if (Workspace->GetPipelinePanel().IsValid())
			{
				Workspace->GetPipelinePanel()->RefreshFields();
			}

			/*
			 * AUTO-PLAY. A user who opens a time-varying case wants to see it
			 * move; a load that lands on frame 1/20, paused, at 0.000s reads
			 * as "nothing really happened" -- verbatim how the gap was
			 * reported. Opening is the request to watch.
			 */
			Workspace->GetModel().Player.Play();

			const FCFDVizCase* Case = Workspace->GetModel().GetCase();
			Ar.Logf(TEXT("FlowViz: opened '%s' -- %d frames, displaying '%s'"),
				Case != nullptr ? *Case->Metadata.Name : TEXT("?"),
				Case != nullptr ? Case->Timeline.FrameCount : 0,
				*Workspace->GetModel().Player.GetFieldId().ToString());
		}

		void ExecReloadCase(const TArray<FString>& Args, FOutputDevice& Ar)
		{
			const TSharedPtr<SFlowVizWorkspace> Workspace = ResolveTarget(Ar, /*bRequireCase*/ true);
			if (!Workspace.IsValid())
			{
				return;
			}

			FFlowVizWorkspaceModel& Model = Workspace->GetModel();
			const FCFDVizCase* Case = Model.GetCase();
			if (Case == nullptr || Case->CaseRootDir.IsEmpty())
			{
				Ar.Logf(TEXT("FlowViz: the open case does not record where it was loaded from, so "
							 "there is nothing to re-read."));
				return;
			}

			/*
			 * COPIED BEFORE THE RE-OPEN, not read through the pointer during it.
			 * OpenCase calls CloseCase before it commits, which releases the shared
			 * case these two strings live inside -- passing `Case->CaseRootDir`
			 * directly would hand OpenCase a reference into memory it frees partway
			 * through its own execution.
			 */
			const FString RootDir = Case->CaseRootDir;
			const FName FieldId = Model.Player.GetFieldId();

			const FCFDVizResult Result = Model.OpenCase(RootDir, FieldId);
			if (!Result.IsOk())
			{
				Ar.Logf(TEXT("FlowViz: reload failed: %s"), *Result.ToString());
				return;
			}

			Workspace->PushToVolume();

			const FCFDVizCase* Reloaded = Model.GetCase();
			Ar.Logf(TEXT("FlowViz: reloaded '%s' from %s -- %d frames, displaying '%s'"),
				Reloaded != nullptr ? *Reloaded->Metadata.Name : TEXT("?"),
				*RootDir,
				Reloaded != nullptr ? Reloaded->Timeline.FrameCount : 0,
				*Model.Player.GetFieldId().ToString());
		}

		void ExecClearCache(const TArray<FString>& Args, FOutputDevice& Ar)
		{
			const TSharedPtr<SFlowVizWorkspace> Workspace = ResolveTarget(Ar, /*bRequireCase*/ false);
			if (!Workspace.IsValid())
			{
				return;
			}

			FFlowVizCasePlayer& Player = Workspace->GetModel().Player;
			FFlowVizFrameCache& Cache = Player.GetCache();

			const int32 EntriesBefore = Cache.Num();
			const int64 CpuBefore = Cache.GetCpuBytes();

			Cache.Reset();

			/*
			 * DELIBERATELY NOT TICKED AFTERWARDS.
			 *
			 * The obvious-looking follow-up -- Player.Tick(0.0), so the player
			 * re-resolves what it may display -- makes this command a no-op you can
			 * watch. Tick's first act is StartPendingLoads, which RESERVES cache
			 * entries for the frames the playhead wants before any decoding
			 * happens, so the cache is repopulated inside the same command that
			 * emptied it and the reported "cleared N entries" describes a state
			 * that lasted microseconds. FlowViz.UI.Console.Execute caught exactly
			 * that: entries went to 2 and CpuBytes to 37632 between the Reset and
			 * the assertion.
			 *
			 * Nothing is left inconsistent by leaving it. The display still names
			 * the frames it named a moment ago, and the GPU texture set still holds
			 * their voxels, so the renderer keeps drawing the image it was already
			 * drawing -- an accurate picture of what is on the GPU. The workspace's
			 * own ticker re-requests whatever the playhead needs on the next frame,
			 * which is the point of the command: the re-read is the user's, not
			 * this function's.
			 */

			Ar.Logf(TEXT("FlowViz: cleared %d cache entries (%.1f MB). Budgets and cumulative "
						 "counters are unchanged."),
				EntriesBefore, BytesToMB(CpuBefore));
		}

		void ExecShowDiagnostics(const TArray<FString>& Args, FOutputDevice& Ar)
		{
			const TSharedPtr<SFlowVizWorkspace> Workspace = ResolveTarget(Ar, /*bRequireCase*/ false);
			if (!Workspace.IsValid())
			{
				return;
			}

			/*
			 * IT SHOWS. The name is not decoration: plan.md section 17 lists this
			 * command and an on-screen overlay in the same breath, and
			 * SFlowVizDiagnosticsOverlay has no other way to be turned on -- the
			 * workspace constructs it collapsed and nothing else drives that slot.
			 * A command called Show that only wrote to the output log would leave a
			 * built, wired, correct widget permanently invisible, and the symptom
			 * ("the overlay doesn't exist") would look like a missing feature
			 * rather than a missing line here.
			 */
			const bool bNowShown = !Workspace->IsDiagnosticsOverlayShown();
			Workspace->SetDiagnosticsOverlayShown(bNowShown);

			/*
			 * AND IT STILL PRINTS, on both edges of the toggle. Two reasons, and
			 * the second is the load-bearing one:
			 *
			 *   - The log is the only readout available when the command is run
			 *     from a headless session, a startup script, or -ExecCmds, where
			 *     there is no screen for the overlay to appear on.
			 *
			 *   - Printing only when turning ON would make this command's output
			 *     depend on which state a previous caller left the overlay in. The
			 *     console suite asserts on the text; that assertion would then pass
			 *     or fail according to test ORDER, which is a coin flip wearing a
			 *     green tick.
			 *
			 * THE SAME COLLECTOR AND THE SAME FORMATTER the on-screen overlay uses.
			 * A console command with its own formatting would drift from the overlay
			 * and the two would disagree about the same session.
			 */
			Ar.Logf(TEXT("FlowViz: diagnostics overlay %s."), bNowShown ? TEXT("shown") : TEXT("hidden"));
			Ar.Logf(TEXT("%s"), *FlowVizDiagnostics::Format(FlowVizDiagnostics::Collect(
									 Workspace->GetModel(), Workspace->GetVolume())));
		}

		void ExecDumpCase(const TArray<FString>& Args, FOutputDevice& Ar)
		{
			const TSharedPtr<SFlowVizWorkspace> Workspace = ResolveTarget(Ar, /*bRequireCase*/ true);
			if (!Workspace.IsValid())
			{
				return;
			}

			const FCFDVizCase* Case = Workspace->GetModel().GetCase();
			if (Case == nullptr)
			{
				Ar.Logf(TEXT("FlowViz: no case is open."));
				return;
			}

			Ar.Logf(TEXT("%s"),
				*FormatCaseSummary(*Case, Workspace->GetModel().Player.GetFieldId()));
		}

		void ExecSetCpuCacheMB(const TArray<FString>& Args, FOutputDevice& Ar)
		{
			SetOneBudget(Args, Ar, /*bCpu*/ true);
		}

		void ExecSetGpuCacheMB(const TArray<FString>& Args, FOutputDevice& Ar)
		{
			SetOneBudget(Args, Ar, /*bCpu*/ false);
		}

		/** Frames a benchmark measures when the command line does not say. */
		constexpr int32 DefaultBenchmarkFrames = 8;

		/** Per-frame ceiling. A decode slower than this is a failure to measure, not a slow measurement. */
		constexpr double BenchmarkFrameTimeoutSeconds = 30.0;

		void ExecBenchmark(const TArray<FString>& Args, FOutputDevice& Ar)
		{
			const TSharedPtr<SFlowVizWorkspace> Workspace = ResolveTarget(Ar, /*bRequireCase*/ true);
			if (!Workspace.IsValid())
			{
				return;
			}

			FFlowVizCasePlayer& Player = Workspace->GetModel().Player;
			const int32 FrameCount = Player.GetTimeline().GetFrameCount();
			if (FrameCount <= 0)
			{
				Ar.Logf(TEXT("FlowViz: the open case has no frames to measure."));
				return;
			}

			int32 Requested = DefaultBenchmarkFrames;
			if (Args.Num() >= 1)
			{
				int32 Parsed = 0;
				if (!LexTryParseString(Parsed, *Args[0]) || Parsed <= 0)
				{
					Ar.Logf(TEXT("usage: %s [frameCount]  -- frameCount must be a positive whole "
								 "number of frames"),
						BenchmarkName);
					return;
				}
				Requested = Parsed;
			}
			Requested = FMath::Min(Requested, FrameCount);

			/*
			 * THE CACHE IS EMPTIED FIRST, AND THAT IS THE MEASUREMENT.
			 *
			 * A benchmark run against a warm cache measures a TMap lookup. The
			 * number this command exists to produce is the disk-read-plus-decode
			 * cost of a frame, and the only way to measure it is to guarantee no
			 * frame is already resident. Said out loud below, because the side
			 * effect is real and a user watching the cache counters deserves to
			 * know what emptied them.
			 */
			Player.GetCache().Reset();

			// Restored at the end. A diagnostic command that left the playhead
			// wherever its last sample landed would silently scrub the user's view.
			const double TimeBefore = Player.GetPhysicalTime();

			double TotalMs = 0.0;
			double WorstMs = 0.0;
			int32 Resolved = 0;

			for (int32 Sample = 0; Sample < Requested; ++Sample)
			{
				/*
				 * SPREAD ACROSS THE TIMELINE rather than the first N in a row.
				 * Consecutive frames are exactly what the player's preload-ahead
				 * has already fetched, so measuring 0,1,2 would time one real read
				 * and two cache hits and report the mean as a read cost.
				 */
				const int32 FrameIndex = Requested > 1
					? FMath::RoundToInt(
						  static_cast<double>(Sample) * (FrameCount - 1) / (Requested - 1))
					: 0;

				const double Start = FPlatformTime::Seconds();

				Player.SeekToFrame(FrameIndex);
				Player.Tick(0.0);
				Player.WaitForPendingLoads(BenchmarkFrameTimeoutSeconds);
				Player.Tick(0.0);

				const double ElapsedMs = (FPlatformTime::Seconds() - Start) * 1000.0;

				/*
				 * ONLY A RESIDENT FRAME COUNTS. A decode that timed out or failed
				 * also "took" a measurable number of milliseconds, and folding that
				 * into the mean would report a broken case as a fast one.
				 */
				if (Player.GetFrameLoadState(FrameIndex) == EFlowVizLoadState::Resident)
				{
					++Resolved;
					TotalMs += ElapsedMs;
					WorstMs = FMath::Max(WorstMs, ElapsedMs);
				}
			}

			Player.SeekToTime(TimeBefore);
			Player.Tick(0.0);

			Ar.Logf(TEXT("FlowViz benchmark"));
			Ar.Logf(TEXT("  resolved %d of %d frames (cache emptied first, so these are cold "
						 "read-and-decode times)"),
				Resolved, Requested);

			/*
			 * plan.md section 17: "Do not claim performance that was not measured."
			 * A run where nothing resolved prints NO timing at all rather than a
			 * mean of zero samples -- which would render as "0.00 ms" and read as
			 * instantaneous.
			 */
			if (Resolved > 0)
			{
				Ar.Logf(TEXT("  mean %.2f ms, worst %.2f ms, over the %d frames that resolved"),
					TotalMs / Resolved, WorstMs, Resolved);
			}
			else
			{
				Ar.Logf(TEXT("  no timing reported: not one frame resolved, so there is nothing "
							 "measured to report."));
			}

			if (Resolved < Requested)
			{
				Ar.Logf(TEXT("  %d frame(s) did not resolve within %.0f s each; check the log for "
							 "decode errors."),
					Requested - Resolved, BenchmarkFrameTimeoutSeconds);
			}
		}

		/**
		 * What Register handed the console manager, so Unregister can hand it back.
		 *
		 * Function-local for the same reason the workspace registry's array is: this
		 * module loads at PostConfigInit, and a namespace-scope TArray would be
		 * constructed during static initialisation in an order no one controls.
		 */
		TArray<IConsoleCommand*>& GetRegistered()
		{
			static TArray<IConsoleCommand*> Registered;
			return Registered;
		}

		void Add(const TCHAR* Name, const TCHAR* Help,
			void (*Function)(const TArray<FString>&, FOutputDevice&))
		{
			IConsoleCommand* Command = IConsoleManager::Get().RegisterConsoleCommand(
				Name, Help,
				FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(Function));

			if (Command != nullptr)
			{
				GetRegistered().Add(Command);
			}
		}
	}

	/* ====================================================================== */
	/* Registration                                                            */
	/* ====================================================================== */

	void Register()
	{
		// IDEMPOTENT. A live-coding module reload re-runs StartupModule, and
		// registering an existing name a second time trips the console manager's
		// own duplicate check.
		if (Local::GetRegistered().Num() > 0)
		{
			return;
		}

		Local::Add(LoadCaseName,
			TEXT("FlowViz.LoadCase <path-to-case.cfdviz> [fieldId] -- open a case in the active "
				 "FlowViz workspace."),
			&Local::ExecLoadCase);

		Local::Add(ReloadCaseName,
			TEXT("Re-read the open case's manifest from disk, keeping the bound field."),
			&Local::ExecReloadCase);

		Local::Add(ClearCacheName,
			TEXT("Drop every cached frame. Budgets and cumulative counters survive."),
			&Local::ExecClearCache);

		Local::Add(ShowDiagnosticsName,
			TEXT("Toggle the on-screen diagnostics overlay, and print its numbers to the console."),
			&Local::ExecShowDiagnostics);

		Local::Add(SetCpuCacheMBName,
			TEXT("FlowViz.SetCpuCacheMB <megabytes> -- set the CPU frame-cache budget."),
			&Local::ExecSetCpuCacheMB);

		Local::Add(SetGpuCacheMBName,
			TEXT("FlowViz.SetGpuCacheMB <megabytes> -- set the GPU frame-cache budget."),
			&Local::ExecSetGpuCacheMB);

		Local::Add(DumpCaseName,
			TEXT("Describe the open case: timeline, grids, fields, codecs."),
			&Local::ExecDumpCase);

		Local::Add(BenchmarkName,
			TEXT("FlowViz.Benchmark [frameCount] -- empty the cache and time a cold "
				 "read-and-decode of that many frames."),
			&Local::ExecBenchmark);
	}

	void Unregister()
	{
		IConsoleManager& Manager = IConsoleManager::Get();
		for (IConsoleCommand* Command : Local::GetRegistered())
		{
			/*
			 * bKeepState = false. Keeping state is for cvars, whose value a
			 * re-registration should inherit; a command has no value to keep, and
			 * leaving a placeholder behind would let FindConsoleObject answer for a
			 * name whose delegate points into a dylib that has been unloaded.
			 */
			Manager.UnregisterConsoleObject(Command, /*bKeepState*/ false);
		}
		Local::GetRegistered().Reset();
	}
}
