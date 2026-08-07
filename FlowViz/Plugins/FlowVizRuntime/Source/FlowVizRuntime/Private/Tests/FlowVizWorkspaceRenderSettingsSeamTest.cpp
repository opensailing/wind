// Copyright FlowViz contributors. All Rights Reserved.

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Render/FlowVizVolumeRayMarchShader.h"
#include "Scene/FlowVizCaseActor.h"
#include "Scene/FlowVizVolumeComponent.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "UI/SFlowVizRenderSettingsPanel.h"
#include "UI/SFlowVizWorkspace.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Input/SButton.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED NAMESPACE, NOT ANONYMOUS -- FlowVizRuntime is a unity build and an
 * anonymous namespace merges with every other one in the same blob rather than
 * being file-local (#37).
 */
namespace FlowVizWorkspaceRenderSettingsSeamTest
{
	/**
	 * NONE OF THESE IS A DEFAULT. A default-constructed render settings view
	 * model is an identity over FillDefaults' output -- deliberately, and it is
	 * asserted by FlowViz.UI.RenderSettings -- so a fixture built from defaults
	 * would make a wired push and a missing one produce identical readings at
	 * every assertion. Repo memory degenerate-data-defeats-assertions.
	 *
	 * Maximum rather than IsoSurface: MIP needs no companion threshold, so a
	 * broken iso value cannot confound the mode assertion.
	 */
	constexpr EFlowVizCompositeMode TestMode = EFlowVizCompositeMode::Maximum;
	constexpr float TestStepVoxels = 0.375f;
	constexpr uint32 TestMaxSteps = 640u;

	/*
	 * NO SAMPLE CASE IS LOADED ANYWHERE IN THIS FILE. Render settings are
	 * value state -- a composite mode means the same thing with no case open --
	 * so loading one would add the reader, the decoder and the fixture as ways
	 * for this test to go red for a reason that is not the seam. It is also the
	 * stronger fixture: no case open is the state in which the CLIP push
	 * refuses, so these arms double as proof that PushToVolume does not
	 * short-circuit this channel behind the clip's failure.
	 */

	UWorld* MakeWorld(FWorldContext& OutContext)
	{
		// GLOBALLY UNIQUE NAME, matching the sibling seam tests. Two tests that
		// both create an unnamed world in the transient package get the same
		// object name, and the second one fails for a reason that is not its
		// subject.
		const FName WorldName = MakeUniqueObjectName(
			nullptr, UWorld::StaticClass(), NAME_None, EUniqueObjectNameOptions::GloballyUnique);

		UWorld* World = UWorld::CreateWorld(
			EWorldType::Game, /*bInformEngineOfWorld*/ false, WorldName, GetTransientPackage());
		if (World == nullptr)
		{
			return nullptr;
		}
		World->AddToRoot();
		OutContext.SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());
		return World;
	}

	/** Configure settings the way a user would, and prove each step took. */
	bool Configure(FFlowVizRenderSettingsViewModel& Model, FAutomationTestBase& Test)
	{
		Model.SetCompositeMode(TestMode);

		if (!Test.TestTrue(TEXT("CONTROL: the fixture's step size was accepted"),
				Model.SetStepVoxels(TestStepVoxels)))
		{
			return false;
		}
		Model.SetMaxSteps(TestMaxSteps);

		// READ BACK, not assumed. A setter that stored nothing would pass the
		// checks above.
		return Test.TestEqual(TEXT("CONTROL: the fixture really holds the chosen mode"),
				   Model.GetCompositeMode(), TestMode)
			&& Test.TestEqual(TEXT("CONTROL: and the chosen step"),
				   Model.GetStepVoxels(), TestStepVoxels);
	}
}

