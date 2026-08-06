// Copyright FlowViz contributors. All Rights Reserved.

#include "../Render/FlowVizVolumeRayMarchDispatcher.h"

#include "CFDViz/CFDVizColorMaps.h"
#include "Misc/AutomationTest.h"
#include "RenderGraphBuilder.h"
#include "Render/FlowVizVolumeRayMarchShader.h"
#include "Scene/FlowVizVolumeComponent.h"
#include "UI/FlowVizTransferFunctionViewModel.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * DOES A COLOURING CHOICE MADE OUTSIDE THE RENDERER REACH THE GPU?
 *
 * The third instance of the same defect as #40/#41 (render settings) and #42
 * (clip planes), and the last of the three view models to still be unreachable.
 * FFlowVizTransferFunctionViewModel validates its range, resolves the component
 * mode, and writes nine rows of the ray-march constant buffer -- all covered by
 * FlowViz.UI.TransferFunctionViewModel, all passing, and every caller of
 * ApplyToRayMarchParameters is a test. What ships is FillDefaults.
 *
 * THIS ONE HAS TWO CHANNELS, WHICH IS WHY IT IS NOT A COPY OF THE CLIP FIX.
 * A transfer function reaches the shader by two separate routes and BOTH are
 * cut today:
 *
 *   1. the PARAMETER BLOCK -- value range, component mode, opacity multiplier,
 *      the four invalid-value colours and bClampToRange, written by
 *      ApplyToRayMarchParameters into the cbuffer;
 *
 *   2. the LUT TEXTURE -- the colour table itself, which the dispatcher builds
 *      with a hard-coded `MakeDefault(CFDViz::ColorMaps::Default, ...)`.
 *
 * Closing only the first would carry the user's RANGE while still rendering
 * every field through viridis: pick Inferno, get viridis, and the range readout
 * agrees with the picker so nothing looks wrong. That is the more dangerous
 * half, because a partial fix produces a plausible image, and the assertions
 * below are split so that closing one channel cannot pass for closing both.
 *
 * THE ASSERTIONS ARE MADE AT THE DISPATCHER, for the reason recorded in
 * FlowVizRenderSettingsSeamTest: a fixture that rebuilds the parameters by
 * calling the same production functions in the same order passes whether or not
 * the dispatcher makes those calls at all. That mirror was measured to survive
 * deleting the seam outright, at 81/81 green. Nothing here reimplements the
 * dispatcher; every case drives DispatchVolumeRayMarch and reads the queue.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTransferFunctionSeamTest,
	"FlowViz.Render.TransferFunctionSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

namespace FlowVizTransferFunctionSeamFixture
{
	/** The view rect the fixture renders into. Asserted on, so it is named once. */
	constexpr int32 ViewWidth = 8;
	constexpr int32 ViewHeight = 4;

	/**
	 * A range no default can produce, so an assertion on it cannot be satisfied
	 * by the value that was already there.
	 *
	 * FillDefaults leaves ValueRangeMin/Max at 0/1. A test that asked for 0..1
	 * would pass against a completely unwired seam -- repo memory
	 * degenerate-data-defeats-assertions. These two are deliberately not 0, not
	 * 1, and not symmetric.
	 */
	constexpr float TestRangeMin = -3.25f;
	constexpr float TestRangeMax = 11.75f;

	/**
	 * An opacity multiplier that is not the default.
	 *
	 * FillDefaults leaves OpacityMultiplier at 1.0, so asking for 1.0 would be
	 * satisfied by an unwired seam for the same reason the range bounds above
	 * avoid 0 and 1.
	 */
	constexpr float TestOpacity = 0.375f;

