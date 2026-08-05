// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/FlowVizTransferFunctionViewModel.h"

#include "CFDViz/CFDVizManifest.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Render/FlowVizVolumeRayMarchShader.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Colouring choices (plan.md sections 5F, 10.3; engineering rules 8 and 15).
 *
 * THE ASSERTIONS BELOW ARE ABOUT CONSUMPTION, NOT STORAGE. A test that sets a
 * colormap and reads it back tests an assignment operator. Every control here is
 * therefore checked either against the value the RENDER layer receives - the
 * ray-march constant buffer, via ApplyToRayMarchParameters - or against a
 * derived quantity a wrong stored value cannot produce.
 *
 * WHERE A CONTROL HAS NO CONSUMER, THE TEST SAYS SO RATHER THAN COVERING IT.
 * bClampToRange used to be the sharp case: the ray-march block had nowhere to
 * put it, so the control changed a legend and could not change the render. That
 * gap is CLOSED - FFlowVizVolumeRayMarchParameters carries the flag and the
 * shader reads it - and the assertion that once pinned the gap has been replaced
 * by one that pins the CONNECTION: two constant buffers produced by
 * ApplyToRayMarchParameters must DIFFER in that slot. Note why the differential
 * form matters. The assertion that read the struct member back after assigning
 * it was green for the entire life of the original defect, because it passes
 * identically on the broken and the fixed build. Never restate it.
 *
 * THE FIXTURE IS THE SHIPPED SAMPLE, AND ITS NUMBERS ARE ASYMMETRIC. `pressure`
 * is coolwarm (DIVERGING) over [-62.9375, 27.6875] - a range whose midpoint
 * (-17.625) is nowhere near zero, so a reset that centres on the midpoint
 * instead of on zero gives a visibly different answer. `U` is viridis
 * (SEQUENTIAL) with 3 components, so component legality and magnitude are both
 * exercisable. Both ranges are read out of the manifest below rather than
 * hardcoded, and then the DERIVED expectations are written as literals.
 */

namespace FlowVizTransferFunctionViewModelTest
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

	bool LoadSampleCase(FAutomationTestBase& Test, FCFDVizCase& OutCase)
	{
		const FString CaseDir = GetSampleCaseDir();
		const FString ManifestPath = FPaths::Combine(CaseDir, TEXT("manifest.json"));
		if (CaseDir.IsEmpty() || !FPaths::FileExists(ManifestPath))
		{
			Test.AddError(FString::Printf(
				TEXT("the sample case is required for this test and is missing at '%s'"), *ManifestPath));
			return false;
		}
		const FCFDVizResult Load = FCFDVizCase::LoadFromFile(ManifestPath, OutCase);
		if (!Load.IsOk())
		{
			Test.AddError(FString::Printf(TEXT("failed to load the sample manifest: %s"), *Load.ToString()));
			return false;
		}
		return true;
	}
}

