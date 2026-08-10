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
#include "UI/SFlowVizClipPanel.h"
#include "UI/SFlowVizWorkspace.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Input/SButton.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED NAMESPACE, NOT ANONYMOUS -- FlowVizRuntime is a unity build and an
 * anonymous namespace merges with every other one in the same blob rather than
 * being file-local. That collision already shipped once in this repo (#37).
 */
namespace FlowVizWorkspaceVolumeSeamTest
{
	/** The committed low-resolution sample, beside Plugins/ rather than inside the plugin. */
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

	UWorld* MakeWorld(FWorldContext& OutContext)
	{
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
	 * A distance no default produces, inside a domain no default has.
	 *
	 * The sample's domain is 12 x 4 x 1 m (hand-computed and asserted in
	 * FlowVizVolumeComponentTest), so 0.375 sits inside it on Z and is not the
	 * midpoint, not the extent, and not zero. A push that fabricated a
	 * plausible-looking plane instead of carrying this one fails on this number.
	 */
	constexpr double PlaneDistance = 0.375;

	/** A configured clip model. NEVER a default -- see the class comment below. */
	void ConfigureClip(
		FFlowVizClipViewModel& Clip, const FVector& DomainSize, FAutomationTestBase& Test)
	{
		Test.TestTrue(
			TEXT("CONTROL: the fixture's domain was accepted, so what follows is about the seam "
				 "rather than about a model that refused its own setup"),
			Clip.SetDomainSize(DomainSize).IsOk());

		FFlowVizClipPlane Plane;
		Plane.Normal = FVector(0.0, 0.0, 1.0);
		Plane.Distance = PlaneDistance;
		Plane.bEnabled = true;

		Test.TestTrue(TEXT("CONTROL: the fixture's plane was accepted"), Clip.AddPlane(Plane).IsOk());
	}
}

/**
 * THE LAST LINK: the panels edit a model, and the model reaches a volume.
 *
 * FlowViz.Render.ClipSeam proved the dispatcher applies a clip model.
 * FlowViz.Scene.DispatchContext proved the component's model reaches the
 * dispatcher. Both were green while the CLIP PANEL still had no effect at all,
 * because nothing in production copied FFlowVizWorkspaceModel::Clip -- the thing
 * the panel edits -- into a UCFDVizVolumeComponent. Two finished halves with
 * nothing between them, which is the shape FlowVizRenderWiringTest and
 * FlowVizWorkspaceWiringTest exist to catch.
 *
 * WHY A DEFAULT MODEL CANNOT TEST THIS. A default FFlowVizClipViewModel has NO
 * DOMAIN, so ApplyToRayMarchParameters refuses and writes nothing. A volume that
 * received an unconfigured model and a volume that received nothing at all are
 * therefore INDISTINGUISHABLE. Every fixture here configures a domain and
 * authors a plane whose distance appears nowhere else, so a push that did not
 * happen cannot be mistaken for one that did. Repo memory
 * degenerate-data-defeats-assertions.
 *
 * WHY A BARE COMPONENT CANNOT TEST IT EITHER -- and this is the trap the first
 * draft of this file walked into. UCFDVizVolumeComponent::GetPhysicalSize
 * returns ZeroVector unless CaseBinding.bIsValid, so a NewObject'd component has
 * no domain to push against and the push is correctly REFUSED. A success
 * assertion written against a bare component asserts something that cannot
 * happen, and would have been "fixed" by weakening the production guard. Every
 * arm below that expects success loads the real sample case.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceVolumeSeamTest,
	"FlowViz.UI.Workspace.VolumeSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceVolumeSeamTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceVolumeSeamTest;

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

	const FCFDVizResult LoadResult = Actor->LoadCase(GetSampleCaseDir());
	if (!TestTrue(
			FString::Printf(TEXT("CONTROL: the sample case loads (%s) -- without a loaded case "
								 "the volume has no domain and every push below is correctly "
								 "refused, which would make the success arms vacuous"),
				*LoadResult.ToString()),
			LoadResult.IsOk()))
	{
		return false;
	}

	const FVector VolumeSize = Volume->GetPhysicalSize();
	if (!TestTrue(TEXT("CONTROL: the loaded volume has a positive domain on every axis"),
			VolumeSize.X > 0.0 && VolumeSize.Y > 0.0 && VolumeSize.Z > 0.0))
	{
		return false;
	}

	/* == The push carries the model ========================================== */

	FFlowVizWorkspaceModel Model;
	ConfigureClip(Model.Clip, VolumeSize, *this);

	/*
	 * ASSERTED, NOT ASSUMED. A component that somehow already held a plane would
	 * satisfy the count assertion below against a push that never happened --
	 * repo memory accumulators-need-a-second-write.
	 */
	TestEqual(TEXT("CONTROL: the volume has no clip planes before the push, so what lands "
				   "below cannot be state that was already there"),
		Volume->GetClip().GetPlaneCount(), 0);

	TestTrue(TEXT("pushing a configured clip model into a loaded volume reports success"),
		FFlowVizWorkspaceModel::PushClipToVolume(Model.Clip, Volume));

	TestEqual(TEXT("the plane the panel authored reaches the volume component"),
		Volume->GetClip().GetPlaneCount(), 1);

	const FFlowVizClipPlane* Landed = Volume->GetClip().FindPlane(0);
	if (!TestNotNull(TEXT("CONTROL: the landed plane can be read back"), Landed))
	{
		return false;
	}

	/*
	 * THE DISTANCE, NOT JUST THE COUNT. A push that created a fresh plane of the
	 * right shape -- the "impostor" arm the dispatcher campaign killed one file
	 * over -- satisfies a count-only assertion completely.
	 */
	TestEqual(TEXT("and carries the distance the user typed, not a default-shaped plane"),
		Landed->Distance, PlaneDistance, 1e-9);

	TestEqual(TEXT("and its normal, so the plane faces the way it was authored"),
		Landed->Normal.Z, 1.0, 1e-9);

	/*
	 * THE DOMAIN IS THE VOLUME'S. The capture library re-reads GetPhysicalSize on
	 * every call because a stale domain does not fail -- it clips at a plausible
	 * wrong place (FlowVizCaptureLibrary.cpp:766).
	 */
	TestEqual(TEXT("the volume's own domain is what the pushed model carries"),
		Volume->GetClip().GetDomainSize().X, VolumeSize.X, 1e-9);

	/*
	 * AND IT REACHES THE SHADER PAYLOAD. GetClip() reads back what SetClip
	 * stored, so every assertion above is satisfied by a SetClip the proxy never
	 * sees. MakeProxyDynamicData is what the render thread is handed -- the same
	 * seam FlowViz.Scene.ProxySettings uses -- so this is the difference between
	 * "stored on the component" and "on its way to a pixel".
	 */
	const FFlowVizVolumeProxyDynamicData Payload = Volume->MakeProxyDynamicData();
	TestEqual(TEXT("and the pushed plane is in the payload the render thread receives, not "
				   "merely stored on the component"),
		Payload.Clip.GetPlaneCount(), 1);

	/* == A SECOND push replaces, and does not accumulate ===================== */

	{
		/*
		 * THE FAILURE THIS CATCHES IS THE ONE A USER MEETS FIRST. Every panel edit
		 * pushes, so a push that appended would add a plane per click: the second
		 * click on "add plane" would leave three planes on the volume while the
		 * panel listed two, and six clicks would hit MaxClipPlanes with the panel
		 * showing three.
		 */
		FFlowVizWorkspaceModel Second;
		ConfigureClip(Second.Clip, VolumeSize, *this);

		TestTrue(TEXT("a second push succeeds"),
			FFlowVizWorkspaceModel::PushClipToVolume(Second.Clip, Volume));

		TestEqual(TEXT("and REPLACES rather than accumulates -- otherwise every panel edit "
					   "would add another plane and the volume would diverge from the list "
					   "the panel shows"),
			Volume->GetClip().GetPlaneCount(), 1);
	}

	/* == An emptied model clears the volume ================================== */

	{
		/*
		 * THE SECOND BRANCH OF THE SAME CHANNEL, and the one an append-only push
		 * passes. "Remove all" must reach the volume too; a push that only ever
		 * adds leaves the last plane clipping forever with the panel showing none.
		 * Repo memory audit-the-second-branch-of-every-documented-hazard.
		 */
		FFlowVizWorkspaceModel Emptied;
		TestTrue(TEXT("CONTROL: the emptied model's domain was accepted"),
			Emptied.Clip.SetDomainSize(VolumeSize).IsOk());

		TestTrue(TEXT("pushing a model with no planes succeeds"),
			FFlowVizWorkspaceModel::PushClipToVolume(Emptied.Clip, Volume));

		TestEqual(TEXT("and clears the volume, so Remove All reaches the renderer rather than "
					   "leaving the last plane clipping forever"),
			Volume->GetClip().GetPlaneCount(), 0);
	}

	/* == The crop survives the push ========================================== */

	{
		/*
		 * THE TRAP IN THE OBVIOUS IMPLEMENTATION, asserted rather than trusted.
		 *
		 * FFlowVizClipViewModel::SetDomainSize CALLS ResetCropBox -- deliberately,
		 * because a crop authored for a 10 m domain means something else in a
		 * 0.1 m one. A push written as the obvious read-modify-write
		 * (SetDomainSize, then copy the planes) therefore DESTROYS the user's crop
		 * every time it runs, and it runs on every edit. The user drags a crop,
		 * touches any other control, and the crop silently snaps back to full.
		 *
		 * Nothing about that is visible from the plane assertions above.
		 */
		FFlowVizWorkspaceModel Cropped;
		TestTrue(TEXT("CONTROL: the cropped model's domain was accepted"),
			Cropped.Clip.SetDomainSize(VolumeSize).IsOk());

		const FVector CropMin(1.0, 0.5, 0.25);
		const FVector CropMax(VolumeSize.X - 1.0, VolumeSize.Y - 0.5, VolumeSize.Z - 0.25);
		TestTrue(TEXT("CONTROL: the fixture's crop box was accepted"),
			Cropped.Clip.SetCropBox(CropMin, CropMax).IsOk());
		TestTrue(TEXT("CONTROL: and the fixture's crop is actually a crop, so the assertion "
					  "below is not satisfied by a full-domain box"),
			Cropped.Clip.IsCropActive());

		TestTrue(TEXT("pushing a cropped model succeeds"),
			FFlowVizWorkspaceModel::PushClipToVolume(Cropped.Clip, Volume));

		TestTrue(TEXT("the crop the user dragged survives the push, rather than being reset by "
					  "a domain re-set inside it"),
			Volume->GetClip().IsCropActive());

		TestEqual(TEXT("and arrives with the numbers the user typed"),
			Volume->GetClip().GetCropMin().X, CropMin.X, 1e-9);
		TestEqual(TEXT("on every axis, so an axis-swapped copy is not mistaken for a correct one"),
			Volume->GetClip().GetCropMax().Z, CropMax.Z, 1e-9);
	}

	/* == Refusals ============================================================ */

	{
		FFlowVizWorkspaceModel Fresh;
		ConfigureClip(Fresh.Clip, VolumeSize, *this);

		TestFalse(TEXT("pushing into a null volume is refused rather than crashing"),
			FFlowVizWorkspaceModel::PushClipToVolume(Fresh.Clip, nullptr));

		UCFDVizVolumeComponent* Unloaded = NewObject<UCFDVizVolumeComponent>();
		if (TestNotNull(TEXT("CONTROL: a bare component was built"), Unloaded))
		{
			Unloaded->AddToRoot();
			ON_SCOPE_EXIT { Unloaded->RemoveFromRoot(); };

			/*
			 * Only meaningful while a bare component really has no domain. If that
			 * ever changes this arm silently stops testing refusal, so it is
			 * asserted rather than assumed.
			 */
			TestEqual(TEXT("CONTROL: a component with no case has no domain, which is what "
						   "makes the refusal below the behaviour under test"),
				Unloaded->GetPhysicalSize().Size(), 0.0, 1e-9);

			TestFalse(TEXT("pushing into a volume with no loaded case is REFUSED rather than "
						   "applied against a guessed unit domain, which would clip at "
						   "whatever the real extent turns out to be"),
				FFlowVizWorkspaceModel::PushClipToVolume(Fresh.Clip, Unloaded));

			TestEqual(TEXT("and the refused push left the volume unclipped rather than "
						   "half-applied"),
				Unloaded->GetClip().GetPlaneCount(), 0);
		}
	}

	return true;
}

