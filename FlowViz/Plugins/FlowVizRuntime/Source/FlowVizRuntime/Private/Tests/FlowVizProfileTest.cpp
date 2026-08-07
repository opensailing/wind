// Copyright FlowViz contributors. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "UI/FlowVizWorkspaceModel.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The Scientific/Presentation profile toggle (#83 / Milestone F).
 *
 * What it honestly does: owns the session's bPresentationMode and applies a
 * preset bundle on switch. What it does NOT do -- VISUAL_QA section 2's
 * film-grade bar -- is stated in its header rather than implied by the name.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizProfileTest,
	"FlowViz.UI.WorkspaceModel.Profiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizProfileTest::RunTest(const FString& Parameters)
{
	FFlowVizWorkspaceModel Model;

	/* == Scientific is the default -- the mode whose fidelity is stated ====== */
	TestFalse(TEXT("the default profile is Scientific (VISUAL_QA: Presentation must never "
				   "be the default for quantitative work)"),
		Model.IsPresentationMode());
	TestFalse(TEXT("and its render state is unlit"),
		Model.RenderSettings.IsLightingEnabled());

	/* == Entering Presentation applies the bundle ============================ */
	Model.SetPresentationMode(true);
	TestTrue(TEXT("the flag follows"), Model.IsPresentationMode());
	TestTrue(TEXT("presentation lights the volume"),
		Model.RenderSettings.IsLightingEnabled());
	TestTrue(TEXT("and enables jitter"), Model.RenderSettings.IsJitterEnabled());

	/* == The user may then adjust freely -- the profile applies ONCE ========= */
	Model.RenderSettings.SetLightingEnabled(false);
	TestTrue(TEXT("hand-disabling lighting does not flip the profile -- the profile is a "
				  "selection, not a constraint that fights the render panel"),
		Model.IsPresentationMode());

	/* == Entering Scientific SETS the honest state =========================== */
	Model.RenderSettings.SetLightingEnabled(true);
	Model.SetPresentationMode(false);
	TestFalse(TEXT("scientific turns lighting off -- rule 1: lighting must not modulate "
				   "apparent scalar value, whatever was tuned before"),
		Model.RenderSettings.IsLightingEnabled());
	TestFalse(TEXT("and jitter off (ADR 002)"), Model.RenderSettings.IsJitterEnabled());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