/* ========================================================================== */
/* Binding, defaults, and rule 8                                              */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTransferFunctionViewModelBindTest,
	"FlowViz.UI.TransferFunctionViewModel.Bind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizTransferFunctionViewModelBindTest::RunTest(const FString& Parameters)
{
	FCFDVizCase Case;
	if (!FlowVizTransferFunctionViewModelTest::LoadSampleCase(*this, Case))
	{
		return false;
	}

	FFlowVizTransferFunctionViewModel ViewModel;

	/* == An unknown field is refused, and nothing is bound =================== */
	{
		TestFalse(TEXT("binding a field the case does not declare is refused"),
			ViewModel.BindField(Case, FName(TEXT("noSuchField"))).IsOk());
		TestFalse(TEXT("a refused bind leaves the view model unbound"), ViewModel.IsBound());
	}

	/* == pressure: a DIVERGING map over an asymmetric range ================== */
	{
		TestTrue(TEXT("binding pressure succeeds"),
			ViewModel.BindField(Case, FName(TEXT("pressure"))).IsOk());
		TestTrue(TEXT("the view model is bound"), ViewModel.IsBound());
		TestEqual(TEXT("the bound field is pressure"), ViewModel.GetFieldId(), FName(TEXT("pressure")));

		// The manifest declares defaultColorMap "coolwarm". Adopting it is not a
		// nicety: a case authored for a signed field and shown on viridis has no
		// visual zero at all.
		TestEqual(TEXT("the manifest's declared colormap is adopted"),
			ViewModel.GetColorMap(), ECFDVizColorMap::CoolWarm);
		TestTrue(TEXT("coolwarm is a diverging map"),
			CFDViz::ColorMaps::IsDiverging(ViewModel.GetColorMap()));

		/*
		 * VISUAL_QA RULE 7, AND THE REASON THIS FIXTURE WAS CHOSEN.
		 *
		 * pressure's declared global range is [-62.9375, 27.6875]. Its MIDPOINT
		 * is -17.625. A diverging map centred on the midpoint would put white -
		 * the colour that means "zero" - at -17.625 Pa, a value with no physical
		 * meaning, presented as the neutral one.
		 *
		 * The correct domain is symmetric about zero at +/- max(|min|, |max|) =
		 * 62.9375. Both numbers are written as literals: deriving the expectation
		 * from MakeDefaultDomain would make this assertion agree with the code
		 * under test no matter what either did.
		 */
		TestTrue(TEXT("pressure's range is known from the manifest"), ViewModel.IsRangeKnown());
		TestEqual(TEXT("a diverging map centres the domain on zero: min = -62.9375"),
			ViewModel.GetRangeMin(), -62.9375f, 1.0e-4f);
		TestEqual(TEXT("a diverging map centres the domain on zero: max = +62.9375"),
			ViewModel.GetRangeMax(), 62.9375f, 1.0e-4f);

		// A control that cannot fail is not a check: state what the WRONG answer
		// would have been, and assert we are not it.
		TestNotEqual(TEXT("the domain is NOT the data midpoint-centred one a naive reset would give"),
			ViewModel.GetRangeMin(), -62.9375f + 17.625f);

		// ENGINEERING RULE 8: stable global ranges are the DEFAULT.
		TestEqual(TEXT("a freshly bound field ranges globally, per engineering rule 8"),
			ViewModel.GetRangeSource(), EFlowVizRangeSource::Global);
		TestTrue(TEXT("a global range is stable across the animation"),
			ViewModel.IsRangeStableAcrossAnimation());
	}

	/* == U: a SEQUENTIAL map keeps its data range unchanged ================== */
	{
		TestTrue(TEXT("binding U succeeds"), ViewModel.BindField(Case, FName(TEXT("U"))).IsOk());
		TestEqual(TEXT("U's declared colormap viridis is adopted"),
			ViewModel.GetColorMap(), ECFDVizColorMap::Viridis);
		TestFalse(TEXT("viridis is not diverging"),
			CFDViz::ColorMaps::IsDiverging(ViewModel.GetColorMap()));

		// U's declared magnitude range is [0.9291473871349224, 13.497203546099266].
		// A sequential map keeps it: re-centring would throw away half the
		// dynamic range for nothing.
		TestEqual(TEXT("a sequential map keeps the declared minimum"),
			ViewModel.GetRangeMin(), 0.92914739f, 1.0e-4f);
		TestEqual(TEXT("a sequential map keeps the declared maximum"),
			ViewModel.GetRangeMax(), 13.4972035f, 1.0e-4f);
		TestTrue(TEXT("a sequential domain is NOT symmetric about zero"),
			ViewModel.GetRangeMin() > 0.0f);

		// U declares defaultComponent "magnitude" and has 3 components.
		TestEqual(TEXT("U has 3 components"), ViewModel.GetComponentCount(), 3);
		TestEqual(TEXT("the manifest's declared default component is adopted"),
			ViewModel.GetComponent(), EFlowVizComponentChoice::Magnitude);
	}

	/* == Component legality is per FIELD, not universal ====================== */
	{
		// U is a 3-vector: X, Y, Z and Magnitude are all legal; W is not.
		TestTrue(TEXT("X is legal on a 3-component field"),
			ViewModel.SetComponent(EFlowVizComponentChoice::X).IsOk());
		TestTrue(TEXT("Z is legal on a 3-component field"),
			ViewModel.SetComponent(EFlowVizComponentChoice::Z).IsOk());
		TestFalse(TEXT("W is refused on a 3-component field"),
			ViewModel.SetComponent(EFlowVizComponentChoice::W).IsOk());
		TestEqual(TEXT("a refused component leaves the previous one in place"),
			ViewModel.GetComponent(), EFlowVizComponentChoice::Z);

		// pressure is a scalar. Y on a scalar is not a harmless no-op: it colours
		// the whole field by a component that does not exist.
		TestTrue(TEXT("binding pressure succeeds"),
			ViewModel.BindField(Case, FName(TEXT("pressure"))).IsOk());
		TestEqual(TEXT("pressure has 1 component"), ViewModel.GetComponentCount(), 1);
		TestFalse(TEXT("Y is refused on a 1-component field"),
			ViewModel.SetComponent(EFlowVizComponentChoice::Y).IsOk());
		TestTrue(TEXT("X is legal on a 1-component field"),
			ViewModel.SetComponent(EFlowVizComponentChoice::X).IsOk());
		TestTrue(TEXT("Magnitude is legal on any component count"),
			ViewModel.SetComponent(EFlowVizComponentChoice::Magnitude).IsOk());
	}

	return true;
}

