// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizRenderSettingsViewModel.h"

#include "Misc/AutomationTest.h"
#include "Render/FlowVizVolumeRayMarchShader.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * CAN PRODUCTION SELECT A COMPOSITE MODE OTHER THAN ALPHA?
 *
 * Until this file existed the answer was no, and the whole suite was green
 * anyway. #24 covers all six composite modes and both lighting states, and it
 * does so by writing `Params->CompositeMode` itself -- which is the correct way
 * to test the shader and structurally incapable of noticing that no other caller
 * ever writes it. `FlowVizRayMarch::FillDefaults` was the sole non-test writer of
 * 16 parameters, so `Maximum`, `Minimum`, `Average`, `IsoSurface` and
 * `Diagnostic` could not be reached in a shipped build, and the gradient-lighting
 * path was dead behind a constant 0. See Docs/BACKLOG.md item 2e.
 *
 * WHAT THIS FILE ASSERTS, AND WHY IT IS SHAPED THIS WAY. Every test here drives
 * the parameters through the same call the dispatcher makes -- FillDefaults
 * followed by ApplyToRayMarchParameters -- rather than setting fields directly.
 * A test that assigns `OutParameters.CompositeMode` would pass whether or not the
 * view model applies anything, which is exactly the blindness above repeated one
 * layer up.
 *
 * THE DEFAULTS ARE DELIBERATE AND MUST SURVIVE. Jitter is off because per-ray
 * jitter causes temporal shimmer (ADR 002); the render is unlit because
 * VISUAL_QA rule 1 forbids lighting from modulating apparent scalar value. The
 * defect was never that those values are wrong -- it was that they were the only
 * REACHABLE values. So the first test below pins that an untouched view model
 * changes nothing, and every other test changes exactly one thing.
 */

namespace FlowVizRenderSettingsTestHelpers
{
	/** FillDefaults, then the view model -- the same order the dispatcher uses. */
	FFlowVizVolumeRayMarchParameters Apply(const FFlowVizRenderSettingsViewModel& ViewModel)
	{
		FFlowVizVolumeRayMarchParameters Parameters;
		FlowVizRayMarch::FillDefaults(Parameters);
		ViewModel.ApplyToRayMarchParameters(Parameters);
		return Parameters;
	}

