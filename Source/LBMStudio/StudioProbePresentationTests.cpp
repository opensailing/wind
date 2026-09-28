#include "StudioScene.h"
#include "StudioProbeSampling.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioProbePresentationCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioProbePresentationCommand(FAutomationTestBase* InTest):Test(InTest){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>45){Test->AddError(TEXT("Presented probe frame test timed out"));return true;}
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model||GFrameCounter-ChangedFrame<3)return false;
        auto& M=*Scene->Model;
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending()||(Phase!=5&&!Scene->HasCurrentFrame()))return false;
        switch(Phase)
        {
        case 0:
        {
            Work=FPaths::ProjectSavedDir()/TEXT("Automation/ProbePresentation")/FGuid::NewGuid().ToString();
            IFileManager::Get().MakeDirectory(*Work,true);FString Error;
            Test->TestTrue(TEXT("Preserve preceding project"),StudioProjectIO::Save(Work/TEXT("original.lbms"),M.SnapshotProject(),Error));
            Workspace=M.Workspace;M.NewProject(TEXT("Published wing probe"));M.Navigate(EStudioWorkspace::Solve);
            M.EditView(TEXT("Pressure probe"),[](auto& S){S.Display.ScalarField=TEXT("pressure");});Next();break;
        }
        case 1:
        {
            auto R=Request();VerifyIdentity(R);First=StudioProbeSampling::Evaluate(R);Pinned=R;
            if(!Test->TestTrue(TEXT("Presented immutable field supplies a pressure sample"),First.Samples.Num()==1&&First.Samples[0].Value.IsSet()))return true;
            Test->TestTrue(TEXT("Presented pressure agrees with independently decoded original value"),FMath::IsNearlyEqual(First.Samples[0].Value.GetValue(),98699.78125,1.e-5));
            M.Scrub(1);M.Scrub(.7);M.Scrub(.3);Next();break;
        }
        case 2:
        {
            auto R=Request();VerifyIdentity(R);
            Test->TestFalse(TEXT("Rapid scrubs invalidate old capture results"),First.Matches(R));
            Current=StudioProbeSampling::Evaluate(R);Test->TestTrue(TEXT("Matching presented result is publishable"),Current.Matches(R));
            Test->TestEqual(TEXT("Retained earlier frame still samples the same original values"),
                StudioProbeSampling::Evaluate(Pinned).Samples[0].Value.GetValue(),First.Samples[0].Value.GetValue());
            CameraField=R.Field;Frame=Scene->PresentedFrame();Scene->Orbit(30,10);Next();break;
        }
        case 3:
        {
            auto R=Request();VerifyIdentity(R);
            Test->TestTrue(TEXT("Camera-only capture reuses the same immutable field"),R.Field==CameraField);
            Test->TestEqual(TEXT("Camera edit retains physical source frame"),Scene->PresentedFrame().Index,Frame.Index);
            Test->TestFalse(TEXT("View capture identity invalidates an older projected result"),Current.Matches(R));
            M.EditView(TEXT("Density probe"),[](auto& S){S.Display.ScalarField=TEXT("density");});Next();break;
        }
        case 4:
        {
            const auto R=Request();VerifyIdentity(R);const auto Density=StudioProbeSampling::Evaluate(R);
            Test->TestEqual(TEXT("Units follow the scalar actually presented"),Density.Unit,FString(TEXT("kg/m3")));
            Test->TestEqual(TEXT("Previous sample retains its original units"),First.Unit,FString(TEXT("Pa")));
            Test->TestFalse(TEXT("Scalar replacement rejects pressure results"),First.Matches(R));
            M.Navigate(EStudioWorkspace::Geometry);Next();break;
        }
        case 5:
            Test->TestFalse(TEXT("Geometry view exposes no CFD frame"),Scene->HasPresentedFrame());
            Test->TestFalse(TEXT("Geometry view releases its presented field"),Scene->PresentedField().IsValid());
            M.Navigate(EStudioWorkspace::Solve);Next();break;
        case 6:
            VerifyIdentity(Request());
            Test->TestTrue(TEXT("Restore preceding project"),M.RequestProjectOpen(Work/TEXT("original.lbms")));Next();break;
        case 7:M.Navigate(Workspace);return true;
        }
        return false;
    }
private:
    void Next(){++Phase;ChangedFrame=GFrameCounter;}
    FStudioProbeRequest Request()
    {
        FStudioProbeRequest R;R.ProjectId=Scene->PresentedProjectId();R.PresentationId=Scene->GetCaptureCount();
        R.Probe.Id=ProbeId;R.Probe.Name=TEXT("Pressure reference");R.Probe.A=FVector(.03250676393508911,0,.1194048523902893);
        R.Field=Scene->PresentedField();R.DisplayedScalar=Scene->PresentedScalar().Id;
        if(R.Field)if(const auto I=R.Field->Identity();I.IsSet())R.Probe.Source={I->Dataset,I->MetadataSHA256,I->PayloadSHA256};
        return R;
    }
    void VerifyIdentity(const FStudioProbeRequest& R)
    {
        if(!Test->TestTrue(TEXT("Rendered frame retains its immutable sample field"),R.Field.IsValid()))return;
        const auto I=R.Field->Identity();if(!Test->TestTrue(TEXT("Presented field has source identity"),I.IsSet()))return;
        Test->TestEqual(TEXT("Sample dataset agrees with captured pixels"),I->Dataset,Scene->PresentedDatasetId());
        Test->TestEqual(TEXT("Sample step agrees with captured pixels"),I->Frame.Index,Scene->PresentedFrame().Index);
        Test->TestEqual(TEXT("Sample time agrees with captured pixels"),I->Frame.Time,Scene->PresentedFrame().Time);
        Test->TestEqual(TEXT("Sample ordinal agrees after the pending frame completes"),I->Ordinal,Scene->Model->SelectedFrame);
        Test->TestTrue(TEXT("Presented scalar is supplied by the same frame"),R.Field->Scalar(R.DisplayedScalar).IsSet());
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;
    FStudioProbeRequest Pinned;FStudioProbeResult First,Current;
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> CameraField;
    FStudioFrame Frame;FGuid ProbeId=FGuid::NewGuid();FString Work;
    EStudioWorkspace Workspace=EStudioWorkspace::Solve;
    double Started=0;uint64 ChangedFrame=0;int32 Phase=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioProbePresentation,"Studio.ProbePresentation.PinnedSourceAndFrame",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioProbePresentation::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioProbePresentationCommand(this));return true;}
#endif
