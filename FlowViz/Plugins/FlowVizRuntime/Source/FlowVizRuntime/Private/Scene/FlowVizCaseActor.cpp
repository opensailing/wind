// Copyright FlowViz contributors. All Rights Reserved.

#include "Scene/FlowVizCaseActor.h"

#include "Components/SceneComponent.h"
#include "FlowVizRuntime.h"
#include "Async/TaskGraphInterfaces.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "Scene/FlowVizBoundaryMesh.h"
#include "Scene/FlowVizFlowComponent.h"
#include "Scene/FlowVizMeshPayload.h"
#include "Scene/FlowVizSurfaceMeshComponent.h"
#include "Scene/FlowVizVolumeComponent.h"
#include "Tasks/Task.h"

ACFDVizCaseActor::ACFDVizCaseActor()
{
	// Nothing here ticks. Playback advances through the case player, which drives
	// the component's frame source; an actor tick would be a second clock and the
	// two would disagree about which frame is displayed.
	PrimaryActorTick.bCanEverTick = false;

	// A plain scene component as the root - see the class comment. The volume's
	// own placement matrix must not become part of the actor transform, or every
	// future representation inherits the grid-origin translation and the Y mirror
	// a second time.
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	VolumeComponent = CreateDefaultSubobject<UCFDVizVolumeComponent>(TEXT("Volume"));
	VolumeComponent->SetupAttachment(Root);

	// Glyphs and streamlines live beside the volume (#86): same actor, same
	// transform, so flow geometry and volume agree about where the case is.
	FlowComponent = CreateDefaultSubobject<UCFDVizFlowComponent>(TEXT("Flow"));
	FlowComponent->SetupAttachment(Root);

	// The obstacle's opaque lit surface (renderer overhaul P2). Its OWN
	// component, not a shared one: P4's per-frame iso rebuilds must never
	// dirty the obstacle's render state, and separate components is what
	// guarantees that.
	ObstacleComponent = CreateDefaultSubobject<UCFDVizSurfaceMeshComponent>(TEXT("Obstacle"));
	ObstacleComponent->SetupAttachment(Root);

	// The cut plane (P3): its own component so per-frame rebuilds never
	// recreate the obstacle's render state.
	CutPlaneComponent = CreateDefaultSubobject<UCFDVizSurfaceMeshComponent>(TEXT("CutPlane"));
	CutPlaneComponent->SetupAttachment(Root);

	IsoSurfaceComponent = CreateDefaultSubobject<UCFDVizSurfaceMeshComponent>(TEXT("IsoSurface"));
	IsoSurfaceComponent->SetupAttachment(Root);

	/*
	 * MATERIALS BY PATH, TOLERANT OF ABSENCE. The assets are authored by
	 * Tools/author_materials.py into plugin Content; a cooked build carries
	 * them, but a headless test world without content mounting must still
	 * construct the actor -- null keeps PMC's default material, which renders
	 * (grey) rather than vanishing.
	 */
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> SurfaceMaterial(
		TEXT("/FlowVizRuntime/M_FlowVizSurface.M_FlowVizSurface"));
	if (SurfaceMaterial.Succeeded())
	{
		ObstacleComponent->SetMaterial(0, SurfaceMaterial.Object);
	}
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> ColormapMaterial(
		TEXT("/FlowVizRuntime/M_FlowVizColormapSurface.M_FlowVizColormapSurface"));
	if (ColormapMaterial.Succeeded())
	{
		CutPlaneComponent->SetMaterial(0, ColormapMaterial.Object);
		// The iso surface shares the colormap material: same LUT, same UV0
		// scalar contract -- one color authority (P3's rule).
		IsoSurfaceComponent->SetMaterial(0, ColormapMaterial.Object);
	}
}

FCFDVizResult ACFDVizCaseActor::LoadCase(const FString& InCaseDirectory, FName InFieldId)
{
	check(VolumeComponent != nullptr);

	const FCFDVizResult Result = VolumeComponent->LoadCase(InCaseDirectory, InFieldId);
	if (Result.IsOk())
	{
		// Recorded only on success, so the properties always name what is
		// actually displayed. Storing the requested path regardless would leave
		// the details panel describing a case that failed to load while the
		// previous one is on screen.
		CaseDirectory = InCaseDirectory;
		FieldId = VolumeComponent->GetCaseBinding().FieldId;

		// The obstacle is part of what "loaded" means (renderer overhaul P2):
		// every reference image anchors on it, so it is not opt-in.
		LoadBoundaryMeshes();
	}
	return Result;
}