	/** The defaults alone, for differencing against. */
	FFlowVizVolumeRayMarchParameters DefaultsOnly()
	{
		FFlowVizVolumeRayMarchParameters Parameters;
		FlowVizRayMarch::FillDefaults(Parameters);
		return Parameters;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizRenderSettingsTest,
	"FlowViz.UI.RenderSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizRenderSettingsTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizRenderSettingsTestHelpers;

	/*
	 * THE IDENTITY CONTROL, FIRST.
	 *
	 * A fresh view model must reproduce FillDefaults exactly. Two reasons this
	 * comes before anything else: it pins that the deliberate defaults are not
	 * quietly replaced by a second set of literals living in the view model, and
	 * it is the control for every assertion below. Without it, a test showing
	 * "CompositeMode == Maximum after SetCompositeMode(Maximum)" could be
	 * satisfied by a view model that writes Maximum unconditionally.
	 */
	{
		const FFlowVizVolumeRayMarchParameters Defaults = DefaultsOnly();
		const FFlowVizRenderSettingsViewModel Fresh;
		const FFlowVizVolumeRayMarchParameters Applied = Apply(Fresh);

		TestEqual(TEXT("a fresh view model does not change CompositeMode"),
			Applied.CompositeMode, Defaults.CompositeMode);
		TestEqual(TEXT("a fresh view model leaves lighting off (VISUAL_QA rule 1)"),
			Applied.bEnableLighting, Defaults.bEnableLighting);
		TestEqual(TEXT("a fresh view model leaves jitter off (ADR 002 shimmer)"),
			Applied.bEnableJitter, Defaults.bEnableJitter);
		TestEqual(TEXT("a fresh view model does not change StepVoxels"),
			Applied.StepVoxels, Defaults.StepVoxels);
		TestEqual(TEXT("a fresh view model does not change MaxSteps"),
			Applied.MaxSteps, Defaults.MaxSteps);
		TestEqual(TEXT("a fresh view model does not change AmbientStrength"),
			Applied.AmbientStrength, Defaults.AmbientStrength);
		TestEqual(TEXT("a fresh view model does not change DiffuseStrength"),
			Applied.DiffuseStrength, Defaults.DiffuseStrength);
		TestEqual(TEXT("a fresh view model does not change bFilterField"),
			Applied.bFilterField, Defaults.bFilterField);
	}

	/*
	 * EVERY COMPOSITE MODE IS SELECTABLE. This is the finding, inverted into an
	 * assertion. All six, not just one non-Alpha mode: a seam that reaches
	 * exactly one extra branch is the same defect with a larger constant.
	 */
	{
		const EFlowVizCompositeMode AllModes[] = {
			EFlowVizCompositeMode::Alpha,
			EFlowVizCompositeMode::Maximum,
			EFlowVizCompositeMode::Minimum,
			EFlowVizCompositeMode::Average,
			EFlowVizCompositeMode::IsoSurface,
			EFlowVizCompositeMode::Diagnostic,
		};

		for (const EFlowVizCompositeMode Mode : AllModes)
		{
			FFlowVizRenderSettingsViewModel ViewModel;
			ViewModel.SetCompositeMode(Mode);
			const FFlowVizVolumeRayMarchParameters Applied = Apply(ViewModel);

			TestEqual(
				*FString::Printf(TEXT("composite mode %u reaches the parameters"),
					static_cast<uint32>(Mode)),
				Applied.CompositeMode, static_cast<uint32>(Mode));
		}
	}

	/*
	 * A MODE OUTSIDE THE ENUM IS REFUSED, NOT PASSED THROUGH. The shader
	 * switches on this value; an unknown one falls to whatever the .usf's
	 * default branch does, which is a render that looks like a mode nobody
	 * selected. Rule 15: a control that appears to work and does nothing.
	 */
	{
		FFlowVizRenderSettingsViewModel ViewModel;
		ViewModel.SetCompositeMode(EFlowVizCompositeMode::IsoSurface);
		const bool bAccepted = ViewModel.SetCompositeModeByValue(99u);
		const FFlowVizVolumeRayMarchParameters Applied = Apply(ViewModel);

		TestFalse(TEXT("an out-of-range composite mode is refused"), bAccepted);
		TestEqual(TEXT("and the previous valid mode is kept, not clobbered"),
			Applied.CompositeMode, static_cast<uint32>(EFlowVizCompositeMode::IsoSurface));
	}

	/*
	 * LIGHTING. The gradient path in the .usf is gated on this being non-zero,
	 * so with it welded to 0 the entire FlowVizGradient path was unreachable --
	 * taking AmbientStrength, DiffuseStrength and LightDirection with it. All
	 * four are asserted together because turning lighting on without being able
	 * to aim or balance it is not a usable control.
	 */
	{
		FFlowVizRenderSettingsViewModel ViewModel;
		ViewModel.SetLightingEnabled(true);
		ViewModel.SetAmbientStrength(0.2f);
		ViewModel.SetDiffuseStrength(0.9f);
		ViewModel.SetLightDirection(FVector3f(1.0f, 0.0f, 0.0f));
		const FFlowVizVolumeRayMarchParameters Applied = Apply(ViewModel);

		TestEqual(TEXT("lighting can be turned on"), Applied.bEnableLighting, 1u);
		TestEqual(TEXT("ambient strength reaches the parameters"), Applied.AmbientStrength, 0.2f);
		TestEqual(TEXT("diffuse strength reaches the parameters"), Applied.DiffuseStrength, 0.9f);
		TestTrue(TEXT("light direction reaches the parameters"),
			Applied.LightDirection.Equals(FVector3f(1.0f, 0.0f, 0.0f), UE_KINDA_SMALL_NUMBER));
	}

	/*
	 * A ZERO LIGHT DIRECTION IS REFUSED. Normalising it yields (0,0,0), and a
	 * dot product against that is 0 everywhere -- a uniformly ambient render
	 * that reads as "lighting does nothing" rather than as a rejected input.
	 */
	{
		FFlowVizRenderSettingsViewModel ViewModel;
		const bool bAccepted = ViewModel.SetLightDirection(FVector3f::ZeroVector);
		const FFlowVizVolumeRayMarchParameters Applied = Apply(ViewModel);

		TestFalse(TEXT("a zero light direction is refused"), bAccepted);
		TestFalse(TEXT("and the applied direction is not zero"),
			Applied.LightDirection.IsNearlyZero());
	}

	/*
	 * THE LIGHT DIRECTION IS NORMALISED ON THE WAY OUT. The shader dots it
	 * against a unit gradient; an unnormalised N scales the diffuse term, so a
	 * direction entered as (0,0,10) would be ten times as bright as (0,0,1)
	 * pointing the same way -- a brightness control disguised as an aim control.
	 */
	{
		FFlowVizRenderSettingsViewModel ViewModel;
		ViewModel.SetLightDirection(FVector3f(0.0f, 0.0f, 10.0f));
		const FFlowVizVolumeRayMarchParameters Applied = Apply(ViewModel);

		TestTrue(TEXT("the light direction is normalised"),
			FMath::IsNearlyEqual(Applied.LightDirection.Size(), 1.0f, UE_KINDA_SMALL_NUMBER));
	}

	/*
	 * ISO VALUE. Meaningless in Alpha mode and load-bearing in IsoSurface mode,
	 * which is why it is set independently of the mode rather than bundled.
	 */
	{
		FFlowVizRenderSettingsViewModel ViewModel;
		ViewModel.SetCompositeMode(EFlowVizCompositeMode::IsoSurface);
		ViewModel.SetIsoValue(2.5f);
		const FFlowVizVolumeRayMarchParameters Applied = Apply(ViewModel);

		TestEqual(TEXT("iso value reaches the parameters"), Applied.IsoValue, 2.5f);
	}

	/*
	 * A NON-FINITE ISO VALUE IS REFUSED. NaN compares false against everything,
	 * so an iso-surface at NaN finds no crossing anywhere and renders empty --
	 * indistinguishable from a threshold outside the data range.
	 */
	{
		FFlowVizRenderSettingsViewModel ViewModel;
		const bool bAccepted = ViewModel.SetIsoValue(FMath::Sqrt(-1.0f));
		const FFlowVizVolumeRayMarchParameters Applied = Apply(ViewModel);

		TestFalse(TEXT("a NaN iso value is refused"), bAccepted);
		TestFalse(TEXT("and no NaN reaches the parameters"),
			FMath::IsNaN(Applied.IsoValue));
	}

	/*
	 * STEP SIZE. Smaller steps cost fill rate and buy accuracy; this is the
	 * quality/perf control PERFORMANCE.md's protocol will sweep.
	 */
	{
		FFlowVizRenderSettingsViewModel ViewModel;
		ViewModel.SetStepVoxels(0.25f);
		const FFlowVizVolumeRayMarchParameters Applied = Apply(ViewModel);

		TestEqual(TEXT("step size reaches the parameters"), Applied.StepVoxels, 0.25f);
	}

	/*
	 * A ZERO OR NEGATIVE STEP IS REFUSED. Zero is an infinite loop bounded only
	 * by MaxSteps -- the ray advances nowhere and the volume renders as its
	 * first sample smeared over every step, which looks like a data problem.
	 */
	{
		FFlowVizRenderSettingsViewModel ViewModel;
		const bool bZero = ViewModel.SetStepVoxels(0.0f);
		const bool bNegative = ViewModel.SetStepVoxels(-1.0f);
		const FFlowVizVolumeRayMarchParameters Applied = Apply(ViewModel);

		TestFalse(TEXT("a zero step size is refused"), bZero);
		TestFalse(TEXT("a negative step size is refused"), bNegative);
		TestTrue(TEXT("and the applied step size is still positive"),
			Applied.StepVoxels > 0.0f);
	}

	/*
	 * MAX STEPS. FillDefaults clamps this to MaxStepsLimit in one place so a
	 * caller cannot ask the GPU for an unbounded loop; a settings source that
	 * writes it AFTER that clamp reopens exactly the hole the clamp closed.
	 * This is the test that would catch the fix being applied in the wrong order.
	 */
	{
		FFlowVizRenderSettingsViewModel ViewModel;
		ViewModel.SetMaxSteps(4096u);
		const FFlowVizVolumeRayMarchParameters Applied = Apply(ViewModel);

		TestEqual(TEXT("max steps reaches the parameters"), Applied.MaxSteps, 4096u);
	}

	{
		FFlowVizRenderSettingsViewModel ViewModel;
		ViewModel.SetMaxSteps(100000000u);
		const FFlowVizVolumeRayMarchParameters Applied = Apply(ViewModel);

		TestTrue(TEXT("an absurd max-steps request is still clamped after the view model runs"),
			Applied.MaxSteps <= FlowVizRayMarch::MaxStepsLimit);
		TestTrue(TEXT("and it is not clamped to zero, which would render nothing"),
			Applied.MaxSteps >= 1u);
	}

	/*
	 * JITTER. Off by default per ADR 002; selectable because banding and shimmer
	 * are a real trade and which one is worse depends on the shot.
	 */
	{
		FFlowVizRenderSettingsViewModel ViewModel;
		ViewModel.SetJitterEnabled(true);
		ViewModel.SetJitterAmount(0.5f);
		ViewModel.SetJitterSeed(7u);
		const FFlowVizVolumeRayMarchParameters Applied = Apply(ViewModel);

		TestEqual(TEXT("jitter can be turned on"), Applied.bEnableJitter, 1u);
		TestEqual(TEXT("jitter amount reaches the parameters"), Applied.JitterAmount, 0.5f);
		TestEqual(TEXT("jitter seed reaches the parameters"), Applied.JitterSeed, 7u);
	}

	/*
	 * FILTERING AND THE STRICT STATUS FILTER. bStrictStatusFilter rejects a
	 * filtered sample whose trilinear footprint touches an invalid voxel -- the
	 * difference between a correct edge and a plausible one that has bled
	 * invalid data into it.
	 */
	{
		FFlowVizRenderSettingsViewModel ViewModel;
		ViewModel.SetFieldFilteringEnabled(false);
		ViewModel.SetStrictStatusFilter(true);
		const FFlowVizVolumeRayMarchParameters Applied = Apply(ViewModel);

		TestEqual(TEXT("field filtering can be turned off"), Applied.bFilterField, 0u);
		TestEqual(TEXT("the strict status filter can be turned on"), Applied.bStrictStatusFilter, 1u);
	}

	/*
	 * EARLY TERMINATION. An alpha threshold at which the march stops; at 1.0 it
	 * never triggers, which is the reference-quality setting.
	 */
	{
		FFlowVizRenderSettingsViewModel ViewModel;
		ViewModel.SetEarlyTerminationAlpha(1.0f);
		const FFlowVizVolumeRayMarchParameters Applied = Apply(ViewModel);

		TestEqual(TEXT("early termination alpha reaches the parameters"),
			Applied.EarlyTerminationAlpha, 1.0f);
	}

	/*
	 * THE NO-DATA COLOUR. Transparent by default. It is in the disclosure
	 * palette with NaN/masked/under/over, and VISUAL_QA rule 4 requires those to
	 * stay distinct -- so it is settable, and the view model is not the place
	 * that decides they may collide.
	 */
	{
		FFlowVizRenderSettingsViewModel ViewModel;
		ViewModel.SetNoDataColor(FLinearColor(0.1f, 0.2f, 0.3f, 1.0f));
		const FFlowVizVolumeRayMarchParameters Applied = Apply(ViewModel);

		TestTrue(TEXT("the no-data colour reaches the parameters"),
			Applied.NoDataColor.Equals(FLinearColor(0.1f, 0.2f, 0.3f, 1.0f), UE_KINDA_SMALL_NUMBER));
	}

	/*
	 * REFERENCE STEP VOXELS. The opacity-correction reference: alpha is
	 * compensated by StepVoxels/ReferenceStepVoxels so that changing the step
	 * size changes the noise, not the apparent density. Setting the two
	 * independently is what makes that correction verifiable.
	 */
	{
		FFlowVizRenderSettingsViewModel ViewModel;
		ViewModel.SetReferenceStepVoxels(0.5f);
		const FFlowVizVolumeRayMarchParameters Applied = Apply(ViewModel);

		TestEqual(TEXT("reference step voxels reaches the parameters"),
			Applied.ReferenceStepVoxels, 0.5f);
	}

	{
		FFlowVizRenderSettingsViewModel ViewModel;
		const bool bAccepted = ViewModel.SetReferenceStepVoxels(0.0f);
		const FFlowVizVolumeRayMarchParameters Applied = Apply(ViewModel);

		TestFalse(TEXT("a zero reference step is refused"), bAccepted);
		TestTrue(TEXT("and the applied reference step is still positive, so the "
					  "opacity correction cannot divide by zero"),
			Applied.ReferenceStepVoxels > 0.0f);
	}

	/*
	 * TWO SETTINGS AT ONCE DO NOT OVERWRITE EACH OTHER. The obvious broken
	 * implementation -- ApplyToRayMarchParameters writing a fresh struct rather
	 * than mutating the one it is handed -- passes every single-setting test
	 * above and fails here. It would also silently discard everything
	 * FillFromVolumeParameters and SetVolumeTextures wrote, which is a black
	 * screen rather than a wrong colour.
	 */
	{
		FFlowVizRenderSettingsViewModel ViewModel;
		ViewModel.SetCompositeMode(EFlowVizCompositeMode::Maximum);
		ViewModel.SetLightingEnabled(true);
		ViewModel.SetStepVoxels(0.125f);
		const FFlowVizVolumeRayMarchParameters Applied = Apply(ViewModel);

		TestEqual(TEXT("composite mode survives alongside other settings"),
			Applied.CompositeMode, static_cast<uint32>(EFlowVizCompositeMode::Maximum));
		TestEqual(TEXT("lighting survives alongside other settings"),
			Applied.bEnableLighting, 1u);
		TestEqual(TEXT("step size survives alongside other settings"),
			Applied.StepVoxels, 0.125f);
	}

	/*
	 * AND THE VIEW MODEL DOES NOT DISTURB WHAT IT DOES NOT OWN. Applied after
	 * FillFromVolumeParameters and SetVolumeTextures in the real dispatch, so a
	 * view model that writes a whole struct would erase the geometry rows. Here
	 * the value range is set to a marker before the view model runs.
	 */
	{
		FFlowVizVolumeRayMarchParameters Params;
		FlowVizRayMarch::FillDefaults(Params);
		Params.ValueRangeMin = -12.5f;
		Params.ValueRangeMax = 37.25f;
		Params.bHasStatusTexture = 1u;

		FFlowVizRenderSettingsViewModel ViewModel;
		ViewModel.SetCompositeMode(EFlowVizCompositeMode::Average);
		ViewModel.ApplyToRayMarchParameters(Params);

		TestEqual(TEXT("the view model leaves ValueRangeMin alone"), Params.ValueRangeMin, -12.5f);
		TestEqual(TEXT("the view model leaves ValueRangeMax alone"), Params.ValueRangeMax, 37.25f);
		TestEqual(TEXT("the view model leaves texture flags alone"), Params.bHasStatusTexture, 1u);
		TestEqual(TEXT("and it still applied its own setting"),
			Params.CompositeMode, static_cast<uint32>(EFlowVizCompositeMode::Average));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
