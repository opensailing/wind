// Copyright FlowViz contributors. All Rights Reserved.

#include "../Render/FlowVizVolumeRayMarchDispatcher.h"

#include "Misc/AutomationTest.h"
#include "Render/FlowVizVolumeRayMarchShader.h"
#include "Scene/FlowVizVolumeComponent.h"
#include "UI/FlowVizClipViewModel.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * DOES A CLIP PLANE CHOSEN OUTSIDE THE RENDERER REACH THE REQUEST?
 *
 * This is the same question FlowViz.Render.SettingsSeam asks about composite
 * modes, about a different channel, and it is asked separately because the
 * answer was different. FFlowVizClipViewModel is thorough and correct: it
 * validates every plane before writing any, zeroes the unused tail so a stale
 * plane cannot reappear when the count rises, converts crop units in exactly one
 * place, and writes NumClipPlanes from the enabled count. FlowViz.UI.ClipViewModel
 * covers all of that and passes.
 *
 * And none of it ran. Every caller of
 * FFlowVizClipViewModel::ApplyToRayMarchParameters was a test -- grep it -- so
 * what shipped was FillDefaults' `NumClipPlanes = 0`, and the shader's
 * `min(NumClipPlanes, FLOWVIZ_MAX_CLIP_PLANES)` loop ran zero times on every
 * frame ever rendered. The clip UI was a panel whose own header said so.
 *
 * A view model with no production caller is indistinguishable, from inside its
 * own unit test, from one that is wired -- so the assertion has to be made
 * somewhere the absence can be observed. That is the DISPATCHER: build a context
 * the way the scene does, ask for a plane, and read what the request carries.
 * The only way to fail is for the seam to be missing or broken, which is the
 * defect.
 *
 * THE IDENTITY CONTROL RUNS FIRST, and here it carries more weight than usual.
 * A default-constructed clip view model has no domain, so ApplyToRayMarchParameters
 * returns an error and writes nothing -- meaning a context that ignores clipping
 * and a context that applies an unconfigured model produce IDENTICAL parameters.
 * Every assertion below therefore has to configure a domain and a plane; a case
 * that forgot to would pass against a seam that does not exist. The control pins
 * the identity so that the difference the other cases observe is real.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizClipSeamTest,
	"FlowViz.Render.ClipSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

namespace FlowVizClipSeamFixture
{
	constexpr int32 ViewWidth = 8;
	constexpr int32 ViewHeight = 4;

	/** The domain every configured case uses. Named once so the crop maths below is checkable. */
	static const FVector DomainSize(10.0, 20.0, 40.0);

	/**
	 * Drive the REAL production dispatcher and read back what it built.
	 *
	 * Deliberately NOT a mirror that calls FillDefaults and then the view model
	 * in the dispatcher's order. That shape was tried on the settings seam and
	 * measured: with the dispatcher's apply call deleted, the mirror-based suite
	 * stayed green at 81/81 while the shipped renderer lost five of six composite
	 * modes. A mirror asserts that the view model works, which is already covered
	 * elsewhere, and is structurally incapable of failing on a missing seam.
	 */
	struct FClipSeamHarness
	{
		FlowVizVolumeRayMarchProduction::FDispatcher Dispatcher;
		FFlowVizVolumeSlotTextures Slot;
		FSceneViewInitOptions ViewInit;
		TUniquePtr<FSceneView> View;

		/**
		 * A 1x1x1 R32F volume plus a view, both required to get past
		 * DispatchVolumeRayMarch's early returns. Nothing samples the texture.
		 * The null RHI returns a real FNullTexture, so this runs in the default
		 * -nullrhi suite rather than skipping there and reporting Success for
		 * having done nothing.
		 */
		void CreateResources()
		{
			FFlowVizVolumeSlotTextures* SlotPtr = &Slot;
			ENQUEUE_RENDER_COMMAND(FlowVizClipSeamTestCreateField)(
				[SlotPtr](FRHICommandListImmediate& RHICmdList)
				{
					const FRHITextureCreateDesc Desc =
						FRHITextureCreateDesc::Create3D(TEXT("FlowVizClipSeamTestField"))
							.SetExtent(1, 1)
							.SetDepth(1)
							.SetFormat(PF_R32_FLOAT)
							.SetFlags(ETextureCreateFlags::ShaderResource);

					SlotPtr->ScalarTexture = RHICmdList.CreateTexture(Desc);
				});
			FlushRenderingCommands();

			ViewInit.SetViewRectangle(FIntRect(0, 0, ViewWidth, ViewHeight));
			ViewInit.ViewOrigin = FVector::ZeroVector;
			ViewInit.ViewRotationMatrix = FMatrix::Identity;
			ViewInit.ProjectionMatrix = FMatrix::Identity;
			View = MakeUnique<FSceneView>(ViewInit);
		}

