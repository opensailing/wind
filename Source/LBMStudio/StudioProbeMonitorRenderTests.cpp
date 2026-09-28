#include "StudioScene.h"
#include "StudioMonitorChart.h"
#include "StudioProbeHistory.h"
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
#include "HAL/FileManager.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioProbeMonitorUICommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioProbeMonitorUICommand(FAutomationTestBase* In):Test(In){}
    ~FStudioProbeMonitorUICommand(){if(bCaptured&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>120){Test->AddError(FString::Printf(TEXT("Probe history UI timed out at phase %d"),Phase));return true;}
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
        {
            if(!Scene->HasCurrentFrame())return false;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/ProbeMonitorUI");IFileManager::Get().MakeDirectory(*Root,true);
            Work=Root/FGuid::NewGuid().ToString();FString Error;
            Test->TestTrue(TEXT("Preserve original session"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error));
            bTooltips=App.GetAllowTooltips();bCaptured=true;App.SetAllowTooltips(false);StudioAuthoringTestCapture::DismissTooltips();
            M.NewProject(TEXT("Wing · Recorded probe histories"));Next();break;
        }
        case 1:
        {
            if(!Scene->HasCurrentFrame())return false;
            if(M.State==EStudioRunState::Running)M.Pause();M.bViewportExpanded=false;
            FStudioProbeObject P;P.Name=TEXT("Upper surface pressure");P.Field=TEXT("pressure");P.A=FVector(.03250676393508911,0,.1194048523902893);
            Test->TestTrue(TEXT("Save source-bound point probe"),M.AddProbe(P));Point=P.Id;
            P.Id=FGuid::NewGuid();P.Name=TEXT("Wake survey");P.Kind=EStudioProbeKind::Line;P.Samples=9;P.B=FVector(10,0,10);
            Test->TestTrue(TEXT("Save line probe with covered and uncovered positions"),M.AddProbe(P));Line=P.Id;
            Case=StudioCaseIO::Serialize(M.Project.Draft);Camera=Scene->SavedCameraState();Frame=M.SelectedFrame;Source=M.Project.Dataset;
            Press(TEXT("Workspace8"));Next();break;
        }
        case 2:Capture(TEXT("empty.png"));Press(TEXT("MonitorSource"));Next();break;
        case 3:Capture(TEXT("source-menu.png"));Pick(Point);Next();break;
        case 4:
            Test->TestFalse(TEXT("Selecting a probe does not start an expensive history read"),History().IsValid());
            Capture(TEXT("configured.png"));Press(TEXT("ProbeHistoryGenerate"));Next();break;
        case 5:
            if(!History())return false;
            Test->TestEqual(TEXT("Every original recording frame retained"),History()->Times.Num(),601);
            Test->TestTrue(TEXT("Native chart uses sampled source values"),History()->ProbeHistory.IsValid()&&History()->ProbeHistory->Scalar==TEXT("pressure"));
            CheckIsolation();Capture(TEXT("point-history.png"));Press(TEXT("MonitorLog"),EKeys::SpaceBar);Next();break;
        case 6:
            Test->TestTrue(TEXT("Probe chart supports log axis"),Chart()->CurrentSettings().bLogY);
            Capture(TEXT("log-history.png"));Press(TEXT("MonitorChart"),EKeys::Equals);Next();break;
        case 7:
            Test->TestTrue(TEXT("Probe chart supports time zoom"),Chart()->CurrentSettings().bManualTime);
            Capture(TEXT("zoomed.png"));Press(TEXT("MonitorFit"));Press(TEXT("MonitorChart"),EKeys::Down);Next();break;
        case 8:
        {
            const int32 Before=Chart()->SelectedSample();Press(TEXT("MonitorChart"),EKeys::Down);
            Test->TestEqual(TEXT("Keyboard selects the next original sample"),Chart()->SelectedSample(),Before==INDEX_NONE?0:FMath::Min(Before+1,History()->Times.Num()-1));
            Next();break;
        }
        case 9:
        {
            const int32 Selected=Chart()->SelectedSample();
            if(!Test->TestTrue(TEXT("Frame link has an exact selected source sample"),History()->ProbeHistory->Frames.IsValidIndex(Selected)))return true;
            ChosenOrdinal=History()->ProbeHistory->Frames[Selected].Ordinal;
            Capture(TEXT("selected-frame.png"));Press(TEXT("ProbeHistoryReveal"));Next();break;
        }
        case 10:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestTrue(TEXT("Source frame link reveals the actual recording in Solve"),M.Workspace==EStudioWorkspace::Solve&&M.SelectedFrame==ChosenOrdinal&&Scene->PresentedFrame().Index==M.Frames[ChosenOrdinal].Index);
            Test->TestTrue(TEXT("Frame reveal preserves the camera"),StudioView::CameraEquals(Scene->SavedCameraState(),Camera));
            Test->TestTrue(TEXT("Frame reveal selects the saved probe"),M.SelectedInspectionObject==Point);
            Frame=ChosenOrdinal;Capture(TEXT("revealed-in-solve.png"));Press(TEXT("Workspace8"));Next();break;
        case 11:
            Test->TestTrue(TEXT("History survives workspace navigation"),History().IsValid());
            Type(TEXT("ProbeHistoryLast"),TEXT("11"));Type(TEXT("ProbeHistoryFirst"),TEXT("2"));Next();break;
        case 12:
            Test->TestFalse(TEXT("Changing range removes the old chart"),History().IsValid());
            Capture(TEXT("range-draft.png"));Press(TEXT("ProbeHistoryGenerate"));Next();break;
        case 13:
            if(!History())return false;
            Test->TestTrue(TEXT("Inclusive native frame range is exact"),History()->Times.Num()==10&&History()->ProbeHistory->Frames[0].Ordinal==1&&History()->ProbeHistory->Frames.Last().Ordinal==10);
            Export=Work/TEXT("probe-history.csv");StudioFileDialog::SetNextProbeCSVForAutomation(Export);Press(TEXT("MonitorExport"));Next();break;
        case 14:
        {
            FString CSV;if(!FFileHelper::LoadFileToString(CSV,*Export))return false;
            Test->TestTrue(TEXT("Native export retains original frame and source identity"),CSV.Contains(TEXT("frame_ordinal,source_step,time_s,sample_index"))&&CSV.Contains(History()->MetadataSHA256)&&CSV.Contains(TEXT("source_x_m,source_y_m,source_z_m")));
            CheckIsolation();Capture(TEXT("exported.png"));Type(TEXT("ProbeHistoryLast"),TEXT("999999"));Press(TEXT("ProbeHistoryGenerate"));Next();break;
        }
        case 15:
            Test->TestFalse(TEXT("Invalid range cannot leave a stale plot"),History().IsValid());
            Capture(TEXT("invalid-range.png"));Press(TEXT("MonitorSource"));Next();break;
        case 16:Pick(Line);Next();break;
        case 17:Press(TEXT("ProbeHistoryGenerate"));Press(TEXT("MonitorCancel"));Next();break;
        case 18:
            if(!FindTag(TEXT("ProbeHistoryGenerate"))->IsEnabled())return false;
            Test->TestFalse(TEXT("Native cancel publishes no partial history"),History().IsValid());
            Capture(TEXT("cancelled.png"));Press(TEXT("ProbeHistoryGenerate"));Next();break;
        case 19:
            if(!History())return false;
            Test->TestEqual(TEXT("Every saved line position is available"),History()->Columns.Num(),9);
            Test->TestTrue(TEXT("Uncovered line endpoint is explicitly missing"),FMath::IsNaN(History()->Columns.Last().Values[0]));
            Capture(TEXT("line-history.png"));Press(TEXT("MonitorSeries_sample_8"),EKeys::SpaceBar);Press(TEXT("MonitorSeries_sample_0"),EKeys::SpaceBar);Next();break;
        case 20:
            Test->TestTrue(TEXT("Only the requested missing position is selected"),Chart()->CurrentSettings().Series==TArray<FString>{TEXT("sample_8")});
            Capture(TEXT("missing-position.png"));Press(TEXT("MonitorExpand"));Next();break;
        case 21:
            Capture(TEXT("expanded.png"));Press(TEXT("MonitorExpand"));M.EditProbe(Line,[](auto& P){P.A.X+=.01;});M.EndViewEdit();Next();break;
        case 22:
            Test->TestFalse(TEXT("Editing the saved probe invalidates the visible history"),History().IsValid());
            Test->TestFalse(TEXT("Changed selection cannot silently regenerate"),FindTag(TEXT("ProbeHistoryGenerate"))->IsEnabled());
            Capture(TEXT("probe-changed.png"));Test->TestTrue(TEXT("Restore original project"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 23:return true;
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
        Test->AddError(TEXT("Missing probe history widget: ")+Tag.ToString());return {};
    }
    TSharedPtr<SWidget> Focusable(const TSharedRef<SWidget>& W)
    {
        if(!W->GetVisibility().IsVisible()||!W->IsEnabled())return {};
        if(W->SupportsKeyboardFocus())return W;
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Found=Focusable(Children->GetChildAt(I)))return Found;return {};
    }
    void Key(FKey K,const FModifierKeysState& Mods=FModifierKeysState())
    {auto& A=FSlateApplication::Get();A.ProcessKeyDownEvent(FKeyEvent(K,Mods,0,false,0,0));A.ProcessKeyUpEvent(FKeyEvent(K,Mods,0,false,0,0));}
    void Press(FName Tag,FKey K=EKeys::Enter)
    {
        const auto W=FindTag(Tag);if(!W)return;
        // Consecutive actions in one latent step must observe current Slate
        // attributes, including Cancel immediately after starting a worker.
        W->UpdateAllAttributes();
        if(!Test->TestTrue(FString::Printf(TEXT("%s enabled at phase %d"),*Tag.ToString(),Phase),W->IsEnabled()))return;
        const auto Target=Focusable(W.ToSharedRef());if(!Test->TestTrue(TEXT("Probe history control is focusable"),Target.IsValid()))return;
        FSlateApplication::Get().SetKeyboardFocus(Target,EFocusCause::Navigation);
        if(!Test->TestTrue(TEXT("Routed key reaches intended history control"),Target->HasKeyboardFocus()))return;Key(K);
    }
    void Type(FName Tag,const FString& Value)
    {
        const auto W=FindTag(Tag);if(!W)return;const auto Target=Focusable(W.ToSharedRef());if(!Target){Test->AddError(TEXT("No numeric field focus target"));return;}
        auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Target,EFocusCause::Navigation);
        Key(EKeys::A,FModifierKeysState(false,false,true,false,false,false,false,false,false));
        for(TCHAR C:Value)App.ProcessKeyCharEvent(FCharacterEvent(C,FModifierKeysState(),0,false));Key(EKeys::Enter);
    }
    void Pick(const FGuid& Id){Press(FName(*(TEXT("MonitorProbe_")+Id.ToString(EGuidFormats::Digits))));}
    TSharedPtr<SStudioMonitorChart> Chart(){return StaticCastSharedPtr<SStudioMonitorChart>(FindTag(TEXT("MonitorChart")));}
    TSharedPtr<const FStudioHistory,ESPMode::ThreadSafe> History(){const auto C=Chart();return C?C->CurrentHistory():nullptr;}
    void CheckIsolation()
    {
        const auto& M=*Scene->Model;
        Test->TestEqual(TEXT("Probe history preserves authored case"),StudioCaseIO::Serialize(M.Project.Draft),Case);
        Test->TestEqual(TEXT("Probe history preserves playback frame"),M.SelectedFrame,Frame);
        Test->TestEqual(TEXT("Probe history preserves source"),M.Project.Dataset,Source);
        Test->TestTrue(TEXT("Probe history preserves camera"),StudioView::CameraEquals(Scene->SavedCameraState(),Camera));
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;Test->TestTrue(TEXT("Capture native probe history"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size));
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Save native probe history evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FStudioCameraState Camera;
    FString Root,Work,Case,Source,Export;FGuid Point,Line;int32 Phase=0,Frame=0,ChosenOrdinal=INDEX_NONE;
    uint64 Changed=0;double Started=0,LastActivation=0;bool bCaptured=false,bTooltips=true;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioProbeMonitorUI,"Studio.ProbeMonitorUI.RecordedHistoriesAndFrameLinks",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioProbeMonitorUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioProbeMonitorUICommand(this));return true;}
#endif