	/**
	 * Drive the REAL production dispatcher and read back what it built.
	 *
	 * Modelled on FlowVizSettingsSeamFixture::FSeamHarness; see that file for
	 * why a mirror fixture is unacceptable here. DispatchVolumeRayMarch returns
	 * early on a null view, an empty view rect, a null slot or an invalid field
	 * texture, so this supplies a view rect and a real 1x1x1 texture. If any of
	 * those guards changes, Dispatch returns false and the case reports it
	 * rather than passing vacuously on an empty queue.
	 */
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
		 * invalid. FNullDynamicRHI::RHICreateTextureInitializer returns a real
		 * FNullTexture, so this runs in the default -nullrhi suite rather than
		 * skipping there and reporting Success for having done nothing.
		 */
		void CreateResources()
		{
			FFlowVizVolumeSlotTextures* SlotPtr = &Slot;
			ENQUEUE_RENDER_COMMAND(FlowVizTfSeamTestCreateField)(
				[SlotPtr](FRHICommandListImmediate& RHICmdList)
				{
					const FRHITextureCreateDesc Desc =
						FRHITextureCreateDesc::Create3D(TEXT("FlowVizTfSeamTestField"))
							.SetExtent(1, 1)
							.SetDepth(1)
							.SetFormat(PF_R32_FLOAT)
							.SetFlags(ETextureCreateFlags::ShaderResource);

					SlotPtr->ScalarTexture = RHICmdList.CreateTexture(Desc);
				});
			FlushRenderingCommands();

			// NO VIEW FAMILY, for the reason recorded in the settings seam
			// fixture: FSceneView guards every Family dereference, and a family
			// would need a scene and a world for a camera the dispatcher
			// reduces to eight numbers.
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
		 * Dispatch with this transfer function and return the parameters the
		 * dispatcher built. Returns false if nothing was queued -- a failure to
		 * report, never a case to skip.
		 */
		bool Dispatch(
			const FFlowVizTransferFunctionViewModel& TransferFunction,
			FFlowVizVolumeRayMarchParameters& OutBuilt)
		{
			FFlowVizVolumeRayMarchContext Context;
			Context.View = View.Get();
			Context.LocalToWorld = FMatrix::Identity;
			Context.SlotA = &Slot;
			Context.TransferFunction = TransferFunction;

			const int32 Before = Dispatcher.NumPendingRequests();
			Dispatcher.DispatchVolumeRayMarch(Context);
			if (Dispatcher.NumPendingRequests() != Before + 1)
			{
				return false;
			}

			return Dispatcher.PeekRequestParameters(Before, OutBuilt);
		}

		/**
		 * Drain the queue through the real DrainView, which is where the LUT is
		 * built.
		 *
		 * WITHOUT THIS THE COLOUR-MAP ASSERTIONS TEST NOTHING. Dispatch only
		 * records a request; ResidentColorMap is written by the drain. A test
		 * that dispatched and then read GetResidentColorMap would see the
		 * default-constructed value forever and would pass identically against
		 * a build with the LUT seam deleted -- which is the exact defect this
		 * file exists to detect. Repo memory: a control needs a known-nonzero
		 * expectation, and a pass criterion that cannot fail is not a check.
		 *
		 * On the render thread with a real FRDGBuilder, as the shipped view
		 * extension does it. FNullDynamicRHI is sufficient: nothing here reads
		 * back a pixel.
		 */
		void Drain()
		{
			FlowVizVolumeRayMarchProduction::FDispatcher* DispatcherPtr = &Dispatcher;
			const FSceneView* ViewPtr = View.Get();
			ENQUEUE_RENDER_COMMAND(FlowVizTfSeamTestDrain)(
				[DispatcherPtr, ViewPtr](FRHICommandListImmediate& RHICmdList)
				{
					FRDGBuilder GraphBuilder(RHICmdList);
					DispatcherPtr->DrainView(GraphBuilder, *ViewPtr);
					GraphBuilder.Execute();
				});
			FlushRenderingCommands();
		}
	};

	/**
	 * A view model carrying choices no default produces.
	 *
	 * SetManualRange, not a range source switch: it moves RangeSource to Manual
	 * itself and applies immediately, and Manual is the only source that is
	 * always legal here. Global would need a bound field with declared
	 * statistics, and CurrentFrame is REFUSED until something supplies a
	 * per-frame range -- either would leave the range at its default and make
	 * the assertions below fail for a reason that is not the missing seam.
	 *
	 * Every setter's result is checked. A refused SetColorMap or SetManualRange
	 * would leave a default-valued model, and asserting that a default reaches
	 * the shader is exactly the vacuous test this file exists to avoid.
	 */
	FFlowVizTransferFunctionViewModel MakeConfigured(FAutomationTestBase& Test)
	{
		FFlowVizTransferFunctionViewModel Model;

		Test.TestTrue(TEXT("CONTROL: the colour map is accepted"),
			Model.SetColorMap(ECFDVizColorMap::Inferno).IsOk());
		Test.TestTrue(TEXT("CONTROL: the manual range is accepted"),
			Model.SetManualRange(TestRangeMin, TestRangeMax).IsOk());
		Test.TestTrue(TEXT("CONTROL: the opacity multiplier is accepted"),
			Model.SetOpacityMultiplier(TestOpacity).IsOk());
		Model.SetClampToRange(true);

		// AND THE MODEL REALLY HOLDS THEM. The setters above could each report
		// success while a later one reset an earlier choice; this reads the
		// state back, so a configured fixture cannot silently be a default one.
		Test.TestEqual(TEXT("CONTROL: the model holds the chosen range minimum"),
			Model.GetRangeMin(), TestRangeMin);
		Test.TestEqual(TEXT("CONTROL: the model holds the chosen range maximum"),
			Model.GetRangeMax(), TestRangeMax);
		Test.TestEqual(TEXT("CONTROL: the model holds the chosen colour map"),
			Model.GetColorMap(), ECFDVizColorMap::Inferno);

		Test.TestTrue(
			TEXT("CONTROL: the configured model validates, without which "
				 "ApplyToRayMarchParameters would refuse and write NOTHING -- "
				 "leaving the defaults in place and making every assertion below "
				 "fail for a reason that is not the missing seam"),
			Model.Validate().IsOk());

		return Model;
	}
}

bool FFlowVizTransferFunctionSeamTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizTransferFunctionSeamFixture;

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

	// --- THE IDENTITY CONTROL, FIRST ----------------------------------------
	//
	// A default transfer function must change nothing. Every assertion below is
	// only meaningful if this holds: a seam that clobbered the geometry, format
	// or camera rows on its way through would still carry the value range
	// correctly and would still be broken.
	{
		FFlowVizVolumeRayMarchParameters Expected;
		FlowVizRayMarch::FillDefaults(Expected);

		FFlowVizVolumeRayMarchParameters Actual;
		if (TestTrue(TEXT("the dispatcher queues a request for a default transfer function"),
				Harness.Dispatch(FFlowVizTransferFunctionViewModel(), Actual)))
		{
			TestEqual(TEXT("a default transfer function leaves ValueRangeMin at the default"),
				Actual.ValueRangeMin, Expected.ValueRangeMin);
			TestEqual(TEXT("a default transfer function leaves ValueRangeMax at the default"),
				Actual.ValueRangeMax, Expected.ValueRangeMax);
			TestEqual(TEXT("a default transfer function leaves ComponentMode at the default"),
				Actual.ComponentMode, Expected.ComponentMode);
			TestEqual(TEXT("a default transfer function leaves clamping off"),
				Actual.bClampToRange, Expected.bClampToRange);

			// The rows the transfer function does NOT own must survive. A seam
			// that assigned a fresh struct would erase the geometry rows.
			TestEqual(TEXT("the dispatcher fills the output width from the view rect"),
				Actual.OutputSizeX, static_cast<uint32>(ViewWidth));
			TestEqual(TEXT("the dispatcher fills the output height from the view rect"),
				Actual.OutputSizeY, static_cast<uint32>(ViewHeight));
			TestNotNull(TEXT("the field texture reached the request"),
				static_cast<FRHITexture*>(Actual.FieldTexture));
		}
	}

	// --- CHANNEL 1: THE PARAMETER BLOCK -------------------------------------
	{
		const FFlowVizTransferFunctionViewModel Configured = MakeConfigured(*this);

		FFlowVizVolumeRayMarchParameters Actual;
		if (TestTrue(TEXT("the dispatcher queues a request for a configured transfer function"),
				Harness.Dispatch(Configured, Actual)))
		{
			// THE ASSERTION THIS FILE EXISTS FOR. Nothing in this test called
			// ApplyToRayMarchParameters: the value is either carried by the
			// dispatcher's own seam or it is not carried at all.
			TestEqual(TEXT("a chosen value range reaches the constant buffer -- the channel "
						   "from the transfer-function panel to the shader"),
				Actual.ValueRangeMin, TestRangeMin);
			TestEqual(TEXT("and its upper bound with it, so a seam carrying one endpoint "
						   "is not mistaken for a working one"),
				Actual.ValueRangeMax, TestRangeMax);

			TestEqual(TEXT("the opacity multiplier reaches the constant buffer"),
				Actual.OpacityMultiplier, TestOpacity);

			// bClampToRange is asserted separately from the range itself
			// because it is the row most recently added to the block, and the
			// one whose loss would be hidden by a green shader-level test
			// sitting next to it. See the view model's own note.
			TestEqual(TEXT("the clamp choice reaches the constant buffer, so an out-of-range "
						   "value is coloured the way the user asked"),
				Actual.bClampToRange, 1u);
		}
	}

	// --- CHANNEL 2: THE LUT TEXTURE -----------------------------------------
	//
	// SEPARATE FROM CHANNEL 1 ON PURPOSE. The dispatcher builds its LUT from a
	// hard-coded MakeDefault(ColorMaps::Default, ...), so a fix that wired only
	// the parameter block would carry the user's RANGE while still rendering
	// every field through viridis -- and the range readout would agree with the
	// picker, so nothing would look wrong. This asserts the colour table the
	// dispatcher resolved, not the one the view model holds.
	{
		FFlowVizTransferFunctionViewModel Configured = MakeConfigured(*this);

		FFlowVizVolumeRayMarchParameters Ignored;
		if (TestTrue(TEXT("the dispatcher queues a request for the Inferno map"),
				Harness.Dispatch(Configured, Ignored)))
		{
			// THE CONTROL FOR THIS WHOLE SECTION, and it has to run between the
			// dispatch and the drain. Recording a request must NOT be what moves
			// the resident map -- if it were, the assertion after the drain
			// would pass without DrainView ever having built anything, and a
			// build with the LUT seam deleted would look identical.
			TestEqual(TEXT("CONTROL: recording a request does not by itself change the "
						   "resident colour map -- the drain is what builds the LUT"),
				Harness.Dispatcher.GetResidentColorMap(), CFDViz::ColorMaps::Default);

			Harness.Drain();

			TestEqual(TEXT("the CHOSEN colour map is what the dispatcher built its LUT from, "
						   "so picking Inferno does not render viridis"),
				Harness.Dispatcher.GetResidentColorMap(), ECFDVizColorMap::Inferno);
		}

		// AND IT TRACKS, rather than latching on the first choice. Update() is a
		// no-op when the resident function already matches, so a seam that
		// seeded the LUT once and never re-read it would satisfy the assertion
		// above and freeze every later pick.
		//
		// Magma, not viridis: returning to the DEFAULT map would be
		// indistinguishable from a seam that reset to its hard-coded constant.
		Configured.SetColorMap(ECFDVizColorMap::Magma);
		if (TestTrue(TEXT("the dispatcher queues a request for the Magma map"),
				Harness.Dispatch(Configured, Ignored)))
		{
			Harness.Drain();
			TestEqual(TEXT("a SECOND choice replaces the first, so the colour map is not "
						   "latched at whatever the first frame happened to carry"),
				Harness.Dispatcher.GetResidentColorMap(), ECFDVizColorMap::Magma);
		}
	}

	// --- CHANNEL 2b: THE REST OF THE TABLE ----------------------------------
	//
	// EVERY ASSERTION ABOVE READS GetResidentColorMap, WHICH IS ONE FIELD OF
	// FIVE. BuildLut reads ColorMap, bReverseColorMap, ColorBands and the
	// Opacity curve; the map alone decides none of the other three.
	//
	// THIS IS NOT A HYPOTHETICAL GAP. The first version of this seam routed the
	// request through `FFlowVizTransferFunction::MakeDefault(Map, Min, Max)`,
	// which CONSTRUCTS A FRESH DEFAULT and sets two fields on it -- so it
	// carried the colormap perfectly and silently discarded the reverse toggle,
	// the banding control and the whole opacity editor. Every assertion above
	// passes against that version. Three of the panel's controls would have
	// shipped inert, next to three that worked, which reads to a user as those
	// three being broken rather than unwired.
	//
	// Read through the RESIDENT FUNCTION rather than the request, because what
	// is being checked is what the LUT was actually built from.
	{
		FFlowVizTransferFunctionViewModel Configured = MakeConfigured(*this);

		// Non-default on both, or the assertions cannot fail: reverse defaults
		// to false and banding to 0, which is what a dropped field also reads as.
		Configured.SetReverseColorMap(true);
		if (!TestTrue(TEXT("CONTROL: the fixture's colour banding was accepted"),
				Configured.SetColorBands(7).IsOk()))
		{
			Harness.ReleaseResources();
			return false;
		}

		FFlowVizVolumeRayMarchParameters Ignored;
		if (TestTrue(TEXT("the dispatcher queues a reversed, banded request"),
				Harness.Dispatch(Configured, Ignored)))
		{
			Harness.Drain();

			const FFlowVizTransferFunction& Resident =
				Harness.Dispatcher.GetResidentTransferFunction();

			TestEqual(TEXT("the REVERSE toggle reaches the LUT build, so the reversed ramp "
						   "is what the volume is drawn through"),
				Resident.bReverseColorMap, true);

			TestEqual(TEXT("the COLOUR BANDING reaches the LUT build, so the discrete-band "
						   "control is not inert beside a working colormap picker"),
				Resident.ColorBands, 7);

			TestEqual(TEXT("and the colormap still arrives alongside them -- the point is that "
						   "all four travel together, not that one replaced another"),
				Resident.ColorMap, ECFDVizColorMap::Inferno);
		}
	}

	Harness.ReleaseResources();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