/**
 * RENDER SETTINGS CHOSEN IN THE WORKSPACE REACH THE RENDER-THREAD PAYLOAD.
 *
 * FlowViz.Render.RenderSettingsSeam proved the dispatcher applies a settings
 * model the COMPONENT holds. This proves the workspace can put one there:
 * PushRenderSettingsToVolume, the component's setter, and MakeProxyDynamicData's
 * copy. Without this channel the panel edits FFlowVizWorkspaceModel::
 * RenderSettings and nothing downstream reads it -- the exact shape #50
 * documented for the clip model, one channel over.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlowVizWorkspaceRenderSettingsSeamTest,
	"FlowViz.UI.Workspace.RenderSettingsSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceRenderSettingsSeamTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceRenderSettingsSeamTest;

	UCFDVizVolumeComponent* Volume = NewObject<UCFDVizVolumeComponent>();
	if (!TestNotNull(TEXT("CONTROL: a volume component was constructed"), Volume))
	{
		return false;
	}
	Volume->AddToRoot();
	ON_SCOPE_EXIT { Volume->RemoveFromRoot(); };

	{
		TestEqual(TEXT("CONTROL: the bare component holds the default mode, so what lands "
					   "below cannot be state that was already there"),
			Volume->GetRenderSettings().GetCompositeMode(), EFlowVizCompositeMode::Alpha);

		FFlowVizRenderSettingsViewModel Source;
		if (!Configure(Source, *this))
		{
			return false;
		}

		TestTrue(TEXT("pushing render settings into a volume with NO CASE BOUND succeeds -- "
					  "a composite mode is value state, there is no domain to refuse for"),
			FFlowVizWorkspaceModel::PushRenderSettingsToVolume(Source, Volume));

		TestEqual(TEXT("the chosen mode reaches the component"),
			Volume->GetRenderSettings().GetCompositeMode(), TestMode);

		/* == And through to the payload the render thread receives ========== */

		const FFlowVizVolumeProxyDynamicData Payload = Volume->MakeProxyDynamicData();

		TestEqual(TEXT("the mode is in the marshalled payload, so the proxy hands the "
					   "dispatcher the user's mode rather than a default"),
			Payload.RenderSettings.GetCompositeMode(), TestMode);

		// THE MARCHING ROWS TOO, not just the mode. The dispatcher applies the
		// whole block in one call, but a push that hand-copied fields would
		// fail per-field -- and a payload carrying the mode alone would MIP at
		// the wrong step size, which no readout disagrees with.
		TestEqual(TEXT("and the step size"),
			Payload.RenderSettings.GetStepVoxels(), TestStepVoxels);
		TestEqual(TEXT("and the step ceiling"),
			Payload.RenderSettings.GetMaxSteps(), TestMaxSteps);
	}

	/* == Null is refused, not dereferenced =================================== */
	{
		FFlowVizRenderSettingsViewModel Source;
		TestFalse(TEXT("pushing into a null component reports failure rather than crashing"),
			FFlowVizWorkspaceModel::PushRenderSettingsToVolume(Source, nullptr));
	}

	/* == A second push REPLACES ============================================= */
	/*
	 * Repo memory accumulators-need-a-second-write: a seam that assigned once
	 * and ignored later pushes is indistinguishable from a correct one until
	 * something is written twice. The second value is NOT the default --
	 * returning to Alpha would also be the reading of a seam that reset to its
	 * constant.
	 */
	{
		FFlowVizRenderSettingsViewModel Second;
		Second.SetCompositeMode(EFlowVizCompositeMode::Average);

		TestTrue(TEXT("a second push succeeds"),
			FFlowVizWorkspaceModel::PushRenderSettingsToVolume(Second, Volume));

		TestEqual(TEXT("and REPLACES the first, so changing the mode twice does not leave the "
					   "volume rendering the first choice forever"),
			Volume->MakeProxyDynamicData().RenderSettings.GetCompositeMode(),
			EFlowVizCompositeMode::Average);
	}

	return true;
}

