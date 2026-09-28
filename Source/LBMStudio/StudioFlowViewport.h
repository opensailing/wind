#pragma once
#include "Widgets/SWidget.h"
class AStudioScene;
/** The same orbit/pan/fly camera viewport used by Solve, without its inspector. */
TSharedRef<SWidget> MakeStudioFlowViewport(AStudioScene* Scene,FName Tag);
