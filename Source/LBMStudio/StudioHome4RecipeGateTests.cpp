#include "StudioHome4RecipeGates.h"
#include "StudioHome4Validation.h"
#include "StudioHome4Runtime.h"
#include "StudioHome4Recipes.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4RecipeGateTestFixtures
{
    FStudioHome4ReferenceEvidence Evidence(const FStudioHome4Spec& Spec,FGuid Run)
    {
        FStudioHome4ReferenceEvidence E;E.RecipeId=Spec.RecipeId;E.RunId=Run;E.ActualSource=TEXT("Original numerical unit fixture");E.ReferenceSource=TEXT("Identified reference unit fixture");
        E.OriginalRunSpec=Spec;E.bReferenceOwnerVerified=true;E.ReferenceCitation=TEXT("Explicit unit-test reference source, not literature");E.ReferenceSHA256=FString::ChrN(64,'b');
        for(const auto& Id:StudioHome4RecipeGates::RequiredMetrics(E.RecipeId))
        {
            FStudioHome4ReferenceSeries S;S.Id=Id;S.Name=Id;S.AbscissaName=TEXT("t*");S.AbscissaUnit=TEXT("dimensionless");S.Unit=TEXT("original fixture unit");
            S.Abscissae={0,1,2};S.Actual={1,1,1};S.Reference={1,1,1};S.AbsoluteTolerance=.01;S.RelativeTolerance=0;
            if(E.RecipeId==TEXT("sedimentation")||E.RecipeId==TEXT("vugts-barge"))S.AbscissaEpoch=TEXT("explicit original fixture zero");
            if(E.RecipeId==TEXT("sedimentation")){S.SamplingConvention=TEXT("steady-terminal");S.ExtractionMethod=TEXT("original steady arithmetic mean");S.ExtractionWindowStart=0;S.ExtractionWindowEnd=2;S.ExtractionWindowUnit=S.AbscissaUnit;S.ExtractionEpoch=S.AbscissaEpoch;}
            if(E.RecipeId==TEXT("vugts-barge")){S.MotionMode=TEXT("heave");S.Normalization=TEXT("identified unit fixture normalization");S.SamplingConvention=TEXT("original-frequency-curve");S.ExtractionMethod=TEXT("original harmonic least-squares fit");S.ExtractionWindowStart=0;S.ExtractionWindowEnd=2;S.ExtractionWindowUnit=TEXT("s");S.ExtractionEpoch=TEXT("original fit time zero");S.AbscissaName=TEXT("frequency");S.AbscissaUnit=TEXT("Hz");}
            S.Gate=StudioHome4Recipes::Compare(S.Actual,S.Reference,S.AbsoluteTolerance,S.RelativeTolerance,E.ReferenceSource);E.Series.Add(S);
        }
        return E;
    }
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4RecipeCoverageContractsTest,"Studio.Home4.Validation.TenRecipeMetricCoverageOriginalSpecAndVerification",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4RecipeCoverageContractsTest::RunTest(const FString&)
{
    using namespace StudioHome4RecipeGateTestFixtures;FString Error;
    TestEqual(TEXT("All ten supplied cards have coverage contracts"),StudioHome4Recipes::All().Num(),10);
    for(const auto& Recipe:StudioHome4Recipes::All())
    {
        auto E=Evidence(Recipe.Template,FGuid::NewGuid());FStudioHome4ReferenceExpectation X;X.RecipeId=Recipe.Id;FStudioHome4ReferenceEvidence Parsed;
        TestFalse(TEXT("Required metric contract cannot be empty"),StudioHome4RecipeGates::RequiredMetrics(Recipe.Id).IsEmpty());
        TestTrue(TEXT("Explicit original source contract parses"),StudioHome4Validation::Parse(StudioHome4Validation::SerializeEvidence(E),X,Parsed,Error));
        TestEqual(TEXT("All required original metric comparisons can complete"),Parsed.RecipeGateStatus(),FString(TEXT("passed")));
        TestFalse(TEXT("Exact original evidence bytes retained"),Parsed.OriginalBytes.IsEmpty());
        Parsed.bReferenceOwnerVerified=false;
        TestEqual(TEXT("Unverified reference cannot pass recipe gate"),Parsed.RecipeGateStatus(),FString(TEXT("not_evaluated")));
        Parsed.bReferenceOwnerVerified=true;Parsed.Series.RemoveAt(0);
        TestEqual(TEXT("One missing required metric blocks complete recipe gate"),Parsed.RecipeCoverage(),FString(TEXT("missing_metrics")));
    }
    auto C=Evidence(StudioHome4Recipes::Find(TEXT("colagrossi-wb"))->Template,FGuid::NewGuid());
    C.Series.RemoveAll([](const auto& S){return S.Id!=TEXT("Cd");});
    TestEqual(TEXT("Drag-only cannot pass Colagrossi lift/profile contract"),C.RecipeGateStatus(),FString(TEXT("not_evaluated")));
    auto RTI=Evidence(StudioHome4Recipes::Find(TEXT("rti-fakhari"))->Template,FGuid::NewGuid());
    RTI.OriginalRunSpec->Reference.Atwood=.6;
    TestEqual(TEXT("Original envelope departure prevents transferred gate"),RTI.RecipeCoverage(),FString(TEXT("outside_recipe_envelope")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4OriginalLadderExtractionTest,"Studio.Home4.Validation.ExactRungAttachmentWindowExtractionAndSelectedTriplet",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4OriginalLadderExtractionTest::RunTest(const FString&)
{
    using namespace StudioHome4RecipeGateTestFixtures;FString Error;
    FStudioHome4ValidationState State;State.ProjectId=FGuid::NewGuid();State.CaseId=FGuid::NewGuid();State.RecipeId=TEXT("couette-spin");
    const int32 Factors[]={1,2,4,8};const double Values[]={4,2,1.5,1.375};
    for(int32 I=0;I<4;++I)
    {
        FStudioHome4LadderRung R;R.Refinement=Factors[I];R.Spec=StudioHome4Recipes::Find(State.RecipeId)->Template;
        R.Spec.Run.Tag=FString::Printf(TEXT("exact-rung-%d"),Factors[I]);State.Ladder.Add(R);
        auto E=Evidence(R.Spec,R.PlannedRunId);E.AttachedProjectId=State.ProjectId;E.AttachedCaseId=State.CaseId;
        E.Series[0].Actual={Values[I],Values[I],Values[I]};
        FStudioHome4ReferenceEvidence Parsed;FStudioHome4ReferenceExpectation Expected;Expected.RecipeId=State.RecipeId;
        TestTrue(TEXT("Original aligned rung fixture parses"),StudioHome4Validation::Parse(StudioHome4Validation::SerializeEvidence(E),Expected,Parsed,Error));
        Parsed.AttachedProjectId=State.ProjectId;Parsed.AttachedCaseId=State.CaseId;E=MoveTemp(Parsed);
        if(I==0)
        {
            auto Wrong=E;Wrong.OriginalRunSpec->Run.Tag=TEXT("different-run-spec");
            TestFalse(TEXT("Same identity with wrong immutable spec rejected"),StudioHome4Validation::AttachLadderResult(State,Wrong,Error));
            Wrong=E;Wrong.AttachedCaseId=FGuid::NewGuid();
            TestFalse(TEXT("Foreign case evidence rejected"),StudioHome4Validation::AttachLadderResult(State,Wrong,Error));
        }
        TestTrue(TEXT("Exact original result attaches independently of protocol completion"),StudioHome4Validation::AttachLadderResult(State,E,Error));
        TestFalse(TEXT("Same original run cannot be silently replaced"),StudioHome4Validation::AttachLadderResult(State,E,Error));
    }
    FStudioHome4ExtractionPolicy P;P.Metric=TEXT("torque");P.Unit=TEXT("original fixture unit");P.AbscissaUnit=TEXT("dimensionless");P.Epoch=TEXT("original run t*=0");P.Method=TEXT("trapezoid_mean");P.WindowStart=0;P.WindowEnd=2;
    TestTrue(TEXT("Explicit common original window extracts four retained results"),StudioHome4Validation::ApplyExtraction(State,P,Error));
    TestTrue(TEXT("Three of four rungs assemble by explicit triplet index"),StudioHome4Validation::AssembleConvergence(State,1,Error));
    TestEqual(TEXT("All four original results retained"),State.Results.Num(),4);
    TestTrue(TEXT("Selected fine triplet reports observed order"),State.ConvergenceEvidence->ObservedOrder.IsSet());
    TestEqual(TEXT("Observed selected order"),State.ConvergenceEvidence->ObservedOrder.Get(0),2.);
    TestEqual(TEXT("Selected coarse scalar run identity retained"),State.ConvergenceEvidence->OrderRuns[0].RunId,State.Ladder[1].PlannedRunId);
    const auto Before=State.Results[0].Scalar;
    P.WindowStart=.5;
    TestFalse(TEXT("Unsampled endpoint cannot be silently interpolated"),StudioHome4Validation::ApplyExtraction(State,P,Error));
    TestEqual(TEXT("Rejected policy preserves previous original extraction"),State.Results[0].Scalar->Value,Before->Value);
    TestFalse(TEXT("Out-of-range triplet rejected"),StudioHome4Validation::AssembleConvergence(State,MAX_int32,Error));
    return true;
}
#endif
