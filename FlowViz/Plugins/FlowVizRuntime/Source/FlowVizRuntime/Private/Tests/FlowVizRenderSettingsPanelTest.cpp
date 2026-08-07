// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizRenderSettingsPanel.h"

#include "Misc/AutomationTest.h"
#include "Render/FlowVizVolumeRayMarchShader.h"
#include "UI/FlowVizRenderSettingsViewModel.h"
#include "Widgets/Input/SButton.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/FlowVizSlateAttributePump.h"

/*
 * NAMED namespace: unity build. An anonymous namespace here would merge with
 * every sibling test's and collide by ODR (#37).
 */
namespace FlowVizRenderSettingsPanelTest
{
	/**
	 * A counting subscriber, same shape as the transfer function's.
	 *
	 * The panel's channel to the renderer is a FSimpleDelegate, so there is
	 * nothing to observe but the fact of a call -- and the fact of a call is
	 * exactly what was missing for every one of these controls. #74: 14 of the
	 * view model's 17 setters had no production caller, so 13 render controls
	 * were welded to the view model's member initialisers.
	 */
	struct FAnnounceCounter
	{
		int32 Count = 0;

		FSimpleDelegate MakeDelegate()
		{
			return FSimpleDelegate::CreateLambda([this]() { ++Count; });
		}
	};
}

