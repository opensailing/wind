#include "StudioScene.h"
#include "StudioAuthoringTestCapture.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioPerformanceUICommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioPerformanceUICommand(FAutomationTestBase* InTest):Test(InTest){}
    ~FStudioPerformanceUICommand(){if(bCaptured&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>90){Test->AddError(FString::Printf(TEXT("Performance UI timed out at phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model||GFrameCounter<Changed+4)return false;
        auto& App=FSlateApplication::Get();auto& M=*Scene->Model;
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending())return false;
        if(!App.IsActive())
        {if(Now-LastActivation>1){LastActivation=Now;FPlatformApplicationMisc::ActivateApplication();GEngine->GameViewport->GetWindow()->BringToFront(true);}Changed=GFrameCounter;return false;}
        switch(Phase)
        {
        case 0:
            if(!Scene->HasCurrentFrame())return false;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/PerformanceUI");IFileManager::Get().MakeDirectory(*Root,true);
            bTooltips=App.GetAllowTooltips();bCaptured=true;App.SetAllowTooltips(false);StudioAuthoringTestCapture::DismissTooltips();
            if(M.State==EStudioRunState::Running)M.Pause();M.bViewportExpanded=false;M.Navigate(EStudioWorkspace::Solve);Next();break;
        case 1:
            if(!Scene->HasCurrentFrame())return false;
            Case=StudioCaseIO::Serialize(M.Project.Draft);Frame=M.SelectedFrame;Source=M.Solver->Descriptor().Id;
            Camera=Scene->SavedCameraState();Tab=M.InspectorTab;
            Capture(TEXT("solve.png"));Press(TEXT("ViewPerformance"));Next();break;
        case 2:
            if(Now-PhaseAt<3.5)return false;
            Test->TestTrue(TEXT("Opening inspector transfers keyboard focus after layout"),FindTag(TEXT("PerformancePanel"))->HasKeyboardFocus());
            CheckIsolation();Test->TestTrue(TEXT("Actual app footprint with units"),Value(TEXT("Performance_Footprint")).Contains(TEXT("MiB")));
            Test->TestTrue(TEXT("Actual app CPU with units"),Value(TEXT("Performance_CPU")).Contains(TEXT("%")));
            Test->TestTrue(TEXT("UI timing has milliseconds"),Value(TEXT("Performance_Cadence")).Contains(TEXT("ms")));
            Test->TestTrue(TEXT("Actual rendering submit duration available"),Value(TEXT("Performance_CaptureTime")).Contains(TEXT("ms")));
            Capture(TEXT("live.png"));Press(TEXT("PerformancePause"));
            FrozenMemory=Value(TEXT("Performance_Footprint"));FrozenCadence=Value(TEXT("Performance_Cadence"));Next();break;
        case 3:
            if(Now-PhaseAt<2.2)return false;
            Test->TestEqual(TEXT("Frozen memory reading retains original value"),Value(TEXT("Performance_Footprint")),FrozenMemory);
            Test->TestEqual(TEXT("Frozen timing retains original value"),Value(TEXT("Performance_Cadence")),FrozenCadence);
            Test->TestTrue(TEXT("Frozen state includes the snapshot UTC time"),Value(TEXT("PerformanceReadingState")).StartsWith(TEXT("Paused · "))&&Value(TEXT("PerformanceReadingState")).EndsWith(TEXT(" UTC")));
            CheckIsolation();Capture(TEXT("paused.png"));CaptureCount=Scene->GetCaptureCount();Scene->Orbit(12,5);Camera=Scene->SavedCameraState();Next();break;
        case 4:
            if(Scene->GetCaptureCount()<=CaptureCount||!Scene->HasCurrentFrame())return false;
            Test->TestTrue(TEXT("Camera captures continue with performance readings paused"),Scene->GetCaptureCount()>CaptureCount);
            Test->TestEqual(TEXT("Camera activity cannot change frozen UI readings"),Value(TEXT("Performance_Cadence")),FrozenCadence);
            Capture(TEXT("paused-camera.png"));Press(TEXT("PerformancePause"));Next();break;
        case 5:
            if(Now-PhaseAt<2.2)return false;
            Test->TestEqual(TEXT("Readings resume explicitly"),Value(TEXT("PerformanceReadingState")),FString(TEXT("Live · 1 s sampling")));
            CheckIsolation();Capture(TEXT("resumed.png"));
            Press(TEXT("PerformancePanel"),EKeys::End);Next();break;
        case 6:
            Test->TestTrue(TEXT("Control harness source explicitly supplies no numerical data"),Value(TEXT("PerformanceJobSource")).Contains(TEXT("No solver measurements")));
            Test->TestEqual(TEXT("No fabricated solver count"),Value(TEXT("PerformanceJob_Steps")),FString(TEXT("Unavailable")));
            Test->TestEqual(TEXT("No fabricated solver ETA"),Value(TEXT("PerformanceJob_ETA")),FString(TEXT("Unavailable")));
            Test->TestEqual(TEXT("No replay percentage as solver progress"),Value(TEXT("PerformanceJob_Progress")),FString(TEXT("Indeterminate")));
            Capture(TEXT("solver-unavailable.png"));Press(TEXT("PerformancePanel"),EKeys::Escape);Next();break;
        case 7:
            CheckIsolation();Test->TestEqual(TEXT("Prior settings category retained"),M.InspectorTab,Tab);
            Test->TestTrue(TEXT("Close returns focus to sole performance entry"),FindTag(TEXT("ViewPerformance"))->HasKeyboardFocus());
            Capture(TEXT("closed.png"));return true;
        }
        return false;
    }
private:
    void Next(){++Phase;Changed=GFrameCounter;PhaseAt=FPlatformTime::Seconds();}
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& W,FName Tag)
    {
        if(!W->GetVisibility().IsVisible())return {};if(W->GetTag()==Tag)return W;
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Found=Find(Children->GetChildAt(I),Tag))return Found;return {};
    }
    TSharedPtr<SWidget> FindTag(FName Tag)
    {
        TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& Window:Windows)if(auto Found=Find(Window,Tag))return Found;
        Test->AddError(TEXT("Missing performance widget: ")+Tag.ToString());return {};
    }
    TSharedPtr<SWidget> Focusable(const TSharedRef<SWidget>& W)
    {
        if(!W->GetVisibility().IsVisible()||!W->IsEnabled())return {};
        if(W->SupportsKeyboardFocus())return W;
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Found=Focusable(Children->GetChildAt(I)))return Found;
        return {};
    }
    void Press(FName Tag,FKey Key=EKeys::Enter)
    {
        const auto W=FindTag(Tag);if(!W)return;
        if(!Test->TestTrue(FString::Printf(TEXT("%s enabled at phase %d"),*Tag.ToString(),Phase),W->IsEnabled()))return;
        const auto Target=Focusable(W.ToSharedRef());if(!Test->TestTrue(TEXT("Performance input has a focusable target"),Target.IsValid()))return;
        auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Target,EFocusCause::Navigation);
        if(!Test->TestTrue(TEXT("Key events reach the intended performance control"),Target->HasKeyboardFocus()))return;
        App.ProcessKeyDownEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));
    }

    FString Value(FName Tag)
    {const auto W=FindTag(Tag);return W?StaticCastSharedPtr<STextBlock>(W)->GetText().ToString():FString();}
    void CheckIsolation()
    {
        const auto& M=*Scene->Model;
        Test->TestEqual(TEXT("Performance preserves authored case"),StudioCaseIO::Serialize(M.Project.Draft),Case);
        Test->TestEqual(TEXT("Performance preserves recorded frame"),M.SelectedFrame,Frame);
        Test->TestEqual(TEXT("Performance preserves recording identity"),M.Solver->Descriptor().Id,Source);
        Test->TestTrue(TEXT("Performance preserves camera unless explicitly moved"),StudioView::CameraEquals(Scene->SavedCameraState(),Camera));
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;Test->TestTrue(TEXT("Capture native performance inspector"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size));
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Save native performance evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FStudioCameraState Camera;
    FString Root,Case,Source,FrozenMemory,FrozenCadence;int32 Phase=0,Frame=0,Tab=0;
    uint64 Changed=0,CaptureCount=0;double Started=0,LastActivation=0,PhaseAt=0;bool bCaptured=false,bTooltips=true;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPerformanceUI,"ScientificAcceptance.PerformanceUI.MeasurePauseCameraAndRestore",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioPerformanceUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioPerformanceUICommand(this));return true;}
#endif
