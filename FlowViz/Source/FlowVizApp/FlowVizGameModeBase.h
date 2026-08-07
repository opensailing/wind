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

protected:
	/**
	 * THE ONE-COMMAND DEMO PATH: `FlowViz -case=<dir> [-field=<id>]` loads the
	 * case at startup by issuing the same FlowViz.LoadCase console command a
	 * user would type -- one code path, already tested, already spawning the
	 * case actor and feeding both halves. Deferred to the first tick so the
	 * Slate tab infrastructure the command resolves against exists.
	 */
	virtual void BeginPlay() override;
};
