#pragma once
#include "StudioHome4Config.h"
namespace StudioHome4RecipeAuthoring
{
    FString Parameters(const FString& RecipeId);
    FString Relationship(const FStudioHome4Spec&);
    /** Explicit frontend conventions and current declared dimensions only.
     * No driver argv or unavailable original reference geometry is inferred. */
    bool Resolve(const FStudioHome4Spec&,FStudioHome4Spec& Out,FString& Error);
}