/**
 * The render settings panel's controls must drive FFlowVizRenderSettingsViewModel.
 *
 * Every state change below is produced by pressing a real button or committing
 * a real text box -- never by calling a view model setter to make the next
 * assertion true. A control wired to nothing fails at the first assertion in
 * its block. (A test that supplies the input cannot find the gap; this one
 * makes the PANEL supply it.)
 *
 * WHY THIS PANEL EXISTS AT ALL. check_frozen_params.sh went green on 2026-08-06
 * because the view model writes all 16 formerly frozen shader parameters -- and
 * 14 of its setters had no production caller, so the freeze had only moved up
 * one level. This panel is the production caller. check_uncalled_setters.sh is
 * the guard that fails if it, or its successor for the next control, goes away.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizRenderSettingsPanelBindingTest,
	"FlowViz.UI.RenderSettingsPanel.ControlsDriveTheViewModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizRenderSettingsPanelBindingTest::RunTest(const FString& Parameters)
{
	FFlowVizRenderSettingsViewModel Settings;

	const TSharedRef<SFlowVizRenderSettingsPanel> Panel =
		SNew(SFlowVizRenderSettingsPanel).ViewModel(&Settings);

	/* == Composite mode buttons select the mode ============================== */
	{
		// The default is Alpha, so Maximum is a CHANGE -- "it was already
		// Maximum" cannot make this pass without the click doing anything.
		TestEqual(TEXT("precondition: the mode is Alpha"),
			Settings.GetCompositeMode(), EFlowVizCompositeMode::Alpha);

		const TSharedPtr<SButton> MaximumButton =
			Panel->GetCompositeModeButton(EFlowVizCompositeMode::Maximum);
		if (!TestTrue(TEXT("the panel built a button for Maximum"), MaximumButton.IsValid()))
		{
			return false;
		}
		MaximumButton->SimulateClick();
		TestEqual(
			TEXT("clicking MIP selects it; if this fails the button is not wired to the view "
				 "model -- which is the welded-to-Alpha state 2e documents, with a button on top"),
			Settings.GetCompositeMode(), EFlowVizCompositeMode::Maximum);

		// EVERY mode must have a button. Five of the six were unreachable in a
		// shipped build for a month; a panel that offered four would be the same
		// defect with better manners.
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
			const TSharedPtr<SButton> ModeButton = Panel->GetCompositeModeButton(Mode);
			if (!TestTrue(FString::Printf(TEXT("a button exists for mode %u"),
					static_cast<uint32>(Mode)),
					ModeButton.IsValid()))
			{
				continue;
			}
			ModeButton->SimulateClick();
			TestEqual(FString::Printf(TEXT("clicking the button for mode %u selects it"),
					static_cast<uint32>(Mode)),
				Settings.GetCompositeMode(), Mode);
		}
	}

	/* == Iso value: committed text reaches the model, garbage does not ======= */
	{
		const TSharedPtr<SFlowVizNumericEntry> IsoBox = Panel->GetIsoValueBox();
		if (!TestTrue(TEXT("the panel built an iso value box"), IsoBox.IsValid()))
		{
			return false;
		}

		IsoBox->SimulateCommit(FText::FromString(TEXT("0.42")));
		TestEqual(TEXT("committing an iso value sets it"), Settings.GetIsoValue(), 0.42f);

		// UNPARSEABLE TEXT IS IGNORED, NOT COERCED TO ZERO -- an iso-surface at
		// a typo'd 0 renders a plausible surface at a level nobody chose.
		IsoBox->SimulateCommit(FText::FromString(TEXT("abc")));
		TestEqual(TEXT("committing garbage leaves the iso value unchanged"),
			Settings.GetIsoValue(), 0.42f);
	}

	/* == Lighting toggle ===================================================== */
	{
		TestFalse(TEXT("precondition: unlit (VISUAL_QA rule 1's default)"),
			Settings.IsLightingEnabled());

		const TSharedPtr<SButton> LightingButton = Panel->GetLightingButton();
		if (!TestTrue(TEXT("the panel built a lighting toggle"), LightingButton.IsValid()))
		{
			return false;
		}
		LightingButton->SimulateClick();
		TestTrue(TEXT("clicking the lighting toggle turns lighting on"),
			Settings.IsLightingEnabled());
		LightingButton->SimulateClick();
		TestFalse(TEXT("clicking it again turns lighting off"), Settings.IsLightingEnabled());
		LightingButton->SimulateClick();  // leave ON, so the terms below are live
	}

	/* == Lighting terms: clamped on the way in =============================== */
	{
		const TSharedPtr<SFlowVizNumericEntry> AmbientBox = Panel->GetAmbientBox();
		const TSharedPtr<SFlowVizNumericEntry> DiffuseBox = Panel->GetDiffuseBox();
		if (!TestTrue(TEXT("the panel built ambient and diffuse boxes"),
				AmbientBox.IsValid() && DiffuseBox.IsValid()))
		{
			return false;
		}

		AmbientBox->SimulateCommit(FText::FromString(TEXT("0.25")));
		TestEqual(TEXT("committing ambient sets it"), Settings.GetAmbientStrength(), 0.25f);

		// The setter CLAMPS rather than refuses -- outside [0,1] is a brightness
		// bug, not a lighting choice -- so 7 lands as 1, not as a rejection.
		AmbientBox->SimulateCommit(FText::FromString(TEXT("7")));
		TestEqual(TEXT("an out-of-range ambient clamps to 1"),
			Settings.GetAmbientStrength(), 1.0f);

		DiffuseBox->SimulateCommit(FText::FromString(TEXT("0.5")));
		TestEqual(TEXT("committing diffuse sets it"), Settings.GetDiffuseStrength(), 0.5f);
	}

	/* == Light direction: per-axis commits, zero refused ===================== */
	{
		const TSharedPtr<SFlowVizNumericEntry> XBox = Panel->GetLightDirectionBox(0);
		const TSharedPtr<SFlowVizNumericEntry> YBox = Panel->GetLightDirectionBox(1);
		const TSharedPtr<SFlowVizNumericEntry> ZBox = Panel->GetLightDirectionBox(2);
		if (!TestTrue(TEXT("the panel built three light direction boxes"),
				XBox.IsValid() && YBox.IsValid() && ZBox.IsValid()))
		{
			return false;
		}

		// Zeroing X and then Y is legal (the remainder renormalises); zeroing
		// the LAST nonzero axis would make the direction (0,0,0), which the
		// setter refuses -- normalising zero gives a dot of 0 everywhere, a
		// uniformly ambient render that reads as "lighting does nothing".
		XBox->SimulateCommit(FText::FromString(TEXT("0")));
		YBox->SimulateCommit(FText::FromString(TEXT("0")));
		TestTrue(TEXT("after zeroing X and Y the direction renormalised to -Z"),
			FMath::IsNearlyEqual(Settings.GetLightDirection().Z, -1.0f, 1e-5f));

		ZBox->SimulateCommit(FText::FromString(TEXT("0")));
		TestTrue(
			TEXT("zeroing the last axis is REFUSED and the previous direction kept; if this "
				 "fails the panel bypassed the setter's zero-direction guard"),
			FMath::IsNearlyEqual(Settings.GetLightDirection().Z, -1.0f, 1e-5f));
	}

	/* == Marching: refusal for a zero step, clamp for a zero ceiling ========= */
	{
		const TSharedPtr<SFlowVizNumericEntry> StepBox = Panel->GetStepVoxelsBox();
		const TSharedPtr<SFlowVizNumericEntry> ReferenceBox = Panel->GetReferenceStepBox();
		const TSharedPtr<SFlowVizNumericEntry> MaxStepsBox = Panel->GetMaxStepsBox();
		const TSharedPtr<SFlowVizNumericEntry> EarlyOutBox = Panel->GetEarlyOutBox();
		if (!TestTrue(TEXT("the panel built the four marching boxes"),
				StepBox.IsValid() && ReferenceBox.IsValid() && MaxStepsBox.IsValid()
					&& EarlyOutBox.IsValid()))
		{
			return false;
		}

		const float StepBefore = Settings.GetStepVoxels();
		StepBox->SimulateCommit(FText::FromString(TEXT("0")));
		TestEqual(
			TEXT("a zero step is refused and the previous step kept -- at zero the ray never "
				 "advances and the volume renders as its first sample smeared over every step"),
			Settings.GetStepVoxels(), StepBefore);

		StepBox->SimulateCommit(FText::FromString(TEXT("0.25")));
		TestEqual(TEXT("committing a positive step sets it"), Settings.GetStepVoxels(), 0.25f);

		ReferenceBox->SimulateCommit(FText::FromString(TEXT("2")));
		TestEqual(TEXT("committing the reference step sets it"),
			Settings.GetReferenceStepVoxels(), 2.0f);

		// SetMaxSteps clamps to [1, limit]; a committed 0 lands as 1 rather
		// than as an unbounded GPU loop or a rejected control.
		MaxStepsBox->SimulateCommit(FText::FromString(TEXT("0")));
		TestEqual(TEXT("a zero step ceiling clamps to 1"), Settings.GetMaxSteps(), 1u);

		MaxStepsBox->SimulateCommit(FText::FromString(TEXT("256")));
		TestEqual(TEXT("committing a step ceiling sets it"), Settings.GetMaxSteps(), 256u);

		EarlyOutBox->SimulateCommit(FText::FromString(TEXT("0.75")));
		TestEqual(TEXT("committing the early-out alpha sets it"),
			Settings.GetEarlyTerminationAlpha(), 0.75f);
	}

	/* == Jitter ============================================================== */
	{
		TestFalse(TEXT("precondition: jitter off (ADR 002's default)"),
			Settings.IsJitterEnabled());

		const TSharedPtr<SButton> JitterButton = Panel->GetJitterButton();
		if (!TestTrue(TEXT("the panel built a jitter toggle"), JitterButton.IsValid()))
		{
			return false;
		}
		JitterButton->SimulateClick();
		TestTrue(TEXT("clicking the jitter toggle turns jitter on"), Settings.IsJitterEnabled());

		const TSharedPtr<SFlowVizNumericEntry> AmountBox = Panel->GetJitterAmountBox();
		const TSharedPtr<SFlowVizNumericEntry> SeedBox = Panel->GetJitterSeedBox();
		if (!TestTrue(TEXT("the panel built jitter amount and seed boxes"),
				AmountBox.IsValid() && SeedBox.IsValid()))
		{
			return false;
		}
		AmountBox->SimulateCommit(FText::FromString(TEXT("0.5")));
		TestEqual(TEXT("committing the jitter amount sets it"), Settings.GetJitterAmount(), 0.5f);

		SeedBox->SimulateCommit(FText::FromString(TEXT("42")));
		TestEqual(TEXT("committing the jitter seed sets it"), Settings.GetJitterSeed(), 42u);
	}

	/* == Sampling toggles ==================================================== */
	{
		TestTrue(TEXT("precondition: trilinear filtering on"),
			Settings.IsFieldFilteringEnabled());

		const TSharedPtr<SButton> FilteringButton = Panel->GetFilteringButton();
		const TSharedPtr<SButton> StrictButton = Panel->GetStrictFilterButton();
		if (!TestTrue(TEXT("the panel built the two sampling toggles"),
				FilteringButton.IsValid() && StrictButton.IsValid()))
		{
			return false;
		}
		FilteringButton->SimulateClick();
		TestFalse(TEXT("clicking the filtering toggle switches to nearest"),
			Settings.IsFieldFilteringEnabled());

		TestFalse(TEXT("precondition: strict status filtering off"),
			Settings.IsStrictStatusFilter());
		StrictButton->SimulateClick();
		TestTrue(TEXT("clicking the strict toggle turns it on"),
			Settings.IsStrictStatusFilter());
	}

	/* == No-data colour swatches ============================================= */
	{
		if (!TestTrue(TEXT("the panel offers at least two no-data swatches"),
				SFlowVizRenderSettingsPanel::GetNoDataSwatchCount() >= 2))
		{
			return false;
		}

		// Find a swatch whose colour differs from the current one, so "it was
		// already that colour" cannot make the assertion pass.
		const FLinearColor Before = Settings.GetNoDataColor();
		int32 DifferentIndex = INDEX_NONE;
		for (int32 Index = 0; Index < SFlowVizRenderSettingsPanel::GetNoDataSwatchCount(); ++Index)
		{
			if (!SFlowVizRenderSettingsPanel::GetNoDataSwatchColor(Index).Equals(Before))
			{
				DifferentIndex = Index;
				break;
			}
		}
		if (!TestTrue(TEXT("CONTROL: some swatch differs from the current colour"),
				DifferentIndex != INDEX_NONE))
		{
			return false;
		}

		const TSharedPtr<SButton> Swatch = Panel->GetNoDataSwatchButton(DifferentIndex);
		if (!TestTrue(TEXT("the panel built the swatch button"), Swatch.IsValid()))
		{
			return false;
		}
		Swatch->SimulateClick();
		TestTrue(TEXT("clicking a no-data swatch selects its colour"),
			Settings.GetNoDataColor().Equals(
				SFlowVizRenderSettingsPanel::GetNoDataSwatchColor(DifferentIndex)));
	}

	return true;
}