void ACFDVizCaseActor::LoadBoundaryMeshes()
{
	check(VolumeComponent != nullptr && ObstacleComponent != nullptr);
	ObstacleComponent->ClearSurfaceData();

	const FCFDVizCase& Case = VolumeComponent->GetCaseBinding().Case;
	if (Case.Meshes.Num() == 0)
	{
		// A case without meshes is legal; the picture simply has no obstacle.
		return;
	}

	/*
	 * PATHS RESOLVED NOW, GEOMETRY BUILT ON A WORKER. ResolveMeshPath touches
	 * the manifest (game-thread state); BuildPatches does file I/O and
	 * welding, which has no business on the game thread mid-load. The worker
	 * gets copies of everything it reads -- the sampling service's rule.
	 */
	struct FMeshRequest
	{
		FString MeshPath;
		FCFDVizMesh Mesh;
	};
	TArray<FMeshRequest> Requests;
	for (const FCFDVizMesh& Mesh : Case.Meshes)
	{
		FString MeshPath;
		if (Case.ResolveMeshPath(Mesh.Id, MeshPath).IsOk())
		{
			Requests.Add({ MoveTemp(MeshPath), Mesh });
		}
	}

	TWeakObjectPtr<ACFDVizCaseActor> WeakThis(this);
	UE::Tasks::Launch(TEXT("FlowVizBoundaryBuild"),
		[WeakThis, Requests = MoveTemp(Requests)]()
		{
			// ONE payload across every declared mesh: obstacle and domain
			// boundaries are sections of one component, addressed by patch id.
			TSharedRef<FFlowVizMeshPayload> Payload = MakeShared<FFlowVizMeshPayload>();
			for (const FMeshRequest& Request : Requests)
			{
				TArray<FFlowVizBoundaryPatchGeometry> Patches;
				if (!FlowVizBoundary::BuildPatches(
						Request.MeshPath, Request.Mesh,
						CFDViz::MetersToUnrealCentimeters, Patches).IsOk())
				{
					continue;
				}
				for (FFlowVizBoundaryPatchGeometry& Patch : Patches)
				{
					FFlowVizMeshSection& Section = Payload->Sections.AddDefaulted_GetRef();
					Section.SectionId = Patch.PatchId;
					Section.Name = MoveTemp(Patch.Name);
					Section.bDefaultVisible = Patch.bDefaultVisible;
					Section.Vertices = MoveTemp(Patch.Vertices);
					Section.Indices = MoveTemp(Patch.Indices);
					Section.Normals = MoveTemp(Patch.Normals);
				}
			}

			// Apply on the game thread; the weak pointer covers an actor torn
			// down while the build was in flight.
			AsyncTask(ENamedThreads::GameThread, [WeakThis, Payload]()
			{
				if (ACFDVizCaseActor* Actor = WeakThis.Get())
				{
					Actor->GetObstacleComponent()->SetSurfaceData(*Payload);
				}
			});
		});
}

bool ACFDVizCaseActor::LoadCaseFromPath(const FString& InCaseDirectory, FString& OutError)
{
	const FCFDVizResult Result = LoadCase(InCaseDirectory, FieldId);
	if (Result.IsOk())
	{
		OutError.Reset();
		return true;
	}

	// The full diagnostic, naming the file and byte offset where one applies -
	// not a generic "failed to load". A user cannot act on the latter.
	OutError = Result.ToString();
	return false;
}

void ACFDVizCaseActor::BeginPlay()
{
	Super::BeginPlay();

	if (CaseDirectory.IsEmpty())
	{
		return;
	}

	const FCFDVizResult Result = LoadCase(CaseDirectory, FieldId);
	if (!Result.IsOk())
	{
		// Logged as an error rather than swallowed: an actor placed in a level
		// with a bad path otherwise plays as an empty scene, which is
		// indistinguishable from a case that loaded and rendered nothing.
		UE_LOG(LogFlowViz, Error,
			TEXT("ACFDVizCaseActor '%s' could not load '%s': %s"),
			*GetName(), *CaseDirectory, *Result.ToString());
	}
}
