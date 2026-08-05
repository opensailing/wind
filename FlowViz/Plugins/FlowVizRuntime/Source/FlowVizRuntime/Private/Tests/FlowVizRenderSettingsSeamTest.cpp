// Copyright FlowViz contributors. All Rights Reserved.

#include "../Render/FlowVizVolumeRayMarchDispatcher.h"

#include "Misc/AutomationTest.h"
#include "Render/FlowVizVolumeRayMarchShader.h"
#include "Scene/FlowVizVolumeComponent.h"
#include "UI/FlowVizRenderSettingsViewModel.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * DOES A RENDER SETTING CHOSEN OUTSIDE THE RENDERER REACH THE REQUEST?
 *
 * FFlowVizRenderSettingsViewModel can express all six composite modes and it is
 * covered by FlowViz.UI.RenderSettings, which passes. That test writes the view
 * model itself and asserts on the parameters the view model produces, so it is
 * structurally incapable of noticing that NOTHING CONSTRUCTS ONE. When the view
 * model was first added, check_frozen_params went green purely because a
 * production file now contained writes to the sixteen frozen names - and the
 * shipped renderer still had no way to select anything. A writer nobody reaches
 * leaves the parameter exactly as welded as before.
 *
 * That is the gap this test exists for, and it is why the assertion is made at
 * the DISPATCHER, not at the view model. The question is not "can the view model
 * produce IsoSurface" - that is already answered - but "if a caller asks the
 * scene for IsoSurface, does the request that reaches the GPU say IsoSurface".
 * The only way to fail this test is for the seam between them to be missing or
 * broken, which is exactly the defect.
 *
 * THE IDENTITY CONTROL RUNS FIRST. A context carrying default settings must
 * produce the same parameters as no settings at all. Without it, a seam that
 * clobbered the geometry, format or camera rows on its way through would still
 * satisfy every composite-mode assertion below, and the render would be black
 * for a reason no assertion here names. It also pins the defaults themselves:
 * jitter off (ADR 002) and lighting off (VISUAL_QA rule 1) are deliberate, and
 * making them selectable must not make them the wrong thing by default.
 *
 * EVERY CASE DRIVES THE REAL DISPATCHER. An earlier draft of this file built
 * the parameters itself, calling the same production functions in the same
 * order. It passed -- and so did a build with the dispatcher's seam call
 * deleted, at 81/81. See FSeamHarness below; the mirror is gone and the
 * measurement is recorded there because it is the reason for the shape of this
 * file.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizRenderSettingsSeamTest,
	"FlowViz.Render.SettingsSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

namespace FlowVizSettingsSeamFixture
{
	/**
	 * Drive the REAL production dispatcher and read back what it built.
	 *
	 * THE FIRST VERSION OF THIS HELPER REBUILT THE PARAMETERS ITSELF -- it called
	 * FillDefaults, FillFromVolumeParameters and ApplyToRayMarchParameters in the
	 * dispatcher's own order and asserted on the result. Every assertion below
	 * passed against it. Then, measured: deleting
	 * `Context.RenderSettings.ApplyToRayMarchParameters(...)` from
	 * DispatchVolumeRayMarch left the suite at 81/81 green, with the shipped
	 * renderer back to one selectable composite mode. The mirror was asserting
	 * that the view model works -- already covered by FlowViz.UI.RenderSettings --
	 * and was structurally incapable of failing on the defect it was written for.
	 *
	 * So this calls DispatchVolumeRayMarch and reads the queued request. The
	 * dispatcher is the thing under test; nothing here reimplements it.
	 *
	 * WHAT IT TAKES TO GET A REQUEST QUEUED. DispatchVolumeRayMarch returns early
	 * on a null view, an empty view rect, a null slot or an invalid field
	 * texture, so the fixture supplies a view rect and a real (tiny) texture. If
	 * any of those guards changes, this returns false and every case reports the
	 * failure rather than passing vacuously on an empty queue.
	 */
	/** The view rect the fixture renders into. Asserted on, so it is named once. */
	constexpr int32 ViewWidth = 8;
	constexpr int32 ViewHeight = 4;

