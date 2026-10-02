#pragma once
#include "CoreMinimal.h"
class FStudioModel;
class AStudioScene;
enum class EStudioWorkspace : uint8;

namespace StudioHelp
{
    FString WorkspaceName(EStudioWorkspace Workspace);
    /** Guidance describes implemented controls, never invented solver output. */
    FString Guidance(EStudioWorkspace Workspace,bool bControlHarness);
    FString ApplicationVersion();
    /** Value-only JSON snapshot. No source reads, solver commands, project writes
     * or render submission. A missing/pending renderer is explicit. */
    FString Diagnostics(const FStudioModel& Model,const AStudioScene* Scene=nullptr);
}
