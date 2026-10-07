#include "StudioHome4Validation.h"
#include "Dom/JsonObject.h"

namespace StudioHome4LadderWorkflowPrivate
{
    bool Clean(const FString& V)
    { if(V.IsEmpty()||V.Len()>256)return false;for(TCHAR C:V)if(C<32||C==127)return false;return true; }
    bool SameSpec(const FStudioHome4Spec& A,const FStudioHome4Spec& B)
    { return StudioHome4Config::Serialize(A)==StudioHome4Config::Serialize(B); }
}
bool StudioHome4Validation::ExtractScalar(const FStudioHome4ReferenceEvidence& E,int32 Refinement,
    const FStudioHome4ExtractionPolicy& P,FStudioHome4ScalarRun& Out,FString& Error)
{
    using namespace StudioHome4LadderWorkflowPrivate;
    if(Refinement<=0||!Clean(P.Metric)||!Clean(P.Unit)||!Clean(P.AbscissaUnit)||!Clean(P.Epoch)||
        !FMath::IsFinite(P.WindowStart)||!FMath::IsFinite(P.WindowEnd)||P.WindowEnd<=P.WindowStart||
        (P.Method!=TEXT("arithmetic_mean")&&P.Method!=TEXT("trapezoid_mean")&&P.Method!=TEXT("last")))
    {Error=TEXT("Extraction needs explicit metric/units/epoch, increasing finite bounds and arithmetic_mean, trapezoid_mean or last method.");return false;}
    const auto* S=E.Series.FindByPredicate([&](const auto& V){return V.Id==P.Metric;});
    if(!S||S->Unit!=P.Unit||S->AbscissaUnit!=P.AbscissaUnit||S->Abscissae.Num()!=S->Actual.Num())
    {Error=TEXT("Original extraction metric or value/time units do not match the supplied series.");return false;}
    if(!S->AbscissaEpoch.IsEmpty()&&S->AbscissaEpoch!=P.Epoch)
    {Error=TEXT("Declared extraction epoch differs from the exact original series epoch.");return false;}
    int32 First=INDEX_NONE,Last=INDEX_NONE;
    for(int32 I=0;I<S->Abscissae.Num();++I)
    {
        if(S->Abscissae[I]==P.WindowStart)First=I;
        if(S->Abscissae[I]==P.WindowEnd)Last=I;
    }
    if(First==INDEX_NONE||Last==INDEX_NONE||Last<=First)
    {Error=TEXT("Both window endpoints must be original sampled abscissae; no interpolation or inferred alignment is performed.");return false;}
    double Value=0;
    if(P.Method==TEXT("last"))Value=S->Actual[Last];
    else if(P.Method==TEXT("arithmetic_mean"))
    {
        for(int32 I=First;I<=Last;++I)Value+=S->Actual[I]/double(Last-First+1);
    }
    else
    {
        const double Width=P.WindowEnd-P.WindowStart;
        for(int32 I=First;I<Last;++I)Value+=(S->Actual[I]/2+S->Actual[I+1]/2)*((S->Abscissae[I+1]-S->Abscissae[I])/Width);
    }
    if(!FMath::IsFinite(Value)||E.SourceSHA256.Len()!=64||E.ActualSource.IsEmpty())
    {Error=TEXT("Original scalar extraction exceeds finite range or lacks original source identity/hash.");return false;}
    FStudioHome4ScalarRun R;R.RunId=E.RunId;R.Refinement=Refinement;R.Value=Value;
    R.Extraction.WindowStart=P.WindowStart;R.Extraction.WindowEnd=P.WindowEnd;R.Extraction.AbscissaUnit=P.AbscissaUnit;
    R.Extraction.Epoch=P.Epoch;R.Extraction.Method=P.Method;R.Extraction.Source=E.SourcePath.IsEmpty()?E.ActualSource:E.SourcePath;R.Extraction.SourceSHA256=E.SourceSHA256;R.Extraction.bEpochConfirmedFromOriginal=!S->AbscissaEpoch.IsEmpty();
    Out=MoveTemp(R);Error.Empty();return true;
}
bool StudioHome4Validation::AttachLadderResult(FStudioHome4ValidationState& State,const FStudioHome4ReferenceEvidence& E,FString& Error)
{
    const auto* R=State.Ladder.FindByPredicate([&](const auto& V){return V.PlannedRunId==E.RunId;});
    if(!R||State.RecipeId!=E.RecipeId||E.AttachedProjectId!=State.ProjectId||E.AttachedCaseId!=State.CaseId)
    {Error=TEXT("Result must match a planned run and this exact project/case/recipe scope.");return false;}
    if(!E.OriginalRunSpec||!StudioHome4LadderWorkflowPrivate::SameSpec(*E.OriginalRunSpec,R->Spec))
    {Error=TEXT("Result requires the exact immutable original rung specification; current draft or guessed parameters cannot substitute.");return false;}
    if(!VerifyOriginalBytes(E,Error))return false;
    if(State.Results.ContainsByPredicate([&](const auto& V){return V.RunId==E.RunId;}))
    {Error=TEXT("Original rung result is already attached; remove it explicitly before replacing evidence.");return false;}
    auto SourceBytes=[](const FStudioHome4ReferenceEvidence& V){return int64(V.OriginalBytes.Num())+V.ActualOriginalBytes.Num()+V.ReferenceOriginalBytes.Num();};
    int64 Total=SourceBytes(E);for(const auto& Existing:State.Results)if(Existing.Evidence)Total+=SourceBytes(*Existing.Evidence);
    if(E.OriginalBytes.IsEmpty()||Total>32LL*1024*1024)
    {Error=TEXT("Ladder needs exact original evidence bytes within its 32 MiB retained source budget.");return false;}
    FStudioHome4LadderResult Result;Result.RunId=E.RunId;Result.Refinement=R->Refinement;Result.Evidence=MakeShared<FStudioHome4ReferenceEvidence>(E);
    Result.Status=TEXT("Identified original result attached; extraction policy pending.");
    if(State.ExtractionPolicy)
    {
        FStudioHome4ScalarRun Scalar;
        if(ExtractScalar(E,R->Refinement,*State.ExtractionPolicy,Scalar,Error)) {Result.Scalar=MoveTemp(Scalar);Result.Status=TEXT("Original scalar extracted with the common declared window/method.");}
        else Result.Status=Error;
    }
    State.Results.Add(MoveTemp(Result));State.ConvergenceEvidence.Reset();Error.Empty();return true;
}
bool StudioHome4Validation::ApplyExtraction(FStudioHome4ValidationState& State,const FStudioHome4ExtractionPolicy& P,FString& Error)
{
    TArray<FStudioHome4ScalarRun> Scalars;
    for(const auto& R:State.Results)
    {
        FStudioHome4ScalarRun Scalar;
        if(!R.Evidence||!ExtractScalar(*R.Evidence,R.Refinement,P,Scalar,Error))return false;
        Scalars.Add(MoveTemp(Scalar));
    }
    // With no result yet, validate policy shape using an empty source and preserve its useful mismatch error.
    if(State.Results.IsEmpty())
    {
        FStudioHome4ReferenceEvidence Empty;FStudioHome4ScalarRun Scalar;
        ExtractScalar(Empty,1,P,Scalar,Error);
        if(!Error.StartsWith(TEXT("Original extraction metric")))return false;
    }
    State.ExtractionPolicy=P;State.ConvergenceEvidence.Reset();
    for(int32 I=0;I<State.Results.Num();++I){State.Results[I].Scalar=Scalars[I];State.Results[I].Status=TEXT("Original scalar extracted with common declared window/method.");}
    Error.Empty();return true;
}
bool StudioHome4Validation::AssembleConvergence(FStudioHome4ValidationState& State,int32 FirstIndex,FString& Error)
{
    if(!State.ExtractionPolicy||FirstIndex<0||FirstIndex>=State.Ladder.Num()-2)
    {Error=TEXT("Select three consecutive planned rungs and an explicit common extraction policy.");return false;}
    FStudioHome4ReferenceEvidence E;E.RecipeId=State.RecipeId;E.AttachedProjectId=State.ProjectId;E.AttachedCaseId=State.CaseId;
    E.OrderMetric=State.ExtractionPolicy->Metric;E.OrderUnit=State.ExtractionPolicy->Unit;
    for(int32 I=FirstIndex;I<FirstIndex+3;++I)
    {
        const auto* R=State.Results.FindByPredicate([&](const auto& V){return V.RunId==State.Ladder[I].PlannedRunId;});
        if(!R||!R->Scalar||!R->Evidence){Error=TEXT("Original evidence and extracted scalar remain pending for a selected rung.");return false;}
        E.OrderRuns.Add(*R->Scalar);
        if(I==FirstIndex)
        {E.RunId=R->RunId;E.ActualSource=R->Evidence->ActualSource;E.ReferenceSource=R->Evidence->ReferenceSource;E.Series=R->Evidence->Series;}
    }
    const double Ratio=E.OrderRuns[1].Refinement/E.OrderRuns[0].Refinement,Second=E.OrderRuns[2].Refinement/E.OrderRuns[1].Refinement;
    if(Ratio<=1||!FMath::IsNearlyEqual(Ratio,Second,1.e-12*FMath::Max(Ratio,Second)))
    {Error=TEXT("Selected original rungs require a common increasing refinement ratio.");return false;}
    E.ObservedOrder=StudioHome4Recipes::ObservedOrder(E.OrderRuns[0].Value,E.OrderRuns[1].Value,E.OrderRuns[2].Value,Ratio);
    State.ConvergenceEvidence=MakeShared<FStudioHome4ReferenceEvidence>(MoveTemp(E));
    Error.Empty();return true;
}