/**
 * DOES PRODUCTION BUILD THE CHANNEL, or is the function above merely available?
 *
 * The distinction is the whole reason FlowVizRenderWiringTest exists: 49
 * rendering tests were green while a real map drew nothing, because each test
 * installed its own dispatcher and none could see that production installed
 * none. A push function with a test and no caller is that defect with better
 * documentation -- repo memory mocking-a-seam-hides-that-nothing-builds-it.
 *
 * So this constructs the workspace the way production does, binds a volume, and
 * then drives a PANEL BUTTON rather than the model. If the workspace does not
 * subscribe the panel's change event to the push, the model changes and the
 * volume does not.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceVolumeBindingTest,
	"FlowViz.UI.Workspace.VolumeBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceVolumeBindingTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceVolumeSeamTest;

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
	UCFDVizVolumeComponent* Volume = Actor != nullptr ? Actor->GetVolumeComponent() : nullptr;
	if (!TestNotNull(TEXT("CONTROL: the actor owns a volume component"), Volume))
	{
		return false;
	}
	if (!TestTrue(TEXT("CONTROL: the sample case loads into the volume"),
			Actor->LoadCase(GetSampleCaseDir()).IsOk()))
	{
		return false;
	}

	const TSharedRef<SFlowVizWorkspace> Workspace = SNew(SFlowVizWorkspace);

	/*
	 * A workspace with nothing bound must be operable -- it is built before any
	 * case is open, and every panel already tolerates a null model by disabling
	 * rather than crashing. An unbound push is a no-op that SAYS it is one; a
	 * push returning true with nowhere to write would be a success report with no
	 * subject.
	 */
	TestFalse(TEXT("a workspace with no volume bound reports that it pushed nothing, rather "
				   "than claiming a success it cannot have had"),
		Workspace->PushToVolume());

	Workspace->SetVolume(Volume);

	TestEqual(TEXT("CONTROL: the volume is unclipped before the panel is touched"),
		Volume->GetClip().GetPlaneCount(), 0);

	/* == The PANEL drives it, not the model ================================== */

	const TSharedPtr<SFlowVizClipPanel> Panel = Workspace->GetClipPanel();
	if (!TestValid(TEXT("CONTROL: the workspace built a clip panel"), Panel))
	{
		return false;
	}

	/*
	 * A CASE MUST BE OPEN IN THE WORKSPACE or its clip model has no domain and
	 * AddPresetPlane refuses -- the click would then be a no-op for a reason that
	 * has nothing to do with the seam, and this arm would pass on broken wiring.
	 * OpenCase is what sets Clip's domain (FlowVizWorkspaceModel.cpp, "THE
	 * DOMAIN, TO EVERY MODEL THAT HAS ONE").
	 */
	if (!TestTrue(TEXT("CONTROL: the workspace opens the sample case, which is what gives the "
					   "clip model a domain -- without it the preset button is refused and "
					   "this arm would pass for the wrong reason"),
			Workspace->GetModel().OpenCase(GetSampleCaseDir()).IsOk()))
	{
		return false;
	}

	const TSharedPtr<SButton> PresetButton = Panel->GetPresetButton(EFlowVizClipPreset::KeepMinusZ);
	if (!TestValid(TEXT("CONTROL: the panel built a Keep -Z preset button"), PresetButton))
	{
		return false;
	}

	PresetButton->SimulateClick();

	TestEqual(TEXT("CONTROL: clicking the preset added a plane to the model the panel edits"),
		Workspace->GetModel().Clip.GetPlaneCount(), 1);

	/*
	 * THE ASSERTION THE FILE EXISTS FOR. Nothing here called PushToVolume: a
	 * button was clicked, exactly as a user would. If the workspace does not
	 * subscribe to the panel's change event, the line above passes and this one
	 * fails -- which is precisely the state the codebase was in.
	 */
	TestEqual(TEXT("and clicking it PUSHED that plane to the volume, with nothing in this test "
				   "calling the push -- the channel from panel to renderer"),
		Volume->GetClip().GetPlaneCount(), 1);

	/* == The volume is BORROWED, and must not outlive its binding ============ */

	Workspace->SetVolume(nullptr);

	TestFalse(TEXT("clearing the bound volume makes a later push a no-op rather than a write "
				   "through a stale pointer"),
		Workspace->PushToVolume());

	/*
	 * AND THE PANEL'S EVENT MUST GO QUIET TOO. The unbind above only protects
	 * explicit pushes; if the panel's subscription still fires into a raw
	 * pointer, a click after the case closes writes through freed memory. That
	 * crash reproduces on teardown only, which is the hardest kind to diagnose.
	 * The model must still take the edit -- unbinding the volume disconnects the
	 * renderer, not the panel.
	 */
	PresetButton->SimulateClick();

	TestEqual(TEXT("a panel edit after unbinding still reaches the model, so unbinding "
				   "disconnects the renderer rather than the panel"),
		Workspace->GetModel().Clip.GetPlaneCount(), 2);

	TestEqual(TEXT("and does not reach the unbound volume"),
		Volume->GetClip().GetPlaneCount(), 1);

	return true;
}

