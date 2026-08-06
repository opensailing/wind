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

			// NO VIEW FAMILY: one would need a scene and a world, for a camera
			// the dispatcher reduces to eight numbers.
			//
			// AN EARLIER VERSION OF THIS COMMENT CLAIMED THAT WAS SAFE BECAUSE
			// "FSceneView guards every Family dereference". It does not -- it
			// guards its OWN dereferences. DrainView had a raw `*View.Family`
			// and this fixture crashed the editor on it. Channel 3 at the bottom
			// of this file is the arm for that, and the guard it pins is in
			// DrainView, not here.
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

		// THE RANGE IS NOT READ FROM `Expected`, AND THAT IS THE POINT OF THIS
		// PARAGRAPH. FillDefaults does not write ValueRangeMin/Max at all --
		// FillFromVolumeParameters does, from a layout. FFlowVizVolumeRayMarchParameters
		// declares its members with SHADER_PARAMETER, which supplies no
		// initialiser, so those two fields of `Expected` are whatever was on the
		// stack. This test used to compare against them and demanded
		// ValueRangeMin be 25313668722691049538072426315776.0 -- one particular
		// stack frame's garbage, promoted to a specification.
		//
		// It never failed, because it never ran: the crash this file's channel 3
		// now covers killed the process before the result could be reported, and
		// the runner called the truncated session green (#61 and #62). Fixing
		// the crash is what first made this assertion visible.
		//
		// A default-constructed view model is documented as viridis over [0, 1],
		// so THAT is the expectation, written as literals. Reading it from the
		// same object under test would make the check a tautology.
		constexpr float DefaultRangeMin = 0.0f;
		constexpr float DefaultRangeMax = 1.0f;

		FFlowVizVolumeRayMarchParameters Actual;
		if (TestTrue(TEXT("the dispatcher queues a request for a default transfer function"),
				Harness.Dispatch(FFlowVizTransferFunctionViewModel(), Actual)))
		{
			TestEqual(TEXT("a default transfer function carries the documented default range floor"),
				Actual.ValueRangeMin, DefaultRangeMin);
			TestEqual(TEXT("and the documented default ceiling, so the pair is [0, 1] rather than "
						   "whatever was on the stack"),
				Actual.ValueRangeMax, DefaultRangeMax);
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
	// ALL FOUR OF BuildLut'S INPUTS ARE ASSERTED BELOW. That was not true until
	// #63: the opacity curve was the one input nothing read, and the arm that
	// should have caught it was compound -- it dropped the curve alongside
	// reverse and banding, died on those two, and its KILLED read as though the
	// curve were covered. Adding an input to BuildLut means adding an assertion
	// here AND a narrow single-claim arm to tf-wire.txt; a wide arm cannot tell
	// you which of its claims is actually watched.
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

		/*
		 * AND A CURVE, WHICH IS BuildLut'S FOURTH INPUT AND WAS THE ONE NOT
		 * ASSERTED HERE UNTIL #63.
		 *
		 * WHERE THIS CAME FROM. The arm named "reverse, banding and opacity
		 * dropped" scored KILLED and the campaign was read as covering all
		 * three. It did not: only the reverse and banding assertions fired. A
		 * COMPOUND ARM DIES ON ITS FIRST FAILURE and reports ONE verdict for
		 * every claim in its name, so the third claim rode along inside a green
		 * verdict. tf-wire.txt now carries a narrow arm that drops ONLY the
		 * curve -- it is killable by this assertion and by nothing else in the
		 * suite, which is the verdict the compound arm could not produce.
		 *
		 * NOT MakeLinearRamp(). Its endpoints are (0,0) and (1,1), and 0 and 1
		 * are also what an EMPTY curve reads as at those positions once the
		 * multiplier is applied -- so the two shapes agree exactly where a
		 * dropped curve would be caught. The middle point is what makes this
		 * fixture distinguishable from the default: an empty curve evaluates to
		 * a constant, and no constant passes through 0.25 at t=0.5 while also
		 * being 1.0 at t=1.
		 *
		 * The multiplier is left alone here. It travels through the PARAMETER
		 * BLOCK and channel 1 already asserts it (TestOpacity); this is the
		 * curve's own path into the LUT, and conflating them would leave either
		 * one able to carry the other's assertion.
		 */
		FFlowVizOpacityCurve AuthoredCurve;
		AuthoredCurve.Points.Add(FFlowVizOpacityPoint(0.0f, 0.0f));
		AuthoredCurve.Points.Add(FFlowVizOpacityPoint(0.5f, 0.25f));
		AuthoredCurve.Points.Add(FFlowVizOpacityPoint(1.0f, 1.0f));
		if (!TestTrue(TEXT("CONTROL: the fixture's opacity curve was accepted -- a refused "
						   "setter would leave the default EMPTY curve, which is exactly "
						   "what a dropped curve reads as"),
				Configured.SetOpacityCurve(AuthoredCurve).IsOk()))
		{
			Harness.ReleaseResources();
			return false;
		}

		FFlowVizVolumeRayMarchParameters Ignored;
		if (TestTrue(TEXT("the dispatcher queues a reversed, banded, opacity-curved request"),
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

			/*
			 * THE POINT COUNT FIRST, because it is the assertion a dropped curve
			 * fails: an empty curve has none. Checked before indexing, or a
			 * dropped curve crashes the test rather than failing it -- and a
			 * crash on the render thread is reported as a truncated green run
			 * rather than as a failure (#61, #62).
			 */
			if (TestEqual(TEXT("the OPACITY CURVE reaches the LUT build, so the alpha ramp "
							   "the user authored is what the volume is composited through "
							   "-- BuildLut's fourth input, and the one a colormap "
							   "assertion cannot speak for"),
					Resident.Opacity.Points.Num(), 3))
			{
				/*
				 * THE MIDDLE POINT CARRIES THE WEIGHT. A curve rebuilt as a
				 * default linear ramp would also have endpoints at (0,0) and
				 * (1,1); only the interior point says this is the authored
				 * shape rather than a plausible substitute.
				 */
				TestEqual(TEXT("and its interior control point survived, so the curve is the "
							   "authored SHAPE rather than a default ramp that happens to "
							   "share its endpoints"),
					Resident.Opacity.Points[1].Opacity, 0.25f);
				TestEqual(TEXT("at the position it was authored at, so the ramp is not slid "
							   "along the domain on the way through"),
					Resident.Opacity.Points[1].Position, 0.5f);
			}
		}
	}

	// --- CHANNEL 3: A VIEW WITH NO FAMILY MUST NOT TAKE THE PROCESS DOWN ------
	//
	// THIS IS NOT A HYPOTHETICAL EITHER. Every Drain() above runs against a view
	// whose Family is null, and on 2026-08-06 that was a hard crash:
	//
	//     SIGSEGV: invalid attempt to access memory at address 0x30
	//       TryCreateViewFamilyTexture(FRDGBuilder&, FSceneViewFamily const&)
	//       FDispatcher::DrainView(FRDGBuilder&, FSceneView const&)
	//       FSeamHarness::Drain()
	//
	// DrainView dereferenced View.Family with no check. FSceneView::Family is
	// initialised straight from InitOptions.ViewFamily and defaults to nothing,
	// so any view built from bare init options -- which is every view a unit
	// test can build, since a family needs a scene and a world -- is a null
	// dereference waiting for the composite step.
	//
	// The comment in CreateResources above USED TO CLAIM the opposite: that
	// "FSceneView guards every Family dereference". FSceneView guards its own
	// uses. DrainView is ours.
	//
	// WHY THE ARM IS WORTH ITS LINES WHEN THE CHANNELS ABOVE ALREADY DRAIN: a
	// crash is not a failing assertion. It killed the editor on the render
	// thread and took the whole 106-test session with it at test 51, and the
	// runner reported "51/51 passed" (#61). So the channels above cannot report
	// this defect -- they are what triggers it, and the report never survives to
	// be printed. Reaching this line at all is the evidence.
	{
		FFlowVizTransferFunctionViewModel Configured = MakeConfigured(*this);

		FFlowVizVolumeRayMarchParameters Ignored;
		if (TestTrue(TEXT("the dispatcher queues a request to drain"),
				Harness.Dispatch(Configured, Ignored)))
		{
			// The line that used to crash. There is no TestNoCrash, and there
			// does not need to be: control returning here is the assertion.
			Harness.Drain();

			TestTrue(TEXT("draining a view with NO VIEW FAMILY returns instead of "
						  "dereferencing null -- a crash here kills the render thread and "
						  "every test after it, and the runner reports the truncated run green"),
				true);

			// AND IT STILL DID ITS WORK. A guard placed too early would make
			// this channel pass by turning DrainView into a no-op, which would
			// also silence channels 1 and 2 -- so pin the LUT build, which sits
			// BEFORE the composite step and must therefore still have run.
			TestEqual(TEXT("and the LUT was still built on the way out, so the guard skips the "
						   "composite rather than the whole drain"),
				Harness.Dispatcher.GetResidentColorMap(), ECFDVizColorMap::Inferno);

			// The queue must still drain. A guard that returns BEFORE the
			// requests are taken leaves them pending for a view that will never
			// come back, and the queue grows for the life of the process.
			TestEqual(TEXT("and the request was consumed, not left pending for a view that "
						   "is never drained again"),
				Harness.Dispatcher.NumPendingRequests(), 0);
		}
	}

	Harness.ReleaseResources();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
