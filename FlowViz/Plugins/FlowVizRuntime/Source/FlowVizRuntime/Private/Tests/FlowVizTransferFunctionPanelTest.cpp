// Copyright FlowViz contributors. All Rights Reserved.

#include "UI/SFlowVizTransferFunctionPanel.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "Widgets/Input/SButton.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/FlowVizSlateAttributePump.h"

/**
 * The transfer-function panel's controls must drive the transfer-function view
 * model, and rule 8's advisory must actually appear.
 *
 * As in FlowVizTransportBarTest, nothing here calls a view model setter to make
 * an assertion true: every state change goes through SButton::SimulateClick or a
 * committed text box, which runs the production delegate. A panel whose buttons
 * were wired to nothing fails at the first assertion in each block.
 *
 * DIFFERENTIAL PROPERTY. Remove the `.OnClicked(...)` from the colormap buttons
 * in SFlowVizTransferFunctionPanel::Construct and
 * FlowViz.UI.TransferFunctionPanel.ControlsDriveTheViewModel goes red on
 * "clicking a colormap button selects it".
 */

// NAMED namespace: unity build. See the note in every other UI test here.
namespace FlowVizTransferFunctionPanelTest
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTransferFunctionPanelBindingTest,
	"FlowViz.UI.TransferFunctionPanel.ControlsDriveTheViewModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizTransferFunctionPanelBindingTest::RunTest(const FString& Parameters)
{
	const FString CaseDir = FlowVizTransferFunctionPanelTest::GetSampleCaseDir();
	if (CaseDir.IsEmpty() || !FPaths::DirectoryExists(CaseDir))
	{
		AddError(FString::Printf(TEXT("the sample case is required; not found at '%s'"), *CaseDir));
		return false;
	}

	FFlowVizWorkspaceModel Workspace;
	const FCFDVizResult OpenResult = Workspace.OpenCase(CaseDir);
	if (!OpenResult.IsOk())
	{
		AddError(FString::Printf(TEXT("could not open the sample case: %s"), *OpenResult.ToString()));
		return false;
	}

	if (!TestTrue(TEXT("the transfer function bound to a field, so its controls are live"),
			Workspace.TransferFunction.IsBound()))
	{
		return false;
	}

	const TSharedRef<SFlowVizTransferFunctionPanel> Panel = SNew(SFlowVizTransferFunctionPanel)
		.ViewModel(&Workspace.TransferFunction);

	/* == Clicking a colormap button selects that colormap ==================== */
	{
		// Establish a DIFFERENT starting map, so "it was already Plasma" cannot
		// make the assertion pass without the click doing anything.
		Workspace.TransferFunction.SetColorMap(ECFDVizColorMap::Viridis);
		TestEqual(TEXT("precondition: the map is Viridis"),
			Workspace.TransferFunction.GetTransferFunction().ColorMap, ECFDVizColorMap::Viridis);

		const TSharedPtr<SButton> PlasmaButton =
			Panel->GetColorMapButton(ECFDVizColorMap::Plasma);
		if (!TestTrue(TEXT("the panel built a button for Plasma"), PlasmaButton.IsValid()))
		{
			return false;
		}

		PlasmaButton->SimulateClick();

		TestEqual(
			TEXT("clicking a colormap button selects it; if this fails the button is not wired "
				 "to the transfer function view model"),
			Workspace.TransferFunction.GetTransferFunction().ColorMap, ECFDVizColorMap::Plasma);
	}

	/* == The ramp strip paints the SELECTED map, not a fixed one ============= */
	{
		const TSharedPtr<SFlowVizColorRampStrip> Strip = Panel->GetRampStrip();
		if (!TestTrue(TEXT("the panel built a ramp strip"), Strip.IsValid()))
		{
			return false;
		}

		constexpr int32 SampleCount = 32;

		Workspace.TransferFunction.SetColorMap(ECFDVizColorMap::Viridis);
		const TArray<FLinearColor> ViridisRamp = Strip->BuildRampColors(SampleCount);

		Workspace.TransferFunction.SetColorMap(ECFDVizColorMap::Inferno);
		const TArray<FLinearColor> InfernoRamp = Strip->BuildRampColors(SampleCount);

		// COUNT ASSERTED BEFORE CONTENT. An empty array compares equal to another
		// empty array, so "the two ramps differ" on two empty ramps would be a
		// silent false pass - and an empty ramp is exactly what an unbound or
		// broken strip returns.
		if (!TestEqual(TEXT("the ramp produced the requested number of samples"),
				ViridisRamp.Num(), SampleCount)
			|| !TestEqual(TEXT("the second ramp produced the requested number of samples"),
				InfernoRamp.Num(), SampleCount))
		{
			return false;
		}

		bool bAnyDifferent = false;
		for (int32 Index = 0; Index < SampleCount; ++Index)
		{
			if (!ViridisRamp[Index].Equals(InfernoRamp[Index], 0.01f))
			{
				bAnyDifferent = true;
				break;
			}
		}
		TestTrue(
			TEXT("the painted ramp follows the selected colormap; identical ramps for Viridis "
				 "and Inferno mean the strip is drawing a hard-coded gradient"),
			bAnyDifferent);

		// And the ramp must be the REAL table, not an interpolation between two
		// endpoint colours. Viridis' midpoint is a green; a two-stop lerp from its
		// dark-blue start to its yellow end would be a desaturated grey-green.
		Workspace.TransferFunction.SetColorMap(ECFDVizColorMap::Viridis);
		const TArray<FLinearColor> Ramp = Strip->BuildRampColors(SampleCount);
		const FLinearColor Mid = Ramp[SampleCount / 2];
		const FLinearColor Reference = CFDViz::ColorMaps::Sample(ECFDVizColorMap::Viridis, 0.5f);
		TestTrue(
			*FString::Printf(
				TEXT("the ramp samples the real colormap table; midpoint (%g,%g,%g) vs table "
					 "(%g,%g,%g)"),
				Mid.R, Mid.G, Mid.B, Reference.R, Reference.G, Reference.B),
			Mid.Equals(Reference, 0.05f));
	}

	/* == Rule 15: the per-frame button is disabled until the mode is legal === */
	{
		// The view model REFUSES CurrentFrame until a per-frame range has been
		// measured. The button must agree, or it is a control that silently does
		// nothing - and this assertion is what caught exactly that defect.
		const int32 CurrentFrameIndex = static_cast<int32>(EFlowVizRangeSource::CurrentFrame);
		const TSharedPtr<SButton> FrameRangeButton =
			Panel->GetRangeSourceButton(CurrentFrameIndex);
		if (!TestTrue(TEXT("the panel built a per-frame range button"), FrameRangeButton.IsValid()))
		{
			return false;
		}

		TestFalse(TEXT("precondition: no per-frame range has been measured yet"),
			Workspace.TransferFunction.HasCurrentFrameRange());

		// Evaluate the bound attributes, as a drawn frame would. Without this the
		// assertion below reads the constructor's default `true` (SWidget.cpp:248)
		// instead of the predicate - see FlowVizSlateAttributePump.h.
		FlowVizSlateAttributePump::Pump(Panel);

		TestFalse(
			TEXT("the per-frame range button is disabled while the view model would refuse the "
				 "mode (engineering rule 15: disabled and refused must be one fact)"),
			FrameRangeButton->IsEnabled());
	}

	/* == Rule 8: opting into a per-frame range raises a VISIBLE advisory ===== */
	{
		Workspace.TransferFunction.SetRangeSource(EFlowVizRangeSource::Global);
		TestFalse(
			TEXT("a stable global range raises no advisory, so the advisory means something "
				 "when it does appear"),
			Panel->IsRangeAdvisoryVisible());

		// Make the mode legal, exactly as the renderer will once it measures a
		// frame's extent. Done through the view model rather than a widget because
		// this is NOT a UI action - no button supplies a measurement.
		const FCFDVizResult FrameRange =
			Workspace.TransferFunction.SetCurrentFrameRange(0.25f, 3.75f);
		if (!TestTrue(
				*FString::Printf(TEXT("supplying a per-frame range succeeds: %s"),
					*FrameRange.ToString()),
				FrameRange.IsOk()))
		{
			return false;
		}

		const int32 CurrentFrameIndex = static_cast<int32>(EFlowVizRangeSource::CurrentFrame);
		const TSharedPtr<SButton> FrameRangeButton =
			Panel->GetRangeSourceButton(CurrentFrameIndex);

		// Re-evaluate: the view model changed, and a live frame would have picked
		// that up before the user saw the button.
		FlowVizSlateAttributePump::Pump(Panel);

		// THE SAME BUTTON that was disabled above must now be live. This pair is
		// what makes the disabled assertion a check on the PREDICATE rather than on
		// a button that is simply always dead - and, given the pump, it is also the
		// proof that the pump reaches this widget at all. A pump that silently
		// missed the button would leave it at the constructor's `true`, and the
		// FALSE assertion above would fail.
		TestTrue(
			TEXT("the per-frame button becomes enabled once the mode is legal"),
			FrameRangeButton->IsEnabled());

		// Drive it through the BUTTON, so this also tests the range-source wiring.
		FrameRangeButton->SimulateClick();

		TestEqual(
			TEXT("clicking the per-frame range button changes the range source"),
			Workspace.TransferFunction.GetRangeSource(), EFlowVizRangeSource::CurrentFrame);

		TestFalse(TEXT("the view model agrees the range is no longer stable"),
			Workspace.TransferFunction.IsRangeStableAcrossAnimation());

		// THE LOAD-BEARING RULE 8 ASSERTION.
		TestTrue(
			TEXT("a per-frame range is VISIBLY indicated (engineering rule 8); without this "
				 "banner a colour change between frames reads as a physical change"),
			Panel->IsRangeAdvisoryVisible());

		TestFalse(TEXT("the advisory actually says something"),
			Panel->GetRangeAdvisoryText().IsEmpty());
	}

	/* == A non-perceptually-uniform map is marked =========================== */
	{
		Workspace.TransferFunction.SetColorMap(ECFDVizColorMap::Viridis);
		TestTrue(TEXT("precondition: Viridis is perceptually uniform"),
			Workspace.TransferFunction.IsColorMapPerceptuallyUniform());
		TestTrue(TEXT("a perceptually uniform map carries no warning"),
			Panel->GetColorMapWarningText().IsEmpty());

		Workspace.TransferFunction.SetColorMap(ECFDVizColorMap::Turbo);
		TestFalse(TEXT("precondition: Turbo is not perceptually uniform"),
			Workspace.TransferFunction.IsColorMapPerceptuallyUniform());

		// The inverse of the assertion above, which is what stops a panel that
		// simply never warns from passing both.
		TestFalse(
			TEXT("a rainbow-family map is marked, because it fabricates edges where the data "
				 "is smooth"),
			Panel->GetColorMapWarningText().IsEmpty());
	}

	/* == Manual range boxes commit to the view model ======================== */
	{
		Workspace.TransferFunction.SetRangeSource(EFlowVizRangeSource::Manual);
		Workspace.TransferFunction.SetManualRange(0.0f, 1.0f);

		const TSharedPtr<SFlowVizNumericEntry> MinBox = Panel->GetRangeMinBox();
		const TSharedPtr<SFlowVizNumericEntry> MaxBox = Panel->GetRangeMaxBox();
		if (!TestTrue(TEXT("the panel built range entry boxes"),
				MinBox.IsValid() && MaxBox.IsValid()))
		{
			return false;
		}

		// SimulateCommit, not SetText: SetText only changes the displayed string
		// and never fires OnTextCommitted, so a test built on it could not observe
		// the handler at all. See SFlowVizNumericEntry.
		//
		// Committed in max-then-min order: the view model rejects an inverted
		// range, so setting min to 2.5 while max is still 1.0 would be refused and
		// the test would be asserting against a refusal rather than a binding.
		MaxBox->SimulateCommit(FText::FromString(TEXT("7.5")));
		MinBox->SimulateCommit(FText::FromString(TEXT("2.5")));

		TestEqual(TEXT("committing the max box sets the view model's range max"),
			Workspace.TransferFunction.GetRangeMax(), 7.5f);
		TestEqual(TEXT("committing the min box sets the view model's range min"),
			Workspace.TransferFunction.GetRangeMin(), 2.5f);
	}

	/* == Reverse toggles ==================================================== */
	{
		Workspace.TransferFunction.SetReverseColorMap(false);
		const TSharedPtr<SButton> Reverse = Panel->GetReverseButton();
		if (!TestTrue(TEXT("the panel built a reverse button"), Reverse.IsValid()))
		{
			return false;
		}

		Reverse->SimulateClick();
		TestTrue(TEXT("clicking reverse reverses the colormap"),
			Workspace.TransferFunction.IsColorMapReversed());

		Reverse->SimulateClick();
		TestFalse(TEXT("clicking reverse again un-reverses it, so it is a toggle"),
			Workspace.TransferFunction.IsColorMapReversed());
	}

	return true;
}

