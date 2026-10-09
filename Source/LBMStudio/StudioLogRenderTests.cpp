#include "StudioScene.h"
#include "StudioFileDialog.h"
#include "StudioAuthoringTestCapture.h"
#include "StudioLog.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformApplicationMisc.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioLogUICommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioLogUICommand(FAutomationTestBase* InTest):Test(InTest){}
    ~FStudioLogUICommand(){if(bCaptured&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>95){Test->AddError(FString::Printf(TEXT("Log UI timed out at phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model||GFrameCounter<Changed+5)return false;
        auto& App=FSlateApplication::Get();auto& M=*Scene->Model;
        if(!App.IsActive())
        {if(Now-LastActivation>1){LastActivation=Now;FPlatformApplicationMisc::ActivateApplication();GEngine->GameViewport->GetWindow()->BringToFront(true);}Changed=GFrameCounter;return false;}
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending())return false;
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/LogUI");Work=Root/FGuid::NewGuid().ToString();
            PreviousWorkspace=M.Workspace;PreviousExpanded=M.bViewportExpanded;FString Error;
            Test->TestTrue(TEXT("Save previous session"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error));
            bTooltips=App.GetAllowTooltips();bCaptured=true;App.SetAllowTooltips(false);StudioAuthoringTestCapture::DismissTooltips();
            M.AddLog(TEXT("Prior project diagnostic — UI acceptance fixture."),EStudioLogSeverity::Error);
            M.NewProject(TEXT("Wing · Activity log"));M.Pause();M.bViewportExpanded=false;M.Navigate(EStudioWorkspace::Solve);
            M.SetControlHarness(true);M.Control(EStudioJobCommand::Submit);M.Tick(.1);M.SimulateJobEvent(EStudioJobState::Failed);
            M.SetControlHarness(false);Next();break;
        }
        case 1:
            if(!Scene->HasCurrentFrame())return false;
            Case=StudioCaseIO::Serialize(M.Project.Draft);Camera=Scene->SavedCameraState();Frame=M.SelectedFrame;Revision=M.RenderIntentRevision;
            Capture(TEXT("compact.png"));Press(TEXT("LogExpand"));Next();break;
        case 2:
            Test->TestTrue(TEXT("Log expands within Solve"),M.bActivityLogExpanded&&M.Workspace==EStudioWorkspace::Solve);
            CaptureCount=Scene->GetCaptureCount();Capture(TEXT("expanded.png"));Press(TEXT("LogSeverity"));Next();break;
        case 3:Press(TEXT("LogSeverity_1"));Next();break;
        case 4:
            if(const auto L=List())for(const auto& E:L->GetItems())Test->TestTrue(TEXT("Severity filter excludes informational messages"),E->Severity>=EStudioLogSeverity::Warning);
            Capture(TEXT("filtered.png"));Press(TEXT("LogFollow"));Next();break;
        case 5:
            if(const auto L=List())FrozenCount=L->GetItems().Num();
            M.AddLog(TEXT("Background export failed — UI acceptance fixture; source data unchanged."),EStudioLogSeverity::Error);
            M.Step();Frame=M.SelectedFrame;Revision=M.RenderIntentRevision;Next();break;
        case 6:
            if(const auto L=List())Test->TestEqual(TEXT("Pause keeps visible snapshot stable"),L->GetItems().Num(),FrozenCount);
            Test->TestEqual(TEXT("Hidden flow performs no captures after playback step"),Scene->GetCaptureCount(),CaptureCount);
            Capture(TEXT("paused.png"));Press(TEXT("LogClear"));Next();break;
        case 7:
            if(const auto L=List())Test->TestEqual(TEXT("Clear hides visible entries"),L->GetItems().Num(),0);
            Test->TestFalse(TEXT("Empty snapshot cannot export"),FindTag(TEXT("LogExport"))->IsEnabled());
            Capture(TEXT("cleared.png"));Press(TEXT("LogShowRetained"));Next();break;
        case 8:
            if(const auto L=List())Test->TestEqual(TEXT("Show retained restores paused snapshot"),L->GetItems().Num(),FrozenCount);
            ExportPath=Work/TEXT("activity.csv");StudioFileDialog::SetNextProbeCSVForAutomation(ExportPath);Press(TEXT("LogExport"));Next();break;
        case 9:
        {
            FString CSV;if(!FFileHelper::LoadFileToString(CSV,*ExportPath))return false;
            Test->TestTrue(TEXT("CSV carries UTC observation identity"),CSV.Contains(TEXT("observed_at_utc"))&&CSV.Contains(M.Project.Id.ToString()));
            Test->TestFalse(TEXT("CSV excludes arrivals outside frozen snapshot"),CSV.Contains(TEXT("Background export failed")));
            Capture(TEXT("exported.png"));Press(TEXT("LogFollow"));Next();break;
        }
        case 10:
            if(const auto L=List())Test->TestTrue(TEXT("Follow resumes new matching arrivals"),L->GetItems().Num()>FrozenCount);
            Press(TEXT("LogSource"));Next();break;
        case 11:Press(TEXT("LogSource_1"));Next();break;
        case 12:
            if(const auto L=List())
            {
                Test->TestEqual(TEXT("Combined source and severity filter exact"),L->GetItems().Num(),1);
                for(const auto& E:L->GetItems())Test->TestTrue(TEXT("Application source is explicit"),E->Source==EStudioLogSource::Application);
            }
            Press(TEXT("LogList"),EKeys::Home);Next();break;
        case 13:
            Test->TestTrue(TEXT("Keyboard row selection exposes copyable full detail"),FindTag(TEXT("LogDetail")).IsValid());
            Capture(TEXT("detail.png"));TypeSearch(TEXT("no matching observation"));Next();break;
        case 14:
            if(const auto L=List())Test->TestEqual(TEXT("Native search filters visible entries"),L->GetItems().Num(),0);
            Capture(TEXT("no-match.png"));TypeSearch(TEXT("Background"));Next();break;
        case 15:
            if(const auto L=List())Test->TestEqual(TEXT("Native search can recover the exact event"),L->GetItems().Num(),1);
            TypeSearch(TEXT(""));Press(TEXT("LogSource"));Next();break;
        case 16:Press(TEXT("LogSource_0"));Next();break;
        case 17:Press(TEXT("LogCurrentRun"),EKeys::SpaceBar);Next();break;
        case 18:
            if(const auto L=List())
            {
                Test->TestEqual(TEXT("Current run excludes application errors"),L->GetItems().Num(),1);
                for(const auto& E:L->GetItems())Test->TestEqual(TEXT("Current run owns visible row"),E->RunId,M.Job().Run()->GetId());
            }
            Capture(TEXT("current-run.png"));Press(TEXT("LogScope"));Next();break;
        case 19:Press(TEXT("LogScope_1"));Next();break;
        case 20:
            Test->TestFalse(TEXT("All-project scope disables the project-specific current run filter"),FindTag(TEXT("LogCurrentRun"))->IsEnabled());
            if(const auto L=List())Test->TestEqual(TEXT("All-session scope includes original project's diagnostic"),L->GetItems().Num(),3);
            Capture(TEXT("all-projects.png"));VerifyIsolation();Press(TEXT("LogRestore"));Next();break;
        case 21:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestFalse(TEXT("Restore returns flow layout"),M.bActivityLogExpanded);
            VerifyIsolation();Capture(TEXT("restored.png"));
            Test->TestTrue(TEXT("Restore previous document"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 22:M.bViewportExpanded=PreviousExpanded;M.Navigate(PreviousWorkspace);return true;
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
        Test->AddError(TEXT("Missing log widget: ")+Tag.ToString());return {};
    }
    TSharedPtr<SListView<TSharedPtr<FStudioLogEntry>>> List()
    {return StaticCastSharedPtr<SListView<TSharedPtr<FStudioLogEntry>>>(FindTag(TEXT("LogList")));}
    TSharedPtr<SWidget> Focusable(const TSharedRef<SWidget>& W)
    {
        if(!W->GetVisibility().IsVisible()||!W->IsEnabled())return {};
        if(W->SupportsKeyboardFocus())return W;
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Found=Focusable(Children->GetChildAt(I)))return Found;return {};
    }
    void Press(FName Tag,FKey Key=EKeys::Enter)
    {
        const auto W=FindTag(Tag);if(!W)return;
        if(!Test->TestTrue(FString::Printf(TEXT("%s enabled at phase %d"),*Tag.ToString(),Phase),W->IsEnabled()))return;
        const auto Target=Focusable(W.ToSharedRef());if(!Test->TestTrue(TEXT("Log input has a focusable target"),Target.IsValid()))return;
        auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Target,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));
    }
    void TypeSearch(const FString& Value)
    {
        const auto W=FindTag(TEXT("LogSearch"));if(!W)return;
        auto Search=StaticCastSharedPtr<SEditableTextBox>(W);auto& App=FSlateApplication::Get();
        Search->SetText(FText::GetEmpty());App.SetKeyboardFocus(Search,EFocusCause::Navigation);
        for(const TCHAR C:Value)App.ProcessKeyCharEvent(FCharacterEvent(C,FModifierKeysState(),0,false));
    }
    void VerifyIsolation()
    {
        const auto& M=*Scene->Model;Test->TestEqual(TEXT("Log controls preserve case configuration"),StudioCaseIO::Serialize(M.Project.Draft),Case);
        Test->TestEqual(TEXT("Log controls preserve selected frame"),M.SelectedFrame,Frame);
        Test->TestEqual(TEXT("Log controls preserve render settings"),M.RenderIntentRevision,Revision);
        Test->TestTrue(TEXT("Log controls preserve camera"),StudioView::CameraEquals(Scene->SavedCameraState(),Camera));
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;Test->TestTrue(TEXT("Capture activity log"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size));
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);Test->TestTrue(TEXT("Save log evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FStudioCameraState Camera;
    FString Root,Work,Case,ExportPath;EStudioWorkspace PreviousWorkspace=EStudioWorkspace::Solve;
    int32 Phase=0,Frame=0,FrozenCount=0;uint64 Changed=0,Revision=0,CaptureCount=0;double Started=0,LastActivation=0;
    bool bTooltips=true,bCaptured=false,PreviousExpanded=false;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioLogUI,"Studio.LogUI.FilterPauseExportAndRestore",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioLogUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioLogUICommand(this));return true;}
#endif