		bool IsReady() const { return View.IsValid() && Slot.ScalarTexture.IsValid(); }

		void ReleaseResources()
		{
			Dispatcher.ReleaseResources();
			Slot.ScalarTexture.SafeRelease();
			FlushRenderingCommands();
		}

		/**
		 * Dispatch with this clip state and return what the dispatcher built.
		 * False means nothing was queued -- a failure to report, never a case to
		 * skip.
		 */
		bool Dispatch(
			const FFlowVizClipViewModel& Clip,
			FFlowVizVolumeRayMarchParameters& OutBuilt)
		{
			FFlowVizVolumeRayMarchContext Context;
			Context.View = View.Get();
			Context.LocalToWorld = FMatrix::Identity;
			Context.SlotA = &Slot;
			Context.Clip = Clip;

			const int32 Before = Dispatcher.NumPendingRequests();
			Dispatcher.DispatchVolumeRayMarch(Context);
			if (Dispatcher.NumPendingRequests() != Before + 1)
			{
				return false;
			}

			return Dispatcher.PeekRequestParameters(Before, OutBuilt);
		}
	};

	/** A clip model with a domain and one enabled +X plane at the given distance. */
	FFlowVizClipViewModel MakeOnePlaneModel(double Distance)
	{
		FFlowVizClipViewModel Clip;
		Clip.SetDomainSize(DomainSize);

		FFlowVizClipPlane Plane;
		Plane.Normal = FVector(1.0, 0.0, 0.0);
		Plane.Distance = Distance;
		Plane.bEnabled = true;
		Clip.AddPlane(Plane);

		return Clip;
	}
}