/* ========================================================================== */
/* Range sources, and rule 15's "no nonfunctional controls"                   */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTransferFunctionViewModelRangeTest,
	"FlowViz.UI.TransferFunctionViewModel.Range",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizTransferFunctionViewModelRangeTest::RunTest(const FString& Parameters)
{
	FCFDVizCase Case;
	if (!FlowVizTransferFunctionViewModelTest::LoadSampleCase(*this, Case))
	{
		return false;
	}

	FFlowVizTransferFunctionViewModel ViewModel;
	TestTrue(TEXT("binding pressure succeeds"),
		ViewModel.BindField(Case, FName(TEXT("pressure"))).IsOk());

	/* == The per-frame range is UNAVAILABLE until something supplies one ===== */
	{
		/*
		 * RULE 15 IN THE MODEL RATHER THAN IN A WIDGET. Nothing in this codebase
		 * measures a per-frame range yet. A view model that accepted
		 * CurrentFrame and quietly behaved like Global would ship a radio button
		 * that appears to do something and does not - and the resulting figure
		 * would be labelled "per-frame range" while showing a global one, which
		 * is a rule 8 mislabelling on top of a rule 15 dead control.
		 */
		TestFalse(TEXT("no per-frame range has been supplied"), ViewModel.HasCurrentFrameRange());
		TestFalse(TEXT("selecting the per-frame range is REFUSED while no producer supplies one"),
			ViewModel.SetRangeSource(EFlowVizRangeSource::CurrentFrame).IsOk());
		TestEqual(TEXT("a refused range source leaves the global range selected"),
			ViewModel.GetRangeSource(), EFlowVizRangeSource::Global);
		TestEqual(TEXT("a refused range source leaves the domain untouched"),
			ViewModel.GetRangeMin(), -62.9375f, 1.0e-4f);

		// Supply one, and the control becomes available. Without this half the
		// assertion above would be untestable: a refusal that can never be
		// lifted is indistinguishable from a feature that does not exist.
		TestTrue(TEXT("supplying a per-frame range is accepted"),
			ViewModel.SetCurrentFrameRange(-10.0f, 4.0f).IsOk());
		TestTrue(TEXT("a per-frame range is now available"), ViewModel.HasCurrentFrameRange());
		TestTrue(TEXT("selecting the per-frame range now succeeds"),
			ViewModel.SetRangeSource(EFlowVizRangeSource::CurrentFrame).IsOk());

		// The domain must actually CHANGE. Asserting only that the call succeeded
		// would pass on a view model that accepted the mode and kept the global
		// range - the exact silent equivalence the refusal exists to prevent.
		TestEqual(TEXT("the per-frame domain is the supplied one, zero-centred for a diverging map"),
			ViewModel.GetRangeMax(), 10.0f, 1.0e-5f);
		TestEqual(TEXT("and its minimum is the negation"),
			ViewModel.GetRangeMin(), -10.0f, 1.0e-5f);

		// ENGINEERING RULE 8's OTHER HALF: a per-frame range must be labelled.
		TestFalse(TEXT("a per-frame range reports itself as NOT stable across the animation"),
			ViewModel.IsRangeStableAcrossAnimation());
	}

	/* == Manual, and refusals that leave the domain alone ==================== */
	{
		TestTrue(TEXT("a manual range is accepted"), ViewModel.SetManualRange(-1.0f, 5.0f).IsOk());
		TestEqual(TEXT("selecting a manual range switches the source"),
			ViewModel.GetRangeSource(), EFlowVizRangeSource::Manual);
		// A MANUAL range is taken EXACTLY as typed, diverging map or not: the
		// user asked for these numbers. Zero-centring a typed range would
		// silently rewrite the input, which is what rule 5 forbids of data and
		// what a numeric entry box must never do either.
		TestEqual(TEXT("a manual minimum is taken exactly as typed"),
			ViewModel.GetRangeMin(), -1.0f, 1.0e-6f);
		TestEqual(TEXT("a manual maximum is taken exactly as typed"),
			ViewModel.GetRangeMax(), 5.0f, 1.0e-6f);
		TestTrue(TEXT("a manual range is stable across the animation"),
			ViewModel.IsRangeStableAcrossAnimation());

		TestFalse(TEXT("an inverted range is refused"), ViewModel.SetManualRange(5.0f, -1.0f).IsOk());
		TestEqual(TEXT("a refused inverted range leaves the previous minimum"),
			ViewModel.GetRangeMin(), -1.0f, 1.0e-6f);

		TestFalse(TEXT("a zero-width range is refused"), ViewModel.SetManualRange(2.0f, 2.0f).IsOk());
		TestFalse(TEXT("a NaN range is refused"),
			ViewModel.SetManualRange(FMath::Sqrt(-1.0f), 1.0f).IsOk());
		TestEqual(TEXT("a refused NaN leaves the previous maximum"),
			ViewModel.GetRangeMax(), 5.0f, 1.0e-6f);
	}

	/* == Reset goes back through the shared zero-centring rule =============== */
	{
		TestTrue(TEXT("resetting the range is accepted"), ViewModel.ResetRange().IsOk());
		TestEqual(TEXT("reset returns to the global source"),
			ViewModel.GetRangeSource(), EFlowVizRangeSource::Global);
		TestEqual(TEXT("reset re-derives the zero-centred domain"),
			ViewModel.GetRangeMin(), -62.9375f, 1.0e-4f);
		TestEqual(TEXT("reset re-derives the zero-centred domain"),
			ViewModel.GetRangeMax(), 62.9375f, 1.0e-4f);
	}

	/* == A field with no declared statistics says so ========================= */
	{
		// Not a hypothetical: a manifest may omit statistics entirely, and a view
		// model that silently substituted [0,1] would colour a pressure field of
		// thousands of pascals as though it were a normalised scalar - every
		// value over-range, the whole field one flag colour, and no indication
		// why.
		FCFDVizCase Stripped = Case;
		for (FCFDVizField& Field : Stripped.Fields)
		{
			Field.Statistics = FCFDVizFieldStatistics();
			Field.Display.RecommendedRange.Reset();
		}

		FFlowVizTransferFunctionViewModel Unknown;
		TestTrue(TEXT("binding a field with no statistics still succeeds"),
			Unknown.BindField(Stripped, FName(TEXT("pressure"))).IsOk());
		TestFalse(TEXT("but the range is reported as UNKNOWN rather than fabricated"),
			Unknown.IsRangeKnown());

		// It must still be VALID - a degenerate domain divides by zero in every
		// mapping - just not presented as measured.
		TestTrue(TEXT("an unknown range still yields a usable, non-degenerate domain"),
			Unknown.Validate().IsOk());
	}

	return true;
}

