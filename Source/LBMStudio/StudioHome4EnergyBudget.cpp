#include "StudioHome4EnergyBudget.h"
#include "Dom/JsonObject.h"

namespace StudioHome4EnergyBudgetPrivate
{
    bool Inside(const FBox& Near,const FBox& Far)
    {for(int32 I=0;I<3;++I)if(Near.Min[I]<=Far.Min[I]||Near.Max[I]>=Far.Max[I])return false;return true;}
    FString Vector(const FVector& V)
    {return FString::Printf(TEXT("[%.6g, %.6g, %.6g]"),V.X,V.Y,V.Z);}
}
bool StudioHome4EnergyBudget::Validate(const TArray<FStudioHome4EnergyBudgetRegion>& Regions,FString& Error)
{
    if(Regions.Num()>3){Error=TEXT("Supply at most three original near/far/air boxes.");return false;}
    TSet<FString> Roles;
    for(const auto& R:Regions)
    {
        if((R.Role!=TEXT("near")&&R.Role!=TEXT("far")&&R.Role!=TEXT("air"))||Roles.Contains(R.Role)||R.BodyId.TrimStartAndEnd().IsEmpty()||R.BodyId.Len()>64||R.Frame!=TEXT("body-CoG-local-XYZ")||(R.Tracking!=TEXT("follow-body")&&R.Tracking!=TEXT("fixed-initial-body"))||R.PhaseMask.TrimStartAndEnd().IsEmpty()||R.PhaseMask.Len()>256||
            (R.Role==TEXT("far")?R.Region!=TEXT("shell-excluding-near"):R.Region!=TEXT("inside-box"))||
            (R.Units!=TEXT("root-cells")&&R.Units!=TEXT("body-lengths")&&R.Units!=TEXT("physical-metres"))||
            R.Minimum.ContainsNaN()||R.Maximum.ContainsNaN()||R.Minimum.GetAbsMax()>1.e12||R.Maximum.GetAbsMax()>1.e12||(R.Maximum-R.Minimum).GetMin()<=0)
        {Error=TEXT("Budget boxes need unique near/far/air roles, explicit body identity/units and finite increasing XYZ bounds.");return false;}
        for(TCHAR C:R.BodyId+R.PhaseMask)if(C<32||C==127){Error=TEXT("Budget body identity contains a control character.");return false;}
        Roles.Add(R.Role);
    }
    const auto* Near=Regions.FindByPredicate([](const auto& R){return R.Role==TEXT("near");});
    const auto* Far=Regions.FindByPredicate([](const auto& R){return R.Role==TEXT("far");});
    if(Far&&!Near){Error=TEXT("Far shell requires its explicitly declared near exclusion box.");return false;}
    if(Near&&Far&&(Near->BodyId!=Far->BodyId||Near->Frame!=Far->Frame||Near->Tracking!=Far->Tracking||Near->PhaseMask!=Far->PhaseMask)){Error=TEXT("Near/far boxes must bind the same explicit body frame.");return false;}
    if(Near&&Far&&Near->BodyId==Far->BodyId&&Near->Units==Far->Units&&
        !StudioHome4EnergyBudgetPrivate::Inside(FBox(Near->Minimum,Near->Maximum),FBox(Far->Minimum,Far->Maximum)))
    {Error=TEXT("Near must lie strictly inside far in the same original body/unit frame.");return false;}
    Error.Empty();return true;
}
TOptional<FBox> StudioHome4EnergyBudget::RootBox(const FStudioHome4EnergyBudgetRegion& R,const FStudioHome4Spec& Map)
{
    TOptional<double> Scale;
    if(R.Units==TEXT("root-cells"))Scale=1.;
    else if(R.Units==TEXT("body-lengths"))Scale=Map.Reference.LengthCells;
    else if(R.Units==TEXT("physical-metres")&&Map.Units.DxMeters&&*Map.Units.DxMeters>0)Scale=1. / *Map.Units.DxMeters;
    if(!Scale||!FMath::IsFinite(*Scale)||*Scale<=0)return {};
    const FVector Min=R.Minimum * *Scale,Max=R.Maximum * *Scale;
    if(Min.ContainsNaN()||Max.ContainsNaN())return {};
    return FBox(Min,Max);
}
bool StudioHome4EnergyBudget::ValidateRequest(const FStudioHome4Spec& Spec,FString& Error)
{
    return ValidateMapped(Spec.Authoring.EnergyBudgetRegions,Spec,Error)&&StudioHome4Config::Validate(Spec,Error);
}
bool StudioHome4EnergyBudget::ValidateMapped(const TArray<FStudioHome4EnergyBudgetRegion>& Regions,const FStudioHome4Spec& Map,FString& Error)
{
    if(!Validate(Regions,Error))return false;
    const auto* Near=Regions.FindByPredicate([](const auto& R){return R.Role==TEXT("near");});
    const auto* Far=Regions.FindByPredicate([](const auto& R){return R.Role==TEXT("far");});
    if(Near&&Far&&Near->Units!=Far->Units)
    {
        const auto N=RootBox(*Near,Map),F=RootBox(*Far,Map);
        if(!N||!F){Error=TEXT("Mixed-unit near/far nesting requires their explicit original body-length or physical unit map.");return false;}
        if(!StudioHome4EnergyBudgetPrivate::Inside(*N,*F)){Error=TEXT("Near must lie strictly inside far after the explicitly supplied unit map is applied.");return false;}
    }
    Error.Empty();return true;
}
FString StudioHome4EnergyBudget::Description(const TArray<FStudioHome4EnergyBudgetRegion>& Regions,const FStudioHome4Spec* Map)
{
    if(Regions.IsEmpty())return TEXT("Energy-budget domains not supplied. Near/far/air terms do not identify their spatial regions.");
    FString Result=TEXT("Body-CoG local XYZ boxes with explicit tracking, phase mask and integration region; no world pose or numerical integral inferred.");
    for(const auto& R:Regions)Result+=TEXT("\n")+R.Role+TEXT(" · body ")+R.BodyId+TEXT(" · ")+R.Frame+TEXT(" · ")+R.Tracking+TEXT(" · ")+R.Region+TEXT(" · phase ")+R.PhaseMask+TEXT(" · ")+R.Units+TEXT(" · ")+StudioHome4EnergyBudgetPrivate::Vector(R.Minimum)+TEXT(" to ")+StudioHome4EnergyBudgetPrivate::Vector(R.Maximum);
    for(const auto* Role:{TEXT("near"),TEXT("far"),TEXT("air")})if(!Regions.ContainsByPredicate([Role](const auto& R){return R.Role==Role;}))Result+=TEXT("\n")+FString(Role)+TEXT(" domain not supplied; spatial region unknown.");
    const auto* Near=Regions.FindByPredicate([](const auto& R){return R.Role==TEXT("near");});const auto* Far=Regions.FindByPredicate([](const auto& R){return R.Role==TEXT("far");});
    if(Near&&Far)
    {
        const bool Same=Near->BodyId==Far->BodyId&&Near->Units==Far->Units;
        const bool Converted=Map&&Near->BodyId==Far->BodyId&&RootBox(*Near,*Map)&&RootBox(*Far,*Map);
        Result+=(Same||Converted)?TEXT("\nNear/far nesting uses the declared common body/unit frame."):TEXT("\nNear/far nesting unknown: original body/unit conversion is not supplied.");
    }
    return Result;
}
bool StudioHome4EnergyBudget::ParseOriginal(const TArray<TSharedPtr<FJsonValue>>& Values,TArray<FStudioHome4EnergyBudgetRegion>& Out,FString& Error)
{
    if(Values.Num()>3){Error=TEXT("Original budget domains exceed three boxes.");return false;}
    TArray<FStudioHome4EnergyBudgetRegion> Candidate;
    for(const auto& V:Values)
    {
        if(!V||V->Type!=EJson::Object){Error=TEXT("Original budget domain must be an object.");return false;}
        const auto O=V->AsObject();const TSet<FString> Known={TEXT("role"),TEXT("body_id"),TEXT("coordinate_unit"),TEXT("frame"),TEXT("tracking"),TEXT("region"),TEXT("phase_mask"),TEXT("minimum"),TEXT("maximum")};
        for(const auto& P:O->Values)if(!Known.Contains(FString(*P.Key))){Error=TEXT("Unknown original budget domain field.");return false;}
        FStudioHome4EnergyBudgetRegion R;
        if(!O->TryGetStringField(TEXT("role"),R.Role)||!O->TryGetStringField(TEXT("body_id"),R.BodyId)||!O->TryGetStringField(TEXT("coordinate_unit"),R.Units)||!O->TryGetStringField(TEXT("frame"),R.Frame)||!O->TryGetStringField(TEXT("tracking"),R.Tracking)||!O->TryGetStringField(TEXT("region"),R.Region)||!O->TryGetStringField(TEXT("phase_mask"),R.PhaseMask))
        {Error=TEXT("Original budget domain requires explicit role/body/units/frame/tracking/integration-region/phase-mask.");return false;}
        for(bool Min:{true,false})
        {
            const auto A=O->TryGetField(Min?TEXT("minimum"):TEXT("maximum"));
            if(!A||A->Type!=EJson::Array||A->AsArray().Num()!=3){Error=TEXT("Original budget box bounds need three explicit coordinates.");return false;}
            for(int32 I=0;I<3;++I){const auto N=A->AsArray()[I];double X=0;if(!N||N->Type!=EJson::Number||!N->TryGetNumber(X)||!FMath::IsFinite(X)){Error=TEXT("Original budget coordinates must be finite numbers.");return false;}(Min?R.Minimum:R.Maximum)[I]=X;}
        }
        Candidate.Add(MoveTemp(R));
    }
    if(!Validate(Candidate,Error))return false;
    Out=MoveTemp(Candidate);Error.Empty();return true;
}
TArray<TSharedPtr<FJsonValue>> StudioHome4EnergyBudget::OriginalJSON(const TArray<FStudioHome4EnergyBudgetRegion>& Regions)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    for(const auto& R:Regions)
    {
        auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("role"),R.Role);O->SetStringField(TEXT("body_id"),R.BodyId);O->SetStringField(TEXT("coordinate_unit"),R.Units);O->SetStringField(TEXT("frame"),R.Frame);O->SetStringField(TEXT("tracking"),R.Tracking);O->SetStringField(TEXT("region"),R.Region);O->SetStringField(TEXT("phase_mask"),R.PhaseMask);
        for(bool Min:{true,false}){TArray<TSharedPtr<FJsonValue>> A;const FVector V=Min?R.Minimum:R.Maximum;for(int32 I=0;I<3;++I)A.Add(MakeShared<FJsonValueNumber>(V[I]));O->SetArrayField(Min?TEXT("minimum"):TEXT("maximum"),A);}
        Values.Add(MakeShared<FJsonValueObject>(O));
    }
    return Values;
}
