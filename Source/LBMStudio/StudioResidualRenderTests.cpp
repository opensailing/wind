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

#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/FileManager.h"
#include "Widgets/Text/STextBlock.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
// Opt-in native acceptance uses the exact published source supplied by the runner.
// Dialog injection proves UI routing only; OS picker/bookmark access is a separate gate.
class FStudioResidualUICommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioResidualUICommand(FAutomationTestBase* InTest):Test(InTest){}
    ~FStudioResidualUICommand(){if(bCaptured&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>150){Test->AddError(FString::Printf(TEXT("Residual UI timed out at phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model||GFrameCounter<Changed+(bCancelNextFrame?1:bCaptureLoading?2:4))return false;
        auto& App=FSlateApplication::Get();auto& M=*Scene->Model;
        if(!App.IsActive())
        {if(Now-LastActivation>1){LastActivation=Now;FPlatformApplicationMisc::ActivateApplication();GEngine->GameViewport->GetWindow()->BringToFront(true);}Changed=GFrameCounter;return false;}
        if(bCaptureLoading)
        {
            Test->TestTrue(TEXT("Loading remains visible after layout settles"),M.IsResidualLoading());
            Capture(TEXT("loading.png"));bCaptureLoading=false;Changed=GFrameCounter;return false;
        }
        if(bCancelNextFrame)
        {
            Test->TestTrue(TEXT("Original log verification remains cancellable after the next layout"),M.IsResidualLoading());
            Press(TEXT("MonitorCancel"));bCancelNextFrame=false;Next();return false;
        }
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending()||M.IsMonitorLoading()||M.IsResidualLoading())return false;
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            FString Source;FParse::Value(FCommandLine::Get(),TEXT("StudioResidualLog="),Source);
            if(!Test->TestTrue(TEXT("Explicit published source exists"),IFileManager::Get().FileExists(*Source)))return true;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/ResidualUI");Work=Root/FGuid::NewGuid().ToString();
            IFileManager::Get().MakeDirectory(*Work,true);LogPath=Work/TEXT("cylinder-log.pimpleFoam");
            if(!Test->TestEqual(TEXT("Stage original bytes"),IFileManager::Get().Copy(*LogPath,*Source),uint32(COPY_OK)))return true;
            BadPath=Work/TEXT("invalid.log");FFileHelper::SaveStringToFile(TEXT("This is not a solver log."),*BadPath);
            PreviousWorkspace=M.Workspace;PreviousExpanded=M.bViewportExpanded;FString Error;
            Test->TestTrue(TEXT("Save previous session"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error));
            bTooltips=App.GetAllowTooltips();bCaptured=true;App.SetAllowTooltips(false);StudioAuthoringTestCapture::DismissTooltips();
            M.NewProject(TEXT("Wing · Original residuals"));M.Pause();M.bViewportExpanded=false;Next();break;
        }
        case 1:
            if(!Scene->HasCurrentFrame())return false;
            Case=StudioCaseIO::Serialize(M.Project.Draft);Camera=Scene->SavedCameraState();Frame=M.SelectedFrame;Revision=M.RenderIntentRevision;
            Press(TEXT("Workspace8"));Next();break;
        case 2:Capture(TEXT("empty.png"));Press(TEXT("MonitorSource"));Next();break;
        case 3:
            Capture(TEXT("source-menu.png"));StudioFileDialog::SetNextResidualLogForAutomation(LogPath);Press(TEXT("MonitorImportResidual"));
            Test->TestTrue(TEXT("Import starts asynchronous verification"),M.IsResidualLoading());bCaptureLoading=true;
            M.RequestMonitorHistory(TEXT("NaluWind_NACA0021_Re270k_AoA30"));Next();break;
        case 4:
        {
            const auto H=M.ResidualHistory();if(!Test->TestTrue(TEXT("Native import publishes residuals"),H.IsValid()))return true;
            Test->TestEqual(TEXT("Exact published source"),H->SourceSHA256,FString(TEXT("6ff51d5a70736df30adfff60eaa9ae96f1a4a9d60d565483fbe744571d5408e2")));
            Test->TestEqual(TEXT("Complete time history"),H->Times.Num(),20000);
            Test->TestEqual(TEXT("All published solves"),H->SourceRecordCount,int64(291444));
            Test->TestTrue(TEXT("Default first-initial logarithmic chart"),M.Project.Residual.Chart.bLogY&&M.Project.Residual.Chart.Series.Num()==3);
            Force=SerializeSettings(M.Project.Monitor);
            VerifyIsolation();Capture(TEXT("residuals.png"));
            const auto Chart=FindTag(TEXT("MonitorChart"));if(!Chart)return true;
            const auto G=Chart->GetCachedGeometry();const auto At=G.LocalToAbsolute(G.GetLocalSize()*.5);
            Chart->OnMouseMove(G,FPointerEvent(0,At,At,TSet<FKey>(),EKeys::Invalid,0,FModifierKeysState()));
            const auto Tip=Chart->GetToolTip();
            const FString TipText=Tip?TooltipText(Tip->GetContentWidget()):FString();
            Test->TestTrue(TEXT("Hover retains exact original line references"),TipText.Contains(TEXT("Time line "))&&TipText.Contains(TEXT("log line ")));
            Press(TEXT("MonitorSeries_p.FinalLast"),EKeys::SpaceBar);Press(TEXT("MonitorLog"),EKeys::SpaceBar);Next();break;
        }
        case 5:
            Test->TestTrue(TEXT("Series and scale route to residual family"),M.Project.Residual.Chart.Series.Num()==4&&!M.Project.Residual.Chart.bLogY);
            Capture(TEXT("selected-linear.png"));Press(TEXT("MonitorChart"),EKeys::Equals);Next();break;
        case 6:
            Test->TestTrue(TEXT("Keyboard time zoom applies to residuals"),M.Project.Residual.Chart.bManualTime);
            VerifyIsolation();Test->TestEqual(TEXT("Force settings untouched"),SerializeSettings(M.Project.Monitor),Force);
            Capture(TEXT("zoomed.png"));ExportPath=Work/TEXT("residual-ui.csv");StudioFileDialog::SetNextProbeCSVForAutomation(ExportPath);Press(TEXT("MonitorExport"));Next();break;
        case 7:
        {
            FString CSV;if(!FFileHelper::LoadFileToString(CSV,*ExportPath))return false;
            Test->TestTrue(TEXT("Export includes original source identity"),CSV.Contains(M.ResidualHistory()->SourceSHA256));
            Test->TestTrue(TEXT("Export includes original source lines"),CSV.Contains(TEXT("time_source_line"))&&CSV.Contains(TEXT("p.FinalLast.source_line")));
            Test->TestEqual(TEXT("Retain this run export evidence"),IFileManager::Get().Copy(*(Root/TEXT("residual-ui.csv")),*ExportPath),uint32(COPY_OK));
            Capture(TEXT("exported.png"));Press(TEXT("MonitorExpand"));Next();break;
        }
        case 8:Capture(TEXT("expanded.png"));Press(TEXT("MonitorExpand"));Next();break;
        case 9:Press(TEXT("MonitorFit"));Press(TEXT("MonitorSource"));Next();break;
        case 10:Press(TEXT("MonitorSource_NaluWind_NACA0021_Re270k_AoA30"));Next();break;
        case 11:
            Capture(TEXT("force-independent.png"));Test->TestTrue(TEXT("Residual fit reset only residual time window"),!M.Project.Residual.Chart.bManualTime);
            Press(TEXT("MonitorSource"));Next();break;
        case 12:Press(TEXT("MonitorSource_Residual"));Next();break;
        case 13:
            Press(TEXT("MonitorLog"),EKeys::SpaceBar);Press(TEXT("Workspace7"));Next();break;
        case 14:
            Test->TestTrue(TEXT("Both preview charts exist together"),FindTag(TEXT("ResidualPreviewChart")).IsValid()&&FindTag(TEXT("MonitorPreviewChart")).IsValid());
            Capture(TEXT("solve-both.png"));Press(TEXT("Workspace8"));Next();break;
        case 15:
            Capture(TEXT("selected-log.png"));Press(TEXT("MonitorClearSeries"));Next();break;
        case 16:
            Test->TestTrue(TEXT("Residual series can be cleared"),M.Project.Residual.Chart.Series.IsEmpty());Capture(TEXT("no-series.png"));
            Press(TEXT("MonitorSeries_p.FinalLast"),EKeys::SpaceBar);Press(TEXT("MonitorChart"),EKeys::Equals);Next();break;
        case 17:
            SavedSettings=SerializeSettings(M.Project.Residual.Chart);
            Test->TestTrue(TEXT("Save residual and force settings"),M.SaveProject(Work/TEXT("residual.lbms")));
            M.NewProject(TEXT("Before residual reopen"));Test->TestTrue(TEXT("Reopen saved project"),M.RequestProjectOpen(Work/TEXT("residual.lbms")));Next();break;
        case 18:
            Test->TestTrue(TEXT("Both original histories reload"),M.ResidualHistory().IsValid()&&M.MonitorHistory().IsValid());
            Test->TestEqual(TEXT("Residual chart settings persist"),SerializeSettings(M.Project.Residual.Chart),SavedSettings);
            Press(TEXT("Workspace8"));Next();break;
        case 19:Press(TEXT("MonitorSource"));Next();break;
        case 20:Press(TEXT("MonitorSource_Residual"));Next();break;
        case 21:
            Capture(TEXT("reopened.png"));MovedPath=Work/TEXT("moved-original.log");
            Test->TestTrue(TEXT("Move only owned source copy"),IFileManager::Get().Move(*MovedPath,*LogPath));
            M.NewProject(TEXT("Before missing source"));M.RequestProjectOpen(Work/TEXT("residual.lbms"));Next();break;
        case 22:
            Test->TestFalse(TEXT("Missing log does not invent residuals"),M.ResidualHistory().IsValid());
            Test->TestTrue(TEXT("Force history survives missing log"),M.MonitorHistory().IsValid());
            Press(TEXT("Workspace8"));Next();break;
        case 23:Press(TEXT("MonitorSource"));Next();break;
        case 24:Press(TEXT("MonitorSource_Residual"));Next();break;
        case 25:Capture(TEXT("missing-source.png"));Press(TEXT("MonitorSource"));Next();break;
        case 26:StudioFileDialog::SetNextResidualLogForAutomation(BadPath);Press(TEXT("MonitorLocateResidual"));Next();break;
        case 27:
            Test->TestFalse(TEXT("Invalid locate publishes nothing"),M.ResidualHistory().IsValid());
            Test->TestEqual(TEXT("Failed locate retains original chart choices"),SerializeSettings(M.Project.Residual.Chart),SavedSettings);
            Capture(TEXT("locate-rejected.png"));Press(TEXT("MonitorSource"));Next();break;
        case 28:StudioFileDialog::SetNextResidualLogForAutomation(MovedPath);Press(TEXT("MonitorLocateResidual"));Next();break;
        case 29:
            Test->TestTrue(TEXT("Exact original source located"),M.ResidualHistory().IsValid());
            Test->TestEqual(TEXT("Locate retains chart choices"),SerializeSettings(M.Project.Residual.Chart),SavedSettings);
            Capture(TEXT("located.png"));Press(TEXT("MonitorRemove"));Next();break;
        case 30:
            Test->TestTrue(TEXT("Remove clears saved residual source only"),M.Project.Residual.Path.IsEmpty()&&M.MonitorHistory().IsValid());
            Capture(TEXT("removed.png"));Press(TEXT("MonitorSource"));Next();break;
        case 31:
            StudioFileDialog::SetNextResidualLogForAutomation(MovedPath);Press(TEXT("MonitorImportResidual"));bCancelNextFrame=true;Changed=GFrameCounter;break;
        case 32:
            Test->TestFalse(TEXT("Cancelled import does not publish"),M.ResidualHistory().IsValid());Capture(TEXT("cancelled.png"));
            Press(TEXT("MonitorSource"));Next();break;
        case 33:
            StudioFileDialog::SetNextResidualLogForAutomation(TEXT(""));Press(TEXT("MonitorImportResidual"));
            Test->TestFalse(TEXT("Cancel file picker does not load"),M.IsResidualLoading());
            Test->TestTrue(TEXT("Restore previous project"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 34:M.bViewportExpanded=PreviousExpanded;M.Navigate(PreviousWorkspace);IFileManager::Get().DeleteDirectory(*Work,false,true);return true;
        }
        return false;
    }
private:
    FString TooltipText(const TSharedRef<SWidget>& Widget)
    {
        if(Widget->GetTypeAsString()==TEXT("STextBlock"))return StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
        FString Text;auto* Children=Widget->GetChildren();
        for(int32 I=0;I<Children->Num();++I)Text+=TooltipText(Children->GetChildAt(I));return Text;
    }
    FString SerializeSettings(const FStudioMonitorSettings& S)
    {FString Text;auto Writer=TJsonWriterFactory<>::Create(&Text);FJsonSerializer::Serialize(StudioMonitor::ToJSON(S),Writer);return Text;}
    void Next(){++Phase;Changed=GFrameCounter;UE_LOG(LogTemp,Display,TEXT("Residual UI phase %d"),Phase);}
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& W,FName Tag)
    {
        if(!W->GetVisibility().IsVisible())return {};if(W->GetTag()==Tag)return W;
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Found=Find(Children->GetChildAt(I),Tag))return Found;return {};
    }
    TSharedPtr<SWidget> FindTag(FName Tag)
    {
        TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& Window:Windows)if(auto Found=Find(Window,Tag))return Found;
        Test->AddError(TEXT("Missing residual widget: ")+Tag.ToString());return {};
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
        TArray<FColor> Pixels;FIntVector Size;Test->TestTrue(TEXT("Capture residual UI"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size));
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);Test->TestTrue(TEXT("Save residual evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FStudioCameraState Camera;
    FString Root,Work,Case,ExportPath,LogPath,MovedPath,BadPath,Force,SavedSettings;EStudioWorkspace PreviousWorkspace=EStudioWorkspace::Solve;
    int32 Phase=0,Frame=0;uint64 Changed=0,Revision=0;double Started=0,LastActivation=0;
    bool bTooltips=true,bCaptured=false,PreviousExpanded=false,bCancelNextFrame=false,bCaptureLoading=false;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioResidualUI,"ScientificAcceptance.ResidualUI.SourceChartExportAndReopen",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioResidualUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioResidualUICommand(this));return true;}
#endif
