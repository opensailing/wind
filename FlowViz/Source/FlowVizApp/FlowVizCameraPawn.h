// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Scene/FlowVizOrbitCamera.h"

#include "FlowVizCameraPawn.generated.h"

class UCameraComponent;

/**
 * The orbit camera's input binding (#87 / Milestone D's last thin consumer).
 *
 * EVERYTHING INTERESTING LIVES IN FFlowVizOrbitCamera, which is tested
 * headless (FlowViz.Scene.OrbitCamera): orbit preserves distance, pan never
 * dollies, zoom clamps, pitch stops short of the poles. This pawn only maps
 * input to those calls and copies GetLocation/GetRotation onto a camera
 * component each tick -- deliberately too thin to be wrong in a way the
 * model's tests would not catch.
 *
 * CLASSIC INPUT AXES rather than EnhancedInput assets: an input ASSET cannot
 * be constructed in code without content the packaged app must then cook,
 * and the four bindings here are the whole surface. The axis/key names are
 * bound directly in SetupPlayerInputComponent.
 *
 * Drag with LEFT mouse to orbit, MIDDLE (or left+shift) to pan, wheel to
 * zoom -- the ParaView convention, since that is who the user is.
 */
UCLASS()
class AFlowVizCameraPawn : public APawn
{
	GENERATED_BODY()

public:
	AFlowVizCameraPawn();

	/** Frame a world-space box (the case actor's bounds). Forwards to the model. */
	UFUNCTION(BlueprintCallable, Category = "FlowViz|Camera")
	bool FrameBox(FVector WorldCenter, FVector WorldExtent);

	const FFlowVizOrbitCamera& GetOrbitCamera() const { return OrbitCamera; }
	FFlowVizOrbitCamera& GetOrbitCameraMutable() { return OrbitCamera; }

	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

private:
	void OnMouseX(float Value);
	void OnMouseY(float Value);
	void OnWheel(float Value);
	void OnOrbitPressed();
	void OnOrbitReleased();
	void OnPanPressed();
	void OnPanReleased();

	UPROPERTY()
	TObjectPtr<UCameraComponent> Camera;

	FFlowVizOrbitCamera OrbitCamera;

	bool bOrbiting = false;
	bool bPanning = false;

	/*
	 * AUTO-FRAME, ONCE. On the first tick where a case actor with real bounds
	 * exists, frame it -- so a packaged launch with -case= opens LOOKING AT
	 * the data instead of at whatever the map's default view was. Once only:
	 * after that the camera is the user's, and a reload must not yank it.
	 */
	bool bAutoFramed = false;

	/** Degrees of orbit per mouse-axis unit, and world units of pan per unit at reference distance. */
	static constexpr double OrbitDegreesPerUnit = 2.0;
	static constexpr double PanFractionPerUnit = 0.002;
};
