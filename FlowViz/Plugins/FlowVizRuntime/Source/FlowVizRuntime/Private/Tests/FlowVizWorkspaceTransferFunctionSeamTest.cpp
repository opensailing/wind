// Copyright FlowViz contributors. All Rights Reserved.

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Scene/FlowVizCaseActor.h"
#include "Scene/FlowVizVolumeComponent.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "UI/SFlowVizTransferFunctionPanel.h"
#include "UI/SFlowVizWorkspace.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Input/SButton.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED NAMESPACE, NOT ANONYMOUS -- FlowVizRuntime is a unity build and an
 * anonymous namespace merges with every other one in the same blob rather than
 * being file-local. That collision already shipped once in this repo (#37).
 */
namespace FlowVizWorkspaceTransferFunctionSeamTest
{
	/**
	 * NONE OF THESE IS A DEFAULT, and that is the whole reason they were chosen.
	 *
	 * A default-constructed transfer function is viridis over [0, 1] with an
	 * opacity multiplier of 1 -- which is exactly what the dispatcher hard-coded
	 * before this seam existed. So a fixture built from defaults would make a
	 * wired push and a missing one produce identical readings at every
	 * assertion, and the test would pass against the bug it exists to catch.
	 *
	 * Inferno rather than Plasma or Magma: it is the map whose low end is
	 * visibly black rather than blue, so a mix-up with the viridis default shows
	 * up in a capture as well as in an assertion.
	 */
	constexpr ECFDVizColorMap TestMap = ECFDVizColorMap::Inferno;
	constexpr float TestRangeMin = -3.25f;
	constexpr float TestRangeMax = 11.75f;
	constexpr float TestOpacity = 0.375f;

	/*
	 * NO GetSampleCaseDir HELPER HERE, unlike every sibling seam test.
	 *
	 * Those tests load the sample because a clip fraction is meaningless
	 * without a domain to crop. A transfer function maps VALUES to colours and
	 * needs no geometry at all, so loading a case here would add a dependency on
	 * the reader, the decoder and the sample fixture -- three more ways for this
	 * test to go red for a reason that is not the seam.
	 *
	 * It also happens to be the stronger fixture: no case open is exactly the
	 * state in which the clip push fails, which is what the short-circuit arm
	 * below needs.
	 */

	UWorld* MakeWorld(FWorldContext& OutContext)
	{
		// GLOBALLY UNIQUE NAME, matching FlowVizWorkspaceVolumeSeamTest. Two
		// tests that both create an unnamed world in the transient package get
		// the same object name, and the second one is the one that fails -- for
		// a reason that has nothing to do with what it is testing.
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

	/**
	 * Configure a transfer function the way a user would, and prove each step
	 * took.
	 *
	 * EVERY SETTER IS CHECKED because five of the six can refuse, and a refusal
	 * here would leave the value at its default -- which is precisely the
	 * reading a broken seam produces. Without these controls, "the volume shows
	 * viridis over [0,1]" would be ambiguous between "the push is broken" and
	 * "the fixture never asked for anything else".
	 */
	bool Configure(FFlowVizTransferFunctionViewModel& Model, FAutomationTestBase& Test)
	{
		if (!Test.TestTrue(TEXT("CONTROL: the fixture's colormap was accepted"),
				Model.SetColorMap(TestMap).IsOk()))
		{
			return false;
		}

		// SetManualRange sets RangeSource to Manual itself. Global would need a
		// bound field and CurrentFrame is refused until a frame range is
		// supplied -- either would leave the range at its default and make the
		// assertions below fail for a reason that is not the seam.
		if (!Test.TestTrue(TEXT("CONTROL: the fixture's manual range was accepted"),
				Model.SetManualRange(TestRangeMin, TestRangeMax).IsOk()))
		{
			return false;
		}

		if (!Test.TestTrue(TEXT("CONTROL: the fixture's opacity multiplier was accepted"),
				Model.SetOpacityMultiplier(TestOpacity).IsOk()))
		{
			return false;
		}

		// READ BACK, not assumed from the results above. A setter that returned
		// Ok and stored nothing would pass all three checks.
		return Test.TestEqual(TEXT("CONTROL: the fixture really holds the chosen map"),
				   static_cast<int32>(Model.GetColorMap()), static_cast<int32>(TestMap))
			&& Test.TestEqual(TEXT("CONTROL: and the chosen range"),
				   Model.GetRangeMax(), TestRangeMax);
	}
}

/**
 * THE TRANSFER FUNCTION REACHES THE RENDER-THREAD PAYLOAD.
 *
 * This covers the four links between the panel's announcement and the
 * dispatcher: PushTransferFunctionToVolume, the component's setter and field,
 * MakeProxyDynamicData's copy, and (by asserting on the payload) the fact that
 * there is something for the proxy to assign into the context at all.
 *
 * WHY THIS ASSERTS ON MakeProxyDynamicData AND NOT ON A RENDERED FRAME. Same
 * reason FlowVizVolumeComponentTest gives for the settings channel: the payload
 * is what the render thread is handed, so a test that reads it is reading the
 * actual seam rather than a parallel path. A pixel test would additionally
 * depend on the shader, the RHI and the upload, and would fail for four reasons
 * that are not this one.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlowVizWorkspaceTransferFunctionSeamTest,
	"FlowViz.UI.Workspace.TransferFunctionSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceTransferFunctionSeamTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceTransferFunctionSeamTest;

	UCFDVizVolumeComponent* Volume = NewObject<UCFDVizVolumeComponent>();
	if (!TestNotNull(TEXT("CONTROL: a volume component was constructed"), Volume))
	{
		return false;
	}
	Volume->AddToRoot();
	ON_SCOPE_EXIT { Volume->RemoveFromRoot(); };

	/*
	 * NO CASE IS LOADED HERE, AND THAT IS THE POINT OF USING A BARE COMPONENT.
	 *
	 * PushClipToVolume refuses a volume with no physical extent -- correctly,
	 * because a crop fraction means nothing without a domain. The transfer
	 * function has no such dependency: it maps VALUES to colours, and nothing
	 * about it is expressed in the volume's geometry.
	 *
	 * So this arm asserts the asymmetry deliberately. Copying the clip push's
	 * extent guard into the transfer function push is the tempting symmetry, and
	 * it would silently drop every colour edit made before a case is opened --
	 * which is most of them, during setup.
	 */
	{
		TestEqual(TEXT("CONTROL: a bare component has no domain, which is what makes this the "
					   "case that PushClipToVolume refuses"),
			Volume->GetPhysicalSize().X, 0.0);

		TestEqual(TEXT("CONTROL: and its transfer function is the default, so what lands below "
					   "cannot be state that was already there"),
			static_cast<int32>(Volume->GetTransferFunction().GetColorMap()),
			static_cast<int32>(CFDViz::ColorMaps::Default));

		FFlowVizTransferFunctionViewModel Source;
		if (!Configure(Source, *this))
		{
			return false;
		}

		TestTrue(TEXT("pushing a transfer function into a volume with NO CASE BOUND succeeds -- "
					  "colours map values, not positions, so there is no domain to refuse for"),
			FFlowVizWorkspaceModel::PushTransferFunctionToVolume(Source, Volume));

		TestEqual(TEXT("the chosen colormap reaches the component"),
			static_cast<int32>(Volume->GetTransferFunction().GetColorMap()),
			static_cast<int32>(TestMap));

		/* == And through to the payload the render thread receives ========== */

		const FFlowVizVolumeProxyDynamicData Payload = Volume->MakeProxyDynamicData();

		TestEqual(TEXT("the colormap is in the marshalled payload, so the proxy has something "
					   "to put in the context rather than a default"),
			static_cast<int32>(Payload.TransferFunction.GetColorMap()),
			static_cast<int32>(TestMap));

		// THE RANGE AND OPACITY TOO, not just the map. The two ends of this seam
		// fail independently: the dispatcher applies the parameter block in one
		// place and builds the LUT in another, so a payload carrying the map
		// alone would render Inferno over the wrong range -- and the panel's
		// readout would agree with the picker, so nothing would look wrong.
		TestEqual(TEXT("and the range minimum, so the colours are stretched over the domain "
					   "the user typed"),
			Payload.TransferFunction.GetRangeMin(), TestRangeMin);

		TestEqual(TEXT("and the range maximum"),
			Payload.TransferFunction.GetRangeMax(), TestRangeMax);

		TestEqual(TEXT("and the opacity multiplier, so the volume thins as asked"),
			Payload.TransferFunction.GetOpacityMultiplier(), TestOpacity);
	}

	/* == Null is refused, not dereferenced =================================== */

	{
		FFlowVizTransferFunctionViewModel Source;
		TestFalse(TEXT("pushing into a null component reports failure rather than crashing"),
			FFlowVizWorkspaceModel::PushTransferFunctionToVolume(Source, nullptr));
	}

	/* == A second push REPLACES ============================================= */

	/*
	 * Repo memory accumulators-need-a-second-write: a seam that assigned once
	 * and then ignored later pushes is indistinguishable from a correct one
	 * until something is written twice. The second value is deliberately NOT the
	 * default -- returning to viridis would also be the reading of a seam that
	 * reset to its constant.
	 */
	{
		FFlowVizTransferFunctionViewModel Second;
		if (!TestTrue(TEXT("CONTROL: the second fixture's colormap was accepted"),
				Second.SetColorMap(ECFDVizColorMap::Magma).IsOk()))
		{
			return false;
		}

		TestTrue(TEXT("a second push succeeds"),
			FFlowVizWorkspaceModel::PushTransferFunctionToVolume(Second, Volume));

		TestEqual(TEXT("and REPLACES the first, so changing the colormap twice does not leave "
					   "the volume showing the first choice forever"),
			static_cast<int32>(Volume->MakeProxyDynamicData().TransferFunction.GetColorMap()),
			static_cast<int32>(ECFDVizColorMap::Magma));
	}

	return true;
}

/**
 * THE PANEL'S EDIT REACHES THE BOUND VOLUME, through the workspace.
 *
 * The seam test above proves the push works when called. This proves the
 * WORKSPACE CALLS IT -- that SFlowVizWorkspace subscribed to the panel's
 * delegate, and that its PushToVolume carries the transfer function and not
 * only the clip model.
 *
 * THE TWO ARE SEPARATE FAILURES and both have shipped in this repo: #40 was a
 * working mechanism with no assignment, #41 a working setter with no caller,
 * #57 a frame source with no production caller. A test that only called the
 * push directly would pass against a workspace that never subscribed.
 *
 * WHY THE CLIP PUSH'S RETURN VALUE MATTERS HERE. PushToVolume attempts both
 * channels and ANDs the results. Written as `PushClip(...) && PushTF(...)` it
 * would short-circuit: with no case open the clip push returns false, and the
 * transfer function -- which has no such precondition -- would never be
 * attempted. That is the arm below with no case loaded.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlowVizWorkspaceTransferFunctionBindingTest,
	"FlowViz.UI.Workspace.TransferFunctionBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceTransferFunctionBindingTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceTransferFunctionSeamTest;

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

	const TSharedPtr<SFlowVizTransferFunctionPanel> Panel = Workspace->GetTransferFunctionPanel();
	if (!TestTrue(TEXT("CONTROL: the workspace built a transfer function panel"),
			Panel.IsValid()))
	{
		return false;
	}

	/*
	 * NO CASE IS LOADED, DELIBERATELY -- see the header note on short-circuiting.
	 * This is the state in which the clip push returns false, so if PushToVolume
	 * were written with && the assertion below could not pass. It is also the
	 * ordinary state during setup: the workspace opens before a case does.
	 */
	TestEqual(TEXT("CONTROL: the volume has no case, which is the state in which the clip "
				   "push fails and a short-circuiting PushToVolume would skip this channel"),
		Volume->GetPhysicalSize().X, 0.0);

	TestEqual(TEXT("CONTROL: the bound volume shows the default colormap before the panel "
				   "is touched"),
		static_cast<int32>(Volume->GetTransferFunction().GetColorMap()),
		static_cast<int32>(CFDViz::ColorMaps::Default));

	const TSharedPtr<SButton> MapButton = Panel->GetColorMapButton(TestMap);
	if (!TestTrue(TEXT("CONTROL: the panel built a button for the chosen map"),
			MapButton.IsValid()))
	{
		return false;
	}

	// THROUGH THE BUTTON, not the view model. Calling the model directly would
	// pass against a panel that announces nothing -- which is exactly the state
	// this panel was in.
	MapButton->SimulateClick();

	TestEqual(TEXT("CONTROL: the click changed the workspace's own model"),
		static_cast<int32>(Workspace->GetModel().TransferFunction.GetColorMap()),
		static_cast<int32>(TestMap));

	TestEqual(TEXT("clicking a colormap in the panel reaches the BOUND VOLUME, so the picker "
				   "and the render agree rather than the volume staying viridis"),
		static_cast<int32>(Volume->GetTransferFunction().GetColorMap()),
		static_cast<int32>(TestMap));

	TestEqual(TEXT("and reaches the render-thread payload, which is what the proxy reads"),
		static_cast<int32>(Volume->MakeProxyDynamicData().TransferFunction.GetColorMap()),
		static_cast<int32>(TestMap));

	/* == Binding pushes what is already there =============================== */

	/*
	 * The same requirement #60 established for the clip model: a workspace whose
	 * colours were chosen BEFORE the actor existed -- from a session load, or
	 * simply from picking a map while the case dialog was open -- must not wait
	 * for the next edit to take effect. A control that needs wiggling reads as
	 * broken.
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

		TestEqual(TEXT("CONTROL: the fresh volume shows the default map, so what lands below "
					   "cannot be state it already had"),
			static_cast<int32>(SecondVolume->GetTransferFunction().GetColorMap()),
			static_cast<int32>(CFDViz::ColorMaps::Default));

		// The workspace's model still holds TestMap from the click above.
		Workspace->SetVolume(SecondVolume);

		TestEqual(TEXT("binding a volume pushes the colours already chosen, so a map picked "
					   "before the case was open does not wait for the next edit"),
			static_cast<int32>(SecondVolume->GetTransferFunction().GetColorMap()),
			static_cast<int32>(TestMap));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