/* ========================================================================== */
/* What actually reaches the renderer                                         */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTransferFunctionViewModelConsumerTest,
	"FlowViz.UI.TransferFunctionViewModel.Consumer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizTransferFunctionViewModelConsumerTest::RunTest(const FString& Parameters)
{
	FCFDVizCase Case;
	if (!FlowVizTransferFunctionViewModelTest::LoadSampleCase(*this, Case))
	{
		return false;
	}

	FFlowVizTransferFunctionViewModel ViewModel;
	TestTrue(TEXT("binding U succeeds"), ViewModel.BindField(Case, FName(TEXT("U"))).IsOk());

	/* == The choices reach the ray-march constant buffer ===================== */
	{
		TestTrue(TEXT("a manual range is accepted"), ViewModel.SetManualRange(-3.5f, 11.25f).IsOk());
		TestTrue(TEXT("selecting the Y component is accepted"),
			ViewModel.SetComponent(EFlowVizComponentChoice::Y).IsOk());
		ViewModel.SetNaNColor(FLinearColor(0.125f, 0.25f, 0.375f, 1.0f));
		ViewModel.SetMaskedColor(FLinearColor(0.5f, 0.625f, 0.75f, 1.0f));
		ViewModel.SetUnderRangeColor(FLinearColor(0.875f, 0.0f, 0.0f, 1.0f));
		ViewModel.SetOverRangeColor(FLinearColor(0.0f, 0.875f, 0.0f, 1.0f));
		TestTrue(TEXT("an opacity multiplier is accepted"),
			ViewModel.SetOpacityMultiplier(0.375f).IsOk());

		// FillDefaults first, so this test cannot pass by leaving zeroes that
		// happen to match. Every member below is DIFFERENT from its default.
		FFlowVizVolumeRayMarchParameters Params;
		FlowVizRayMarch::FillDefaults(Params);

		const FCFDVizResult Applied = ViewModel.ApplyToRayMarchParameters(Params);
		TestTrue(TEXT("applying to the ray-march parameters succeeds"), Applied.IsOk());

		/*
		 * THE ASSERTIONS THAT MATTER. These read the constant buffer the compute
		 * shader is dispatched with - the actual consumer - not a getter on the
		 * view model. A view model that stored the range and never handed it over
		 * passes every round-trip test and renders the wrong colours.
		 */
		TestEqual(TEXT("the domain minimum reaches the shader parameter block"),
			Params.ValueRangeMin, -3.5f, 1.0e-6f);
		TestEqual(TEXT("the domain maximum reaches the shader parameter block"),
			Params.ValueRangeMax, 11.25f, 1.0e-6f);
		TestEqual(TEXT("the component choice reaches the shader as its own enum"),
			Params.ComponentMode, static_cast<uint32>(EFlowVizComponentMode::Y));
		TestEqual(TEXT("the opacity multiplier reaches the shader"),
			Params.OpacityMultiplier, 0.375f, 1.0e-6f);

		// The four invalid-value colours are separately colourable on purpose
		// (rule 10, VISUAL_QA rule 4). Collapsing any two of them is the failure
		// those rules name, so each is asserted individually.
		TestEqual(TEXT("the NaN colour reaches the shader"),
			Params.NaNColor.R, 0.125f, 1.0e-6f);
		TestEqual(TEXT("the masked colour reaches the shader"),
			Params.MaskedColor.G, 0.625f, 1.0e-6f);
		TestEqual(TEXT("the under-range colour reaches the shader"),
			Params.UnderRangeColor.R, 0.875f, 1.0e-6f);
		TestEqual(TEXT("the over-range colour reaches the shader"),
			Params.OverRangeColor.G, 0.875f, 1.0e-6f);
		TestFalse(TEXT("NaN and masked colours remain distinguishable in the buffer"),
			Params.NaNColor.Equals(Params.MaskedColor, 1.0e-6f));

		// Magnitude is a different code, so the mapping is not the identity on
		// one lucky value.
		TestTrue(TEXT("selecting magnitude is accepted"),
			ViewModel.SetComponent(EFlowVizComponentChoice::Magnitude).IsOk());
		TestTrue(TEXT("re-applying succeeds"), ViewModel.ApplyToRayMarchParameters(Params).IsOk());
		TestEqual(TEXT("magnitude reaches the shader as the magnitude code"),
			Params.ComponentMode, static_cast<uint32>(EFlowVizComponentMode::Magnitude));
	}

	/* == An invalid choice does not half-write the constant buffer =========== */
	{
		FFlowVizVolumeRayMarchParameters Params;
		FlowVizRayMarch::FillDefaults(Params);
		Params.ValueRangeMin = -999.0f;
		Params.ValueRangeMax = 999.0f;

		// Reach past the view model's own guards to build a state the render
		// layer must refuse. A half-populated constant buffer renders a plausible
		// wrong image, which is worse than not rendering.
		FFlowVizTransferFunctionViewModel Broken;
		TestTrue(TEXT("binding U succeeds"), Broken.BindField(Case, FName(TEXT("U"))).IsOk());

		FFlowVizOpacityCurve BadCurve;
		BadCurve.Points.Add(FFlowVizOpacityPoint{ 2.0f, 0.5f }); // position outside [0,1]
		TestFalse(TEXT("an out-of-domain opacity point is refused by the view model"),
			Broken.SetOpacityCurve(BadCurve).IsOk());

		// And the view model that refused it still applies cleanly, because the
		// refusal left it in its previous good state.
		TestTrue(TEXT("a view model that refused a bad curve still applies"),
			Broken.ApplyToRayMarchParameters(Params).IsOk());
		TestNotEqual(TEXT("a successful apply overwrote the sentinel"), Params.ValueRangeMin, -999.0f);
	}

	/* == bClampToRange reaches BOTH consumers, and changes only colour ======= */
	{
		/*
		 * THIS BLOCK USED TO PIN A GAP. FFlowVizVolumeRayMarchParameters had no
		 * clamp member, so the control reached the CPU mapping and stopped. A
		 * peer has since added SHADER_PARAMETER(uint32, bClampToRange), and the
		 * gap moved rather than closing: the flag now has somewhere to go, and if
		 * ApplyToRayMarchParameters does not write it, the user's choice stops
		 * HERE - one layer higher, with a green shader-level test beside it
		 * attesting the plumbing works.
		 *
		 * SO THE ASSERTIONS BELOW ARE DIFFERENTIAL, NOT READ-BACK. Reading
		 * Params.bClampToRange after setting it passes identically whether or not
		 * ApplyToRayMarchParameters wrote it, because FillDefaults leaves a value
		 * there either way. Each assertion instead compares the block produced
		 * with the flag SET against the block produced with it CLEAR, from the
		 * same starting state - which can only differ if the view model wrote it.
		 */
		FFlowVizTransferFunctionViewModel Clamping;
		TestTrue(TEXT("binding U succeeds"), Clamping.BindField(Case, FName(TEXT("U"))).IsOk());
		TestTrue(TEXT("a manual range is accepted"), Clamping.SetManualRange(0.0f, 10.0f).IsOk());

		/* -- The GPU consumer -------------------------------------------- */
		FFlowVizVolumeRayMarchParameters ClearParams;
		FlowVizRayMarch::FillDefaults(ClearParams);
		Clamping.SetClampToRange(false);
		TestTrue(TEXT("applying with clamping off succeeds"),
			Clamping.ApplyToRayMarchParameters(ClearParams).IsOk());

		FFlowVizVolumeRayMarchParameters SetParams;
		FlowVizRayMarch::FillDefaults(SetParams);
		Clamping.SetClampToRange(true);
		TestTrue(TEXT("applying with clamping on succeeds"),
			Clamping.ApplyToRayMarchParameters(SetParams).IsOk());

		// THE LOAD-BEARING ASSERTION. Two blocks, identical inputs but for the
		// flag, must DIFFER in the shader's clamp slot. A view model that never
		// writes it produces two identical blocks and fails here.
		TestNotEqual(
			TEXT("the clamp choice reaches the ray-march block: setting and clearing it "
				 "produce different constant buffers"),
			SetParams.bClampToRange, ClearParams.bClampToRange);
		TestEqual(TEXT("clamping on reaches the shader as 1"), SetParams.bClampToRange, 1u);
		TestEqual(TEXT("clamping off reaches the shader as 0"), ClearParams.bClampToRange, 0u);

		/* -- The CPU consumer -------------------------------------------- */
		TArray<FLinearColor> Lut;
		TestTrue(TEXT("the LUT builds"),
			FlowVizTransferFunction::BuildLut(Clamping.GetTransferFunction(), Lut).IsOk());

		// 20.0 is over the [0,10] domain: unclamped it takes the over-range flag
		// colour, clamped it takes the colormap's top. Differential again.
		Clamping.SetClampToRange(false);
		const FLinearColor Unclamped = Clamping.GetTransferFunction().EvaluateColor(Lut, 20.0f);
		Clamping.SetClampToRange(true);
		const FLinearColor Clamped = Clamping.GetTransferFunction().EvaluateColor(Lut, 20.0f);
		TestFalse(TEXT("the CPU colour mapping observes bClampToRange"),
			Unclamped.Equals(Clamped, 1.0e-6f));

		/* -- And it is a COLOUR choice, not a value one ------------------- */
		/*
		 * The flag may change what is drawn and must never change what is
		 * reported. If clamping ever moved the classification, an over-range
		 * value would be presented as an in-range one - the display choice
		 * becoming a quantitative claim, which is the failure the flag colours
		 * exist to prevent. Asserted here on the CPU classifier because that is
		 * the primitive both paths are specified against.
		 */
		FFlowVizTransferFunction ClampOff = Clamping.GetTransferFunction();
		ClampOff.bClampToRange = false;
		FFlowVizTransferFunction ClampOn = Clamping.GetTransferFunction();
		ClampOn.bClampToRange = true;

		const FFlowVizMappedValue MappedOff = ClampOff.MapValueToNormalized(20.0f);
		const FFlowVizMappedValue MappedOn = ClampOn.MapValueToNormalized(20.0f);
		TestEqual(TEXT("clamping does NOT change the classification of an over-range value"),
			static_cast<int32>(MappedOn.ValueClass), static_cast<int32>(MappedOff.ValueClass));
		TestFalse(TEXT("an over-range value stays classified as out-of-range when clamped"),
			MappedOn.IsInRange());
	}

	return true;
}

