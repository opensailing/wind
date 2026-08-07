// Copyright FlowViz contributors. All Rights Reserved.

#include "FlowVizGameModeBase.h"

#include "FlowVizCameraPawn.h"

AFlowVizGameModeBase::AFlowVizGameModeBase()
{
	DefaultPawnClass = AFlowVizCameraPawn::StaticClass();
}
