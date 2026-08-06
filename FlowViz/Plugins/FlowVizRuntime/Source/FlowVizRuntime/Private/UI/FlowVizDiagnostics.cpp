// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizDiagnostics.h"

#include "CFDViz/CFDVizManifest.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "RenderTimer.h"
#include "Render/FlowVizVolumeTexture.h"
#include "Scene/FlowVizVolumeComponent.h"
#include "UI/FlowVizWorkspaceModel.h"

namespace FlowVizDiagnostics
{
	namespace
	{
		/** Engine cycle counters are uint32 cycles; every timing row wants milliseconds. */
		double CyclesToMs(uint32 Cycles)
		{
			return FPlatformTime::ToMilliseconds(Cycles);
		}

		/** Bytes as MB, for rows a human reads at a glance. */
		double BytesToMB(int64 Bytes)
		{
			return static_cast<double>(Bytes) / (1024.0 * 1024.0);
		}
	}

	FFlowVizDiagnosticsSnapshot Collect(
		const FFlowVizWorkspaceModel& Model, const UCFDVizVolumeComponent* Volume)
	{
		FFlowVizDiagnosticsSnapshot Out;

		/* --- Engine timing --------------------------------------------------- */

		Out.FrameDeltaSeconds = FApp::GetDeltaTime();

		// GUARDED, NOT ASSUMED POSITIVE. A zero delta is the state before the
		// first frame is timed, and dividing there would put an infinity on
		// screen where a reader expects a frame rate.
		Out.FramesPerSecond = Out.FrameDeltaSeconds > 0.0 ? 1.0 / Out.FrameDeltaSeconds : 0.0;

		Out.GameThreadMs = CyclesToMs(GGameThreadTime);
		Out.RenderThreadMs = CyclesToMs(GRenderThreadTime);

		/* --- Playback -------------------------------------------------------- */

		Out.bCaseOpen = Model.IsCaseOpen();

		const FFlowVizPlaybackDiagnostics Playback = Model.Player.GetDiagnostics();

		Out.FrameCount = Playback.FrameCount;
		Out.FrameA = Playback.Selection.FrameA;
		Out.FrameB = Playback.Selection.FrameB;
		Out.InterpolationAlpha = Playback.Selection.Alpha;
		Out.PhysicalTime = Playback.PhysicalTime;
		Out.DisplayFrameA = Playback.Display.FrameA;
		Out.DisplayFrameB = Playback.Display.FrameB;

		if (const FCFDVizCase* Case = Model.GetCase())
		{
			Out.LoadedFields = Model.GetVolumeFieldIds();
		}

		/* --- Loading --------------------------------------------------------- */

		Out.LoadsInFlight = Playback.LoadsInFlight;
		Out.LoadsStarted = Playback.LoadsStarted;
		Out.LoadsCompleted = Playback.LoadsCompleted;
		Out.LoadsFailed = Playback.LoadsFailed;
		Out.CacheHits = Playback.CacheHits;
		Out.CacheMisses = Playback.CacheMisses;
		Out.Cache = Playback.Cache;

		// NEGATIVE FOR "NOT YET MEASURED", per the header's reasoning: a cold
		// start and a total cache failure are different facts and must not share
		// a spelling.
		const int64 Selections = Playback.CacheHits + Playback.CacheMisses;
		Out.CacheHitRate = Selections > 0
			? static_cast<double>(Playback.CacheHits) / static_cast<double>(Selections)
			: -1.0;

		/* --- Clip ------------------------------------------------------------ */

		Out.ClipPlaneCount = Model.Clip.GetPlaneCount();
		Out.EnabledClipPlaneCount = Model.Clip.GetEnabledPlaneCount();

		/* --- Render settings and resolution, which live on the component ----- */

		/*
		 * OPTIONAL, AND LEFT ZERO WHEN ABSENT rather than defaulted. The step and
		 * the resolution belong to the volume the renderer draws; with no volume
		 * there is no answer, and filling in FlowVizRayMarch::DefaultStepVoxels
		 * here would put a number on screen that describes no live object.
		 */
		if (Volume != nullptr)
		{
			const FFlowVizRenderSettingsViewModel& Settings = Volume->GetRenderSettings();
			Out.StepVoxels = Settings.GetStepVoxels();
			Out.MaxSteps = Settings.GetMaxSteps();

			// THE UPLOADED LAYOUT, not the case's declared grid. These agree for a
			// whole-volume upload and diverge the moment bricking or downsampling
			// lands -- and the number a reader needs is what the GPU actually holds.
			Out.VolumeDimensions = Volume->GetTextureSet().GetUploadedFieldLayout().Extent;
		}

		return Out;
	}