/**
 * THE PANEL'S EDIT REACHES THE BOUND VOLUME, through the workspace.
 *
 * The seam test above proves the push works when called. This proves the
 * WORKSPACE CALLS IT: that it built a render settings panel at all, subscribed
 * to its delegate, and that PushToVolume carries this channel too. Those are
 * separate failures and each shape has shipped here -- #40 a mechanism with no
 * assignment, #41 a setter with no caller, #48 a panel with no delegate, #74
 * a view model with no production caller for 14 of its 17 setters.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlowVizWorkspaceRenderSettingsBindingTest,
	"FlowViz.UI.Workspace.RenderSettingsBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceRenderSettingsBindingTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceRenderSettingsSeamTest;

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	UWorld* World = MakeWorld(WorldContext);
	if (!TestNotNull(TEXT("CONTROL: a test world was created"), World))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		World->DestroyWorld(/*bInformEngineOfWorld*/ true);
		World->RemoveFromRoot();
		GEngine->DestroyWorldContext(World);
	};

	ACFDVizCaseActor* Actor = World->SpawnActor<ACFDVizCaseActor>();
	if (!TestNotNull(TEXT("CONTROL: a case actor spawns"), Actor))
	{
		return false;
	}
	UCFDVizVolumeComponent* Volume = Actor->GetVolumeComponent();
	if (!TestNotNull(TEXT("CONTROL: the actor owns a volume component"), Volume))
	{
		return false;
	}

	const TSharedRef<SFlowVizWorkspace> Workspace = SNew(SFlowVizWorkspace);
	Workspace->SetVolume(Volume);

	const TSharedPtr<SFlowVizRenderSettingsPanel> Panel = Workspace->GetRenderSettingsPanel();
	if (!TestTrue(
			TEXT("the workspace built a render settings panel; if this fails the 16 controls "
				 "2e names have working view model plumbing and no user-facing surface -- "
				 "which is #74's finding restated as a widget"),
			Panel.IsValid()))
	{
		return false;
	}

	TestEqual(TEXT("CONTROL: the volume has no case, which is the state in which the clip "
				   "push fails and a short-circuiting PushToVolume would skip this channel"),
		Volume->GetPhysicalSize().X, 0.0);

	TestEqual(TEXT("CONTROL: the bound volume holds the default mode before the panel is "
				   "touched"),
		Volume->GetRenderSettings().GetCompositeMode(), EFlowVizCompositeMode::Alpha);

	const TSharedPtr<SButton> ModeButton = Panel->GetCompositeModeButton(TestMode);
	if (!TestTrue(TEXT("CONTROL: the panel built a button for the chosen mode"),
			ModeButton.IsValid()))
	{
		return false;
	}

	// THROUGH THE BUTTON, not the view model. Calling the model directly would
	// pass against a panel that announces nothing (#48's exact defect), and
	// against a workspace that never subscribed.
	ModeButton->SimulateClick();

	TestEqual(TEXT("CONTROL: the click changed the workspace's own model"),
		Workspace->GetModel().RenderSettings.GetCompositeMode(), TestMode);

	TestEqual(TEXT("clicking a mode in the panel reaches the BOUND VOLUME, so the picker and "
				   "the render agree rather than the volume staying in Alpha"),
		Volume->GetRenderSettings().GetCompositeMode(), TestMode);

	TestEqual(TEXT("and reaches the render-thread payload, which is what the proxy reads"),
		Volume->MakeProxyDynamicData().RenderSettings.GetCompositeMode(), TestMode);

	/* == Binding pushes what is already there =============================== */
	/*
	 * #60's requirement, third channel: settings chosen before the actor
	 * existed -- a session load, or picking MIP while the case dialog was open
	 * -- must not wait for the next edit to take effect.
	 */
	{
		ACFDVizCaseActor* SecondActor = World->SpawnActor<ACFDVizCaseActor>();
		if (!TestNotNull(TEXT("CONTROL: a second case actor spawns"), SecondActor))
		{
			return false;
		}
		UCFDVizVolumeComponent* SecondVolume = SecondActor->GetVolumeComponent();
		if (!TestNotNull(TEXT("CONTROL: the second actor owns a volume"), SecondVolume))
		{
			return false;
		}

		TestEqual(TEXT("CONTROL: the fresh volume holds the default mode, so what lands below "
					   "cannot be state it already had"),
			SecondVolume->GetRenderSettings().GetCompositeMode(), EFlowVizCompositeMode::Alpha);

		// The workspace's model still holds TestMode from the click above.
		Workspace->SetVolume(SecondVolume);

		TestEqual(TEXT("binding a volume pushes the settings already chosen, so a mode picked "
					   "before the case was open does not wait for the next edit"),
			SecondVolume->GetRenderSettings().GetCompositeMode(), TestMode);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
