#include "StudioScene.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Crc.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"

#if WITH_DEV_AUTOMATION_TESTS
// Runs in the real game viewport with Metal/RHI enabled. NullRHI unit tests
// cannot catch stale camera textures, unwanted idle draws or GPU lifetime bugs.
class FStudioRenderSoakCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioRenderSoakCommand(FAutomationTestBase* InTest) : Test(InTest)
    {
        FParse::Value(FCommandLine::Get(),TEXT("StudioRenderSoakSeconds="),Duration);
        Duration=FMath::Max(30.,Duration);
    }
    virtual bool Update() override
    {
        if(Test->HasAnyErrors()) return true;
        const double Now=FPlatformTime::Seconds();
        if(Started==0.) Started=PhaseStarted=LastProgress=Now;
        if(Phase==0)
        {
            for(const FWorldContext& Context:GEngine->GetWorldContexts())
                if(Context.World()&&Context.World()->IsGameWorld())
                    for(TActorIterator<AStudioScene> It(Context.World());It;++It) Scene=*It;
            if(Scene.IsValid()&&Scene->Model&&!bResetTestState)
            {
                Scene->Model->NewProject(TEXT("Rendering acceptance"));
                Scene->Model->Navigate(EStudioWorkspace::Solve);bResetTestState=true;return false;
            }
            if(!Scene.IsValid()||!Scene->Model||Scene->bBuilding)
            {
                if(Now-Started<20.) return false;
                Test->AddError(TEXT("The live studio scene did not become ready")); return true;
            }
            if(Scene->Model->State==EStudioRunState::Running) Scene->Model->Pause();
            Scene->Model->bLoopPlayback=false;
            Test->TestTrue(TEXT("No hidden second world view"),GEngine->GameViewport->bDisableWorldRendering);
            NextPhase(Now); return false;
        }
        if(!Scene.IsValid()) { Test->AddError(TEXT("Scene disappeared during render soak")); return true; }
        const double InPhase=Now-PhaseStarted;
        if(Now-LastProgress>=60.)
        {
            UE_LOG(LogTemp,Display,TEXT("Studio render soak: %.0fs, phase %d, captures %llu, resident %.1f MiB"),
                Now-Started,Phase,Scene->GetCaptureCount(),FPlatformMemory::GetStats().UsedPhysical/1048576.);
            LastProgress=Now;
        }
        switch(Phase)
        {
        case 1:
            if(InPhase<1.||Scene->bBuilding) break;
            InitialHash=ReadFrame(); BaselineCaptures=Scene->GetCaptureCount(); NextPhase(Now); break;
        case 2:
            Scene->Fly(FVector::ZeroVector,1./60.);
            if(InPhase<3.) break;
            Test->TestEqual(TEXT("Idle and zero camera input submit no 3D captures"),Scene->GetCaptureCount(),BaselineCaptures);
            {
                const auto SavedCamera=Scene->CameraState();
                const int32 SavedFrame=Scene->Model->SelectedFrame;
                auto Arbitrary=SavedCamera;
                Arbitrary.Position=FVector(2.1,3.7,4.9);
                Arbitrary.Orientation=FRotator(90,143,72).Quaternion();
                Arbitrary.FieldOfView=67; Arbitrary.bFreeCamera=true;
                Scene->ApplyCamera(Arbitrary);
                const auto Actual=Scene->CameraState();
                Test->TestTrue(TEXT("Arbitrary camera survives actor application"),Actual.Orientation.Equals(Arbitrary.Orientation,1.e-6));
                Test->TestEqual(TEXT("Camera restore never changes source frame"),Scene->Model->SelectedFrame,SavedFrame);
                Scene->ApplyCamera(SavedCamera);
                auto Ortho=SavedCamera; Ortho.bOrthographic=true;
                Scene->ApplyCamera(Ortho); Scene->Zoom(1);
                Test->TestTrue(TEXT("Orthographic zoom changes projection width"),Scene->CameraState().OrthoWidth<Ortho.OrthoWidth);
                Scene->ApplyCamera(SavedCamera);
            }
            VerifyViewInput();
            Phase=12; PhaseStarted=Now; break;
        case 12:
            if(InPhase<.3) break;
            Scene->Orbit(90.,20.);
            CameraHash=ReadFrame();
            Test->TestTrue(TEXT("Camera changes render new pixels immediately"),CameraHash!=InitialHash);
            Test->TestTrue(TEXT("Camera change requests a capture"),Scene->GetCaptureCount()>BaselineCaptures);
            BaselineCaptures=Scene->GetCaptureCount(); Scene->Model->Scrub(.5); Phase=3; PhaseStarted=Now; break;
        case 3:
            if(InPhase<1.||Scene->bBuilding) break;
            Test->TestTrue(TEXT("Recorded frame changes request a capture"),Scene->GetCaptureCount()>BaselineCaptures);
            Test->TestTrue(TEXT("Recorded frame changes render new pixels"),ReadFrame()!=CameraHash);
            Scene->ResizeViewport(640,400);
            Test->TestEqual(TEXT("Resize changes render target width"),Scene->GetRenderTarget()->SizeX,640);
            ReadFrame();
            Scene->Model->PlaybackRate=4.; Scene->Model->bLoopPlayback=true; Scene->Model->Run();
            HiddenPosition=Scene->CameraPosition(); HiddenFrame=Scene->Model->PlaybackFrame;
            Scene->Model->Navigate(EStudioWorkspace::Dashboard);
            BaselineCaptures=Scene->GetCaptureCount(); Phase=8; PhaseStarted=Now; break;
        case 8:
            if(InPhase<2.) break;
            Test->TestEqual(TEXT("Hidden Solve viewport submits no captures"),Scene->GetCaptureCount(),BaselineCaptures);
            Test->TestNotEqual(TEXT("Playback advances while browsing Dashboard"),Scene->Model->PlaybackFrame,HiddenFrame);
            Test->TestEqual(TEXT("Hidden workspace retains camera"),Scene->CameraPosition(),HiddenPosition);
            Scene->Model->Pause(); Scene->Model->Navigate(EStudioWorkspace::Solve);
            Phase=9; PhaseStarted=Now; break;
        case 9:
            if(InPhase<1.||Scene->bBuilding) break;
            Test->TestTrue(TEXT("Return to Solve refreshes the retained scene"),Scene->GetCaptureCount()>BaselineCaptures);
            Test->TestEqual(TEXT("Returning to Solve retains camera"),Scene->CameraPosition(),HiddenPosition);
            SourceHash=ReadFrame();
            Test->TestTrue(TEXT("Request second packaged published recording"),Scene->Model->RequestRecording(TEXT("MeshGraphNets_Airfoil_test010")));
            Phase=10; PhaseStarted=Now; break;
        case 10:
            if(Scene->Model->IsRecordingLoadPending()||Scene->bBuilding||!Scene->HasCurrentFrame())
            { if(InPhase>15.) { Test->AddError(TEXT("Second recording never reached the rendered viewport")); return true; } break; }
            Test->TestEqual(TEXT("Second recording is actually presented"),Scene->PresentedDatasetId(),FString(TEXT("MeshGraphNets_Airfoil_test010")));
            Test->TestEqual(TEXT("Source switch retains camera"),Scene->CameraPosition(),HiddenPosition);
            Test->TestNotEqual(TEXT("Independent CFD recording renders different pixels"),ReadFrame(),SourceHash);
            Scene->Model->Scrub(.8);
            Test->TestFalse(TEXT("Pending frame snapshot is refused"),Scene->Snapshot(FPaths::ProjectSavedDir()/TEXT("Automation/StaleFrame.png")));
            Phase=11; PhaseStarted=Now; break;
        case 11:
            if(Scene->bBuilding||!Scene->HasCurrentFrame()) break;
            Test->TestEqual(TEXT("Presented identity matches scrubbed frame"),Scene->PresentedFrame().Index,480);
            Test->TestEqual(TEXT("Presented timestamp matches source"),Scene->PresentedFrame().Time,Scene->Model->DisplayFrame().Time);
            ReadFrame();
            Scene->Model->Run(); Phase=4; PhaseStarted=Now; break;
        case 4:
            // Repeated mesh replacement, looping source playback, and arbitrary
            // camera placement exercise the same RHI resource path as the app.
            Scene->SetCameraPosition(FVector(-.8+FMath::Sin(InPhase)*.3,6.9,1.25));
            Scene->SetCameraRotation((FVector(.6,0,0)-Scene->CameraPosition()).Rotation());
            if(InPhase<FMath::Min(120.,Duration*.2)) break;
            Scene->Model->Pause(); Scene->Model->bLoopPlayback=false; Scene->Model->PlaybackRate=1.;
            Scene->FitCamera(); NextPhase(Now); break;
        case 5:
            if(InPhase<2.||Scene->bBuilding) break;
            IdleHash=ReadFrame(); BaselineCaptures=Scene->GetCaptureCount(); NextPhase(Now);
            IdlePosition=Scene->CameraPosition(); IdleRotation=Scene->CameraRotation();
            IdleWidth=Scene->GetRenderTarget()->SizeX; IdleHeight=Scene->GetRenderTarget()->SizeY;
            IdleRevision=Scene->Model->Revision;
            UE_LOG(LogTemp,Display,TEXT("Studio render soak: entering idle hold at capture %llu"),BaselineCaptures);
            break;
        case 6:
            if(Scene->GetCaptureCount()!=BaselineCaptures)
            {
                Test->AddError(FString::Printf(TEXT("Idle capture changed: revision %d -> %d, camera moved %s, rotated %s, target %dx%d -> %dx%d. Leave the test window untouched."),
                    IdleRevision,Scene->Model->Revision,
                    Scene->CameraPosition().Equals(IdlePosition)?TEXT("no"):TEXT("yes"),
                    Scene->CameraRotation().Equals(IdleRotation)?TEXT("no"):TEXT("yes"),
                    IdleWidth,IdleHeight,Scene->GetRenderTarget()->SizeX,Scene->GetRenderTarget()->SizeY));
                return true;
            }
            if(Now-Started<Duration) break;
            Test->TestEqual(TEXT("Render texture remains intact throughout idle hold"),ReadFrame(),IdleHash);
            Scene->Orbit(-50.,-10.); NextPhase(Now); break;
        case 7:
            if(InPhase<1.) break;
            Test->TestTrue(TEXT("Camera remains responsive after extended idle"),ReadFrame()!=IdleHash);
            Scene->FitCamera();
            UE_LOG(LogTemp,Display,TEXT("Studio render soak complete: %.1fs, %llu captures, resident %.1f MiB"),
                Now-Started,Scene->GetCaptureCount(),FPlatformMemory::GetStats().UsedPhysical/1048576.);
            return true;
        }
        return false;
    }
