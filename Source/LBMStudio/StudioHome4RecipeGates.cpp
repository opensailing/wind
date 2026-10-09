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
    FString Context;
    if(Recipe==TEXT("sedimentation"))Context=TEXT(" terminal_velocity also needs metric_context sampling_convention=steady-terminal, original extraction_method/window_start/window_end/window_unit/window_epoch matching series x_unit/epoch.");
    if(Recipe==TEXT("vugts-barge"))Context=TEXT(" added_mass/damping also need matching original motion_mode=heave|roll, normalization, sampling_convention=original-frequency-curve and explicit extraction_method/window_start/window_end/window_unit/window_epoch. Curves retain their original frequency axis.");
    return Metrics.IsEmpty()?TEXT("No metric coverage contract supplied for this recipe."):
        TEXT("Required original reference metrics: ")+FString::Join(Metrics,TEXT(", "))+
        TEXT(". Each needs aligned actual/reference samples and explicit tolerances. Recipe eligibility also requires an original run specification and owner-verified source citation/hash.")+Context;
}
FStudioHome4RecipeCoverage StudioHome4RecipeGates::Evaluate(const FStudioHome4ReferenceEvidence& E)
{
    FStudioHome4RecipeCoverage C; C.Required=RequiredMetrics(E.RecipeId);
    if(C.Required.IsEmpty()) { C.Reason=TEXT("Unknown recipe coverage contract."); return C; }
    for(const auto& Id:C.Required)
        if(!E.Series.ContainsByPredicate([&](const auto& S){return S.Id==Id&&S.Gate.bEvaluated;})) C.Missing.Add(Id);
    for(const auto& S:E.Series)
    {
        const bool ValidWindow=S.ExtractionWindowStart&&S.ExtractionWindowEnd&&
            FMath::IsFinite(*S.ExtractionWindowStart)&&FMath::IsFinite(*S.ExtractionWindowEnd)&&
            *S.ExtractionWindowStart<*S.ExtractionWindowEnd&&!S.ExtractionMethod.IsEmpty()&&
            !S.ExtractionWindowUnit.IsEmpty()&&!S.ExtractionEpoch.IsEmpty();
        if(E.RecipeId==TEXT("sedimentation")&&S.Id==TEXT("terminal_velocity")&&
            (S.SamplingConvention!=TEXT("steady-terminal")||!ValidWindow||S.ExtractionWindowUnit!=S.AbscissaUnit||S.ExtractionEpoch!=S.AbscissaEpoch||S.AbscissaEpoch.IsEmpty()||S.Abscissae.Num()<2||S.Abscissae[0]<*S.ExtractionWindowStart||S.Abscissae.Last()>*S.ExtractionWindowEnd))
            C.Missing.AddUnique(TEXT("terminal_velocity: original steady-terminal window/unit/epoch/method"));
        if(E.RecipeId==TEXT("vugts-barge")&&(S.Id==TEXT("added_mass")||S.Id==TEXT("damping"))&&
            ((S.MotionMode!=TEXT("heave")&&S.MotionMode!=TEXT("roll"))||S.Normalization.IsEmpty()||!ValidWindow||S.AbscissaEpoch.IsEmpty()||S.SamplingConvention!=TEXT("original-frequency-curve")))
            C.Missing.AddUnique(S.Id+TEXT(": original heave/roll, frequency, normalization and fit window/method"));
        if(E.RecipeId==TEXT("vugts-barge")&&(S.Id==TEXT("added_mass")||S.Id==TEXT("damping"))&&E.OriginalRunSpec&&
            E.OriginalRunSpec->Geometry.BodyMotion!=TEXT("forced-")+S.MotionMode)
            C.Missing.AddUnique(S.Id+TEXT(": original motion mode does not match the original run specification"));
    }
    if(E.RecipeId==TEXT("vugts-barge"))
    {
        const auto* A=E.Series.FindByPredicate([](const auto& S){return S.Id==TEXT("added_mass");});const auto* D=E.Series.FindByPredicate([](const auto& S){return S.Id==TEXT("damping");});
        if(A&&D&&(A->MotionMode!=D->MotionMode||A->Normalization!=D->Normalization||A->AbscissaName!=D->AbscissaName||A->AbscissaUnit!=D->AbscissaUnit||A->AbscissaEpoch!=D->AbscissaEpoch))C.Missing.AddUnique(TEXT("added_mass/damping: matching original motion/frequency/normalization scope"));
    }
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
