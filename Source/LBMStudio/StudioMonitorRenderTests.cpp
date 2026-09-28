#include "StudioScene.h"
#include "StudioFileDialog.h"
#include "StudioAuthoringTestCapture.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformApplicationMisc.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioMonitorUICommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioMonitorUICommand(FAutomationTestBase* InTest):Test(InTest){}
    ~FStudioMonitorUICommand(){if(bCaptured&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>100){Test->AddError(FString::Printf(TEXT("Monitor UI timed out at phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model||GFrameCounter<Changed+4)return false;
        auto& App=FSlateApplication::Get();auto& M=*Scene->Model;
        if(!App.IsActive())
        {if(Now-LastActivation>1){LastActivation=Now;FPlatformApplicationMisc::ActivateApplication();GEngine->GameViewport->GetWindow()->BringToFront(true);}Changed=GFrameCounter;return false;}
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending()||M.IsMonitorLoading())return false;
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/MonitorUI");Work=Root/FGuid::NewGuid().ToString();
            PreviousWorkspace=M.Workspace;PreviousExpanded=M.bViewportExpanded;FString Error;Test->TestTrue(TEXT("Save previous session"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error));
            bTooltips=App.GetAllowTooltips();bCaptured=true;App.SetAllowTooltips(false);StudioAuthoringTestCapture::DismissTooltips();
            M.NewProject(TEXT("Wing · Published monitors"));M.Pause();M.bViewportExpanded=false;Next();break;
        }
        case 1:
            if(!Scene->HasCurrentFrame())return false;
            Case=StudioCaseIO::Serialize(M.Project.Draft);Camera=Scene->SavedCameraState();Frame=M.SelectedFrame;Revision=M.RenderIntentRevision;
            Press(TEXT("Workspace8"));Next();break;
        case 2:
            Test->TestTrue(TEXT("Sidebar opens Monitors"),M.Workspace==EStudioWorkspace::Monitors);
            Capture(TEXT("empty.png"));Press(TEXT("MonitorSource"));Next();break;
        case 3:Press(TEXT("MonitorSource_NaluWind_NACA0021_Re270k_AoA30"));Next();break;
        case 4:
            if(!Test->TestTrue(TEXT("Selected source is verified"),M.MonitorHistory().IsValid()))return true;
            Test->TestEqual(TEXT("All original source samples"),M.MonitorHistory()->Times.Num(),6967);
            Test->TestFalse(TEXT("Independent history has no false spatial association"),M.MonitorHistory()->FieldRecordingId.IsSet());
            VerifyIsolation();Capture(TEXT("coefficients.png"));Press(TEXT("MonitorSeries_CL"),EKeys::SpaceBar);Press(TEXT("MonitorLog"),EKeys::SpaceBar);Next();break;
        case 5:
            Test->TestTrue(TEXT("Native series selection and log scale applied"),M.Project.Monitor.Series==TArray<FString>{TEXT("CD")}&&M.Project.Monitor.bLogY);
            Capture(TEXT("log-scale.png"));Press(TEXT("MonitorChart"),EKeys::Equals);Next();break;
        case 6:
            Test->TestTrue(TEXT("Keyboard zoom changes only history time range"),M.Project.Monitor.bManualTime&&M.Project.Monitor.TimeMaximum-M.Project.Monitor.TimeMinimum<2.7864);
            VerifyIsolation();Capture(TEXT("zoomed.png"));
            ExportSettings=M.Project.Monitor;ExportPath=Work/TEXT("history.csv");StudioFileDialog::SetNextProbeCSVForAutomation(ExportPath);Press(TEXT("MonitorExport"));Next();break;
        case 7:
        {
            FString CSV;if(!FFileHelper::LoadFileToString(CSV,*ExportPath))return false;
            Test->TestTrue(TEXT("CSV pins original source identity"),CSV.Contains(M.MonitorHistory()->MetadataSHA256));
            Test->TestTrue(TEXT("CSV carries original source time meaning"),CSV.Contains(TEXT("solver_time (s)")));
            Test->TestTrue(TEXT("CSV exports selected source column"),CSV.Contains(TEXT("\"CD\"")));
            Capture(TEXT("exported.png"));Press(TEXT("MonitorExpand"));Next();break;
        }
        case 8:Capture(TEXT("expanded.png"));Press(TEXT("MonitorExpand"));Press(TEXT("MonitorFit"));Next();break;
        case 9:
            Test->TestFalse(TEXT("Fit restores original full time domain"),M.Project.Monitor.bManualTime);
            Press(TEXT("MonitorClearSeries"));Next();break;
        case 10:
            Test->TestTrue(TEXT("Clear leaves an explicit empty series choice"),M.Project.Monitor.Series.IsEmpty());
            Capture(TEXT("no-series.png"));Press(TEXT("MonitorSeries_Fpx"),EKeys::SpaceBar);Next();break;
        case 11:
            Test->TestTrue(TEXT("Force selected with native units"),M.Project.Monitor.Series==TArray<FString>{TEXT("Fpx")});
            if(const auto CL=FindTag(TEXT("MonitorSeries_CL")))Test->TestFalse(TEXT("Unlike unit cannot share force axis"),CL->IsEnabled());else return true;
            Capture(TEXT("force-units.png"));VerifyIsolation();
            Test->TestTrue(TEXT("Save complete monitor project"),M.SaveProject(Work/TEXT("monitor.lbms")));
            Saved=StudioProjectIO::Serialize(M.SnapshotProject());M.NewProject(TEXT("Before monitor reopen"));
            Test->TestTrue(TEXT("Request reopen"),M.RequestProjectOpen(Work/TEXT("monitor.lbms")));Next();break;
        case 12:
            if(!M.MonitorHistory())return false;
            Test->TestEqual(TEXT("Reopen preserves monitor and scientific project"),StudioProjectIO::Serialize(M.SnapshotProject()),Saved);
            Press(TEXT("Workspace8"));Next();break;
        case 13:Capture(TEXT("reopened.png"));Press(TEXT("Workspace7"));Next();break;
        case 14:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestTrue(TEXT("Solve preview has original history chart"),FindTag(TEXT("MonitorPreviewChart")).IsValid());
            Capture(TEXT("solve-preview.png"));
            {auto S=M.Project.Monitor;S.Series={TEXT("CL"),TEXT("CD")};S.bLogY=false;M.UpdateMonitorSettings(S);}Next();break;
        case 15:
            Capture(TEXT("solve-coefficients.png"));
            {auto S=M.Project.Monitor;S.Series={TEXT("Fpx"),TEXT("Fpy"),TEXT("Fpz"),TEXT("Fvx"),TEXT("Fvy"),TEXT("Fvz")};M.UpdateMonitorSettings(S);}Next();break;
        case 16:Capture(TEXT("solve-forces.png"));Press(TEXT("MonitorPreviewSeries"));Next();break;
        case 17:Press(TEXT("MonitorPreviewSeries_Fpy"));Next();break;
        case 18:
            Test->TestEqual(TEXT("Preview choice preserves all selected scientific series"),M.Project.Monitor.Series.Num(),6);
            Capture(TEXT("solve-selected-force.png"));
            {auto S=M.Project.Monitor;S.Series={TEXT("CL"),TEXT("CD")};S.bManualTime=true;S.TimeMinimum=1.7900;S.TimeMaximum=1.7908;M.UpdateMonitorSettings(S);}
            Press(TEXT("Workspace8"));Next();break;
        case 19:Capture(TEXT("deep-zoom.png"));Press(TEXT("Workspace7"));Next();break;
        case 20:
            Capture(TEXT("solve-deep-zoom.png"));
            Test->TestTrue(TEXT("Restore previous project"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 21:M.bViewportExpanded=PreviousExpanded;M.Navigate(PreviousWorkspace);return true;
        }
        return false;
    }
private:
    void Next(){++Phase;Changed=GFrameCounter;}
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& W,FName Tag)
    {
        if(!W->GetVisibility().IsVisible())return {};if(W->GetTag()==Tag)return W;
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Found=Find(Children->GetChildAt(I),Tag))return Found;return {};
    }
    TSharedPtr<SWidget> FindTag(FName Tag)
    {
        TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& Window:Windows)if(auto Found=Find(Window,Tag))return Found;
        Test->AddError(TEXT("Missing monitor widget: ")+Tag.ToString());return {};
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
        const auto Target=Focusable(W.ToSharedRef());if(!Test->TestTrue(TEXT("Monitor input has a focusable target"),Target.IsValid()))return;
        auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Target,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));
    }
    void VerifyIsolation()
    {
        const auto& M=*Scene->Model;Test->TestEqual(TEXT("Monitor controls preserve authoring case"),StudioCaseIO::Serialize(M.Project.Draft),Case);
        Test->TestEqual(TEXT("Monitor controls preserve recorded frame"),M.SelectedFrame,Frame);
        Test->TestEqual(TEXT("Monitor controls preserve flow render revision"),M.RenderIntentRevision,Revision);
        Test->TestTrue(TEXT("Monitor controls preserve Solve camera"),StudioView::CameraEquals(Scene->SavedCameraState(),Camera));
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;Test->TestTrue(TEXT("Capture Monitors"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size));
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);Test->TestTrue(TEXT("Save monitor evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FStudioCameraState Camera;FStudioMonitorSettings ExportSettings;
    FString Root,Work,Case,Saved,ExportPath;EStudioWorkspace PreviousWorkspace=EStudioWorkspace::Solve;
    int32 Phase=0,Frame=0;uint64 Changed=0,Revision=0;double Started=0,LastActivation=0;bool bTooltips=true,bCaptured=false,PreviousExpanded=false;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioMonitorUI,"Studio.MonitorUI.SourceChartExportAndReopen",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioMonitorUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioMonitorUICommand(this));return true;}
#endif