/* ========================================================================== */
/* Unbound                                                                     */
/* ========================================================================== */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizTransferFunctionPanelUnboundTest,
	"FlowViz.UI.TransferFunctionPanel.Unbound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizTransferFunctionPanelUnboundTest::RunTest(const FString& Parameters)
{
	const TSharedRef<SFlowVizTransferFunctionPanel> Panel = SNew(SFlowVizTransferFunctionPanel);

	const TSharedPtr<SButton> ViridisButton = Panel->GetColorMapButton(ECFDVizColorMap::Viridis);
	if (!TestTrue(TEXT("an unbound panel still builds its controls"), ViridisButton.IsValid()))
	{
		return false;
	}

	FlowVizSlateAttributePump::Pump(Panel);

	TestFalse(TEXT("with no view model the colormap buttons are disabled (rule 15)"),
		ViridisButton->IsEnabled());

	// SimulateClick bypasses the enabled check, so this proves the HANDLER is
	// null-safe rather than merely unreachable.
	ViridisButton->SimulateClick();

	const TSharedPtr<SFlowVizColorRampStrip> Strip = Panel->GetRampStrip();
	if (TestTrue(TEXT("an unbound panel still builds a ramp strip"), Strip.IsValid()))
	{
		// An unbound strip must return an EMPTY ramp rather than a default one: a
		// ramp drawn with no field bound would show a colour scale for data that is
		// not there.
		TestEqual(TEXT("an unbound ramp strip paints nothing rather than a fabricated scale"),
			Strip->BuildRampColors(16).Num(), 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
