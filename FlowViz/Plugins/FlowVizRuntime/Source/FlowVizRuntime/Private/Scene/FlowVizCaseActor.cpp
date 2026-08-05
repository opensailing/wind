// Copyright FlowViz contributors. All Rights Reserved.

#include "Scene/FlowVizCaseActor.h"

#include "Components/SceneComponent.h"
#include "FlowVizRuntime.h"
#include "Scene/FlowVizVolumeComponent.h"

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
	}
	return Result;
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
