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
        case 6:Capture(TEXT("fly.png"));Scene->SetCameraMode(false);PaneBefore=M.InspectionState();PaneRevision=M.RenderIntentRevision;PaneFrame=M.SelectedFrame;Next();break;
        case 7:
            for(const auto Name:StudioFloatingPanes::Names())Click(FName(*(TEXT("PaneToggle_")+Name.ToString())));
            Next();break;
        case 8:
            for(const auto Name:StudioFloatingPanes::Names())Test->TestTrue(TEXT("Each pane minimizes"),M.FloatingPanes.FindChecked(Name).bMinimized);
            Capture(TEXT("minimized.png"));
            Drag(TEXT("Legend"),FVector2D(90,-80));Next();break;
        case 9:
            Test->TestTrue(TEXT("Minimized title remains draggable"),M.FloatingPanes.FindChecked(TEXT("Legend")).bMoved);
            for(const auto Name:StudioFloatingPanes::Names())Press(FName(*(TEXT("PaneToggle_")+Name.ToString())));
            Next();break;
        case 10:
            for(const auto Name:StudioFloatingPanes::Names())Test->TestFalse(TEXT("Each pane restores"),M.FloatingPanes.FindChecked(Name).bMinimized);
            Capture(TEXT("restored.png"));Drag(TEXT("Tools"),FVector2D(10000,10000));Next();break;
        case 11:
        {
            const auto W=Find(GEngine->GameViewport->GetWindow().ToSharedRef(),TEXT("FloatingPane_Tools"));
            const auto V=Find(GEngine->GameViewport->GetWindow().ToSharedRef(),TEXT("FlowViewport"));
            if(Test->TestTrue(TEXT("Tools pane and viewport exist"),W.IsValid()&&V.IsValid()))
            {
                const auto& G=V->GetCachedGeometry();const auto Min=G.AbsoluteToLocal(W->GetCachedGeometry().GetAbsolutePosition());
                const auto Max=G.AbsoluteToLocal(W->GetCachedGeometry().LocalToAbsolute(W->GetCachedGeometry().GetLocalSize()));
                Test->TestTrue(TEXT("Extreme drag stays inside viewport"),Min.X>=0&&Min.Y>=0&&Max.X<=G.GetLocalSize().X&&Max.Y<=G.GetLocalSize().Y);
            }
            Test->TestTrue(TEXT("Floating pane gestures cannot alter camera or data display"),M.InspectionState().Equals(PaneBefore));
            Test->TestEqual(TEXT("Floating pane gestures do not rebuild CFD"),M.RenderIntentRevision,PaneRevision);
            Test->TestEqual(TEXT("Pane gestures leave frame untouched"),M.SelectedFrame,PaneFrame);
            Capture(TEXT("dragged.png"));
            const auto Prior=M.FloatingPanes.FindChecked(TEXT("Legend"));Drag(TEXT("Legend"),FVector2D(50,-40),true);
            const auto After=M.FloatingPanes.FindChecked(TEXT("Legend"));
            Test->TestTrue(TEXT("Escape cancels only the tentative pane drag"),Prior.Position==After.Position&&Prior.bMoved==After.bMoved);
            FStudioModel Read(FPaths::ProjectSavedDir()/TEXT("Automation/Session"));Read.OpenSession();
            const auto* Saved=Read.FloatingPanes.Find(TEXT("Legend"));
            Test->TestTrue(TEXT("Session saved during canceled drag retains committed position"),Saved&&Saved->Position==Prior.Position);
            Press(TEXT("ResetViewportPanes"));Next();break;
        }
        case 12:
            for(const auto& Pair:M.FloatingPanes)Test->TestTrue(TEXT("Reset panels restores position and expanded body"),!Pair.Value.bMoved&&!Pair.Value.bMinimized);
            Capture(TEXT("reset-panels.png"));PaneBefore=M.InspectionState();PaneFrame=M.SelectedFrame;
            VerifyMarkers(true);OpenStreamMenu();Next();break;
        case 13:Capture(TEXT("direction-settings.png"));Press(TEXT("StreamDirectionMarkers"));Next();break;
        case 14:
            VerifyMarkers(false);Test->TestEqual(TEXT("Marker toggle leaves frame intact"),M.SelectedFrame,PaneFrame);
            Test->TestTrue(TEXT("Marker toggle leaves camera intact"),StudioView::CameraEquals(M.InspectionState().Camera,PaneBefore.Camera));
            Press(TEXT("StreamDirectionMarkers"));Next();break;
        case 15:
            VerifyMarkers(true);Test->TestTrue(TEXT("Direction markers are undoable"),M.UndoView());Next();break;
        case 16:
            VerifyMarkers(false);Test->TestTrue(TEXT("Direction markers can redo"),M.RedoView());Next();break;
        case 17:
            VerifyMarkers(true);FSlateApplication::Get().DismissAllMenus();Capture(TEXT("direction-markers.png"));return true;
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
        TSharedPtr<SWidget> W;TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& Window:Windows)if(auto Found=Find(Window,Tag)){W=Found;break;}
        if(!Test->TestTrue(TEXT("View control is accessible: ")+Tag.ToString(),W.IsValid()))return;
        auto& App=FSlateApplication::Get();App.SetKeyboardFocus(W,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
    }
    void VerifyMarkers(bool Expected)
    {
        const auto Mesh=Scene->FindComponentByClass<UProceduralMeshComponent>();const auto Section=Mesh?Mesh->GetProcMeshSection(3):nullptr;
        if(!Test->TestNotNull(TEXT("Streamline mesh exists"),Section))return;
        const int32 Extra=Section->ProcVertexBuffer.Num()-Scene->PresentedStreams().Segments*24;
        Test->TestEqual(TEXT("Marker setting applied"),Scene->Model->StreamlineSettings.bDirectionMarkers,Expected);
        Test->TestTrue(TEXT("Only bounded arrowheads add vertices"),Expected?(Extra>0&&Extra%36==0&&Extra<=2048*36):Extra==0);
    }
    void OpenStreamMenu()
    {
        const auto Menu=Find(GEngine->GameViewport->GetWindow().ToSharedRef(),TEXT("StreamlineSettings"));
        if(!Test->TestTrue(TEXT("Streamline settings available"),Menu.IsValid()))return;
        TFunction<TSharedPtr<SWidget>(TSharedRef<SWidget>)> Button;
        Button=[&](TSharedRef<SWidget> W)->TSharedPtr<SWidget>{if(W->GetType()==TEXT("SButton"))return W;
            auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto Found=Button(C->GetChildAt(I)))return Found;return {};};
        const auto W=Button(Menu.ToSharedRef());if(!Test->TestTrue(TEXT("Menu has keyboard opener"),W.IsValid()))return;
        auto& App=FSlateApplication::Get();App.SetKeyboardFocus(W,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
    }
    void Click(FName Tag)
    {
        const auto W=Find(GEngine->GameViewport->GetWindow().ToSharedRef(),Tag);
        if(!Test->TestTrue(TEXT("Pointer control exists"),W.IsValid()))return;
        const auto& G=W->GetCachedGeometry();const auto P=G.LocalToAbsolute(G.GetLocalSize()*.5);
        auto& App=FSlateApplication::Get();const auto Hit=App.LocateWindowUnderMouse(P,App.GetInteractiveTopLevelWindows(),false,0);
        if(!Test->TestTrue(TEXT("Pointer reaches visible panel control"),Hit.ContainsWidget(W.Get())))return;
        App.ProcessMouseMoveEvent(FPointerEvent(0,P,P,{},FKey(),0,FModifierKeysState()));
        App.ProcessMouseButtonDownEvent(GEngine->GameViewport->GetWindow()->GetNativeWindow(),FPointerEvent(0,P,P,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
        App.ProcessMouseButtonUpEvent(FPointerEvent(0,P,P,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
    }
    void Drag(FName Name,FVector2D Delta,bool Cancel=false)
    {
        auto Handle=Find(GEngine->GameViewport->GetWindow().ToSharedRef(),FName(*(TEXT("PaneDrag_")+Name.ToString())));
        if(!Test->TestTrue(TEXT("Pane drag handle exists"),Handle.IsValid()))return;
        const auto& G=Handle->GetCachedGeometry();const auto P=G.LocalToAbsolute(G.GetLocalSize()*.5),Q=P+Delta;
        auto& App=FSlateApplication::Get();const auto Hit=App.LocateWindowUnderMouse(P,App.GetInteractiveTopLevelWindows(),false,0);
        if(!Test->TestTrue(TEXT("Drag header receives pointer"),Hit.ContainsWidget(Handle.Get())))return;
        const TSet<FKey> Down={EKeys::LeftMouseButton};
        App.ProcessMouseButtonDownEvent(GEngine->GameViewport->GetWindow()->GetNativeWindow(),FPointerEvent(0,P,P,Down,EKeys::LeftMouseButton,0,FModifierKeysState()));
        App.ProcessMouseMoveEvent(FPointerEvent(0,Q,P,Down,FKey(),0,FModifierKeysState()));
        if(Cancel)
        {
            Scene->Model->SaveSession(); // An unrelated save must not publish a tentative drag.
            App.ProcessKeyDownEvent(FKeyEvent(EKeys::Escape,FModifierKeysState(),0,false,0,0));
            App.ProcessKeyUpEvent(FKeyEvent(EKeys::Escape,FModifierKeysState(),0,false,0,0));
        }
        App.ProcessMouseButtonUpEvent(FPointerEvent(0,Q,Q,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
    }
    void Capture(const TCHAR* Name)
    {
        FSlateApplication::Get().CloseToolTip();TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture native flow view"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Save view evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FString Root;
    FStudioInspectionState Before,PaneBefore;uint64 PaneRevision=0;int32 PaneFrame=0;FStudioCameraState Camera;double Started=0;uint64 Changed=0;int32 Phase=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioFlowPresentationRender,"Studio.FlowPresentationUI.OverviewAndCamera",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioFlowPresentationRender::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioFlowPresentationCommand(this));return true;}
#endif
