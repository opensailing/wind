#include "StudioScene.h"
#include "StudioOrientation.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/Crc.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioOrientationControlsCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioOrientationControlsCommand(FAutomationTestBase* InTest):Test(InTest){}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors()||Now-Started>60)
        {FSlateApplication::Get().DismissAllMenus();if(!Test->HasAnyErrors())Test->AddError(TEXT("Orientation controls timed out"));return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model)return false;
        auto& M=*Scene->Model;
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            if(M.State==EStudioRunState::Running)M.Pause();
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/OrientationControls");FString Error;
            Test->TestTrue(TEXT("Preserve original project"),StudioProjectIO::Save(Root/TEXT("original.lbms"),M.SnapshotProject(),Error));
            M.NewProject(TEXT("Wing inspection"));M.Navigate(EStudioWorkspace::Solve);M.Scrub(.5);Phase=1;break;
        }
        case 1:
            if(!Scene->HasCurrentFrame())return false;
            Scene->FitCamera();Before=Scene->SavedCameraState();Phase=2;break;
        case 2:
            Capture(TEXT("overview.png"));InitialPixels=Pixels();
            Frame=M.SelectedFrame;Case=StudioCaseIO::Serialize(M.Project.Draft);Revision=M.Revision;
            ClickRegion(1);Phase=3;break;
        case 3:
            VerifyState();Test->TestTrue(TEXT("Routed face click changes actual field pixels"),Pixels()!=InitialPixels);
            Capture(TEXT("face.png"));
            Test->TestTrue(TEXT("Face edit undoable"),M.UndoView());Scene->ApplyCamera(M.Project.Camera);
            Test->TestTrue(TEXT("Face undo restores exact overview"),StudioView::CameraEquals(M.Project.Camera,Before));Phase=4;break;
        case 4:
            ClickRegion(2);Phase=5;break;
        case 5:
            VerifyState();Capture(TEXT("edge.png"));
            Test->TestTrue(TEXT("Edge edit undoable"),M.UndoView());Scene->ApplyCamera(M.Project.Camera);Phase=6;break;
        case 6:
            ClickRegion(3);Phase=7;break;
        case 7:
            VerifyState();Capture(TEXT("corner.png"));
            OpenViews();Phase=8;break;
        case 8:
            Capture(TEXT("directions-menu.png"));
            for(const auto& D:StudioOrientation::Directions())FindTag(Tag(D));
            Press(Tag(FIntVector(0,0,-1)));Expected=StudioOrientation::Align(Scene->SavedCameraState(),FIntVector(0,0,-1));Phase=9;break;
        case 9:
            VerifyState();Capture(TEXT("bottom.png"));
            M.EditView(TEXT("Orthographic projection"),[](auto& S){S.Camera.bOrthographic=true;S.Camera.OrthoWidth=4.5;});
            Scene->ApplyCamera(M.Project.Camera);OpenViews();Phase=10;break;
        case 10:
            Expected=StudioOrientation::Align(Scene->SavedCameraState(),FIntVector(1,1,1));
            Press(Tag(FIntVector(1,1,1)));Phase=11;break;
        case 11:
            VerifyState();Test->TestTrue(TEXT("Menu direction preserves orthographic projection"),M.Project.Camera.bOrthographic);
            Test->TestEqual(TEXT("Menu direction preserves orthographic width"),M.Project.Camera.OrthoWidth,4.5);
            Capture(TEXT("orthographic.png"));
            M.Run();PlaybackFrame=M.PlaybackFrame;PlaybackStart=Now;
            OpenViews();Phase=12;break;
        case 12:
            Press(Tag(FIntVector(0,1,0)));
            Test->TestEqual(TEXT("Direction menu keeps replay running"),M.State,EStudioRunState::Running);
            Test->TestEqual(TEXT("Direction menu keeps case unchanged"),StudioCaseIO::Serialize(M.Project.Draft),Case);
            Phase=13;break;
        case 13:
            if(Now-PlaybackStart<.25)return false;
            Test->TestTrue(TEXT("Replay advances through camera changes"),M.PlaybackFrame>PlaybackFrame);
            M.Pause();Phase=14;break;
        case 14:
            if(!Scene->HasCurrentFrame())return false;
            Saved=M.SnapshotProject();
            Test->TestTrue(TEXT("Save inspection camera"),M.SaveProject(Root/TEXT("view.lbms")));
            Scene->Orbit(10,12);Test->TestTrue(TEXT("Reopen inspection camera"),M.LoadProject(Root/TEXT("view.lbms")));Phase=15;break;
        case 15:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestTrue(TEXT("Saved camera restores exactly"),StudioView::CameraEquals(M.Project.Camera,Saved.Camera));
            Test->TestEqual(TEXT("Saved selected frame restores exactly"),M.SelectedFrame,Saved.SelectedFrame);
            Capture(TEXT("reopened.png"));BaselineCaptures=Scene->GetCaptureCount();IdleStarted=Now;Phase=16;break;
        case 16:
            if(Now-IdleStarted<.3)return false;
            Test->TestEqual(TEXT("Idle orientation UI submits no field captures"),Scene->GetCaptureCount(),BaselineCaptures);
            Test->TestTrue(TEXT("Restore original project"),M.LoadProject(Root/TEXT("original.lbms")));Phase=17;break;
        case 17:if(!Scene->HasCurrentFrame())return false;return true;
        }
        return false;
    }
