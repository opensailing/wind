// Copyright FlowViz contributors. All Rights Reserved.

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Render/FlowVizVolumeRayMarchShader.h"
#include "Scene/FlowVizCaseActor.h"
#include "Scene/FlowVizVolumeComponent.h"
#include "UI/FlowVizWorkspaceModel.h"
#include "UI/SFlowVizSlicePanel.h"
#include "UI/SFlowVizWorkspace.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Input/SButton.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * NAMED NAMESPACE, NOT ANONYMOUS -- unity build (#37).
 */
namespace FlowVizWorkspaceSliceSeamTest
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
	 * A position no default produces: 0.375 of the domain on Z, off-centre,
	 * not an edge. The sample's domain is 12 x 4 x 1 m, so the plane sits at
	 * z = 0.375 m. Repo memory degenerate-data-defeats-assertions.
	 */
	constexpr double SliceZ = 0.375;
	constexpr double SlabThickness = 0.25;
}

/**
 * THE SLICE REACHES THE RENDERER, AS A SLAB OF THE VOLUME (#77 / Milestone D).
 *
 * A slab IS two opposed half-spaces: keep >= (z - t/2) AND keep <= (z + t/2).
 * The clip machinery already renders half-spaces end to end, so the slice
 * composes into the PUSHED clip model rather than growing a second renderer --
 * one definition of "which side", one shader path, no new parameters. The
 * slice view model's header used to say "NO RENDERER CONSUMES THIS"; this seam
 * is what retires that.
 *
 * WHY COMPOSITION AT PUSH TIME AND NOT A PLANE IN THE CLIP VIEW MODEL. The
 * user's clip planes are THEIRS: a slice that inserted planes into the clip
 * panel's list would show phantom rows that vanish when the slice hides, and
 * deleting one would half-disable a control that lives in another panel. So
 * the workspace composes slice + clip into one FFlowVizClipViewModel at push
 * time and hands THAT to the volume; both panels keep editing their own models.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlowVizWorkspaceSliceSeamTest,
	"FlowViz.UI.Workspace.SliceSeam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizWorkspaceSliceSeamTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceSliceSeamTest;

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
	UCFDVizVolumeComponent* Volume =
		Actor != nullptr ? Actor->GetVolumeComponent() : nullptr;
	if (!TestNotNull(TEXT("CONTROL: the actor owns a volume component"), Volume))
	{
		return false;
	}

	const FCFDVizResult LoadResult = Actor->LoadCase(GetSampleCaseDir());
	if (!TestTrue(FString::Printf(TEXT("CONTROL: the sample case loads (%s)"),
			*LoadResult.ToString()),
			LoadResult.IsOk()))
	{
		return false;
	}

	const TSharedRef<SFlowVizWorkspace> Workspace = SNew(SFlowVizWorkspace);
	Workspace->GetModel().OpenCase(GetSampleCaseDir());
	Workspace->SetVolume(Volume);

	FFlowVizSliceViewModel& Slice = Workspace->GetModel().Slice;

	/* == CONTROL: before the slice is visible, its planes are absent ========= */
	{
		TestEqual(TEXT("CONTROL: the freshly bound volume carries no slice planes -- the "
					   "user has authored no clip and the slice defaults hidden-from-push"),
			Volume->GetClip().GetPlaneCount(), 0);
	}

	/* == Author a slab through the view model, then push through the panel === */
	{
		TestTrue(TEXT("CONTROL: the slice normal is accepted"),
			Slice.SetNormal(FVector(0.0, 0.0, 1.0)).IsOk());
		TestTrue(TEXT("CONTROL: the slice origin is accepted"),
			Slice.SetOrigin(FVector(6.0, 2.0, SliceZ)).IsOk());
		TestTrue(TEXT("CONTROL: the slab thickness is accepted"),
			Slice.SetThickness(SlabThickness).IsOk());

		// THROUGH THE PANEL'S VISIBILITY BUTTON, not a direct push call: the
		// button is the user's path, and it must announce (#48's lesson). The
		// slice defaults HIDDEN (#77 flipped it -- visible now means "slab the
		// volume"), so ONE click shows it and pushes the slab.
		const TSharedPtr<SFlowVizSlicePanel> Panel = Workspace->GetSlicePanel();
		if (!TestTrue(TEXT("CONTROL: the workspace built a slice panel"), Panel.IsValid()))
		{
			return false;
		}
		const TSharedPtr<SButton> VisibleButton = Panel->GetVisibleButton();
		if (!TestTrue(TEXT("CONTROL: the panel built a visibility toggle"),
				VisibleButton.IsValid()))
		{
			return false;
		}
		// P3: visible now defaults TRUE (the cut plane is the default
		// picture) and the sliver hazard moved to the volume-slab flag. Put
		// the slice in the hidden state EXPLICITLY so the button click below
		// still exercises hidden -> visible, and arm the slab so the push has
		// planes to carry.
		Slice.SetVisible(false);
		Slice.SetVolumeSlabEnabled(true);
		TestFalse(TEXT("CONTROL: the slice is hidden (set above), so the click is a "
					   "hide-to-show transition"),
			Slice.IsVisible());
		VisibleButton->SimulateClick();  // show -- and push

		TestTrue(TEXT("CONTROL: the slice ends visible"), Slice.IsVisible());
	}

	/* == The volume's clip now carries the slab: TWO opposed planes ========== */
	{
		const FFlowVizClipViewModel& Pushed = Volume->GetClip();
		if (!TestEqual(
				TEXT("the pushed clip carries exactly the slab's two planes; if this is 0 the "
					 "slice still reaches no renderer, if 1 the slab collapsed to a knife-edge "
					 "that keeps half the domain"),
				Pushed.GetPlaneCount(), 2))
		{
			return false;
		}

		const TArray<FFlowVizClipPlane>& Planes = Pushed.GetPlanes();

		// The two planes face each OTHER: normals opposed, and the kept band is
		// [z - t/2, z + t/2]. Assert by BEHAVIOUR -- which points survive -- not
		// by storage, so a sign convention flip anywhere in the chain fails here.
		const auto Keeps = [&Planes](const FVector& Point) -> bool
		{
			for (const FFlowVizClipPlane& Plane : Planes)
			{
				if (FVector::DotProduct(Plane.Normal, Point) + Plane.Distance < 0.0)
				{
					return false;
				}
			}
			return true;
		};

		TestTrue(TEXT("a point ON the slice plane survives both planes"),
			Keeps(FVector(6.0, 2.0, SliceZ)));
		TestTrue(TEXT("a point just inside the slab's near face survives"),
			Keeps(FVector(6.0, 2.0, SliceZ - SlabThickness * 0.49)));
		TestTrue(TEXT("and just inside the far face"),
			Keeps(FVector(6.0, 2.0, SliceZ + SlabThickness * 0.49)));
		TestFalse(TEXT("a point beyond the near face is clipped"),
			Keeps(FVector(6.0, 2.0, SliceZ - SlabThickness)));
		TestFalse(TEXT("a point beyond the far face is clipped"),
			Keeps(FVector(6.0, 2.0, SliceZ + SlabThickness)));

		/* -- And through to the render-thread payload's parameter block. ---- */
		FFlowVizVolumeRayMarchParameters Params;
		FlowVizRayMarch::FillDefaults(Params);
		TestTrue(TEXT("the pushed clip applies to the parameter block"),
			Volume->GetClip().ApplyToRayMarchParameters(Params).IsOk());
		TestEqual(TEXT("with both slab planes enabled on the GPU"),
			static_cast<int32>(Params.NumClipPlanes), 2);
	}

	/* == Hiding the slice retracts its planes and KEEPS the user's clip ====== */
	{
		// A user clip plane, authored in the CLIP model -- the slab must not
		// eat it and hiding the slice must not delete it.
		FFlowVizClipViewModel& Clip = Workspace->GetModel().Clip;
		FFlowVizClipPlane UserPlane;
		UserPlane.Normal = FVector(1.0, 0.0, 0.0);
		UserPlane.Distance = -2.0;
		UserPlane.Label = TEXT("user");
		TestTrue(TEXT("CONTROL: a user clip plane is accepted"),
			Clip.AddPlane(UserPlane).IsOk());

		const TSharedPtr<SButton> VisibleButton =
			Workspace->GetSlicePanel()->GetVisibleButton();
		VisibleButton->SimulateClick();  // hide the slice

		const FFlowVizClipViewModel& Pushed = Volume->GetClip();
		TestEqual(TEXT("hiding the slice retracts its two planes and keeps the user's one"),
			Pushed.GetPlaneCount(), 1);
		if (Pushed.GetPlaneCount() == 1)
		{
			TestEqual(TEXT("and it IS the user's plane, not a slab remnant"),
				Pushed.GetPlanes()[0].Label, FString(TEXT("user")));
		}

		TestEqual(TEXT("CONTROL: the clip panel's own model still holds exactly the user's "
					   "plane -- the slab never leaked into it"),
			Clip.GetPlaneCount(), 1);
	}

	return true;
}

