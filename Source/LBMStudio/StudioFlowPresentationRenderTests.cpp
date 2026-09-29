#include "StudioScene.h"
#include "ProceduralMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioFlowPresentationCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioFlowPresentationCommand(FAutomationTestBase* T):Test(T){}
    bool Update() override
    {
        if(!Started)Started=FPlatformTime::Seconds();
        if(Test->HasAnyErrors())return true;
        if(FPlatformTime::Seconds()-Started>100){Test->AddError(TEXT("Flow presentation timed out"));return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        auto& M=*Scene->Model;const auto Target=Scene->GetRenderTarget();
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending()||!Scene->HasCurrentFrame()||!Target||GFrameCounter-Changed<4||
            Scene->PresentedViewportSize()!=FIntPoint(Target->SizeX,Target->SizeY))return false;
        switch(Phase)
        {
        case 0:
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/FlowPresentation");IFileManager::Get().MakeDirectory(*Root,true);
            M.NewProject(TEXT("Airfoil · flow overview"));M.Navigate(EStudioWorkspace::Solve);M.Pause();M.bViewportExpanded=false;Next();break;
        case 1:
            Before=M.InspectionState();Capture(TEXT("before.png"));Press(TEXT("FlowOverview"));Next();break;
        case 2:
        {
            Test->TestTrue(TEXT("Overview button changes layers and mapping"),!M.bVectors&&!M.bVolume&&M.ActiveColorMapping().bManualRange);
            const auto Mesh=Scene->FindComponentByClass<UProceduralMeshComponent>();const auto Slice=Mesh?Mesh->GetProcMeshSection(2):nullptr;
            Test->TestTrue(TEXT("Full original triangles render around wing"),Slice&&Slice->ProcVertexBuffer.Num()>15000);
            Camera=Scene->SavedCameraState();Capture(TEXT("overview.png"));
            Test->TestTrue(TEXT("One undo restores original view"),M.UndoView()&&M.InspectionState().Equals(Before));Next();break;
        }
        case 3:Test->TestTrue(TEXT("Redo overview"),M.RedoView());M.ReviewRecordedFrame(420);Next();break;
        case 4:
            Test->TestEqual(TEXT("Actual later CFD snapshot"),Scene->PresentedFrame().Index,420);
            Test->TestTrue(TEXT("Playback retains view"),StudioView::CameraEquals(Camera,Scene->SavedCameraState()));
            Capture(TEXT("frame420.png"));Scene->Orbit(30,-15);Next();break;
        case 5:
            Test->TestFalse(TEXT("Overview remains freely orbitable"),StudioView::CameraEquals(Camera,Scene->SavedCameraState()));
            Capture(TEXT("orbit.png"));Scene->SetCameraMode(true);Next();break;
        case 6:Capture(TEXT("fly.png"));return true;
        }
        return false;
    }
private:
    void Next(){++Phase;Changed=GFrameCounter;}
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& W,FName Tag)
    {
        if(!W->GetVisibility().IsVisible())return {};
        if(W->GetTag()==Tag)return W;
        const auto C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto F=Find(C->GetChildAt(I),Tag))return F;return {};
    }
    void Press(FName Tag)
    {
        auto W=Find(GEngine->GameViewport->GetWindow().ToSharedRef(),Tag);
        if(!Test->TestTrue(TEXT("Overview is accessible"),W.IsValid()))return;
        auto& App=FSlateApplication::Get();App.SetKeyboardFocus(W,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
    }
    void Capture(const TCHAR* Name)
    {
        FSlateApplication::Get().CloseToolTip();TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture native flow view"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Save view evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FString Root;
    FStudioInspectionState Before;FStudioCameraState Camera;double Started=0;uint64 Changed=0;int32 Phase=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioFlowPresentationRender,"Studio.FlowPresentationUI.OverviewAndCamera",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioFlowPresentationRender::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioFlowPresentationCommand(this));return true;}
#endif
