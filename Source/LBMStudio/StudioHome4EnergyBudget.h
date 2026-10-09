#pragma once
#include "StudioHome4Config.h"
class FJsonObject;
class FJsonValue;

/** Explicit body-relative XYZ boxes. Their existence requests diagnostics;
 * it never creates or reinterprets measured energy terms. */
namespace StudioHome4EnergyBudget
{
    bool Validate(const TArray<FStudioHome4EnergyBudgetRegion>& Regions,FString& Error);
    bool ValidateMapped(const TArray<FStudioHome4EnergyBudgetRegion>& Regions,const FStudioHome4Spec& Map,FString& Error);
    bool ValidateRequest(const FStudioHome4Spec& Spec,FString& Error);
    TOptional<FBox> RootBox(const FStudioHome4EnergyBudgetRegion& Region,const FStudioHome4Spec& Map);
    FString Description(const TArray<FStudioHome4EnergyBudgetRegion>& Regions,const FStudioHome4Spec* Map=nullptr);
    bool ParseOriginal(const TArray<TSharedPtr<FJsonValue>>& Values,TArray<FStudioHome4EnergyBudgetRegion>& Out,FString& Error);
    TArray<TSharedPtr<FJsonValue>> OriginalJSON(const TArray<FStudioHome4EnergyBudgetRegion>& Regions);
}