/**
 * EVERY ACCEPTED EDIT MUST REACH THE RENDERER; NO REFUSED OR NO-OP EDIT MAY.
 *
 * The first half is #48's lesson: the transfer function panel drove its view
 * model perfectly and announced nothing, so the strip's colours moved and the
 * volume's did not. The second half is the announce test's own refusal
 * discipline: announcing a refused edit pushes a byte-identical model to the
 * render thread for a frame that cannot have changed, and announcing a no-op
 * (clamp landing on the current value, re-clicking the selected mode) does the
 * same. Both directions are asserted, so a future edit that "normalises" the
 * handlers has to argue with a test.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizRenderSettingsPanelAnnounceTest,
	"FlowViz.UI.RenderSettingsPanel.Announce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizRenderSettingsPanelAnnounceTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizRenderSettingsPanelTest;

	FFlowVizRenderSettingsViewModel Settings;
	FAnnounceCounter Counter;

	const TSharedRef<SFlowVizRenderSettingsPanel> Panel =
		SNew(SFlowVizRenderSettingsPanel)
			.ViewModel(&Settings)
			.OnRenderSettingsChanged(Counter.MakeDelegate());

	/* -- A mode change announces once; re-selecting the same mode does not -- */
	Panel->GetCompositeModeButton(EFlowVizCompositeMode::Maximum)->SimulateClick();
	TestEqual(TEXT("selecting a new mode announces once"), Counter.Count, 1);

	Panel->GetCompositeModeButton(EFlowVizCompositeMode::Maximum)->SimulateClick();
	TestEqual(
		TEXT("re-selecting the current mode does NOT announce -- the model did not change, so "
			 "a push would republish an identical frame"),
		Counter.Count, 1);

	/* -- A refused edit does not announce ----------------------------------- */
	Panel->GetIsoValueBox()->SimulateCommit(FText::FromString(TEXT("abc")));
	TestEqual(TEXT("an unparseable commit does not announce"), Counter.Count, 1);

	Panel->GetIsoValueBox()->SimulateCommit(FText::FromString(TEXT("0.42")));
	TestEqual(TEXT("an accepted iso commit announces once"), Counter.Count, 2);

	Panel->GetIsoValueBox()->SimulateCommit(FText::FromString(TEXT("0.42")));
	TestEqual(TEXT("re-committing the same iso value does not announce"), Counter.Count, 2);

	/* -- A clamp that lands on the current value is a no-op ------------------ */
	Panel->GetAmbientBox()->SimulateCommit(FText::FromString(TEXT("7")));
	TestEqual(TEXT("clamping 7 to 1 is a change from 0.35, so it announces"), Counter.Count, 3);

	Panel->GetAmbientBox()->SimulateCommit(FText::FromString(TEXT("9")));
	TestEqual(
		TEXT("clamping 9 to 1 lands on the value already held; no change, no announcement"),
		Counter.Count, 3);

	/* -- The zero-direction refusal does not announce ------------------------ */
	Panel->GetLightDirectionBox(0)->SimulateCommit(FText::FromString(TEXT("0")));
	Panel->GetLightDirectionBox(1)->SimulateCommit(FText::FromString(TEXT("0")));
	TestEqual(TEXT("two accepted axis commits announce twice"), Counter.Count, 5);

	Panel->GetLightDirectionBox(2)->SimulateCommit(FText::FromString(TEXT("0")));
	TestEqual(TEXT("the refused zero direction does not announce"), Counter.Count, 5);

	/* -- The refused zero step does not announce ----------------------------- */
	Panel->GetStepVoxelsBox()->SimulateCommit(FText::FromString(TEXT("0")));
	TestEqual(TEXT("a refused step does not announce"), Counter.Count, 5);

	Panel->GetStepVoxelsBox()->SimulateCommit(FText::FromString(TEXT("0.25")));
	TestEqual(TEXT("an accepted step announces once"), Counter.Count, 6);

	return true;
}