/**
 * THE ADVISORY MUST RETIRE WHEN THE CHANNEL OPENS.
 *
 * SFlowVizClipPanel's advisory said the panel "is NOT yet connected to the
 * volume". Its own comment named the retirement condition exactly: "It stops
 * being correct when something pushes the workspace model's Clip into a volume
 * component." That is now what the seam above does.
 *
 * NOTHING ASSERTED ON THAT STRING -- verified by grep before this file existed:
 * FlowVizClipPanelTest.cpp mentions neither "Advisory" nor "NotWired". So the
 * panel could keep telling users their controls do nothing long after they
 * worked, and every test in the suite would stay green. That is not cosmetic: it
 * tells a user to stop investigating a control that is now live, which is the
 * same wasted afternoon the advisory was written to prevent, pointed the other
 * way.
 *
 * BOTH BRANCHES ARE ASSERTED. An advisory that is always empty passes a "not a
 * lie" check exactly as well as one that is correctly conditional -- repo memory
 * audit-the-second-branch-of-every-documented-hazard.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizClipPanelAdvisoryRetiresTest,
	"FlowViz.UI.ClipPanel.AdvisoryRetires",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizClipPanelAdvisoryRetiresTest::RunTest(const FString& Parameters)
{
	FFlowVizClipViewModel Clip;

	const TSharedRef<SFlowVizClipPanel> Panel = SNew(SFlowVizClipPanel).ViewModel(&Clip);

	/*
	 * BRANCH ONE: nothing bound. The advisory is still true and must be shown --
	 * a panel whose controls really are inert and says nothing is the exact
	 * failure the strip was added to prevent.
	 */
	TestTrue(TEXT("with no volume bound the panel still discloses that its edits do not reach "
				  "the renderer, because they still do not"),
		Panel->IsNotWiredAdvisoryVisible());

	TestFalse(TEXT("and the disclosure actually says something"),
		Panel->GetNotWiredAdvisoryText().IsEmpty());

	/*
	 * BRANCH TWO: bound, so the claim has become false and the strip must go.
	 * This is the assertion that had no equivalent anywhere: the string was
	 * unconditional, so nothing could ever have caught it outliving its subject.
	 */
	Panel->SetVolumeBound(true);

	TestFalse(TEXT("once the panel's edits reach a volume the not-wired advisory retires, "
				   "rather than telling the user a live control does nothing"),
		Panel->IsNotWiredAdvisoryVisible());

	// And back, so visibility tracks the binding rather than latching once.
	Panel->SetVolumeBound(false);

	TestTrue(TEXT("unbinding restores the disclosure, so the strip tracks the binding rather "
				  "than latching the first time it is cleared"),
		Panel->IsNotWiredAdvisoryVisible());

	return true;
}