	FString Format(const FFlowVizDiagnosticsSnapshot& S)
	{
		TStringBuilder<2048> B;

		B.Appendf(TEXT("FlowViz diagnostics\n"));
		B.Appendf(TEXT("  FPS               %.1f (%.2f ms)\n"),
			S.FramesPerSecond, S.FrameDeltaSeconds * 1000.0);
		B.Appendf(TEXT("  Game thread       %.2f ms\n"), S.GameThreadMs);
		B.Appendf(TEXT("  Render thread     %.2f ms\n"), S.RenderThreadMs);

		if (!S.bCaseOpen)
		{
			// SAID PLAINLY. The alternative is a screen of zeros that reads as a
			// loaded case performing terribly.
			B.Appendf(TEXT("  (no case open)\n"));
			return B.ToString();
		}

		B.Appendf(TEXT("  Frame             %d of %d\n"), S.FrameA, S.FrameCount);
		if (S.FrameB != S.FrameA && S.FrameB != INDEX_NONE)
		{
			B.Appendf(TEXT("  Blending          %d -> %d, alpha %.4f\n"),
				S.FrameA, S.FrameB, S.InterpolationAlpha);
		}
		B.Appendf(TEXT("  Physical time     %.4f\n"), S.PhysicalTime);

		if (S.DisplayFrameA != S.FrameA)
		{
			// The renderer is behind the playhead. Worth a row of its own -- it is
			// the difference between "the case is slow" and "the case is wrong".
			B.Appendf(TEXT("  Displaying        %d (playhead is ahead)\n"), S.DisplayFrameA);
		}

		B.Appendf(TEXT("  Fields            %d"), S.LoadedFields.Num());
		for (int32 Index = 0; Index < S.LoadedFields.Num(); ++Index)
		{
			B.Appendf(TEXT("%s%s"), Index == 0 ? TEXT(" (") : TEXT(", "), *S.LoadedFields[Index].ToString());
		}
		B.Appendf(TEXT("%s\n"), S.LoadedFields.Num() > 0 ? TEXT(")") : TEXT(""));

		B.Appendf(TEXT("  Resolution        %d x %d x %d\n"),
			S.VolumeDimensions.X, S.VolumeDimensions.Y, S.VolumeDimensions.Z);
		B.Appendf(TEXT("  Ray step          %.3f voxels, max %u steps\n"), S.StepVoxels, S.MaxSteps);
		B.Appendf(TEXT("  Clip planes       %d enabled of %d\n"),
			S.EnabledClipPlaneCount, S.ClipPlaneCount);

		B.Appendf(TEXT("  CPU cache         %.1f / %.1f MB (%d entries)\n"),
			BytesToMB(S.Cache.CpuBytes), BytesToMB(S.Cache.CpuBudgetBytes), S.Cache.EntryCount);
		B.Appendf(TEXT("  GPU cache         %.1f / %.1f MB\n"),
			BytesToMB(S.Cache.GpuBytes), BytesToMB(S.Cache.GpuBudgetBytes));

		if (S.CacheHitRate >= 0.0)
		{
			B.Appendf(TEXT("  Cache hit rate    %.1f%% (%lld hit, %lld missed)\n"),
				S.CacheHitRate * 100.0, S.CacheHits, S.CacheMisses);
		}
		else
		{
			// NOT "0%". No selection has been made, so there is no rate to report.
			B.Appendf(TEXT("  Cache hit rate    -- (no selection yet)\n"));
		}

		B.Appendf(TEXT("  Decodes in flight %d (%lld started, %lld done, %lld failed)\n"),
			S.LoadsInFlight, S.LoadsStarted, S.LoadsCompleted, S.LoadsFailed);
		B.Appendf(TEXT("  Evictions         %lld admitted, %lld evicted, %lld refused\n"),
			S.Cache.AdmitCount, S.Cache.EvictionCount, S.Cache.RefusalCount);

		return B.ToString();
	}
}
