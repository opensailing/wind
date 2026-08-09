// Copyright FlowViz contributors. All Rights Reserved.

#include "FlowVizCameraPawn.h"

#include "EngineUtils.h"
#include "Scene/FlowVizCaseActor.h"

#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "TimerManager.h"

AFlowVizCameraPawn::AFlowVizCameraPawn()
{
	PrimaryActorTick.bCanEverTick = true;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	RootComponent = Camera;

	// The pawn IS the camera: no mesh, no collision, nothing to bump the case
	// actor with. Auto-possess player 0 so the packaged app -- whose game mode
	// is the engine default -- still gets this camera without a custom mode.
	AutoPossessPlayer = EAutoReceiveInput::Player0;
}

bool AFlowVizCameraPawn::FrameBox(FVector WorldCenter, FVector WorldExtent)
{
	return OrbitCamera.FrameBox(WorldCenter, WorldExtent);
}

void AFlowVizCameraPawn::BeginPlay()
{
	Super::BeginPlay();

	// The framing search is TIMER work, not tick work: an actor iterator plus a
	// component-bounds gather every frame, forever, whenever no case ever loads.
	// 0.25 s is imperceptible against a case load and 1/15th of the per-frame
	// cost. The timer cancels itself on success and EndPlay covers the rest.
	GetWorldTimerManager().SetTimer(
		AutoFrameTimerHandle, this, &AFlowVizCameraPawn::TryAutoFrame,
		0.25f, /*bLoop*/ true, /*FirstDelay*/ 0.0f);
}

void AFlowVizCameraPawn::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(AutoFrameTimerHandle);
	Super::EndPlay(EndPlayReason);
}

void AFlowVizCameraPawn::TryAutoFrame()
{
	if (bAutoFramed)
	{
		GetWorldTimerManager().ClearTimer(AutoFrameTimerHandle);
		return;
	}

	for (TActorIterator<ACFDVizCaseActor> It(GetWorld()); It; ++It)
	{
		// Validity by BOUNDS, not by a loaded-state flag: an actor whose
		// case failed to load has a degenerate box, and framing a point
		// would pin the camera inside it. Degenerate bounds leave the timer
		// running -- the case may still be loading.
		const FBox Bounds = It->GetComponentsBoundingBox(/*bNonColliding*/ true);
		if (Bounds.IsValid && Bounds.GetExtent().GetMax() > 1.0)
		{
			FrameBox(Bounds.GetCenter(), Bounds.GetExtent());
			bAutoFramed = true;
			GetWorldTimerManager().ClearTimer(AutoFrameTimerHandle);
			break;
		}
	}
}

void AFlowVizCameraPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// COPIED EVERY TICK, derived never stored (the model's own rule): the pawn
	// holds no pose of its own to drift from the model's. This copy is the one
	// legitimate per-tick job this pawn has; the auto-frame search lives on a
	// timer (TryAutoFrame).
	SetActorLocationAndRotation(OrbitCamera.GetLocation(), OrbitCamera.GetRotation());
}

void AFlowVizCameraPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	// Axis mappings straight off the hardware axes: no project input config
	// required, so the binding works in a packaged app with default settings.
	PlayerInputComponent->BindAxisKey(EKeys::MouseX, this, &AFlowVizCameraPawn::OnMouseX);
	PlayerInputComponent->BindAxisKey(EKeys::MouseY, this, &AFlowVizCameraPawn::OnMouseY);
	PlayerInputComponent->BindAxisKey(
		EKeys::MouseWheelAxis, this, &AFlowVizCameraPawn::OnWheel);

	PlayerInputComponent->BindKey(
		EKeys::LeftMouseButton, IE_Pressed, this, &AFlowVizCameraPawn::OnOrbitPressed);
	PlayerInputComponent->BindKey(
		EKeys::LeftMouseButton, IE_Released, this, &AFlowVizCameraPawn::OnOrbitReleased);
	PlayerInputComponent->BindKey(
		EKeys::MiddleMouseButton, IE_Pressed, this, &AFlowVizCameraPawn::OnPanPressed);
	PlayerInputComponent->BindKey(
		EKeys::MiddleMouseButton, IE_Released, this, &AFlowVizCameraPawn::OnPanReleased);
}

void AFlowVizCameraPawn::OnMouseX(float Value)
{
	if (Value == 0.0f)
	{
		return;
	}
	if (bPanning)
	{
		// Pan scale follows the distance, so a drag moves the focus by the same
		// SCREEN fraction whether zoomed in or out -- constant-feel panning.
		OrbitCamera.Pan(-Value * OrbitCamera.GetDistance() * PanFractionPerUnit, 0.0);
	}
	else if (bOrbiting)
	{
		OrbitCamera.Orbit(Value * OrbitDegreesPerUnit, 0.0);
	}
}

void AFlowVizCameraPawn::OnMouseY(float Value)
{
	if (Value == 0.0f)
	{
		return;
	}
	if (bPanning)
	{
		OrbitCamera.Pan(0.0, Value * OrbitCamera.GetDistance() * PanFractionPerUnit);
	}
	else if (bOrbiting)
	{
		// Mouse up (positive) pitches up: the ParaView feel.
		OrbitCamera.Orbit(0.0, Value * OrbitDegreesPerUnit);
	}
}

void AFlowVizCameraPawn::OnWheel(float Value)
{
	if (Value != 0.0f)
	{
		OrbitCamera.Zoom(Value);
	}
}

void AFlowVizCameraPawn::OnOrbitPressed()
{
	bOrbiting = true;
}

void AFlowVizCameraPawn::OnOrbitReleased()
{
	bOrbiting = false;
}

void AFlowVizCameraPawn::OnPanPressed()
{
	bPanning = true;
}

void AFlowVizCameraPawn::OnPanReleased()
{
	bPanning = false;
}