/**
 * A NULL VIEW MODEL IS A LEGAL, INERT STATE -- the workspace builds panels
 * before a case is open, and this panel in particular holds settings that are
 * meaningful with no case at all. Unbound means "no view model", and then every
 * control is disabled (rule 15) and every handler is null-safe.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizRenderSettingsPanelUnboundTest,
	"FlowViz.UI.RenderSettingsPanel.Unbound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizRenderSettingsPanelUnboundTest::RunTest(const FString& Parameters)
{
	const TSharedRef<SFlowVizRenderSettingsPanel> Panel = SNew(SFlowVizRenderSettingsPanel);

	const TSharedPtr<SButton> AlphaButton =
		Panel->GetCompositeModeButton(EFlowVizCompositeMode::Alpha);
	if (!TestTrue(TEXT("an unbound panel still builds its controls"), AlphaButton.IsValid()))
	{
		return false;
	}

	FlowVizSlateAttributePump::Pump(Panel);

	TestFalse(TEXT("with no view model the mode buttons are disabled (rule 15)"),
		AlphaButton->IsEnabled());

	// SimulateClick and SimulateCommit bypass the enabled check, so these prove
	// the HANDLERS are null-safe rather than merely unreachable.
	AlphaButton->SimulateClick();
	if (const TSharedPtr<SButton> LightingButton = Panel->GetLightingButton())
	{
		LightingButton->SimulateClick();
	}
	if (const TSharedPtr<SFlowVizNumericEntry> IsoBox = Panel->GetIsoValueBox())
	{
		IsoBox->SimulateCommit(FText::FromString(TEXT("1.0")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