bool FFlowVizClipSeamTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizClipSeamFixture;

	FClipSeamHarness Harness;
	Harness.CreateResources();
	if (!TestTrue(TEXT("the fixture has a view and a field texture"), Harness.IsReady()))
	{
		// NOT A SKIP. Every case reads the dispatcher's queue, so without these
		// the suite would assert on default-constructed parameters and report
		// green for having tested nothing.
		Harness.ReleaseResources();
		return false;
	}

	// --- THE CONTROL, FIRST -------------------------------------------------
	//
	// An unconfigured clip model has no domain, so applying it fails and writes
	// nothing. The request must therefore be byte-for-byte what FillDefaults
	// produces: no clipping, and a full-extent crop box.
	//
	// This is what makes the cases below meaningful. It also pins the direction
	// of the failure: a seam that applied an invalid model ANYWAY -- ignoring the
	// FCFDVizResult -- would write a zeroed plane array with NumClipPlanes = 0
	// and a crop box divided by an unset domain, and only this case would see it.
	{
		FFlowVizVolumeRayMarchParameters Expected;
		FlowVizRayMarch::FillDefaults(Expected);

		FFlowVizVolumeRayMarchParameters Actual;
		if (TestTrue(TEXT("the dispatcher queues a request for an unconfigured clip model"),
				Harness.Dispatch(FFlowVizClipViewModel(), Actual)))
		{
			TestEqual(TEXT("no domain means no clipping planes"),
				Actual.NumClipPlanes, Expected.NumClipPlanes);
			TestEqual(TEXT("no domain leaves the crop box at full extent (min)"),
				Actual.CropBoxMin, Expected.CropBoxMin);
			TestEqual(TEXT("no domain leaves the crop box at full extent (max)"),
				Actual.CropBoxMax, Expected.CropBoxMax);
		}
	}

	// --- ONE PLANE REACHES THE SHADER ---------------------------------------
	//
	// The assertion that could not have passed before: FillDefaults writes
	// NumClipPlanes = 0 and nothing else ever wrote it, so 1 here is only
	// reachable through the seam.
	{
		FFlowVizVolumeRayMarchParameters Built;
		if (TestTrue(TEXT("the dispatcher queues a request for one clip plane"),
				Harness.Dispatch(MakeOnePlaneModel(2.5), Built)))
		{
			TestEqual(TEXT("the shader is told there is one clip plane"),
				Built.NumClipPlanes, 1u);

			// The plane itself, not just the count. A seam that carried the count
			// and dropped the array would clip against a zero plane -- whose
			// normal is degenerate -- and the count assertion alone would pass.
			TestEqual(TEXT("the plane's normal reaches the shader"),
				FVector3f(Built.ClipPlanes[0]), FVector3f(1.0f, 0.0f, 0.0f));
			TestEqual(TEXT("the plane's distance reaches the shader"),
				Built.ClipPlanes[0].W, 2.5f);
		}
	}

	// --- THE COUNT IS THE ENABLED COUNT, NOT THE PLANE COUNT ----------------
	//
	// Both directions of the same field. A seam that wrote Planes.Num() would
	// pass the case above and clip against a plane the user switched off.
	{
		FFlowVizClipViewModel Clip = MakeOnePlaneModel(2.5);

		FFlowVizClipPlane Second;
		Second.Normal = FVector(0.0, 1.0, 0.0);
		Second.Distance = 4.0;
		Second.bEnabled = false;
		Clip.AddPlane(Second);

		FFlowVizVolumeRayMarchParameters Built;
		if (TestTrue(TEXT("the dispatcher queues a request with one plane disabled"),
				Harness.Dispatch(Clip, Built)))
		{
			TestEqual(TEXT("a disabled plane is not counted"), Built.NumClipPlanes, 1u);
			TestEqual(TEXT("the disabled plane's slot is zeroed, not left stale"),
				Built.ClipPlanes[1], FVector4f(0.0f, 0.0f, 0.0f, 0.0f));
		}
	}

	// --- REMOVING A PLANE LOWERS THE COUNT ----------------------------------
	//
	// The other direction of the same write. Without this, a seam that only ever
	// raised NumClipPlanes -- an accumulate rather than an assign -- satisfies
	// every case above. Two dispatches through ONE dispatcher, second one
	// looser, because that is the only ordering in which the defect is visible.
	{
		FFlowVizClipViewModel Clip = MakeOnePlaneModel(2.5);

		FFlowVizClipPlane Second;
		Second.Normal = FVector(0.0, 1.0, 0.0);
		Second.Distance = 4.0;
		Second.bEnabled = true;
		Clip.AddPlane(Second);

		FFlowVizVolumeRayMarchParameters Two;
		if (TestTrue(TEXT("the dispatcher queues a request for two planes"),
				Harness.Dispatch(Clip, Two)))
		{
			TestEqual(TEXT("two planes reach the shader"), Two.NumClipPlanes, 2u);
		}

		Clip.SetPlaneEnabled(1, false);

		FFlowVizVolumeRayMarchParameters One;
		if (TestTrue(TEXT("the dispatcher queues a request after disabling one"),
				Harness.Dispatch(Clip, One)))
		{
			TestEqual(TEXT("the count DROPS back to one"), One.NumClipPlanes, 1u);
			TestEqual(TEXT("and slot 1 is zeroed on the way down"),
				One.ClipPlanes[1], FVector4f(0.0f, 0.0f, 0.0f, 0.0f));
		}
	}

	// --- THE CROP BOX IS NORMALISED, NOT PASSED THROUGH ---------------------
	//
	// The view model converts solver units to fractions of the domain. The seam
	// must carry the CONVERTED value: passing the raw crop through would clip a
	// 10-unit-wide domain at 250% of its own extent, which looks like no crop at
	// all -- a silent no-op rather than a visible error.
	{
		FFlowVizClipViewModel Clip;
		Clip.SetDomainSize(DomainSize);
		Clip.SetCropBox(FVector(2.5, 5.0, 10.0), FVector(7.5, 15.0, 30.0));

		FFlowVizVolumeRayMarchParameters Built;
		if (TestTrue(TEXT("the dispatcher queues a request with a crop box"),
				Harness.Dispatch(Clip, Built)))
		{
			// 2.5/10, 5/20, 10/40 -- exact in binary, so an exact comparison is
			// honest here rather than lucky.
			TestEqual(TEXT("the crop minimum is normalised by the domain"),
				Built.CropBoxMin, FVector3f(0.25f, 0.25f, 0.25f));
			TestEqual(TEXT("the crop maximum is normalised by the domain"),
				Built.CropBoxMax, FVector3f(0.75f, 0.75f, 0.75f));
		}
	}

	// --- THE SEAM DOES NOT CLOBBER ITS NEIGHBOURS ---------------------------
	//
	// ApplyToRayMarchParameters writes field-by-field into a struct the
	// dispatcher has already filled with geometry, format and camera rows. A
	// seam placed after SetVolumeTextures or SetViewCamera, or one that assigned
	// a fresh struct, would carry every clip assertion above and render black.
	// The settings seam has this same case for the same reason.
	{
		FFlowVizVolumeRayMarchParameters Reference;
		FlowVizRayMarch::FillDefaults(Reference);

		FFlowVizVolumeRayMarchParameters Built;
		if (TestTrue(TEXT("the dispatcher queues a request for one clip plane (neighbours)"),
				Harness.Dispatch(MakeOnePlaneModel(2.5), Built)))
		{
			TestEqual(TEXT("clipping does not disturb the step count"),
				Built.MaxSteps, Reference.MaxSteps);
			TestEqual(TEXT("clipping does not disturb the composite mode"),
				Built.CompositeMode, Reference.CompositeMode);
			TestTrue(TEXT("clipping leaves the field texture bound"),
				Built.FieldTexture != nullptr);
		}
	}

	Harness.ReleaseResources();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
