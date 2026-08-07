// Copyright FlowViz contributors. All Rights Reserved.

#include "FlowVizCameraPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"

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

void AFlowVizCameraPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// COPIED EVERY TICK, derived never stored (the model's own rule): the pawn
	// holds no pose of its own to drift from the model's.
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
