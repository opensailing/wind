#pragma once
#include "StudioProject.h"
namespace StudioHome4ViewerDefaults
{
    FString Path();
    /** Source-independent display defaults only; original ranges/tools/field identity are cleared. */
    bool Save(const FString& Path,const FStudioViewSettings&,const FStudioCameraState&,FString& Error);
    bool Load(const FString& Path,FStudioViewSettings&,FStudioCameraState&,FString& Error);
    bool ApplyToNewProject(FStudioProject&,FString& Error,const FString& OverridePath=FString());
}
