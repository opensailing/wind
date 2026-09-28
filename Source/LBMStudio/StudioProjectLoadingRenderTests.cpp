#include "StudioScene.h"
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
#include "HAL/FileManager.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioProjectLoadingRenderCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioProjectLoadingRenderCommand(FAutomationTestBase* InTest):Test(InTest){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds(); if(Start==0) Start=Now;
        if(Test->HasAnyErrors()) return true;
        if(Now-Start>35) {Test->AddError(TEXT("Async project rendering acceptance timed out"));return true;}
        if(!Scene.IsValid()) for(const auto& C:GEngine->GetWorldContexts()) if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It) Scene=*It;
        if(!Scene.IsValid()||!Scene->Model) return false;
        auto& M=*Scene->Model;
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame()) return false;
            if(M.State==EStudioRunState::Running) M.Pause();
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/ProjectLoading");
            Original=M.SnapshotProject(); Original.Camera=Scene->CameraState();
            FString Error;
            Test->TestTrue(TEXT("Save original test view"),StudioProjectIO::Save(Root/TEXT("original.lbms"),Original,Error));
            Target=Original;Target.Id=FGuid::NewGuid(); Target.Name=TEXT("Async project acceptance");
            Target.Dataset=TEXT("MeshGraphNets_Airfoil_test010");Target.SelectedFrame=75;
            Target.Runs={FStudioRunRecord::Recording(TEXT("Airfoil SU2 010"),Target.Dataset)};
            Target.Camera.Position=FVector(-.5,5.8,1.4);
            Target.Camera.Orientation=(Target.Camera.Focus-Target.Camera.Position).Rotation().Quaternion();
            Target.Camera.OrbitDistance=(Target.Camera.Focus-Target.Camera.Position).Size();
            Test->TestTrue(TEXT("Save target test view"),StudioProjectIO::Save(Root/TEXT("target.lbms"),Target,Error));
            Test->TestTrue(TEXT("Prepare target off-thread"),M.RequestProjectOpen(Root/TEXT("target.lbms")));
            const auto Before=Scene->CameraState(); Scene->Orbit(12,4);
            Test->TestFalse(TEXT("Camera remains interactive while opening"),StudioView::CameraEquals(Before,Scene->CameraState()));
            Test->TestTrue(TEXT("Current source remains renderable during opening"),Scene->Snapshot(Root/TEXT("current-frame.png")));
            CaptureUI(TEXT("loading.png"));
            const auto Window=GEngine->GameViewport->GetWindow();
            const auto Cancel=Window?FindCancel(Window.ToSharedRef()):nullptr;
            Test->TestTrue(TEXT("Cancel opening is a real Slate control"),Cancel.IsValid());
            if(Cancel)
            {
                auto& App=FSlateApplication::Get(); App.SetKeyboardFocus(Cancel,EFocusCause::Navigation);
                App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
                App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
                Test->TestFalse(TEXT("Keyboard cancel stops replacement"),M.IsProjectOpening());
            }
            Phase=1;
            break;
        }
        case 1:
            if(M.IsProjectOpenPending()) return false;
            Test->TestEqual(TEXT("Routed cancel retains original document"),M.Project.Id,Original.Id);
            Test->TestTrue(TEXT("Reopen after cancelled read drains"),M.RequestProjectOpen(Root/TEXT("target.lbms")));
            Phase=2;break;
        case 2:
            if(M.IsProjectOpenPending()||!Scene->HasCurrentFrame()) return false;
            Test->TestEqual(TEXT("Rendered source belongs to loaded project"),Scene->PresentedDatasetId(),Target.Dataset);
            Test->TestEqual(TEXT("Rendered source frame restored"),Scene->PresentedFrame().Index,75);
            Test->TestTrue(TEXT("Loaded camera is applied by the scene"),StudioView::CameraEquals(Scene->CameraState(),Target.Camera));
            Test->TestTrue(TEXT("Exact saved camera survives renderer conversion"),StudioView::CameraEquals(Scene->SavedCameraState(),Target.Camera));
            Test->TestEqual(TEXT("Exact saved orbit distance retained"),Scene->SavedCameraState().OrbitDistance,Target.Camera.OrbitDistance);
            Test->TestFalse(TEXT("Loaded view remains clean after scene application"),M.HasUnsavedChanges());
            Test->TestTrue(TEXT("Loaded source image can be exported"),Scene->Snapshot(Root/TEXT("loaded-frame.png")));
            CaptureUI(TEXT("opened.png")); Captures=Scene->GetCaptureCount(); IdleStart=Now;Phase=3;break;
        case 3:
            if(Now-IdleStart<2) return false;
            Test->TestEqual(TEXT("Opened unchanged view returns to idle capture policy"),Scene->GetCaptureCount(),Captures);
            Test->TestTrue(TEXT("Restore preceding test state asynchronously"),M.RequestProjectOpen(Root/TEXT("original.lbms")));
            Phase=4;break;
        case 4:
            if(M.IsProjectOpenPending()||!Scene->HasCurrentFrame()) return false;
            Test->TestEqual(TEXT("Original recording restored for following checks"),M.Project.Dataset,Original.Dataset);
            return true;
        }
        return false;
    }
private:
    bool HasCancelLabel(const TSharedRef<SWidget>& Widget)
    {
        if(Widget->GetType()==TEXT("STextBlock")&&StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString()==TEXT("Cancel opening"))return true;
        auto* Children=Widget->GetChildren();for(int32 I=0;I<Children->Num();++I)if(HasCancelLabel(Children->GetChildAt(I)))return true;
        return false;
    }
    TSharedPtr<SWidget> FindCancel(const TSharedRef<SWidget>& Widget)
    {
        if(Widget->GetType()==TEXT("SButton")&&HasCancelLabel(Widget))return Widget;
        auto* Children=Widget->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Found=FindCancel(Children->GetChildAt(I)))return Found;
        return nullptr;
    }
    void CaptureUI(const TCHAR* Name)
    {
        const auto Window=GEngine->GameViewport->GetWindow();if(!Window)return;
        TArray<FColor> Pixels;FIntVector Size;
        Test->TestTrue(TEXT("Native Slate window captured"),FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(),Pixels,Size));
        if(Pixels.IsEmpty())return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("UI acceptance image written"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test; TWeakObjectPtr<AStudioScene> Scene;
    FStudioProject Original,Target; FString Root;
    int32 Phase=0; double Start=0,IdleStart=0;uint64 Captures=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioProjectLoadingRenderTest,"Studio.Rendering.AsyncProjectOpenAndCamera",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioProjectLoadingRenderTest::RunTest(const FString&)
{ ADD_LATENT_AUTOMATION_COMMAND(FStudioProjectLoadingRenderCommand(this));return true; }
#endif
