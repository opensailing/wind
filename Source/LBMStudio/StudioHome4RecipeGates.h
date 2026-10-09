#pragma once
#include "StudioHome4Config.h"
struct FStudioHome4ReferenceEvidence;
struct FStudioHome4RecipeCoverage
{
    TArray<FString> Required, Missing, Departures;
    bool bOriginalSpecKnown=false, bReferenceVerified=false;
    FString Status=TEXT("unknown"), Reason;
};
namespace StudioHome4RecipeGates
{
    /** Exact metric identifiers. Supplied labels never imply coverage of another metric. */
    TArray<FString> RequiredMetrics(const FString& RecipeId);
    FString Description(const FString& RecipeId);
    FStudioHome4RecipeCoverage Evaluate(const FStudioHome4ReferenceEvidence& Evidence);
}