private:
    TSharedPtr<SWidget> FindWidget(const TSharedRef<SWidget>& Widget,const FName Type)
    {
        if(Widget->GetType()==Type) return Widget;
        FChildren* Children=Widget->GetChildren();
        for(int32 I=0;I<Children->Num();++I)
            if(auto Found=FindWidget(Children->GetChildAt(I),Type)) return Found;
        return nullptr;
    }
    void VerifyViewInput()
    {
        const auto Window=GEngine->GameViewport->GetWindow();
        if(!Window) { Test->AddError(TEXT("Packaged window is unavailable for input acceptance")); return; }
        const auto Viewport=FindWidget(Window.ToSharedRef(),TEXT("SFlowViewport"));
        if(!Viewport) { Test->AddError(TEXT("Flow viewport is unavailable for input acceptance")); return; }
        auto& App=FSlateApplication::Get(); const auto Before=Scene->CameraState();
        const int32 SourceFrame=Scene->Model->SelectedFrame,GeometryRevision=Scene->Model->Revision;
        // Numeric camera fields now belong to a popover. Their focus, typing,
        // precision and persistence are covered by Studio.CameraClipping's
        // routed native acceptance. This soak owns camera gestures and RHI lifetime.
        const auto& Geometry=Viewport->GetCachedGeometry();
        FVector2D Point=Geometry.LocalToAbsolute(Geometry.GetLocalSize()*.4);
        const TSet<FKey> Down={EKeys::LeftMouseButton};
        App.ProcessMouseButtonDownEvent(Window->GetNativeWindow(),FPointerEvent(0,Point,Point,Down,EKeys::LeftMouseButton,0,FModifierKeysState()));
        Test->TestTrue(TEXT("Slate pointer press captures the flow viewport"),Viewport->HasMouseCapture());
        for(int32 I=0;I<4;++I)
        { const FVector2D Next=Point+FVector2D(12,6); App.ProcessMouseMoveEvent(FPointerEvent(0,Next,Point,Down,FKey(),0,FModifierKeysState())); Point=Next; }
        App.ProcessMouseButtonUpEvent(FPointerEvent(0,Point,Point,TSet<FKey>(),EKeys::LeftMouseButton,0,FModifierKeysState()));
        Test->TestFalse(TEXT("Pointer release ends view gesture"),Scene->Model->IsViewEditActive());
        const auto After=Scene->CameraState();
        Test->TestFalse(TEXT("Routed drag moves camera"),StudioView::CameraEquals(Before,After));
        Test->TestTrue(TEXT("Undo view shortcut routed through Slate"),App.ProcessKeyDownEvent(FKeyEvent(EKeys::Z,FModifierKeysState(false,false,false,false,true,false,true,false,false),0,false,0,0)));
        Test->TestTrue(TEXT("One keyboard undo restores entire drag"),StudioView::CameraEquals(Scene->CameraState(),Before));
        Test->TestTrue(TEXT("Redo view shortcut routed through Slate"),App.ProcessKeyDownEvent(FKeyEvent(EKeys::Z,FModifierKeysState(true,false,false,false,true,false,true,false,false),0,false,0,0)));
        Test->TestTrue(TEXT("Keyboard redo restores final camera"),StudioView::CameraEquals(Scene->CameraState(),After));
        Test->TestEqual(TEXT("Input history leaves source frame intact"),Scene->Model->SelectedFrame,SourceFrame);
        Test->TestEqual(TEXT("Camera history does not rebuild fields"),Scene->Model->Revision,GeometryRevision);
        Scene->Model->UndoView(); Scene->ApplyCamera(Scene->Model->Project.Camera);
        ReadFrame();
    }
    void NextPhase(double Now) { ++Phase; PhaseStarted=Now; }
    uint32 ReadFrame()
    {
        // Snapshot also checks that a just-edited camera is captured before readback.
        const FString Path=FPaths::ProjectSavedDir()/TEXT("Automation/RenderSoak.png");
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);
        Test->TestTrue(TEXT("GPU snapshot succeeds"),Scene->Snapshot(Path));
        TArray<FColor> Pixels;
        if(!Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(Pixels)||Pixels.IsEmpty())
        { Test->AddError(TEXT("GPU readback failed")); return 0; }
        int32 Changed=0;
        for(const FColor& Pixel:Pixels) if(Pixel!=Pixels[0]) ++Changed;
        Test->TestTrue(TEXT("Flow render is populated, not a blank texture"),Changed>Pixels.Num()/100);
        return FCrc::MemCrc32(Pixels.GetData(),Pixels.Num()*sizeof(FColor));
    }
    FAutomationTestBase* Test;
    TWeakObjectPtr<AStudioScene> Scene;
    int32 Phase=0;
    bool bResetTestState=false;
    double Started=0.,PhaseStarted=0.,LastProgress=0.,Duration=45.;
    uint64 BaselineCaptures=0;
    uint32 InitialHash=0,CameraHash=0,IdleHash=0,SourceHash=0;
    FVector IdlePosition;
    FVector HiddenPosition;
    int32 HiddenFrame=0;
    FRotator IdleRotation;
    int32 IdleRevision=0,IdleWidth=0,IdleHeight=0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioRenderSoak,"Studio.Rendering.IdleAndPlaybackSoak",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioRenderSoak::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FStudioRenderSoakCommand(this));
    return true;
}
#endif
