#include "SStudioHome4Validation.h"
#include "StudioHome4Runtime.h"
#include "StudioHome4Reports.h"
#include "StudioModel.h"
#include "StudioHeadlessSlate.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4NativeLadderFixtures
{
    class FWorkflow final:public IAutomationLatentCommand
    {
    public:
        explicit FWorkflow(FAutomationTestBase& T):Test(T)
        {
            Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-native-ladder")/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Root,true);
            M=MakeShared<FStudioModel>(Root/TEXT("project"));auto S=StudioHome4Recipes::Find(TEXT("couette-spin"))->Template;
            S.Lattice.Extents=FIntVector(32,32,8);S.Reference.LengthCells=8;S.Reference.TimeSteps=100;S.Fluids.Xi=5;S.Run.Steps=400;M->Project.Draft.Home4=S;
            Runtime=MakeShared<FStudioHome4RuntimeSession>(M);FStudioHome4BackendVerification V;V.Id=FGuid::NewGuid();V.Target=TEXT("ladder-fixture");V.Host=TEXT("ladder-fixture");V.Device=TEXT("ladder-fixture");V.Source=TEXT("Development control test only");V.EffectiveBackend=EStudioHome4Backend::Metal;V.ExtensionImported=true;V.bDevelopmentResponse=true;FString Error;Runtime->SetBackendVerification(V,Error);
            State=MakeShared<FStudioHome4ValidationState>();Panel=SNew(SStudioHome4Validation).Model(M).State(State).Runtime(Runtime);UI=MakeUnique<FStudioHeadlessSlate>(Test,Panel.ToSharedRef(),FVector2D(1200,4000));
            UI->Type(TEXT("Home4RefinementFactors"),TEXT("1,2,4"));UI->Press(TEXT("Home4ValidationBuildLadder"));
            if(State->Ladder.Num()!=3){Test.AddError(State->Status);bFailed=true;return;}
            UI->Press(TEXT("Home4ValidationQueueLadder"));
            Test.TestEqual(TEXT("Native ladder submits three immutable development requests"),Runtime->QueueJobs().Num(),3);
            const FGuid First=State->Ladder[0].PlannedRunId;UI->Press(TEXT("Home4LadderCancel0"));
            Test.TestTrue(TEXT("Explicit native cancellation has owned outcome"),Runtime->QueueJobs()[0].State==EStudioHome4QueueState::Cancelled);
            UI->Press(TEXT("Home4LadderRetry0"));Test.TestNotEqual(TEXT("Retry gets new original planned identity"),State->Ladder[0].PlannedRunId,First);
            Test.TestEqual(TEXT("Earlier cancelled request stays in queue/history"),Runtime->QueueJobs().Num(),4);
            Test.TestTrue(TEXT("Numerical result cannot be inferred from retry ACK"),State->Results.IsEmpty());
            UI->Type(TEXT("Home4ExtractionMetric"),TEXT("torque"));UI->Type(TEXT("Home4ExtractionUnit"),TEXT("N m"));UI->Type(TEXT("Home4ExtractionTimeUnit"),TEXT("dimensionless"));
            UI->Type(TEXT("Home4ExtractionEpoch"),TEXT("fixture original t*=0"));UI->Type(TEXT("Home4ExtractionMethod"),TEXT("trapezoid_mean"));UI->Type(TEXT("Home4ExtractionStart"),TEXT("0"));UI->Type(TEXT("Home4ExtractionEnd"),TEXT("2"));UI->Press(TEXT("Home4ApplyExtraction"));
            Test.TestTrue(TEXT("Native common extraction policy retained before original results"),State->ExtractionPolicy.IsSet());
            Started=FPlatformTime::Seconds();BeginResult(0);
        }
        ~FWorkflow(){UI.Reset();Panel.Reset();Runtime.Reset();M.Reset();IFileManager::Get().DeleteDirectory(*Root,false,true);}
        bool Update()override
        {
            if(bFailed)return true;Panel->PollImport();Panel->Tick(FGeometry(),0,0);UI->Layout();
            if(FPlatformTime::Seconds()-Started>20){Test.AddError(TEXT("Native ladder original result workflow exceeded bounded deadline."));return true;}
            if(Panel->IsImporting())return false;
            if(State->Results.Num()!=NextResult+1){Test.AddError(State->Status);return true;}
            if(++NextResult<3){BeginResult(NextResult);return false;}
            UI->Type(TEXT("Home4ConvergenceFirstRung"),TEXT("0"));UI->Press(TEXT("Home4AssembleConvergence"));
            if(!Test.TestTrue(TEXT("Native original three-run convergence assembled"),State->ConvergenceEvidence.IsValid()))return true;
            Test.TestEqual(TEXT("Original extracted triplet has expected observed order"),State->ConvergenceEvidence->ObservedOrder.Get(-1),2.);
            for(const auto& R:State->Results)Test.TestTrue(TEXT("Every scalar retains exact method/window/epoch/source provenance"),R.Scalar&&R.Scalar->Extraction.bEpochConfirmedFromOriginal&&R.Scalar->Extraction.Method==TEXT("trapezoid_mean")&&R.Scalar->Extraction.WindowEnd.Get(-1)==2);
            FStudioHome4ReportInputs Inputs;Inputs.Validation=State.Get();FString Out,Error;
            if(!Test.TestTrue(TEXT("Native ladder result bundle publishes original rungs and convergence"),StudioHome4Reports::Export(Root,TEXT("original-ladder-report"),M->SnapshotProject(),nullptr,Out,Error,State->Evidence.Get(),nullptr,nullptr,&Inputs))){Test.AddError(Error);return true;}
            Test.TestTrue(TEXT("Original fine rung bytes retained"),IFileManager::Get().FileExists(*(Out/TEXT("original-rung-02.json"))));
            Test.TestTrue(TEXT("Original convergence numeric table retained"),IFileManager::Get().FileExists(*(Out/TEXT("ladder-convergence.csv"))));
            UI->Press(TEXT("Home4ClearLadder"));Test.TestTrue(TEXT("Explicit clear removes current attachment"),State->Ladder.IsEmpty()&&State->Results.IsEmpty()&&!State->ConvergenceEvidence);
            Test.TestEqual(TEXT("Clear preserves original request history"),Runtime->QueueJobs().Num(),4);return true;
        }
    private:
        void BeginResult(int32 I)
        {
            const double Values[]={4,2,1.5};const auto& R=State->Ladder[I];FStudioHome4ReferenceEvidence E;E.RunId=R.PlannedRunId;E.RecipeId=State->RecipeId;
            E.OriginalRunSpec=R.Spec;E.ActualSource=TEXT("Explicit original scalar automation fixture, not CFD");E.ReferenceSource=TEXT("Identified automation oracle, not literature");
            FStudioHome4ReferenceSeries S;S.Id=TEXT("torque");S.Name=TEXT("Original torque fixture");S.Unit=TEXT("N m");S.AbscissaName=TEXT("t*");S.AbscissaUnit=TEXT("dimensionless");S.AbscissaEpoch=TEXT("fixture original t*=0");
            S.Abscissae={0,1,2};S.Actual={Values[I],Values[I],Values[I]};S.Reference=S.Actual;S.AbsoluteTolerance=0;S.RelativeTolerance=0;E.Series.Add(MoveTemp(S));
            const FString Path=Root/FString::Printf(TEXT("original-rung-%d.json"),I);FFileHelper::SaveStringToFile(StudioHome4Validation::SerializeEvidence(E),*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            FStudioHome4ReferenceExpectation X;X.RecipeId=State->RecipeId;X.RunId=R.PlannedRunId;if(!Panel->ImportPath(Path,X)){Test.AddError(State->Status);bFailed=true;}
        }
        FAutomationTestBase& Test;FString Root;double Started=0;int32 NextResult=0;bool bFailed=false;
        TSharedPtr<FStudioModel> M;TSharedPtr<FStudioHome4RuntimeSession> Runtime;TSharedPtr<FStudioHome4ValidationState> State;TSharedPtr<SStudioHome4Validation> Panel;TUniquePtr<FStudioHeadlessSlate> UI;
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4NativeOriginalLadderJourneyTest,"Studio.Home4.Validation.NativeQueueRetryOriginalExtractionConvergenceAndReport",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4NativeOriginalLadderJourneyTest::RunTest(const FString&)
{if(!FSlateApplication::IsInitialized()){AddError(TEXT("Native Slate unavailable."));return false;}ADD_LATENT_AUTOMATION_COMMAND(StudioHome4NativeLadderFixtures::FWorkflow(*this));return true;}
#endif