	struct FSeamHarness
	{
		FlowVizVolumeRayMarchProduction::FDispatcher Dispatcher;
		FFlowVizVolumeSlotTextures Slot;
		FSceneViewInitOptions ViewInit;
		TUniquePtr<FSceneView> View;

		/**
		 * A 1x1x1 R32F volume, created on the render thread.
		 *
		 * Contents are irrelevant -- nothing here samples it. It exists because
		 * DispatchVolumeRayMarch refuses a request whose field texture is
		 * invalid, and a fixture that could not get past that guard would leave
		 * the queue empty and every case below asserting on nothing.
		 *
		 * Created through the command list, not the deprecated global
		 * RHICreateTexture, which resolves to FRHICommandListImmediate::Get() and
		 * asserts IsInRenderingThread().
		 *
		 * THE NULL RHI IS ENOUGH. FNullDynamicRHI::RHICreateTextureInitializer
		 * returns a real FNullTexture, so this runs in the default -nullrhi suite
		 * rather than skipping there and reporting Success for having done
		 * nothing.
		 */
		void CreateResources()
		{
			FFlowVizVolumeSlotTextures* SlotPtr = &Slot;
			ENQUEUE_RENDER_COMMAND(FlowVizSeamTestCreateField)(
				[SlotPtr](FRHICommandListImmediate& RHICmdList)
				{
					const FRHITextureCreateDesc Desc =
						FRHITextureCreateDesc::Create3D(TEXT("FlowVizSeamTestField"))
							.SetExtent(1, 1)
							.SetDepth(1)
							.SetFormat(PF_R32_FLOAT)
							.SetFlags(ETextureCreateFlags::ShaderResource);

					SlotPtr->ScalarTexture = RHICmdList.CreateTexture(Desc);
				});
			FlushRenderingCommands();

			// NO VIEW FAMILY. FSceneView's constructor guards every Family
			// dereference, and SetupAntiAliasingMethod resets a temporal method
			// to AAM_None when State is null -- so VerifyMembersChecks' checkf on
			// State cannot fire. A family would need a scene, which would need a
			// world, for a camera the dispatcher reduces to eight numbers.
			ViewInit.SetViewRectangle(FIntRect(0, 0, ViewWidth, ViewHeight));
			ViewInit.ViewOrigin = FVector::ZeroVector;
			ViewInit.ViewRotationMatrix = FMatrix::Identity;
			ViewInit.ProjectionMatrix = FMatrix::Identity;
			View = MakeUnique<FSceneView>(ViewInit);
		}

		bool IsReady() const { return View.IsValid() && Slot.ScalarTexture.IsValid(); }

		/** Render-thread teardown for the texture refs the queue and the slot hold. */
		void ReleaseResources()
		{
			Dispatcher.ReleaseResources();
			Slot.ScalarTexture.SafeRelease();
			FlushRenderingCommands();
		}

		/**
		 * Dispatch with these settings and return the parameters the dispatcher
		 * built. Returns false if nothing was queued -- which is a failure to
		 * report, never a case to skip.
		 */
		bool Dispatch(
			const FFlowVizRenderSettingsViewModel& Settings,
			FFlowVizVolumeRayMarchParameters& OutBuilt)
		{
			FFlowVizVolumeRayMarchContext Context;
			Context.View = View.Get();
			Context.LocalToWorld = FMatrix::Identity;
			Context.SlotA = &Slot;
			Context.RenderSettings = Settings;

			const int32 Before = Dispatcher.NumPendingRequests();
			Dispatcher.DispatchVolumeRayMarch(Context);
			if (Dispatcher.NumPendingRequests() != Before + 1)
			{
				return false;
			}

			return Dispatcher.PeekRequestParameters(Before, OutBuilt);
		}
	};
}

bool FFlowVizRenderSettingsSeamTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizSettingsSeamFixture;

	FSeamHarness Harness;
	Harness.CreateResources();
	if (!TestTrue(TEXT("the fixture has a view and a field texture"), Harness.IsReady()))
	{
		// NOT A SKIP. Every case below reads the dispatcher's queue, so without
		// these two the suite would assert on default-constructed parameters and
		// report green for having tested nothing.
		Harness.ReleaseResources();
		return false;
	}

	// --- THE CONTROL, FIRST -------------------------------------------------
	//
	// Default settings change nothing. Every assertion below is only meaningful
	// if this holds: a seam that overwrote unrelated rows would still carry
	// CompositeMode correctly and would still be broken.
	{
		FFlowVizVolumeRayMarchParameters Expected;
		FlowVizRayMarch::FillDefaults(Expected);

		FFlowVizVolumeRayMarchParameters Actual;
		if (TestTrue(TEXT("the dispatcher queues a request for default settings"),
				Harness.Dispatch(FFlowVizRenderSettingsViewModel(), Actual)))
		{
			TestEqual(TEXT("default settings leave CompositeMode at the default"),
				Actual.CompositeMode, Expected.CompositeMode);
			TestEqual(TEXT("default settings leave lighting off (VISUAL_QA rule 1)"),
				Actual.bEnableLighting, Expected.bEnableLighting);
			TestEqual(TEXT("default settings leave jitter off (ADR 002)"),
				Actual.bEnableJitter, Expected.bEnableJitter);
			TestEqual(TEXT("default settings leave StepVoxels at the default"),
				Actual.StepVoxels, Expected.StepVoxels);
			TestEqual(TEXT("default settings leave MaxSteps at the default"),
				Actual.MaxSteps, Expected.MaxSteps);

			// The rows the settings do NOT own must survive. The seam runs
			// BEFORE SetVolumeTextures and SetViewCamera, so a seam that assigned
			// a fresh struct would erase the geometry rows and then have them
			// re-filled -- these three are what actually catch it. bHasStatusTexture
			// is 0 because the fixture binds no status texture, which is the
			// fail-closed value SetVolumeTextures derives from the binding.
			TestEqual(TEXT("the dispatcher fills the output width from the view rect"),
				Actual.OutputSizeX, static_cast<uint32>(ViewWidth));
			TestEqual(TEXT("the dispatcher fills the output height from the view rect"),
				Actual.OutputSizeY, static_cast<uint32>(ViewHeight));
			TestEqual(TEXT("no status texture is bound, so status is fail-closed"),
				Actual.bHasStatusTexture, 0u);
			TestNotNull(TEXT("the field texture reached the request"),
				static_cast<FRHITexture*>(Actual.FieldTexture));
		}
	}

	// --- Every composite mode survives the seam -----------------------------
	//
	// Five of these six were unreachable in a shipped build. Asserting all six
	// rather than one means a seam that carried only the mode it was written
	// against fails here.
	{
		const EFlowVizCompositeMode Modes[] = {
			EFlowVizCompositeMode::Alpha,
			EFlowVizCompositeMode::Maximum,
			EFlowVizCompositeMode::Minimum,
			EFlowVizCompositeMode::Average,
			EFlowVizCompositeMode::IsoSurface,
			EFlowVizCompositeMode::Diagnostic,
		};

		for (const EFlowVizCompositeMode Mode : Modes)
		{
			FFlowVizRenderSettingsViewModel Settings;
			Settings.SetCompositeMode(Mode);

			FFlowVizVolumeRayMarchParameters Built;
			if (TestTrue(
					*FString::Printf(TEXT("the dispatcher queues a request for mode %d"),
						static_cast<int32>(Mode)),
					Harness.Dispatch(Settings, Built)))
			{
				TestEqual(
					*FString::Printf(TEXT("composite mode %d reaches the request"),
						static_cast<int32>(Mode)),
					Built.CompositeMode, static_cast<uint32>(Mode));
			}
		}
	}

	// --- The lighting path is reachable -------------------------------------
	//
	// bEnableLighting was welded to 0, so the whole FlowVizGradient branch in
	// the .usf was dead code that every test covered. Its three companions must
	// arrive with it: lighting enabled with a zero light direction is a
	// uniformly ambient render, which reads as a data problem.
	{
		FFlowVizRenderSettingsViewModel Settings;
		Settings.SetLightingEnabled(true);
		Settings.SetAmbientStrength(0.25f);
		Settings.SetDiffuseStrength(0.75f);
		TestTrue(TEXT("a non-zero light direction is accepted"),
			Settings.SetLightDirection(FVector3f(0.0f, 0.0f, -1.0f)));

		FFlowVizVolumeRayMarchParameters Built;
		if (TestTrue(TEXT("the dispatcher queues a request with lighting on"),
				Harness.Dispatch(Settings, Built)))
		{
			TestEqual(TEXT("lighting enabled reaches the request"),
				Built.bEnableLighting, 1u);
			TestEqual(TEXT("ambient strength reaches the request"),
				Built.AmbientStrength, 0.25f);
			TestEqual(TEXT("diffuse strength reaches the request"),
				Built.DiffuseStrength, 0.75f);
			TestTrue(TEXT("the light direction arrives normalised"),
				FMath::IsNearlyEqual(Built.LightDirection.Size(), 1.0f, KINDA_SMALL_NUMBER));
		}
	}

	// --- Iso-surface value, required by plan.md section 9 -------------------
	{
		FFlowVizRenderSettingsViewModel Settings;
		Settings.SetCompositeMode(EFlowVizCompositeMode::IsoSurface);
		TestTrue(TEXT("a finite iso value is accepted"), Settings.SetIsoValue(0.5f));

		FFlowVizVolumeRayMarchParameters Built;
		if (TestTrue(TEXT("the dispatcher queues an iso-surface request"),
				Harness.Dispatch(Settings, Built)))
		{
			TestEqual(TEXT("the iso value reaches the request"), Built.IsoValue, 0.5f);
		}
	}

	// --- The step-count clamp is re-applied at the seam ---------------------
	//
	// FillDefaults ends by clamping MaxSteps into [1, MaxStepsLimit] so no
	// caller can ask the GPU for an unbounded loop. Anything writing MaxSteps
	// AFTER that point reopens exactly the hole the clamp closed, so the seam
	// has to clamp too. Asserted through the seam rather than on the view model
	// because the seam is the last writer.
	{
		FFlowVizRenderSettingsViewModel Settings;
		Settings.SetMaxSteps(100000000u);

		FFlowVizVolumeRayMarchParameters Built;
		if (TestTrue(TEXT("the dispatcher queues a request with an absurd step count"),
				Harness.Dispatch(Settings, Built)))
		{
			TestTrue(TEXT("an absurd step count is clamped at the seam, not passed to the GPU"),
				Built.MaxSteps <= FlowVizRayMarch::MaxStepsLimit);
			TestTrue(TEXT("and is still at least one step"), Built.MaxSteps >= 1u);
		}
	}

	// --- Two settings coexist ------------------------------------------------
	//
	// A seam that applied one field per call, or that rebuilt the struct per
	// setter, passes every single-setting case above and fails this one.
	{
		FFlowVizRenderSettingsViewModel Settings;
		Settings.SetCompositeMode(EFlowVizCompositeMode::Maximum);
		Settings.SetLightingEnabled(true);
		TestTrue(TEXT("a positive step size is accepted"), Settings.SetStepVoxels(0.25f));

		FFlowVizVolumeRayMarchParameters Built;
		if (TestTrue(TEXT("the dispatcher queues a request with three settings"),
				Harness.Dispatch(Settings, Built)))
		{
			TestEqual(TEXT("the mode survives alongside the others"),
				Built.CompositeMode, static_cast<uint32>(EFlowVizCompositeMode::Maximum));
			TestEqual(TEXT("lighting survives alongside the others"),
				Built.bEnableLighting, 1u);
			TestEqual(TEXT("the step size survives alongside the others"),
				Built.StepVoxels, 0.25f);
		}
	}

	Harness.ReleaseResources();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