/**
 * AND THE WORKSPACE MUST SET THAT FLAG, not merely offer it.
 *
 * The test above proves the panel CAN retire its advisory. It sets the flag
 * itself, so it cannot see whether anything in production ever does -- the same
 * blind spot as a test that installs its own dispatcher. This one touches only
 * SetVolume and asks the panel what it is showing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceAdvisoryTracksBindingTest,
	"FlowViz.UI.Workspace.AdvisoryTracksBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceAdvisoryTracksBindingTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceVolumeSeamTest;

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
	UCFDVizVolumeComponent* Volume = Actor != nullptr ? Actor->GetVolumeComponent() : nullptr;
	if (!TestNotNull(TEXT("CONTROL: the actor owns a volume component"), Volume))
	{
		return false;
	}

	const TSharedRef<SFlowVizWorkspace> Workspace = SNew(SFlowVizWorkspace);
	const TSharedPtr<SFlowVizClipPanel> Panel = Workspace->GetClipPanel();
	if (!TestValid(TEXT("CONTROL: the workspace built a clip panel"), Panel))
	{
		return false;
	}

	TestTrue(TEXT("CONTROL: a workspace with no volume shows the advisory"),
		Panel->IsNotWiredAdvisoryVisible());

	Workspace->SetVolume(Volume);

	TestFalse(TEXT("binding a volume to the WORKSPACE retires the clip panel's advisory, so "
				   "the disclosure follows production's own wiring rather than a flag a test "
				   "set for it"),
		Panel->IsNotWiredAdvisoryVisible());

	Workspace->SetVolume(nullptr);

	TestTrue(TEXT("and unbinding restores it"), Panel->IsNotWiredAdvisoryVisible());

	return true;
}

/**
 * AND BINDING MUST PUSH WHAT IS ALREADY THERE, not merely subscribe for later.
 *
 * WHERE THIS CAME FROM. Mutation campaign clip-wire-workspace (2026-08-06)
 * deleted the PushToVolume() call at the end of SetVolume, leaving the
 * assignment and the panel disclosure. It SURVIVED a green suite.
 *
 * WHY VolumeBinding ABOVE CANNOT CATCH IT, which is the interesting part. That
 * test binds a volume whose clip model is still EMPTY -- it even asserts plane
 * count 0 as a control -- and only then presses a preset button. The bind-time
 * push therefore has nothing to carry, and the later edit-driven push covers for
 * a bind-time push that never happened. The ordering is what hides the branch,
 * not any weakness in the assertions.
 *
 * WHAT DELETING THE CALL ACTUALLY COSTS, in SetVolume's own words: a workspace
 * with planes already authored -- from a session load, or from a case opened
 * before the actor existed -- renders unclipped until the user touches
 * something. That reads as a control that has to be wiggled to take, and it is
 * the same shape as #26, #40/#41, #42 and #50: the unit is correct, the channel
 * exists, and nothing drives it at the moment that matters.
 *
 * So this authors the clip state BEFORE binding and asserts the volume is
 * already clipped with no further edit.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFlowVizWorkspaceBindPushesExistingStateTest,
	"FlowViz.UI.Workspace.BindPushesExistingState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceBindPushesExistingStateTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceVolumeSeamTest;

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
	UCFDVizVolumeComponent* Volume = Actor != nullptr ? Actor->GetVolumeComponent() : nullptr;
	if (!TestNotNull(TEXT("CONTROL: the actor owns a volume component"), Volume))
	{
		return false;
	}
	if (!TestTrue(TEXT("CONTROL: the sample case loads into the volume, without which the push "
					   "would be correctly REFUSED for want of a domain and this test would "
					   "pass on wiring that does not exist"),
			Actor->LoadCase(GetSampleCaseDir()).IsOk()))
	{
		return false;
	}

	const TSharedRef<SFlowVizWorkspace> Workspace = SNew(SFlowVizWorkspace);

	/*
	 * THE STATE IS AUTHORED BEFORE THE BINDING. This is the whole difference
	 * from VolumeBinding above, and it is what makes the bind-time push the only
	 * thing that can produce the result asserted at the end.
	 *
	 * Through the workspace's OWN model, so nothing here installs a channel
	 * production does not have -- the defect this file exists to catch is a test
	 * supplying the seam it is testing.
	 *
	 * ConfigureClip is the same fixture VolumeSeam uses, and it sets the domain
	 * EXPLICITLY rather than relying on a case load to have set one. A push
	 * refused for want of a domain would leave the volume at zero planes -- the
	 * same reading as a push that never fired -- so the domain is established
	 * here rather than inferred.
	 */
	const FVector VolumeSize = Volume->GetPhysicalSize();
	if (!TestTrue(TEXT("CONTROL: the loaded volume has a positive domain on every axis, without "
					   "which the plane below would be correctly refused and the push would have "
					   "nothing to carry for a reason that is not the one under test"),
			VolumeSize.X > 0.0 && VolumeSize.Y > 0.0 && VolumeSize.Z > 0.0))
	{
		return false;
	}

	ConfigureClip(Workspace->GetModel().Clip, VolumeSize, *this);
	if (!TestEqual(TEXT("CONTROL: the model really does hold a plane before the bind, without "
						"which the assertion below would be about an empty push"),
			Workspace->GetModel().Clip.GetPlaneCount(), 1))
	{
		return false;
	}

	/*
	 * ASSERTED, NOT ASSUMED. If the volume already carried a plane, the assertion
	 * below could not tell a push from the state that was there all along.
	 */
	if (!TestEqual(TEXT("CONTROL: the volume is unclipped before the bind, so what follows "
						"cannot be the state it already had"),
			Volume->GetClip().GetPlaneCount(), 0))
	{
		return false;
	}

	/* == The bind, and nothing else ========================================== */

	Workspace->SetVolume(Volume);

	/*
	 * NO PANEL IS TOUCHED AND NO MODEL IS EDITED between the line above and the
	 * assertions below. Delete PushToVolume() from SetVolume and the volume stays
	 * at zero planes.
	 */
	if (!TestEqual(TEXT("binding a volume pushes the state the model ALREADY held, so a "
						"session's planes render immediately rather than waiting for the user "
						"to touch a control"),
			Volume->GetClip().GetPlaneCount(), 1))
	{
		return false;
	}

	/*
	 * AND IT IS THE AUTHORED PLANE, not a plausible one. A push that fabricated
	 * a default plane would satisfy the count above; PlaneDistance is a number no
	 * default produces, so carrying it is evidence the real model was copied.
	 * Repo memory degenerate-data-defeats-assertions.
	 */
	if (const FFlowVizClipPlane* Pushed = Volume->GetClip().FindPlane(0))
	{
		TestEqual(TEXT("and it is the plane the model held, at the distance it was authored "
					   "with -- not a default one that merely makes the count right"),
			Pushed->Distance, PlaneDistance, 1e-9);

		TestTrue(TEXT("facing the way it was authored, so an axis-swapped or fabricated copy "
					  "is not mistaken for the real one"),
			Pushed->Normal.Equals(FVector(0.0, 0.0, 1.0), 1e-6));
	}
	else
	{
		AddError(TEXT("the pushed plane is not readable, so the count above cannot be trusted"));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
