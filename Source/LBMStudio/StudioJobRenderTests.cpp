#include "StudioScene.h"
#include "StudioWorkspace.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioJobRenderCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioJobRenderCommand(FAutomationTestBase* T):Test(T){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>40){Test->AddError(TEXT("Job UI acceptance timed out"));return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        if(GFrameCounter<ResumeFrame)return false;
        auto& M=*Scene->Model;
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            if(M.State==EStudioRunState::Running)M.Pause();
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/JobUI");
            auto Original=M.SnapshotProject();Original.Camera=Scene->SavedCameraState();FString Error;
            Test->TestTrue(TEXT("Save preceding project for test isolation"),StudioProjectIO::Save(Root/TEXT("original.lbms"),Original,Error));
            Frame=M.SelectedFrame;Dataset=M.Project.Dataset;
            Press(TEXT("Setup"));Phase=19;ResumeFrame=GFrameCounter+2;break;
        }
        case 19:
            Press(TEXT("Replay"));Phase=20;break;
        case 20:
        {
            Capture(TEXT("replay.png"));
            Press(TEXT("Control harness"));Press(TEXT("Run"));
            Test->TestTrue(TEXT("Routed submission has frozen run"),M.Job().Run().IsSet());
            const auto Workspace=FindType(GEngine->GameViewport->GetWindow().ToSharedRef(),TEXT("SStudioWorkspace"));
            Test->TestTrue(TEXT("Actual workspace found"),Workspace.IsValid());
            if(Workspace)Test->TestFalse(TEXT("Native close handler protects active job"),StaticCastSharedPtr<SStudioWorkspace>(Workspace)->CanClose());
            Phase=1;break;
        }
        case 1:
            if(M.Job().State()!=EStudioJobState::Running)return false;
            BeforeCamera=Scene->CameraState();Captures=Scene->GetCaptureCount();Scene->Orbit(14,5);Phase=2;break;
        case 2:
            if(Scene->GetCaptureCount()<=Captures)return false;
            Test->TestFalse(TEXT("Camera moves while job running"),StudioView::CameraEquals(BeforeCamera,Scene->CameraState()));
            Test->TestEqual(TEXT("Camera renders same independent CFD frame"),Scene->PresentedFrame().Index,Frame);
            Test->TestEqual(TEXT("Job cannot relabel data"),Scene->PresentedDatasetId(),Dataset);
            Test->TestTrue(TEXT("Current field still exports while job runs"),Scene->Snapshot(Root/TEXT("running-field.png")));
            Capture(TEXT("running.png"));Press(TEXT("Pause"));Phase=3;break;
        case 3:
            if(M.Job().State()!=EStudioJobState::Paused)return false;
            Capture(TEXT("paused.png"));Press(TEXT("Step"));Phase=4;break;
        case 4:
            if(M.Job().CompletedStepCommands()!=1||M.Job().IsPending())return false;
            Test->TestEqual(TEXT("Control Step leaves recorded frame unchanged"),M.SelectedFrame,Frame);
            Press(TEXT("Checkpoint test"));Phase=5;break;
        case 5:
            if(M.Job().CompletedCheckpointCommands()!=1||M.Job().IsPending())return false;
            Press(TEXT("Resume"));Phase=6;break;
        case 6:
            if(M.Job().State()!=EStudioJobState::Running)return false;
            Test->TestTrue(TEXT("Inject authoritative disconnect"),M.SimulateJobEvent(EStudioJobState::Disconnected));Phase=7;break;
        case 7:
            Capture(TEXT("disconnected.png"));Press(TEXT("Reconnect"));Phase=8;break;
        case 8:
            if(M.Job().State()!=EStudioJobState::Running)return false;
            Press(TEXT("Stop"));Phase=9;break;
        case 9:
            if(M.Job().State()!=EStudioJobState::Stopped)return false;
            Test->TestTrue(TEXT("Stopped project is saveable"),M.SaveProject(Root/TEXT("stopped.lbms")));
            RunId=M.Job().Run()->GetId();
            Test->TestTrue(TEXT("Reopen recorded lifecycle"),M.RequestProjectOpen(Root/TEXT("stopped.lbms")));Phase=10;break;
        case 10:
            if(M.IsProjectOpenPending()||!Scene->HasCurrentFrame())return false;
            Test->TestFalse(TEXT("Reopened job is historical"),M.HasActiveJob());
            Test->TestTrue(TEXT("Stopped lifecycle retained"),M.RunStatus(RunId)==TEXT("Saved: Stopped"));
            Test->TestEqual(TEXT("Saved acknowledgements retained"),M.Project.JobHistory.Last().StepCommands,uint64(1));
            M.Navigate(EStudioWorkspace::Dashboard);Phase=11;break;
        case 11:
            Capture(TEXT("history.png"));
            Test->TestTrue(TEXT("Restore preceding project"),M.RequestProjectOpen(Root/TEXT("original.lbms")));Phase=12;break;
        case 12:
            if(M.IsProjectOpenPending())return false;
            M.Navigate(EStudioWorkspace::Solve);Phase=13;break;
        case 13:return Scene->HasCurrentFrame();
        }
        return false;
    }
private:
    bool HasLabel(const TSharedRef<SWidget>& W,const FString& Label)
    {
        if(W->GetType()==TEXT("STextBlock")&&StaticCastSharedRef<STextBlock>(W)->GetText().ToString()==Label)return true;
        auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(HasLabel(C->GetChildAt(I),Label))return true;return false;
    }
    TSharedPtr<SWidget> FindButton(const TSharedRef<SWidget>& W,const FString& Label)
    {
        if(!W->GetVisibility().IsVisible())return nullptr;
        if(W->GetType()==TEXT("SButton")&&HasLabel(W,Label))return W;
        auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto Found=FindButton(C->GetChildAt(I),Label))return Found;return nullptr;
    }
    TSharedPtr<SWidget> FindType(const TSharedRef<SWidget>& W,const FName Type)
    {
        if(W->GetType()==Type)return W;
        auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto Found=FindType(C->GetChildAt(I),Type))return Found;return nullptr;
    }
    void Press(const FString& Label)
    {
        const auto Button=FindButton(GEngine->GameViewport->GetWindow().ToSharedRef(),Label);
        Test->TestTrue(*FString(TEXT("Find routed button: ")+Label),Button.IsValid());if(!Button)return;
        Test->TestTrue(*FString(TEXT("Button enabled: ")+Label),Button->IsEnabled());
        auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Button,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
        App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;
        Test->TestTrue(TEXT("Capture native Slate window"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size));
        if(Pixels.IsEmpty())return;TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Write capture"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FString Root,Dataset;FGuid RunId;
    FStudioCameraState BeforeCamera;double Started=0;int32 Phase=0,Frame=0;uint64 Captures=0,ResumeFrame=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioJobRenderTest,"Studio.Rendering.JobControlsAndCamera",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioJobRenderTest::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioJobRenderCommand(this));return true;}
#endif
