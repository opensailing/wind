// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizSession.h"

#include "CFDViz/CFDVizManifest.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
// Directly, for FlowVizRayMarch::MaxClipPlanes: the overflow test below is
// written against the renderer's real limit rather than a literal 6, so it
// follows the shader if that ever changes.
#include "Render/FlowVizVolumeRayMarchShader.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * `.cfdvizsession` save and reload (plan.md section 14).
 *
 * A ROUND TRIP IS NOT THE TEST THAT MATTERS, AND THIS FILE IS BUILT AROUND
 * THAT. A save/load pair agree with each other just as happily on a field
 * NEITHER writes: drop `colorBands` from both the writer and the reader and
 * every round-trip assertion still passes, because the loaded state carries the
 * default the saved state also had. That is the exact shape of the defect this
 * feature is prone to - a control that silently does not persist.
 *
 * So the assertions come in two kinds:
 *
 *  1. AGAINST THE JSON TEXT. For every value plan.md section 14 names, the
 *     serialised text is searched for the key. A key that is not written cannot
 *     be blamed on the reader.
 *  2. AGAINST A CHANGED VALUE. Each field is set to something that is NOT its
 *     default before saving, and asserted to come back changed. A round trip of
 *     a default proves nothing.
 *
 * WHAT A MISSING CASE MUST DO. plan.md section 14 requires relinking, so a
 * session whose case has moved must still LOAD - everything but the case path
 * restored - with bCaseFound false. A load that failed here would make a session
 * unopenable on any machine with a different layout, which is the same defect as
 * storing an absolute path.
 */

namespace FlowVizSessionTest
{
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

	/** A state with every persisted field set to something that is NOT its default. */
	FFlowVizSessionState MakeNonDefaultState()
	{
		FFlowVizSessionState State;
		State.CasePath = GetSampleManifestPath();
		State.FieldId = FName(TEXT("pressure"));

		State.PhysicalTime = 0.35;
		State.Playback.Mode = EFlowVizPlaybackMode::RealTime;
		State.Playback.LoopMode = EFlowVizLoopMode::PingPong;
		State.Playback.Speed = 2.0;
		State.Playback.SequenceFrameRate = 12.0;
		State.Playback.OutputFrameRate = 60.0;
		State.Playback.bInterpolate = false;

		State.ColorMap = ECFDVizColorMap::Inferno;
		State.bReverseColorMap = true;
		State.ColorBands = 7;
		State.Component = EFlowVizComponentChoice::Y;
		State.RangeSource = EFlowVizRangeSource::Manual;
		State.RangeMin = -12.5f;
		State.RangeMax = 33.25f;
		State.Opacity.Points.Add(FFlowVizOpacityPoint(0.25f, 0.1f));
		State.Opacity.Points.Add(FFlowVizOpacityPoint(0.75f, 0.9f));
		State.Opacity.OpacityMultiplier = 0.6f;

		FFlowVizClipPlane Plane;
		Plane.Normal = FVector(0.0, 1.0, 0.0);
		Plane.Distance = -1.5;
		Plane.Label = TEXT("mid-y");
		Plane.bEnabled = true;
		State.ClipPlanes.Add(Plane);

		FFlowVizClipPlane Disabled;
		Disabled.Normal = FVector(0.0, 0.0, 1.0);
		Disabled.Distance = -0.25;
		Disabled.Label = TEXT("hidden");
		Disabled.bEnabled = false;
		State.ClipPlanes.Add(Disabled);

		State.bHasCropBox = true;
		State.CropMin = FVector(1.0, 0.5, 0.1);
		State.CropMax = FVector(10.0, 3.5, 0.9);

		State.bHasSlice = true;
		State.SliceOrigin = FVector(6.0, 2.0, 0.5);
		State.SliceNormal = FVector(1.0, 0.0, 0.0);
		State.SliceThickness = 0.25;
		State.SliceSlabSamples = 6;
		State.SliceSlabOp = EFlowVizSlabOp::Maximum;
		State.bSliceVisible = false;

		FFlowVizProbe Probe;
		Probe.Id = FGuid(0x11111111, 0x22222222, 0x33333333, 0x44444444);
		Probe.Name = TEXT("wake centre");
		Probe.SolverPosition = FVector(6.107142857142857, 2.071428571428571, 0.5833333333333333);
		Probe.bVisible = false;
		State.Probes.Add(Probe);

		State.bHasLineProbe = true;
		State.LineStart = FVector(1.0, 2.0, 0.5);
		State.LineEnd = FVector(11.0, 2.0, 0.5);
		State.LineSamples = 48;

		State.bPresentationMode = true;
		State.bHasCamera = true;
		State.CameraLocation = FVector(-500.0, 250.0, 120.0);
		State.CameraRotation = FRotator(-15.0, 45.0, 0.0);

		State.PanelVisibility.Add(TEXT("Timeline"), true);
		State.PanelVisibility.Add(TEXT("Probes"), false);

		State.Annotations.Add(TEXT("Vortex shedding begins around t = 0.3 s"));
		State.Annotations.Add(TEXT("Figure 4b"));

		/*
		 * EVERY RENDER SETTING NON-DEFAULT, through the view model's own
		 * setters (the fields are private, which is the point: a session state
		 * cannot hold a render settings value the setters would refuse).
		 * Defaults here would round-trip a writer that dropped the whole block
		 * -- degenerate-data-defeats-assertions.
		 */
		State.RenderSettings.SetCompositeMode(EFlowVizCompositeMode::IsoSurface);
		State.RenderSettings.SetIsoValue(0.4375f);
		State.RenderSettings.SetLightingEnabled(true);
		State.RenderSettings.SetAmbientStrength(0.2f);
		State.RenderSettings.SetDiffuseStrength(0.8f);
		State.RenderSettings.SetLightDirection(FVector3f(1.0f, 0.0f, 0.0f));
		State.RenderSettings.SetStepVoxels(0.375f);
		State.RenderSettings.SetReferenceStepVoxels(0.75f);
		State.RenderSettings.SetMaxSteps(640u);
		State.RenderSettings.SetEarlyTerminationAlpha(0.875f);
		State.RenderSettings.SetJitterEnabled(true);
		State.RenderSettings.SetJitterAmount(0.5f);
		State.RenderSettings.SetJitterSeed(42u);
		State.RenderSettings.SetFieldFilteringEnabled(false);
		State.RenderSettings.SetStrictStatusFilter(true);
		State.RenderSettings.SetNoDataColor(FLinearColor(1.0f, 0.0f, 1.0f, 1.0f));

		return State;
	}
}

