// Copyright FlowViz contributors. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"

#include "FlowVizGameModeBase.generated.h"

/**
 * The game mode whose one job is spawning the orbit camera pawn (#87).
 *
 * DefaultEngine.ini's own comment reserved this slot: pointing
 * GlobalDefaultGameMode at a class that does not exist silently spawns no
 * player and renders black frames, so the config held the engine default
 * until this class was real. AFlowVizCameraPawn::AutoPossessPlayer covers
 * the PLACED-pawn case; this covers the packaged app, where nothing is
 * placed and the game mode is what decides the pawn.
 */
UCLASS()
class AFlowVizGameModeBase : public AGameModeBase
{
	GENERATED_BODY()

public:
	AFlowVizGameModeBase();
};
