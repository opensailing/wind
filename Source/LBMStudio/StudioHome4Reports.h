#pragma once
#include "StudioHome4Config.h"
#include "StudioHome4Telemetry.h"
struct FStudioProject;
struct FStudioHome4ReferenceEvidence;
namespace StudioHome4Reports
{
    FString EscapeLaTeX(const FString& Value);
    /** Export a new directory atomically. Existing reports are never overwritten. */
    bool Export(const FString& Parent,const FString& Folder,const FStudioProject& Project,
        const FStudioHome4TelemetryStream* Telemetry,FString& OutPath,FString& Error,const FStudioHome4ReferenceEvidence* Evidence=nullptr);
}