/* ========================================================================== */
/* Every persisted value is actually written, and comes back CHANGED          */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizSessionRoundTripTest,
	"FlowViz.UI.Session.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizSessionRoundTripTest::RunTest(const FString& Parameters)
{
	const FFlowVizSessionState Saved = FlowVizSessionTest::MakeNonDefaultState();

	FString Json;
	TestTrue(TEXT("saving succeeds"),
		FlowVizSession::SaveToString(Saved, FString(), Json).IsOk());
	TestFalse(TEXT("the saved text is not empty"), Json.IsEmpty());

	/*
	 * THE TEXT ASSERTIONS. A key absent from the JSON cannot be restored, and no
	 * round trip will reveal it - both sides simply carry the default. Searching
	 * the serialised text is what makes "this control persists" checkable.
	 */
	{
		const TCHAR* RequiredKeys[] = {
			TEXT("\"format\""),
			TEXT("\"versionMajor\""),
			TEXT("\"case\""),
			TEXT("\"field\""),
			TEXT("\"physicalTime\""),
			TEXT("\"playback\""),
			TEXT("\"colorMap\""),
			TEXT("\"colorBands\""),
			TEXT("\"reverseColorMap\""),
			TEXT("\"component\""),
			TEXT("\"rangeSource\""),
			TEXT("\"rangeMin\""),
			TEXT("\"rangeMax\""),
			TEXT("\"opacity\""),
			TEXT("\"clipPlanes\""),
			TEXT("\"cropBox\""),
			TEXT("\"slice\""),
			TEXT("\"probes\""),
			TEXT("\"lineProbe\""),
			TEXT("\"presentationMode\""),
			TEXT("\"camera\""),
			TEXT("\"panelVisibility\""),
			TEXT("\"annotations\""),
			TEXT("\"renderSettings\""),
		};
		for (const TCHAR* Key : RequiredKeys)
		{
			TestTrue(FString::Printf(TEXT("the session writes %s"), Key), Json.Contains(Key));
		}
	}

	FFlowVizSessionState Loaded;
	TestTrue(TEXT("loading succeeds"),
		FlowVizSession::LoadFromString(Json, FString(), Loaded).IsOk());

	/* == Case and field ===================================================== */
	{
		TestEqual(TEXT("the field id survives"), Loaded.FieldId, Saved.FieldId);
		TestTrue(TEXT("the case is found, because the sample is on disk"), Loaded.bCaseFound);
		TestTrue(TEXT("and the resolved path points at a real manifest"),
			FPaths::FileExists(Loaded.ResolvedCasePath));
	}

	/* == Playback =========================================================== */
	{
		// EACH OF THESE IS NON-DEFAULT. Sequence/Loop/1.0 are the defaults, so a
		// writer that dropped the playback block entirely would fail here rather
		// than round-tripping its own omission.
		TestEqual(TEXT("the physical time survives"), Loaded.PhysicalTime, 0.35);
		TestEqual(TEXT("the playback mode survives"),
			Loaded.Playback.Mode, EFlowVizPlaybackMode::RealTime);
		TestEqual(TEXT("the loop mode survives"),
			Loaded.Playback.LoopMode, EFlowVizLoopMode::PingPong);
		TestEqual(TEXT("the speed survives"), Loaded.Playback.Speed, 2.0);
		TestEqual(TEXT("the sequence frame rate survives"),
			Loaded.Playback.SequenceFrameRate, 12.0);
		TestEqual(TEXT("the output frame rate survives"), Loaded.Playback.OutputFrameRate, 60.0);
		// bInterpolate defaults to TRUE, so saving false and getting true back is
		// exactly the "field silently not persisted" failure.
		TestFalse(TEXT("interpolation OFF survives - the default is on, so this "
					   "catches a dropped field that a default-valued test would not"),
			Loaded.Playback.bInterpolate);
	}

	/* == Colouring ========================================================== */
	{
		TestEqual(TEXT("the colormap survives"), Loaded.ColorMap, ECFDVizColorMap::Inferno);
		TestTrue(TEXT("colormap reversal survives"), Loaded.bReverseColorMap);
		TestEqual(TEXT("the band count survives"), Loaded.ColorBands, 7);
		TestEqual(TEXT("the component choice survives"),
			Loaded.Component, EFlowVizComponentChoice::Y);
		TestEqual(TEXT("the range source survives"),
			Loaded.RangeSource, EFlowVizRangeSource::Manual);
		TestEqual(TEXT("the range minimum survives"), Loaded.RangeMin, -12.5f);
		TestEqual(TEXT("the range maximum survives"), Loaded.RangeMax, 33.25f);

		// THE OPACITY CURVE IS THE SHAPE OF THE TRANSFER FUNCTION. A session that
		// restored the colormap but not the curve would reopen a figure that
		// looks different in exactly the way the author was tuning.
		TestEqual(TEXT("every opacity control point survives"),
			Loaded.Opacity.Points.Num(), 2);
		if (Loaded.Opacity.Points.Num() == 2)
		{
			TestEqual(TEXT("the first point's position survives"),
				Loaded.Opacity.Points[0].Position, 0.25f);
			TestEqual(TEXT("the first point's opacity survives"),
				Loaded.Opacity.Points[0].Opacity, 0.1f);
			TestEqual(TEXT("the second point's position survives"),
				Loaded.Opacity.Points[1].Position, 0.75f);
			TestEqual(TEXT("the second point's opacity survives"),
				Loaded.Opacity.Points[1].Opacity, 0.9f);
		}
		TestEqual(TEXT("the opacity multiplier survives"),
			Loaded.Opacity.OpacityMultiplier, 0.6f);
	}

	/* == Render settings (#76) ============================================== */
	{
		// A saved workspace used to reopen in Alpha, unlit: the session format
		// predated FFlowVizWorkspaceModel::RenderSettings, so a tuned MIP or
		// iso-surface figure silently lost its mode on reload. Every field is
		// non-default in the fixture, so a dropped key fails HERE rather than
		// round-tripping its own omission.
		const FFlowVizRenderSettingsViewModel& RS = Loaded.RenderSettings;
		TestEqual(TEXT("the composite mode survives"),
			RS.GetCompositeMode(), EFlowVizCompositeMode::IsoSurface);
		TestEqual(TEXT("the iso value survives"), RS.GetIsoValue(), 0.4375f);
		TestTrue(TEXT("lighting ON survives - the default is off, so this catches "
					  "a dropped field a default-valued fixture would not"),
			RS.IsLightingEnabled());
		TestEqual(TEXT("the ambient term survives"), RS.GetAmbientStrength(), 0.2f);
		TestEqual(TEXT("the diffuse term survives"), RS.GetDiffuseStrength(), 0.8f);
		TestTrue(TEXT("the light direction survives"),
			RS.GetLightDirection().Equals(FVector3f(1.0f, 0.0f, 0.0f), 1.0e-6f));
		TestEqual(TEXT("the step size survives"), RS.GetStepVoxels(), 0.375f);
		TestEqual(TEXT("the reference step survives"), RS.GetReferenceStepVoxels(), 0.75f);
		TestEqual(TEXT("the step ceiling survives"), RS.GetMaxSteps(), 640u);
		TestEqual(TEXT("the early-out alpha survives"),
			RS.GetEarlyTerminationAlpha(), 0.875f);
		TestTrue(TEXT("jitter ON survives"), RS.IsJitterEnabled());
		TestEqual(TEXT("the jitter amount survives"), RS.GetJitterAmount(), 0.5f);
		TestEqual(TEXT("the jitter seed survives"), RS.GetJitterSeed(), 42u);
		TestFalse(TEXT("field filtering OFF survives - the default is on"),
			RS.IsFieldFilteringEnabled());
		TestTrue(TEXT("the strict status filter survives"), RS.IsStrictStatusFilter());
		TestTrue(TEXT("the no-data colour survives"),
			RS.GetNoDataColor().Equals(FLinearColor(1.0f, 0.0f, 1.0f, 1.0f)));
	}

	/* == Clip planes and crop =============================================== */
	{
		TestEqual(TEXT("both clip planes survive"), Loaded.ClipPlanes.Num(), 2);
		if (Loaded.ClipPlanes.Num() == 2)
		{
			TestTrue(TEXT("the plane normal survives"),
				Loaded.ClipPlanes[0].Normal.Equals(FVector(0.0, 1.0, 0.0), 1.0e-9));
			// SIGN INCLUDED. A writer that stored |D| would reopen the session
			// clipping the other half of the domain.
			TestTrue(TEXT("the plane distance survives WITH ITS SIGN"),
				FMath::IsNearlyEqual(Loaded.ClipPlanes[0].Distance, -1.5, 1.0e-9));
			TestEqual(TEXT("the plane label survives"),
				Loaded.ClipPlanes[0].Label, FString(TEXT("mid-y")));
			TestTrue(TEXT("an enabled plane comes back enabled"),
				Loaded.ClipPlanes[0].bEnabled);
			// A DISABLED PLANE IS NOT A DELETED ONE. Persisting only enabled
			// planes would silently lose the user's hidden ones on reload.
			TestFalse(TEXT("a DISABLED plane is persisted and comes back disabled, "
						   "rather than being dropped on save"),
				Loaded.ClipPlanes[1].bEnabled);
			TestEqual(TEXT("and keeps its label"),
				Loaded.ClipPlanes[1].Label, FString(TEXT("hidden")));
		}

		TestTrue(TEXT("the crop box is marked present"), Loaded.bHasCropBox);
		TestTrue(TEXT("the crop minimum survives"),
			Loaded.CropMin.Equals(FVector(1.0, 0.5, 0.1), 1.0e-9));
		TestTrue(TEXT("the crop maximum survives"),
			Loaded.CropMax.Equals(FVector(10.0, 3.5, 0.9), 1.0e-9));
	}

	/* == Slice ============================================================== */
	{
		TestTrue(TEXT("the slice is marked present"), Loaded.bHasSlice);
		TestTrue(TEXT("the slice origin survives"),
			Loaded.SliceOrigin.Equals(FVector(6.0, 2.0, 0.5), 1.0e-9));
		TestTrue(TEXT("the slice normal survives"),
			Loaded.SliceNormal.Equals(FVector(1.0, 0.0, 0.0), 1.0e-9));
		TestTrue(TEXT("the slab thickness survives"),
			FMath::IsNearlyEqual(Loaded.SliceThickness, 0.25, 1.0e-9));
		TestEqual(TEXT("the slab sample count survives"), Loaded.SliceSlabSamples, 6);
		TestEqual(TEXT("the slab aggregation survives"),
			Loaded.SliceSlabOp, EFlowVizSlabOp::Maximum);
		TestFalse(TEXT("slice visibility OFF survives - the default is on"),
			Loaded.bSliceVisible);
	}

	/* == Probes ============================================================= */
	{
		TestEqual(TEXT("the probe survives"), Loaded.Probes.Num(), 1);
		if (Loaded.Probes.Num() == 1)
		{
			// THE ID IS WHAT A CHART SERIES IS KEYED BY. A regenerated GUID on
			// load would orphan every saved reference to this probe.
			TestEqual(TEXT("the probe's id survives, so a chart series stays keyed to it"),
				Loaded.Probes[0].Id, Saved.Probes[0].Id);
			TestEqual(TEXT("the probe's name survives"),
				Loaded.Probes[0].Name, FString(TEXT("wake centre")));
			/*
			 * SOLVER UNITS, AND FULL PRECISION. This position is a voxel centre
			 * on the sample grid - 6.107142857142857 m, a repeating fraction. A
			 * session that wrote it through a float, or in centimetres, would
			 * reopen the probe in a NEIGHBOURING CELL and report a different,
			 * entirely plausible velocity.
			 */
			TestTrue(TEXT("the probe's position survives in solver units at full "
						  "double precision - a float round trip would move it to "
						  "the edge of its cell"),
				Loaded.Probes[0].SolverPosition.Equals(
					Saved.Probes[0].SolverPosition, 1.0e-12));
			TestFalse(TEXT("probe visibility OFF survives - the default is on"),
				Loaded.Probes[0].bVisible);
		}

		TestTrue(TEXT("the line probe is marked present"), Loaded.bHasLineProbe);
		TestTrue(TEXT("the line start survives"),
			Loaded.LineStart.Equals(FVector(1.0, 2.0, 0.5), 1.0e-12));
		TestTrue(TEXT("the line end survives"),
			Loaded.LineEnd.Equals(FVector(11.0, 2.0, 0.5), 1.0e-12));
		TestEqual(TEXT("the line sample count survives"), Loaded.LineSamples, 48);
	}

	/* == Workspace ========================================================== */
	{
		TestTrue(TEXT("presentation mode survives - the default is scientific"),
			Loaded.bPresentationMode);

		TestTrue(TEXT("the camera is marked present"), Loaded.bHasCamera);
		TestTrue(TEXT("the camera location survives"),
			Loaded.CameraLocation.Equals(FVector(-500.0, 250.0, 120.0), 1.0e-6));
		TestTrue(TEXT("the camera rotation survives"),
			Loaded.CameraRotation.Equals(FRotator(-15.0, 45.0, 0.0), 1.0e-6));

		TestEqual(TEXT("both panels survive"), Loaded.PanelVisibility.Num(), 2);
		const bool* Timeline = Loaded.PanelVisibility.Find(TEXT("Timeline"));
		const bool* Probes = Loaded.PanelVisibility.Find(TEXT("Probes"));
		TestTrue(TEXT("the visible panel survives"), Timeline != nullptr && *Timeline);
		TestTrue(TEXT("the HIDDEN panel survives as hidden, rather than being omitted"),
			Probes != nullptr && !*Probes);

		TestEqual(TEXT("both annotations survive"), Loaded.Annotations.Num(), 2);
		if (Loaded.Annotations.Num() == 2)
		{
			// VERBATIM, including the order, which is the order they are shown in.
			TestEqual(TEXT("the first annotation is preserved verbatim"),
				Loaded.Annotations[0],
				FString(TEXT("Vortex shedding begins around t = 0.3 s")));
			TestEqual(TEXT("the second annotation is preserved verbatim, in order"),
				Loaded.Annotations[1], FString(TEXT("Figure 4b")));
		}
	}

	/* == EVERY enumerator round-trips, not just the one the fixture uses ===== */
	{
		/*
		 * WHY THIS IS SEPARATE FROM THE FIXTURE ABOVE.
		 *
		 * The fixture pins ONE value per enum (RealTime, PingPong, Y, Manual,
		 * Maximum). Enums are written as NAMES, and every reader falls back to a
		 * documented default for a name it does not recognise - so a typo in any
		 * enumerator the fixture does not happen to use is completely silent: it
		 * writes 'sequnce', reads back Sequence, and the session reopens in the
		 * wrong mode with no error and no failing test.
		 *
		 * The fallback is what makes the writer and the reader unable to disagree
		 * loudly, so totality has to be checked value by value. Each case saves a
		 * state carrying ONLY that enumerator and asserts it comes back identical.
		 */
		auto CheckPlayback = [this](EFlowVizPlaybackMode Mode, const TCHAR* Label)
		{
			FFlowVizSessionState Written;
			Written.Playback.Mode = Mode;
			FString EnumJson;
			FFlowVizSessionState Reloaded;
			if (!FlowVizSession::SaveToString(Written, FString(), EnumJson).IsOk()
				|| !FlowVizSession::LoadFromString(EnumJson, FString(), Reloaded).IsOk())
			{
				AddError(FString::Printf(TEXT("playback mode %s failed to round-trip at all"), Label));
				return;
			}
			TestEqual(
				FString::Printf(
					TEXT("playback mode %s survives; got %d back, wrote %d"),
					Label, static_cast<int32>(Reloaded.Playback.Mode), static_cast<int32>(Mode)),
				Reloaded.Playback.Mode, Mode);
		};
		CheckPlayback(EFlowVizPlaybackMode::Sequence, TEXT("Sequence"));
		CheckPlayback(EFlowVizPlaybackMode::RealTime, TEXT("RealTime"));
		CheckPlayback(EFlowVizPlaybackMode::FixedFps, TEXT("FixedFps"));

		auto CheckLoop = [this](EFlowVizLoopMode Mode, const TCHAR* Label)
		{
			FFlowVizSessionState Written;
			Written.Playback.LoopMode = Mode;
			FString EnumJson;
			FFlowVizSessionState Reloaded;
			if (!FlowVizSession::SaveToString(Written, FString(), EnumJson).IsOk()
				|| !FlowVizSession::LoadFromString(EnumJson, FString(), Reloaded).IsOk())
			{
				AddError(FString::Printf(TEXT("loop mode %s failed to round-trip at all"), Label));
				return;
			}
			TestEqual(
				FString::Printf(
					TEXT("loop mode %s survives; got %d back, wrote %d"),
					Label, static_cast<int32>(Reloaded.Playback.LoopMode), static_cast<int32>(Mode)),
				Reloaded.Playback.LoopMode, Mode);
		};
		CheckLoop(EFlowVizLoopMode::Once, TEXT("Once"));
		CheckLoop(EFlowVizLoopMode::Loop, TEXT("Loop"));
		CheckLoop(EFlowVizLoopMode::PingPong, TEXT("PingPong"));

		auto CheckComponent = [this](EFlowVizComponentChoice Component, const TCHAR* Label)
		{
			FFlowVizSessionState Written;
			Written.Component = Component;
			FString EnumJson;
			FFlowVizSessionState Reloaded;
			if (!FlowVizSession::SaveToString(Written, FString(), EnumJson).IsOk()
				|| !FlowVizSession::LoadFromString(EnumJson, FString(), Reloaded).IsOk())
			{
				AddError(FString::Printf(TEXT("component %s failed to round-trip at all"), Label));
				return;
			}
			TestEqual(
				FString::Printf(
					TEXT("component %s survives; got %d back, wrote %d"),
					Label, static_cast<int32>(Reloaded.Component), static_cast<int32>(Component)),
				Reloaded.Component, Component);
		};
		// X IS THE ONE THAT MATTERS MOST HERE: it is enumerator 0, so a writer that
		// emitted nothing at all for it would still "round-trip" through a
		// zero-initialised default. It is checked alongside the rest rather than
		// trusted.
		CheckComponent(EFlowVizComponentChoice::X, TEXT("X"));
		CheckComponent(EFlowVizComponentChoice::Y, TEXT("Y"));
		CheckComponent(EFlowVizComponentChoice::Z, TEXT("Z"));
		CheckComponent(EFlowVizComponentChoice::W, TEXT("W"));
		CheckComponent(EFlowVizComponentChoice::Magnitude, TEXT("Magnitude"));

		auto CheckRange = [this](EFlowVizRangeSource Source, const TCHAR* Label)
		{
			FFlowVizSessionState Written;
			Written.RangeSource = Source;
			FString EnumJson;
			FFlowVizSessionState Reloaded;
			if (!FlowVizSession::SaveToString(Written, FString(), EnumJson).IsOk()
				|| !FlowVizSession::LoadFromString(EnumJson, FString(), Reloaded).IsOk())
			{
				AddError(FString::Printf(TEXT("range source %s failed to round-trip at all"), Label));
				return;
			}
			TestEqual(
				FString::Printf(
					TEXT("range source %s survives; got %d back, wrote %d"),
					Label, static_cast<int32>(Reloaded.RangeSource), static_cast<int32>(Source)),
				Reloaded.RangeSource, Source);
		};
		CheckRange(EFlowVizRangeSource::Global, TEXT("Global"));
		CheckRange(EFlowVizRangeSource::CurrentFrame, TEXT("CurrentFrame"));
		CheckRange(EFlowVizRangeSource::Manual, TEXT("Manual"));

		auto CheckSlab = [this](EFlowVizSlabOp Op, const TCHAR* Label)
		{
			FFlowVizSessionState Written;
			// The slice block is only written when a slice is present.
			Written.bHasSlice = true;
			Written.SliceSlabOp = Op;
			FString EnumJson;
			FFlowVizSessionState Reloaded;
			if (!FlowVizSession::SaveToString(Written, FString(), EnumJson).IsOk()
				|| !FlowVizSession::LoadFromString(EnumJson, FString(), Reloaded).IsOk())
			{
				AddError(FString::Printf(TEXT("slab op %s failed to round-trip at all"), Label));
				return;
			}
			TestEqual(
				FString::Printf(
					TEXT("slab op %s survives; got %d back, wrote %d"),
					Label, static_cast<int32>(Reloaded.SliceSlabOp), static_cast<int32>(Op)),
				Reloaded.SliceSlabOp, Op);
		};
		CheckSlab(EFlowVizSlabOp::None, TEXT("None"));
		CheckSlab(EFlowVizSlabOp::Average, TEXT("Average"));
		CheckSlab(EFlowVizSlabOp::Minimum, TEXT("Minimum"));
		CheckSlab(EFlowVizSlabOp::Maximum, TEXT("Maximum"));

		/*
		 * AND THE FALLBACK ITSELF IS REACHABLE AND DOCUMENTED. An unrecognised name
		 * must land on the stated default rather than on whatever enumerator 0
		 * happens to be. Injected into the JSON text, because no writer produces
		 * this - it models a session written by a build with an enumerator this one
		 * does not have.
		 */
		{
			FFlowVizSessionState Written;
			Written.Playback.Mode = EFlowVizPlaybackMode::RealTime;
			FString EnumJson;
			TestTrue(TEXT("saving succeeds"),
				FlowVizSession::SaveToString(Written, FString(), EnumJson).IsOk());
			const FString Injected = EnumJson.Replace(TEXT("\"realTime\""), TEXT("\"warpDrive\""));
			TestNotEqual(TEXT("the injection actually changed the text, so the "
							  "assertion below is about the fallback and not about a "
							  "no-op edit"),
				Injected, EnumJson);

			FFlowVizSessionState Reloaded;
			TestTrue(TEXT("a session naming an unknown playback mode still LOADS - a "
						  "future enumerator must not make a file unopenable"),
				FlowVizSession::LoadFromString(Injected, FString(), Reloaded).IsOk());
			TestEqual(
				FString::Printf(
					TEXT("and 'warpDrive' falls back to Sequence (%d), not to whatever "
						 "enumerator 0 is; got %d"),
					static_cast<int32>(EFlowVizPlaybackMode::Sequence),
					static_cast<int32>(Reloaded.Playback.Mode)),
				Reloaded.Playback.Mode, EFlowVizPlaybackMode::Sequence);
		}
	}

	return true;
}