/* ========================================================================== */
/* Colormap metadata the UI must disclose                                     */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTransferFunctionViewModelColorMapTest,
	"FlowViz.UI.TransferFunctionViewModel.ColorMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizTransferFunctionViewModelColorMapTest::RunTest(const FString& Parameters)
{
	FCFDVizCase Case;
	if (!FlowVizTransferFunctionViewModelTest::LoadSampleCase(*this, Case))
	{
		return false;
	}

	FFlowVizTransferFunctionViewModel ViewModel;
	TestTrue(TEXT("binding U succeeds"), ViewModel.BindField(Case, FName(TEXT("U"))).IsOk());

	/* == Perceptual uniformity is disclosed, not enforced ==================== */
	{
		// plan.md ships Turbo, and the UI must mark it: a user who selects it
		// should know what they selected. Refusing it outright would be wrong -
		// it is a legitimate choice - so the model reports rather than blocks.
		TestTrue(TEXT("selecting viridis is accepted"),
			ViewModel.SetColorMap(ECFDVizColorMap::Viridis).IsOk());
		TestTrue(TEXT("viridis is perceptually uniform"),
			ViewModel.IsColorMapPerceptuallyUniform());

		TestTrue(TEXT("selecting turbo is accepted, not blocked"),
			ViewModel.SetColorMap(ECFDVizColorMap::Turbo).IsOk());
		TestFalse(TEXT("turbo reports itself as NOT perceptually uniform, so the UI can mark it"),
			ViewModel.IsColorMapPerceptuallyUniform());
	}

	/* == Changing the map does not silently rewrite a typed domain =========== */
	{
		TestTrue(TEXT("a manual range is accepted"), ViewModel.SetManualRange(2.0f, 8.0f).IsOk());

		// Switching TO a diverging map must not re-centre what the user typed.
		// Losing a typed domain on a colormap change is the kind of thing that
		// looks like a rendering bug and is a UI bug.
		TestTrue(TEXT("switching to coolwarm is accepted"),
			ViewModel.SetColorMap(ECFDVizColorMap::CoolWarm).IsOk());
		TestEqual(TEXT("a typed minimum survives a colormap change"),
			ViewModel.GetRangeMin(), 2.0f, 1.0e-6f);
		TestEqual(TEXT("a typed maximum survives a colormap change"),
			ViewModel.GetRangeMax(), 8.0f, 1.0e-6f);

		// An EXPLICIT reset is how a user asks for the map's preferred domain,
		// and now the diverging rule applies. U's global magnitude range is
		// [0.9291, 13.4972], so a zero-centred domain is +/- 13.4972.
		TestTrue(TEXT("an explicit reset is accepted"), ViewModel.ResetRange().IsOk());
		TestEqual(TEXT("reset under a diverging map centres on zero"),
			ViewModel.GetRangeMin(), -13.4972035f, 1.0e-4f);
		TestEqual(TEXT("reset under a diverging map centres on zero"),
			ViewModel.GetRangeMax(), 13.4972035f, 1.0e-4f);
	}

	/* == Bands and reversal reach the LUT, which is their consumer ============ */
	{
		TestTrue(TEXT("selecting viridis is accepted"),
			ViewModel.SetColorMap(ECFDVizColorMap::Viridis).IsOk());
		ViewModel.SetReverseColorMap(false);
		TestTrue(TEXT("a continuous map is accepted"), ViewModel.SetColorBands(0).IsOk());

		TArray<FLinearColor> Continuous;
		TestTrue(TEXT("the continuous LUT builds"),
			FlowVizTransferFunction::BuildLut(ViewModel.GetTransferFunction(), Continuous).IsOk());

		// FOUR BANDS OVER 256 ENTRIES: entries 0 and 1 fall in the same band and
		// must be EQUAL, which a continuous map cannot produce. This is a
		// property a wrong stored band count cannot fake.
		TestTrue(TEXT("four bands are accepted"), ViewModel.SetColorBands(4).IsOk());
		TArray<FLinearColor> Banded;
		TestTrue(TEXT("the banded LUT builds"),
			FlowVizTransferFunction::BuildLut(ViewModel.GetTransferFunction(), Banded).IsOk());

		/*
		 * RGB ONLY, AND THE ALPHA ASSERTION BESIDE IT IS THE POINT.
		 *
		 * FLinearColor::Equals compares alpha, and alpha here is the OPACITY
		 * CURVE, which banding must not touch (FlowVizTransferFunction.h rule 1:
		 * opacity is not baked into the colour table). An earlier draft of this
		 * test compared whole colours and failed - correctly, on a correct
		 * implementation - because entries 0 and 1 share a colour band and do NOT
		 * share an opacity. Comparing RGB and asserting alpha DIFFERS states both
		 * halves of that rule instead of one.
		 */
		auto RgbEquals = [](const FLinearColor& A, const FLinearColor& B)
		{
			return FMath::IsNearlyEqual(A.R, B.R, 1.0e-6f)
				&& FMath::IsNearlyEqual(A.G, B.G, 1.0e-6f)
				&& FMath::IsNearlyEqual(A.B, B.B, 1.0e-6f);
		};

		if (Continuous.Num() >= 2 && Banded.Num() >= 2)
		{
			TestFalse(TEXT("a continuous LUT's first two entries differ in colour"),
				RgbEquals(Continuous[0], Continuous[1]));
			TestTrue(TEXT("a 4-band LUT quantises its first two entries into one colour band"),
				RgbEquals(Banded[0], Banded[1]));

			// Banding is a COLOUR control. If it ever quantised the opacity ramp
			// too, an author banding a figure for legibility would silently also
			// step its transparency.
			TestNotEqual(TEXT("banding does NOT quantise the opacity ramp"),
				Banded[0].A, Banded[1].A);
		}

		// Reversal: the first entry of a reversed map must equal the last entry
		// of the unreversed one. A stored-and-ignored flag fails this.
		TestTrue(TEXT("a continuous map is accepted"), ViewModel.SetColorBands(0).IsOk());
		ViewModel.SetReverseColorMap(true);
		TArray<FLinearColor> Reversed;
		TestTrue(TEXT("the reversed LUT builds"),
			FlowVizTransferFunction::BuildLut(ViewModel.GetTransferFunction(), Reversed).IsOk());

		if (Continuous.Num() > 0 && Reversed.Num() == Continuous.Num())
		{
			// RGB again, for the same reason: reversal flips the colour table and
			// leaves the opacity ramp running the same direction. Comparing whole
			// colours here would demand that an opacity ramp reverse with the
			// colormap, which would make "reverse colormap" silently also invert
			// which values are transparent.
			TestTrue(TEXT("reversal reaches the LUT: its first colour is the unreversed last"),
				RgbEquals(Reversed[0], Continuous.Last()));
			TestNotEqual(TEXT("reversing the colormap does NOT reverse the opacity ramp"),
				Reversed[0].A, Continuous.Last().A);
		}

		TestFalse(TEXT("a negative band count is refused"), ViewModel.SetColorBands(-1).IsOk());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
