#pragma once
#include "Widgets/SWidget.h"
class AStudioScene;
/** Left-drag navigation; middle pan and right look remain available. */
enum class EStudioViewportTool : uint8 { Orbit, Pan, Zoom, Fly };
/** The same orbit/pan/fly camera viewport used by Solve, without its inspector. */
TSharedRef<SWidget> MakeStudioFlowViewport(AStudioScene* Scene,FName Tag);