/* ========================================================================== */
/* Paths, relinking, and version handling                                     */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizSessionPathsTest,
	"FlowViz.UI.Session.Paths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizSessionPathsTest::RunTest(const FString& Parameters)
{
	const FString CaseDir = FlowVizSessionTest::GetSampleCaseDir();
	const FString ManifestPath = FlowVizSessionTest::GetSampleManifestPath();
	if (!FPaths::FileExists(ManifestPath))
	{
		AddError(FString::Printf(
			TEXT("the sample case is required for this test and is missing at '%s'"),
			*ManifestPath));
		return false;
	}

	/* == A RELATIVE case path when the session sits beside the case ========== */
	{
		FFlowVizSessionState State;
		State.CasePath = ManifestPath;
		State.FieldId = FName(TEXT("U"));

		// The session is written to the directory ABOVE the case, so the case is
		// reachable from it by a relative path.
		const FString SessionDir = FPaths::GetPath(CaseDir);

		FString Json;
		TestTrue(TEXT("saving succeeds"),
			FlowVizSession::SaveToString(State, SessionDir, Json).IsOk());

		/*
		 * plan.md section 14: "Use relative case paths when possible". An
		 * absolute path makes the session unopenable on any other machine - and
		 * unopenable in a way that looks like the case is missing, so the user is
		 * told to relink a case that is sitting right there.
		 *
		 * The assertion is that the ABSOLUTE path is absent, not merely that a
		 * relative one is present: a writer that emitted both would pass the
		 * weaker check.
		 */
		TestFalse(TEXT("the absolute case path is NOT written when a relative one works"),
			Json.Contains(ManifestPath));
		TestTrue(TEXT("a relative case path is written instead"),
			Json.Contains(TEXT("MockCylinderWake.cfdviz")));

		FFlowVizSessionState Loaded;
		TestTrue(TEXT("loading succeeds"),
			FlowVizSession::LoadFromString(Json, SessionDir, Loaded).IsOk());
		TestTrue(TEXT("and the relative path resolves back to the real case"),
			Loaded.bCaseFound);
		TestTrue(TEXT("the resolved path names a file that exists"),
			FPaths::FileExists(Loaded.ResolvedCasePath));
	}

	/* == A MISSING case still loads, so the user can relink ================== */
	{
		FFlowVizSessionState State;
		State.CasePath = TEXT("/nowhere/at/all/manifest.json");
		State.FieldId = FName(TEXT("pressure"));
		State.ColorBands = 5;
		State.Annotations.Add(TEXT("keep me"));

		FString Json;
		TestTrue(TEXT("saving succeeds"),
			FlowVizSession::SaveToString(State, FString(), Json).IsOk());

		FFlowVizSessionState Loaded;
		/*
		 * NOT A FAILURE. plan.md section 14 requires the relink path to exist, and
		 * a load that failed here would leave the user with an unopenable file
		 * and no way to point it at the moved case.
		 */
		const FCFDVizResult Load = FlowVizSession::LoadFromString(Json, FString(), Loaded);
		TestTrue(TEXT("a session whose case has moved still LOADS"), Load.IsOk());
		TestFalse(TEXT("but reports the case as not found, which is what a relink "
					   "prompt is keyed on"),
			Loaded.bCaseFound);
		TestFalse(TEXT("and names what it looked for, so the prompt can say so"),
			Loaded.ResolvedCasePath.IsEmpty());

		// THE REST OF THE SCENE SURVIVES. Losing the colouring and annotations
		// because the case moved would make relinking pointless.
		TestEqual(TEXT("the rest of the scene is restored despite the missing case"),
			Loaded.ColorBands, 5);
		TestEqual(TEXT("including the annotations"), Loaded.Annotations.Num(), 1);

		/* Relinking points it at a real case. */
		TestTrue(TEXT("relinking to the real case succeeds"),
			FlowVizSession::RelinkCase(Loaded, ManifestPath).IsOk());
		TestTrue(TEXT("and the case is now found"), Loaded.bCaseFound);
		TestTrue(TEXT("with a resolved path that exists"),
			FPaths::FileExists(Loaded.ResolvedCasePath));

		// A MISTAKEN RELINK MUST NOT LOSE THE ORIGINAL. Refusing and leaving the
		// state alone is what lets the user try again.
		const FString GoodPath = Loaded.ResolvedCasePath;
		TestFalse(TEXT("relinking to a path that does not exist is refused"),
			FlowVizSession::RelinkCase(Loaded, TEXT("/still/nowhere/manifest.json")).IsOk());
		TestEqual(TEXT("and a refused relink leaves the good path in place"),
			Loaded.ResolvedCasePath, GoodPath);
		TestTrue(TEXT("and the case is still found"), Loaded.bCaseFound);
	}

	/* == Version handling =================================================== */
	{
		FFlowVizSessionState State;
		State.FieldId = FName(TEXT("U"));
		FString Json;
		TestTrue(TEXT("saving succeeds"),
			FlowVizSession::SaveToString(State, FString(), Json).IsOk());

		// A NEWER MINOR IS ACCEPTED and its unknown keys ignored, so a session
		// written by a newer build still opens.
		{
			FString Forward = Json;
			TestTrue(TEXT("the minor version is present to bump"),
				Forward.Contains(TEXT("\"versionMinor\"")));
			Forward = Forward.Replace(TEXT("\"versionMinor\": 0"), TEXT("\"versionMinor\": 99"));
			Forward = Forward.Replace(
				TEXT("\"annotations\""), TEXT("\"someFutureKey\": 42,\n\t\"annotations\""));

			FFlowVizSessionState Loaded;
			TestTrue(TEXT("a newer MINOR version loads"),
				FlowVizSession::LoadFromString(Forward, FString(), Loaded).IsOk());
			TestEqual(TEXT("and the keys it does understand still load"),
				Loaded.FieldId, FName(TEXT("U")));
		}

		// AN OLDER SESSION WITHOUT renderSettings LOADS WITH DEFAULTS. Every
		// v1.0 file on disk predates #76; refusing them, or half-reading them,
		// would break every session saved before the key existed.
		{
			FString Old = Json;
			TestTrue(TEXT("CONTROL: the current writer emits renderSettings"),
				Old.Contains(TEXT("\"renderSettings\"")));
			const int32 KeyAt = Old.Find(TEXT("\"renderSettings\""));
			// Excise the whole object: from the key to the matching close brace.
			int32 Depth = 0, End = KeyAt;
			for (int32 i = KeyAt; i < Old.Len(); ++i)
			{
				if (Old[i] == TEXT('{')) { ++Depth; }
				if (Old[i] == TEXT('}') && --Depth == 0) { End = i; break; }
			}
			// Also swallow the trailing comma-newline the writer puts after it.
			int32 Tail = End + 1;
			while (Tail < Old.Len() && (Old[Tail] == TEXT(',') || FChar::IsWhitespace(Old[Tail])))
			{
				++Tail;
			}
			Old = Old.Left(KeyAt) + Old.Mid(Tail);
			TestFalse(TEXT("CONTROL: the excision removed the key"),
				Old.Contains(TEXT("\"renderSettings\"")));

			FFlowVizSessionState Loaded;
			TestTrue(TEXT("a session saved before renderSettings existed still loads"),
				FlowVizSession::LoadFromString(Old, FString(), Loaded).IsOk());
			TestEqual(TEXT("and its render settings are the defaults, not garbage"),
				Loaded.RenderSettings.GetCompositeMode(), EFlowVizCompositeMode::Alpha);
			TestFalse(TEXT("unlit, as a fresh workspace is"),
				Loaded.RenderSettings.IsLightingEnabled());
		}

		// A NEWER MAJOR IS REFUSED. A major bump means a key changed MEANING, so
		// reading it anyway restores a plausible wrong scene - worse than not
		// opening, because nothing looks broken.
		{
			FString Future = Json;
			Future = Future.Replace(TEXT("\"versionMajor\": 1"), TEXT("\"versionMajor\": 2"));
			FFlowVizSessionState Loaded;
			TestFalse(TEXT("a newer MAJOR version is refused rather than restoring a "
						   "plausible wrong scene"),
				FlowVizSession::LoadFromString(Future, FString(), Loaded).IsOk());
		}

		// Malformed input and a wrong format are failures.
		{
			FFlowVizSessionState Loaded;
			TestFalse(TEXT("malformed JSON is refused"),
				FlowVizSession::LoadFromString(TEXT("{not json"), FString(), Loaded).IsOk());
			TestFalse(TEXT("empty text is refused"),
				FlowVizSession::LoadFromString(TEXT(""), FString(), Loaded).IsOk());
			TestFalse(TEXT("valid JSON that is not a session is refused"),
				FlowVizSession::LoadFromString(
					TEXT("{\"format\":\"something-else\",\"versionMajor\":1}"),
					FString(), Loaded).IsOk());
		}
	}

	/* == Save and load through a real file ================================== */
	{
		const FFlowVizSessionState Saved = FlowVizSessionTest::MakeNonDefaultState();
		const FString FilePath = FPaths::Combine(
			FPaths::AutomationTransientDir(),
			FString::Printf(TEXT("FlowVizSessionTest.%s"), FlowVizSession::GetFileExtension()));

		IFileManager::Get().Delete(*FilePath);
		TestTrue(TEXT("saving to a file succeeds"),
			FlowVizSession::SaveToFile(Saved, FilePath).IsOk());
		TestTrue(TEXT("the file exists on disk"), FPaths::FileExists(FilePath));

		FFlowVizSessionState Loaded;
		TestTrue(TEXT("loading from that file succeeds"),
			FlowVizSession::LoadFromFile(FilePath, Loaded).IsOk());
		TestEqual(TEXT("and the scene came back"), Loaded.ColorBands, 7);
		TestEqual(TEXT("with its probes"), Loaded.Probes.Num(), 1);

		// A MISSING SESSION IS A DIFFERENT FAILURE FROM A MISSING CASE. One is an
		// error; the other is a relink prompt.
		FFlowVizSessionState Missing;
		const FCFDVizResult NotFound = FlowVizSession::LoadFromFile(
			FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("no-such.cfdvizsession")),
			Missing);
		TestFalse(TEXT("loading a session file that does not exist fails"), NotFound.IsOk());
		TestEqual(TEXT("and says so specifically, rather than reporting a parse error"),
			NotFound.Error, ECFDVizError::FileNotFound);

		IFileManager::Get().Delete(*FilePath);
	}

	return true;
}

