#include "StudioHome4Reports.h"
#include "StudioHome4Validation.h"

bool StudioHome4Reports::FigureRun(const FStudioHome4Spec& Base,FStudioHome4Spec& Out,FString& Error)
{
    if(!StudioHome4Config::Validate(Base,Error))return false;
    const FString Directory=Base.Run.OutDirectory.TrimStartAndEnd();
    if(Directory.IsEmpty()||Directory.Len()>4000||Directory.EndsWith(TEXT("/"))||Directory.EndsWith(TEXT("\\")))
    {Error=TEXT("Figure run requires an explicit original output directory without a trailing separator.");return false;}
    FStudioHome4Spec Spec=Base;Spec.Run.OutDirectory=Directory.EndsWith(TEXT("_viz"))?Directory:Directory+TEXT("_viz");
    Spec.Run.VizDirectory=Spec.Run.OutDirectory;Spec.Run.Tag=Base.Run.Tag.Left(90)+TEXT("_viz");Spec.BranchId=FGuid::NewGuid().ToString();
    if(!StudioHome4Config::Validate(Spec,Error))return false;Out=MoveTemp(Spec);Error.Empty();return true;
}
bool StudioHome4Reports::PublicationCheck(const FStudioHome4ReferenceEvidence& Current,const FStudioHome4ReferenceEvidence& Published,
    double Absolute,double Relative,FString& Error)
{
    if(Current.RecipeId!=Published.RecipeId||Current.RunId==Published.RunId||!FMath::IsFinite(Absolute)||Absolute<0||!FMath::IsFinite(Relative)||Relative<0||
        !Current.OriginalRunSpec||!Current.OriginalRunSpec->Run.OutDirectory.EndsWith(TEXT("_viz")))
    {Error=TEXT("Publication comparison requires independent identified original figure/published runs, matching recipe, _viz destination and explicit finite tolerances.");return false;}
    if(!StudioHome4Validation::VerifyOriginalBytes(Current,Error)||!StudioHome4Validation::VerifyOriginalBytes(Published,Error))return false;
    if(Published.RecipeGateStatus()!=TEXT("passed"))
    {Error=TEXT("Published coefficient reference has not completed its original verified recipe gate.");return false;}
    if(Current.Series.IsEmpty()){Error=TEXT("No original figure-run coefficient series supplied.");return false;}
    for(const auto& Ref:Published.Series)
    {
        const auto* Actual=Current.Series.FindByPredicate([&](const auto& S){return S.Id==Ref.Id;});
        if(!Actual||Actual->Unit!=Ref.Unit||Actual->AbscissaUnit!=Ref.AbscissaUnit||Actual->Abscissae!=Ref.Abscissae)
        {Error=TEXT("Figure-run coefficient identities, units or original sampled abscissae do not match the published evidence.");return false;}
        const auto Gate=StudioHome4Recipes::Compare(Actual->Actual,Ref.Actual,Absolute,Relative,Published.ActualSource);
        if(!Gate.bEvaluated||!Gate.bPassed){Error=TEXT("Figure run differs from published original coefficients: ")+Ref.Id+TEXT(" · ")+Gate.Reason;return false;}
    }
    Error.Empty();return true;
}
