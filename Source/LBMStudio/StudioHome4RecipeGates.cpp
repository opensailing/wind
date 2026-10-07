#include "StudioHome4RecipeGates.h"
#include "StudioHome4Validation.h"
#include "StudioHome4Recipes.h"

TArray<FString> StudioHome4RecipeGates::RequiredMetrics(const FString& Recipe)
{
    if(Recipe==TEXT("rti-fakhari")) return {TEXT("spike_front"),TEXT("bubble_front")};
    if(Recipe==TEXT("breaking-wave-banari")) return {TEXT("jet_geometry"),TEXT("energy")};
    if(Recipe==TEXT("colagrossi-wb")) return {TEXT("Cd"),TEXT("Cl"),TEXT("surface_profile")};
    if(Recipe==TEXT("oscillating-cylinder")) return {TEXT("force_history")};
    if(Recipe==TEXT("couette-spin")) return {TEXT("torque")};
    if(Recipe==TEXT("magnus")) return {TEXT("lift")};
    if(Recipe==TEXT("sedimentation")) return {TEXT("terminal_velocity")};
    if(Recipe==TEXT("vugts-barge")) return {TEXT("added_mass"),TEXT("damping")};
    if(Recipe==TEXT("th01-hull")) return {TEXT("heave"),TEXT("trim"),TEXT("drag"),TEXT("bem_intercepts")};
    if(Recipe==TEXT("hydrofoil-parkin")) return {TEXT("surface_profile")};
    return {};
}
FString StudioHome4RecipeGates::Description(const FString& Recipe)
{
    const auto Metrics=RequiredMetrics(Recipe);
    return Metrics.IsEmpty()?TEXT("No metric coverage contract supplied for this recipe."):
        TEXT("Required original reference metrics: ")+FString::Join(Metrics,TEXT(", "))+
        TEXT(". Each needs aligned actual/reference samples and explicit tolerances. Recipe eligibility also requires an original run specification and owner-verified source citation/hash.");
}
FStudioHome4RecipeCoverage StudioHome4RecipeGates::Evaluate(const FStudioHome4ReferenceEvidence& E)
{
    FStudioHome4RecipeCoverage C; C.Required=RequiredMetrics(E.RecipeId);
    if(C.Required.IsEmpty()) { C.Reason=TEXT("Unknown recipe coverage contract."); return C; }
    for(const auto& Id:C.Required)
        if(!E.Series.ContainsByPredicate([&](const auto& S){return S.Id==Id&&S.Gate.bEvaluated;})) C.Missing.Add(Id);
    C.bOriginalSpecKnown=E.OriginalRunSpec.IsSet();
    if(C.bOriginalSpecKnown)
    {
        if(E.OriginalRunSpec->RecipeId!=E.RecipeId) C.Departures.Add(TEXT("Original specification recipe does not match evidence recipe."));
        else C.Departures=StudioHome4Recipes::Departures(*E.OriginalRunSpec);
    }
    C.bReferenceVerified=E.bReferenceOwnerVerified&&!E.ReferenceCitation.TrimStartAndEnd().IsEmpty()&&E.ReferenceSHA256.Len()==64;
    if(!C.bOriginalSpecKnown&&!E.bReferenceOwnerVerified) { C.Status=TEXT("unknown"); C.Reason=TEXT("Original run specification/reference verification unavailable. Missing metrics: ")+FString::Join(C.Missing,TEXT(", ")); }
    else if(!C.Missing.IsEmpty()) { C.Status=TEXT("missing_metrics"); C.Reason=TEXT("Missing original reference metrics: ")+FString::Join(C.Missing,TEXT(", ")); }
    else if(!C.bOriginalSpecKnown) { C.Status=TEXT("unknown"); C.Reason=TEXT("All named metrics supplied; original run specification remains unavailable."); }
    else if(!C.Departures.IsEmpty()) { C.Status=TEXT("outside_recipe_envelope"); C.Reason=FString::Join(C.Departures,TEXT("\n")); }
    else if(!C.bReferenceVerified) { C.Status=TEXT("unverified_reference"); C.Reason=TEXT("Reference source/citation/hash has not been explicitly verified by its owner."); }
    else { C.Status=TEXT("complete"); C.Reason=TEXT("Required metrics, original recipe specification and owner-verified reference source supplied. This gate applies only to the identified original evidence run."); }
    return C;
}