/* ========================================================================== */
/* Capture from and apply to the live view models                             */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizSessionViewModelsTest,
	"FlowViz.UI.Session.ViewModels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizSessionViewModelsTest::RunTest(const FString& Parameters)
{
	const FVector Domain(12.0, 4.0, 1.0);

	/* == Capture reads the live view models, not defaults =================== */
	{
		FFlowVizClipViewModel Clip;
		TestTrue(TEXT("the domain is accepted"), Clip.SetDomainSize(Domain).IsOk());
		FFlowVizClipPlane Plane;
		Plane.Normal = FVector(0.0, 1.0, 0.0);
		Plane.Distance = -1.5;
		TestTrue(TEXT("a plane is accepted"), Clip.AddPlane(Plane).IsOk());
		TestTrue(TEXT("a crop is accepted"),
			Clip.SetCropBox(FVector(1.0, 0.5, 0.1), FVector(10.0, 3.5, 0.9)).IsOk());

		FFlowVizSliceViewModel Slice;
		TestTrue(TEXT("the domain is accepted"), Slice.SetDomainSize(Domain).IsOk());
		TestTrue(TEXT("a normal is accepted"), Slice.SetNormal(FVector(1.0, 0.0, 0.0)).IsOk());
		TestTrue(TEXT("an origin is accepted"), Slice.SetOrigin(FVector(6.0, 2.0, 0.5)).IsOk());
		TestTrue(TEXT("a thickness is accepted"), Slice.SetThickness(0.25).IsOk());

		FFlowVizProbeViewModel Probes;
		const FGuid ProbeId = Probes.AddProbeAtSolverPosition(
			FVector(6.107142857142857, 2.071428571428571, 0.5833333333333333), TEXT("wake"));
		TestTrue(TEXT("the probe was placed"), ProbeId.IsValid());
		TestTrue(TEXT("a line probe is accepted"),
			Probes.SetLineProbe(FVector(1.0, 2.0, 0.5), FVector(11.0, 2.0, 0.5)).IsOk());
		TestTrue(TEXT("a line sample count is accepted"), Probes.SetLineSampleCount(48).IsOk());

		FFlowVizSessionState State;
		FlowVizSession::CaptureFromViewModels(
			nullptr, nullptr, &Clip, &Slice, &Probes, nullptr, State);

		TestEqual(TEXT("the live clip plane is captured"), State.ClipPlanes.Num(), 1);
		if (State.ClipPlanes.Num() == 1)
		{
			TestTrue(TEXT("with its distance and sign"),
				FMath::IsNearlyEqual(State.ClipPlanes[0].Distance, -1.5, 1.0e-9));
		}
		TestTrue(TEXT("the live crop is captured"), State.bHasCropBox);
		TestTrue(TEXT("the live slice is captured"), State.bHasSlice);
		TestTrue(TEXT("with its thickness"),
			FMath::IsNearlyEqual(State.SliceThickness, 0.25, 1.0e-9));
		TestEqual(TEXT("the live probe is captured"), State.Probes.Num(), 1);
		if (State.Probes.Num() == 1)
		{
			TestEqual(TEXT("with the id the live view model gave it"),
				State.Probes[0].Id, ProbeId);
		}
		TestTrue(TEXT("the live line probe is captured"), State.bHasLineProbe);
		TestEqual(TEXT("with its sample count"), State.LineSamples, 48);
	}

	/* == A nullptr view model contributes NOTHING =========================== */
	{
		// A workspace with no slice must not get a default slice materialising out
		// of nowhere on the next reload.
		FFlowVizSessionState State;
		FlowVizSession::CaptureFromViewModels(
			nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, State);
		TestFalse(TEXT("no slice view model means no slice was captured"), State.bHasSlice);
		TestFalse(TEXT("no clip view model means no crop was captured"), State.bHasCropBox);
		TestEqual(TEXT("and no clip planes"), State.ClipPlanes.Num(), 0);
		TestEqual(TEXT("and no probes"), State.Probes.Num(), 0);
		TestFalse(TEXT("and no line probe"), State.bHasLineProbe);
	}

	/* == Apply pushes a loaded session back into live view models ============ */
	{
		const FFlowVizSessionState State = FlowVizSessionTest::MakeNonDefaultState();

		FFlowVizClipViewModel Clip;
		TestTrue(TEXT("the domain is accepted"), Clip.SetDomainSize(Domain).IsOk());
		FFlowVizSliceViewModel Slice;
		TestTrue(TEXT("the domain is accepted"), Slice.SetDomainSize(Domain).IsOk());
		FFlowVizProbeViewModel Probes;

		TestTrue(TEXT("applying the session succeeds"),
			FlowVizSession::ApplyToViewModels(
				State, nullptr, nullptr, &Clip, &Slice, &Probes).IsOk());

		/*
		 * THE ASSERTION IS AGAINST BEHAVIOUR, NOT AGAINST STORAGE. A restored
		 * clip plane must clip the same half-space it clipped before the save.
		 * Comparing the stored Distance would pass even if the sign convention
		 * were inverted somewhere in the pipeline.
		 */
		TestEqual(TEXT("both planes are restored, disabled one included"),
			Clip.GetPlaneCount(), 2);
		TestEqual(TEXT("but only the enabled one is enabled"), Clip.GetEnabledPlaneCount(), 1);
		// The plane is +Y at y = 1.5, so y = 3 is kept and y = 0.5 is dropped.
		TestTrue(TEXT("the restored plane keeps the half it kept before the save"),
			Clip.KeepsPoint(FVector(6.0, 3.0, 0.5)));
		TestFalse(TEXT("and drops the half it dropped"),
			Clip.KeepsPoint(FVector(6.0, 0.5, 0.5)));

		TestTrue(TEXT("the crop is restored"),
			Clip.GetCropMin().Equals(FVector(1.0, 0.5, 0.1), 1.0e-9));

		TestTrue(TEXT("the slice origin is restored"),
			Slice.GetOrigin().Equals(FVector(6.0, 2.0, 0.5), 1.0e-9));
		TestTrue(TEXT("the slice normal is restored"),
			Slice.GetNormal().Equals(FVector(1.0, 0.0, 0.0), 1.0e-9));
		TestTrue(TEXT("the slab thickness is restored"),
			FMath::IsNearlyEqual(Slice.GetThickness(), 0.25, 1.0e-9));
		TestEqual(TEXT("the slab aggregation is restored"),
			Slice.GetSlabOp(), EFlowVizSlabOp::Maximum);
		TestEqual(TEXT("the slab sample count is restored"), Slice.GetSlabSamples(), 6);

		TestEqual(TEXT("the probe is restored"), Probes.GetProbeCount(), 1);
		if (Probes.GetProbeCount() == 1)
		{
			// The id must survive the whole save/load/apply path, or a chart's
			// saved series references break on reload.
			TestEqual(TEXT("with the id it was saved under"),
				Probes.GetProbes()[0].Id, State.Probes[0].Id);
			TestTrue(TEXT("and its full-precision solver position"),
				Probes.GetProbes()[0].SolverPosition.Equals(
					State.Probes[0].SolverPosition, 1.0e-12));
		}
		TestTrue(TEXT("the line probe is restored"), Probes.HasLineProbe());
		TestEqual(TEXT("with its sample count"), Probes.GetLineSampleCount(), 48);
	}

	/* == Apply is BEST EFFORT: one bad value does not discard the scene ====== */
	{
		FFlowVizSessionState Bad = FlowVizSessionTest::MakeNonDefaultState();
		// A speed of zero is illegal; the clip planes beside it are fine.
		Bad.Playback.Speed = 0.0;

		FFlowVizCasePlayer Player;
		FFlowVizClipViewModel Clip;
		TestTrue(TEXT("the domain is accepted"), Clip.SetDomainSize(Domain).IsOk());

		const FCFDVizResult Result =
			FlowVizSession::ApplyToViewModels(Bad, &Player, nullptr, &Clip, nullptr, nullptr);

		// THE FAILURE IS REPORTED - a silent best-effort would hide a corrupt
		// session behind a scene that looks almost right.
		TestFalse(TEXT("an illegal playback speed is reported"), Result.IsOk());
		// AND THE REST STILL APPLIED. Discarding a whole scene over one bad
		// number is what makes a session file fragile.
		TestEqual(TEXT("but the clip planes still applied - one bad number does not "
					   "discard the rest of the scene"),
			Clip.GetPlaneCount(), 2);
	}

	/* == A round trip through capture, save, load and apply ================== */
	{
		FFlowVizClipViewModel Original;
		TestTrue(TEXT("the domain is accepted"), Original.SetDomainSize(Domain).IsOk());
		TestTrue(TEXT("a preset is accepted"),
			Original.AddPresetPlane(EFlowVizClipPreset::KeepPlusZ).IsOk());

		FFlowVizSessionState Captured;
		FlowVizSession::CaptureFromViewModels(
			nullptr, nullptr, &Original, nullptr, nullptr, nullptr, Captured);

		FString Json;
		TestTrue(TEXT("saving succeeds"),
			FlowVizSession::SaveToString(Captured, FString(), Json).IsOk());

		FFlowVizSessionState Loaded;
		TestTrue(TEXT("loading succeeds"),
			FlowVizSession::LoadFromString(Json, FString(), Loaded).IsOk());

		FFlowVizClipViewModel Restored;
		TestTrue(TEXT("the domain is accepted"), Restored.SetDomainSize(Domain).IsOk());
		TestTrue(TEXT("applying succeeds"),
			FlowVizSession::ApplyToViewModels(
				Loaded, nullptr, nullptr, &Restored, nullptr, nullptr).IsOk());

		/*
		 * THE WHOLE PATH, CHECKED BY BEHAVIOUR. A KeepPlusZ preset on a 1 m tall
		 * domain keeps z > 0.5 and drops z < 0.5. If ANY stage of
		 * capture -> save -> load -> apply inverted the sign or lost the offset,
		 * these two assertions disagree - and one of them alone would not be
		 * enough, since a plane that keeps everything passes the first.
		 */
		TestTrue(TEXT("after a full capture/save/load/apply the plane keeps the same half"),
			Restored.KeepsPoint(FVector(6.0, 2.0, 0.9)));
		TestFalse(TEXT("and drops the same half"),
			Restored.KeepsPoint(FVector(6.0, 2.0, 0.1)));
	}

	/* == TWO probes, DISTINCT visibility: the per-probe flag follows the id === */
	{
		/*
		 * WHY TWO PROBES AND WHY DIFFERENT FLAGS.
		 *
		 * Restoring a probe mints nothing: it must land under the id from the file,
		 * because a chart series is keyed by that id. The failure this guards is
		 * silent - if apply added under a FRESH id and then set visibility by the
		 * SAVED id, SetProbeVisible would return false for an id nobody has and
		 * every probe would restore with its default (visible), with nothing
		 * logged and no failed result.
		 *
		 * One probe would not catch it: a default of `true` matches a saved `true`
		 * by luck. Asserting only the count would not catch it either. TWO probes
		 * with OPPOSITE flags means no single default can satisfy both, and the
		 * flags must be matched to the right probe BY ID rather than by position
		 * in the array.
		 */
		FFlowVizSessionState State;

		FFlowVizProbe Visible;
		Visible.Id = FGuid(0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC, 0xDDDDDDDD);
		Visible.Name = TEXT("inlet");
		Visible.SolverPosition = FVector(1.0, 2.0, 0.5);
		Visible.bVisible = true;
		State.Probes.Add(Visible);

		FFlowVizProbe Hidden;
		Hidden.Id = FGuid(0x01010101, 0x02020202, 0x03030303, 0x04040404);
		Hidden.Name = TEXT("outlet");
		Hidden.SolverPosition = FVector(11.0, 2.0, 0.5);
		Hidden.bVisible = false;
		State.Probes.Add(Hidden);

		FString Json;
		TestTrue(TEXT("saving two probes succeeds"),
			FlowVizSession::SaveToString(State, FString(), Json).IsOk());

		FFlowVizSessionState Loaded;
		TestTrue(TEXT("loading two probes succeeds"),
			FlowVizSession::LoadFromString(Json, FString(), Loaded).IsOk());

		FFlowVizProbeViewModel Probes;
		TestTrue(TEXT("applying two probes succeeds"),
			FlowVizSession::ApplyToViewModels(
				Loaded, nullptr, nullptr, nullptr, nullptr, &Probes).IsOk());

		TestEqual(TEXT("both probes are restored"), Probes.GetProbeCount(), 2);

		// LOOKED UP BY THE SAVED ID, not by array position: that is the thing under
		// test. FindProbe returning null IS the failure, so it is reported with the
		// id that was not found rather than as a null dereference.
		const FFlowVizProbe* RestoredVisible = Probes.FindProbe(Visible.Id);
		const FFlowVizProbe* RestoredHidden = Probes.FindProbe(Hidden.Id);

		if (RestoredVisible == nullptr)
		{
			AddError(FString::Printf(
				TEXT("no probe was restored under the saved id %s ('inlet'); the ids "
					 "present are: %s"),
				*Visible.Id.ToString(EGuidFormats::DigitsWithHyphens),
				*FString::JoinBy(Probes.GetProbes(), TEXT(", "),
					[](const FFlowVizProbe& P)
					{
						return P.Id.ToString(EGuidFormats::DigitsWithHyphens);
					})));
		}
		else
		{
			TestEqual(
				FString::Printf(
					TEXT("the probe restored under id %s keeps its name"),
					*Visible.Id.ToString(EGuidFormats::DigitsWithHyphens)),
				RestoredVisible->Name, FString(TEXT("inlet")));
			TestTrue(
				FString::Printf(
					TEXT("probe 'inlet' (id %s) restores VISIBLE, as saved"),
					*Visible.Id.ToString(EGuidFormats::DigitsWithHyphens)),
				RestoredVisible->bVisible);
		}

		if (RestoredHidden == nullptr)
		{
			AddError(FString::Printf(
				TEXT("no probe was restored under the saved id %s ('outlet'); the ids "
					 "present are: %s"),
				*Hidden.Id.ToString(EGuidFormats::DigitsWithHyphens),
				*FString::JoinBy(Probes.GetProbes(), TEXT(", "),
					[](const FFlowVizProbe& P)
					{
						return P.Id.ToString(EGuidFormats::DigitsWithHyphens);
					})));
		}
		else
		{
			TestEqual(
				FString::Printf(
					TEXT("the probe restored under id %s keeps its name"),
					*Hidden.Id.ToString(EGuidFormats::DigitsWithHyphens)),
				RestoredHidden->Name, FString(TEXT("outlet")));
			// THE ASSERTION THAT CANNOT PASS BY DEFAULT. bVisible defaults to true,
			// so this is false only if the saved flag reached the right probe.
			TestFalse(
				FString::Printf(
					TEXT("probe 'outlet' (id %s) restores HIDDEN - the default is "
						 "visible, so this fails if the flag was applied to a "
						 "different probe or to an id nobody has"),
					*Hidden.Id.ToString(EGuidFormats::DigitsWithHyphens)),
				RestoredHidden->bVisible);
		}

		/*
		 * AND THE FLAGS ARE NOT MERELY BOTH-PRESENT: they are attached to the right
		 * POSITIONS. A restore that paired inlet's flag with outlet's coordinates
		 * would satisfy everything above.
		 */
		if (RestoredVisible != nullptr && RestoredHidden != nullptr)
		{
			TestTrue(
				FString::Printf(
					TEXT("the visible probe is the one at x = 1 (got %g)"),
					RestoredVisible->SolverPosition.X),
				RestoredVisible->SolverPosition.Equals(FVector(1.0, 2.0, 0.5), 1.0e-12));
			TestTrue(
				FString::Printf(
					TEXT("the hidden probe is the one at x = 11 (got %g)"),
					RestoredHidden->SolverPosition.X),
				RestoredHidden->SolverPosition.Equals(FVector(11.0, 2.0, 0.5), 1.0e-12));
		}
	}

	/* == A REFUSED plane must not disable the plane before it ================ */
	{
		/*
		 * THE FAULT THIS PINS. Apply used to disable by `GetPlaneCount() - 1`
		 * straight after AddPlane, without checking that the add succeeded. The
		 * renderer takes six planes, so a seventh is REFUSED and the list does not
		 * grow - leaving that index pointing at the SIXTH plane. A seventh plane
		 * saved as disabled therefore switched off a legitimate neighbour.
		 *
		 * IT IS INVISIBLE IN THE RESULT CODE. The disable call itself succeeds, so
		 * the only failure reported is the one for the seventh plane, which a
		 * reader would attribute entirely to the plane that was dropped. The
		 * symptom is a plane that silently stops clipping across a save/reload.
		 *
		 * The session state is built DIRECTLY rather than captured from a view
		 * model, because the view model refuses to hold seven planes - which is
		 * exactly why a file can contain them and apply has to cope.
		 */
		FFlowVizSessionState State;
		for (int32 Index = 0; Index < FlowVizRayMarch::MaxClipPlanes + 1; ++Index)
		{
			FFlowVizClipPlane Plane;
			Plane.Normal = FVector(1.0, 0.0, 0.0);
			// Distinct offsets so the planes are told apart by behaviour below.
			Plane.Distance = -1.0 * static_cast<double>(Index);
			Plane.Label = FString::Printf(TEXT("plane %d"), Index);
			// EVERY plane enabled except the last, which is the one that cannot fit.
			Plane.bEnabled = (Index != FlowVizRayMarch::MaxClipPlanes);
			State.ClipPlanes.Add(Plane);
		}

		FFlowVizClipViewModel Clip;
		TestTrue(TEXT("the domain is accepted"), Clip.SetDomainSize(Domain).IsOk());

		const FCFDVizResult Result =
			FlowVizSession::ApplyToViewModels(State, nullptr, nullptr, &Clip, nullptr, nullptr);

		// The seventh plane cannot be restored, and that IS reported.
		TestFalse(
			FString::Printf(
				TEXT("applying %d planes to a renderer that takes %d reports the overflow"),
				State.ClipPlanes.Num(), FlowVizRayMarch::MaxClipPlanes),
			Result.IsOk());
		TestEqual(
			FString::Printf(
				TEXT("exactly %d planes were admitted"), FlowVizRayMarch::MaxClipPlanes),
			Clip.GetPlaneCount(), FlowVizRayMarch::MaxClipPlanes);

		/*
		 * THE ASSERTION THAT FAILS ON THE OLD CODE. All six admitted planes were
		 * saved ENABLED; only the refused seventh was disabled. If the disable
		 * landed on the last admitted plane instead, this count is 5.
		 */
		TestEqual(
			FString::Printf(
				TEXT("all %d admitted planes stay ENABLED - a refused seventh plane "
					 "must not disable the sixth"),
				FlowVizRayMarch::MaxClipPlanes),
			Clip.GetEnabledPlaneCount(), FlowVizRayMarch::MaxClipPlanes);
	}

	/* == A duplicate id is refused rather than silently shadowing ============ */
	{
		// Restoring is the one path that takes an id from OUTSIDE, so it is the one
		// path that can be handed a duplicate. Two probes under one id would make
		// FindProbe - and therefore every mutator - return whichever came first.
		FFlowVizProbeViewModel Probes;
		const FGuid Id(0x99999999, 0x88888888, 0x77777777, 0x66666666);

		TestTrue(TEXT("the first restore under a fresh id succeeds"),
			Probes.RestoreProbe(Id, FVector(1.0, 2.0, 0.5), TEXT("first")).IsOk());
		TestFalse(
			FString::Printf(
				TEXT("a second restore under the SAME id %s is refused"),
				*Id.ToString(EGuidFormats::DigitsWithHyphens)),
			Probes.RestoreProbe(Id, FVector(9.0, 2.0, 0.5), TEXT("second")).IsOk());
		TestEqual(TEXT("and the refusal did not add a probe"), Probes.GetProbeCount(), 1);

		const FFlowVizProbe* Kept = Probes.FindProbe(Id);
		if (Kept != nullptr)
		{
			// THE FIRST ONE SURVIVES INTACT - the refusal is not a half-overwrite.
			TestEqual(TEXT("the original probe is untouched by the refused restore"),
				Kept->Name, FString(TEXT("first")));
		}

		TestFalse(TEXT("an invalid id is refused - it could never be looked up again"),
			Probes.RestoreProbe(FGuid(), FVector(1.0, 2.0, 0.5), TEXT("no id")).IsOk());
		TestFalse(TEXT("a non-finite position is refused"),
			Probes.RestoreProbe(
				FGuid::NewGuid(),
				FVector(std::numeric_limits<double>::quiet_NaN(), 2.0, 0.5),
				TEXT("nan")).IsOk());
		TestEqual(TEXT("neither refusal added a probe"), Probes.GetProbeCount(), 1);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
