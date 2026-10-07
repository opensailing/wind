#pragma once
#include "StudioHome4Config.h"
#include "StudioHome4Telemetry.h"
struct FStudioProject;
namespace StudioHome4Reports
{
    FString EscapeLaTeX(const FString& Value);
    /** Export a new directory atomically. Existing reports are never overwritten. */
    bool Export(const FString& Parent,const FString& Folder,const FStudioProject& Project,
        const FStudioHome4TelemetryStream* Telemetry,FString& OutPath,FString& Error);
}