/**
 * DoD 8: A PRESSURE SLICE CAN COEXIST WITH A VORTICITY VOLUME.
 *
 * Two case actors over the same case: one renders vorticityMagnitude as a full
 * volume, the other renders pressure clipped to a slab. Coexistence is two
 * volumes with independent fields and independent clips in one world -- which
 * this asserts at the seam level: each component holds its own field binding
 * and its own clip state, and pushing the slab to one leaves the other whole.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlowVizSliceCoexistenceTest,
	"FlowViz.UI.Workspace.SliceCoexistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext
		| EAutomationTestFlags::EngineFilter)

bool FFlowVizSliceCoexistenceTest::RunTest(const FString& Parameters)
{
	using namespace FlowVizWorkspaceSliceSeamTest;

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

	const FString CaseDir = GetSampleCaseDir();

	/* -- The vorticity volume, whole. ------------------------------------- */
	ACFDVizCaseActor* VolumeActor = World->SpawnActor<ACFDVizCaseActor>();
	UCFDVizVolumeComponent* VorticityVolume =
		VolumeActor != nullptr ? VolumeActor->GetVolumeComponent() : nullptr;
	if (!TestNotNull(TEXT("CONTROL: the vorticity actor owns a volume"), VorticityVolume))
	{
		return false;
	}
	TestTrue(TEXT("CONTROL: the vorticity case loads bound to vorticityMagnitude"),
		VolumeActor->LoadCase(CaseDir, FName(TEXT("vorticityMagnitude"))).IsOk());

	/* -- The pressure slice, in its own workspace. -------------------------- */
	ACFDVizCaseActor* SliceActor = World->SpawnActor<ACFDVizCaseActor>();
	UCFDVizVolumeComponent* PressureVolume =
		SliceActor != nullptr ? SliceActor->GetVolumeComponent() : nullptr;
	if (!TestNotNull(TEXT("CONTROL: the pressure actor owns a volume"), PressureVolume))
	{
		return false;
	}
	TestTrue(TEXT("CONTROL: the pressure case loads bound to pressure"),
		SliceActor->LoadCase(CaseDir, FName(TEXT("pressure"))).IsOk());

	const TSharedRef<SFlowVizWorkspace> Workspace = SNew(SFlowVizWorkspace);
	Workspace->GetModel().OpenCase(CaseDir, FName(TEXT("pressure")));
	Workspace->SetVolume(PressureVolume);

	FFlowVizSliceViewModel& Slice = Workspace->GetModel().Slice;
	TestTrue(TEXT("CONTROL: the slab is authored"),
		Slice.SetNormal(FVector(0.0, 0.0, 1.0)).IsOk()
			&& Slice.SetOrigin(FVector(6.0, 2.0, SliceZ)).IsOk()
			&& Slice.SetThickness(SlabThickness).IsOk());
	// SHOWN AND ARMED EXPLICITLY: since P3 the slab needs BOTH switches --
	// visible (defaults true now) and the volume-slab flag (defaults false,
	// carrying #77's sliver hazard). The vorticity volume's zero-plane
	// assertion below depends on ITS actor never getting these.
	Slice.SetVisible(true);
	Slice.SetVolumeSlabEnabled(true);

	Workspace->PushToVolume();

	/* -- Independence, both directions. ------------------------------------ */
	TestEqual(TEXT("the pressure volume carries the slab"),
		PressureVolume->GetClip().GetPlaneCount(), 2);
	TestEqual(TEXT("the vorticity volume is UNTOUCHED -- no planes leaked across actors"),
		VorticityVolume->GetClip().GetPlaneCount(), 0);

	TestEqual(TEXT("the pressure volume is bound to pressure"),
		PressureVolume->GetCaseBinding().FieldId, FName(TEXT("pressure")));
	TestEqual(TEXT("the vorticity volume is bound to vorticityMagnitude"),
		VorticityVolume->GetCaseBinding().FieldId, FName(TEXT("vorticityMagnitude")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