private:
    static FName Tag(const FIntVector& D){return FName(*FString::Printf(TEXT("Orientation_%d_%d_%d"),D.X,D.Y,D.Z));}
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& W,FName Name,bool Button=false)
    {
        if(!W->GetVisibility().IsVisible())return {};
        if((!Button&&W->GetTag()==Name)||(Button&&W->GetType()==TEXT("SButton")))return W;
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)
            if(auto Match=Find(Children->GetChildAt(I),Name,Button))return Match;
        return {};
    }
    TSharedPtr<SWidget> FindTag(FName Tag)
    {
        TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& W:Windows)if(auto Match=Find(W,Tag))return Match;
        Test->AddError(TEXT("Missing orientation control: ")+Tag.ToString());return {};
    }
    void Enter(const TSharedPtr<SWidget>& Widget)
    {
        if(!Widget)return;auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Widget,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
        App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
    }
    void Press(FName Tag){Enter(FindTag(Tag));}
    void OpenViews(){if(const auto Menu=FindTag(TEXT("OrientationViews")))Enter(Find(Menu.ToSharedRef(),NAME_None,true));}
    void ClickRegion(int32 Count)
    {
        const auto Widget=FindTag(TEXT("OrientationCube"));if(!Widget)return;
        const auto& G=Widget->GetCachedGeometry();const auto Camera=Scene->SavedCameraState();
        const auto Regions=StudioOrientation::Regions(Camera.Orientation,FVector2D(G.GetLocalSize().X,G.GetLocalSize().Y-20));
        for(const auto& R:Regions)if(int32(R.Direction.X!=0)+int32(R.Direction.Y!=0)+int32(R.Direction.Z!=0)==Count)
        {
            Expected=StudioOrientation::Align(Camera,R.Direction);const FVector2D Point=G.LocalToAbsolute(R.Center);
            auto& App=FSlateApplication::Get();const auto Window=GEngine->GameViewport->GetWindow();const TSet<FKey> Down={EKeys::LeftMouseButton};
            const auto Hit=App.LocateWindowUnderMouse(Point,App.GetInteractiveTopLevelWindows(),false,0);
            FString Path;for(int32 I=0;I<Hit.Widgets.Num();++I){const auto& Entry=Hit.Widgets[I];Path+=Entry.Widget->GetTypeAsString()+TEXT("[")+Entry.Widget->GetTag().ToString()+TEXT("] ");}
            const auto ActualRegions=StudioOrientation::Regions(Scene->CameraState().Orientation,FVector2D(G.GetLocalSize().X,G.GetLocalSize().Y-20));
            const int32 ActualHit=StudioOrientation::Hit(ActualRegions,R.Center);
            Test->AddInfo(FString::Printf(TEXT("Cube route: direction %s; local %s; absolute %s; size %s; actual hit %s; path %s"),
                *R.Direction.ToString(),*R.Center.ToString(),*Point.ToString(),*G.GetLocalSize().ToString(),
                ActualRegions.IsValidIndex(ActualHit)?*ActualRegions[ActualHit].Direction.ToString():TEXT("none"),*Path));
            if(!Test->TestTrue(TEXT("Direction center reaches the cube in the actual hit path"),Hit.ContainsWidget(Widget.Get())))return;
            App.ProcessMouseMoveEvent(FPointerEvent(0,Point,Point,TSet<FKey>(),FKey(),0,FModifierKeysState()));
            App.ProcessMouseButtonDownEvent(Window->GetNativeWindow(),FPointerEvent(0,Point,Point,Down,EKeys::LeftMouseButton,0,FModifierKeysState()));
            Test->TestTrue(TEXT("Cube captures actual routed press"),Widget->HasMouseCapture());
            App.ProcessMouseButtonUpEvent(FPointerEvent(0,Point,Point,TSet<FKey>(),EKeys::LeftMouseButton,0,FModifierKeysState()));
            Test->TestFalse(TEXT("Cube releases mouse after direction change"),Widget->HasMouseCapture());return;
        }
        Test->AddError(TEXT("Required visible cube region is absent"));
    }
    void VerifyState()
    {
        const auto& M=*Scene->Model;
        Test->TestTrue(TEXT("Actual direction action reaches expected pose"),StudioView::CameraEquals(Scene->SavedCameraState(),Expected));
        Test->TestEqual(TEXT("Orientation preserves selected frame"),M.SelectedFrame,Frame);
        Test->TestEqual(TEXT("Orientation preserves source geometry revision"),M.Revision,Revision);
        Test->TestEqual(TEXT("Orientation preserves case"),StudioCaseIO::Serialize(M.Project.Draft),Case);
        Test->TestTrue(TEXT("Pose retains arbitrary focus"),M.Project.Camera.Focus.Equals(Before.Focus,1.e-10));
        Test->TestEqual(TEXT("Pose retains orbit radius"),M.Project.Camera.OrbitDistance,Before.OrbitDistance);
    }
    uint32 Pixels()
    {
        TArray<FColor> Pixels;
        Test->TestTrue(TEXT("Read current field pixels"),Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(Pixels));
        return FCrc::MemCrc32(Pixels.GetData(),Pixels.Num()*sizeof(FColor));
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture orientation controls"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Save orientation UI evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FStudioCameraState Before,Expected;FStudioProject Saved;
    FString Root,Case;int32 Phase=0,Frame=0,Revision=0,PlaybackFrame=0;uint32 InitialPixels=0;uint64 BaselineCaptures=0;
    double Started=0,PlaybackStart=0,IdleStarted=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioOrientationControls,"Studio.Orientation.ControlsAndPersistence",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioOrientationControls::RunTest(const FString&)
{ADD_LATENT_AUTOMATION_COMMAND(FStudioOrientationControlsCommand(this));return true;}
#endif
