#include "StudioScene.h"
#include "StudioFileDialog.h"
#include "StudioAuthoringTestCapture.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioResultsUICommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioResultsUICommand(FAutomationTestBase* In):Test(In){}
    ~FStudioResultsUICommand(){if(bCaptured&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>120){Test->AddError(FString::Printf(TEXT("Results UI timed out at phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model||GFrameCounter<Changed+5)return false;
        auto& App=FSlateApplication::Get();auto& M=*Scene->Model;
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending())return false;
        if(!App.IsActive())
        {if(Now-LastActivation>1){LastActivation=Now;FPlatformApplicationMisc::ActivateApplication();GEngine->GameViewport->GetWindow()->BringToFront(true);}Changed=GFrameCounter;return false;}
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/ResultsUI");IFileManager::Get().MakeDirectory(*Root,true);
            Work=Root/FGuid::NewGuid().ToString();FString Error;
            Test->TestTrue(TEXT("Preserve previous project"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error));
            bTooltips=App.GetAllowTooltips();bCaptured=true;App.SetAllowTooltips(false);StudioAuthoringTestCapture::DismissTooltips();
            M.NewProject(TEXT("Wing · Results inspection"));Next();break;
        }
        case 1:
            if(!Scene->HasCurrentFrame())return false;
            if(M.State==EStudioRunState::Running)M.Pause();M.bViewportExpanded=false;
            Camera=Scene->SavedCameraState();Case=StudioCaseIO::Serialize(M.Project.Draft);
            Press(TEXT("Workspace9"));Next();break;
        case 2:
            Test->TestEqual(TEXT("Sidebar opens Results"),M.Workspace,EStudioWorkspace::Results);
            Test->TestTrue(TEXT("Actual source dimensions and original range are visible"),HasText(TEXT("2D triangle mesh"))&&HasText(TEXT("601 snapshots"))&&HasText(TEXT("duration 0.12 s")));
            Capture(TEXT("recordings.png"));Type(TEXT("ResultsFrame"),TEXT("421"));Next();break;
        case 3:
            Test->TestTrue(TEXT("Exact ordinal changes independently of playback"),M.SelectedFrame==420&&M.PlaybackFrame==0&&M.bReviewing);
            Test->TestTrue(TEXT("Exact source identity appears beside frame input"),HasText(TEXT("Source step 420")));
            Capture(TEXT("review-frame.png"));Press(TEXT("ResultsInspect"));Next();break;
        case 4:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestTrue(TEXT("Solve presents requested original frame"),M.Workspace==EStudioWorkspace::Solve&&Scene->PresentedFrame().Index==420);
            CheckRetained();Capture(TEXT("frame-in-solve.png"));Press(TEXT("Workspace9"));Next();break;
        case 5:Type(TEXT("ResultsSearch"),TEXT("test010"));Next();break;
        case 6:
            Test->TestTrue(TEXT("Filter matches source ID"),Find(TEXT("ResultsDataset_MeshGraphNets_Airfoil_test010")).IsValid());
            Test->TestFalse(TEXT("Nonmatching source is filtered out"),Find(TEXT("ResultsDataset_MeshGraphNets_Airfoil_test009")).IsValid());
            Capture(TEXT("filtered.png"));Type(TEXT("ResultsSearch"),TEXT("no matching output"));Next();break;
        case 7:
            Test->TestTrue(TEXT("Filter empty state explains missing rows"),HasText(TEXT("No results match this filter.")));
            Capture(TEXT("empty-filter.png"));Type(TEXT("ResultsSearch"),TEXT(""));Next();break;
        case 8:Press(TEXT("ResultsDataset_MeshGraphNets_Airfoil_test010"));Next();break;
        case 9:
            Test->TestEqual(TEXT("Second original recording selected"),M.Project.Dataset,FString(TEXT("MeshGraphNets_Airfoil_test010")));
            Test->TestEqual(TEXT("Second recording retains its own complete timeline"),M.Frames.Num(),601);
            Test->TestTrue(TEXT("Second recording retains original physical duration"),FMath::IsNearlyEqual(M.Frames.Last().Time,.12,1.e-12));
            CheckRetained();Capture(TEXT("second-recording.png"));
            M.Run();Type(TEXT("ResultsFrame"),TEXT("11"));Next();break;
        case 10:
            Test->TestTrue(TEXT("Review remains pinned while playback advances"),M.SelectedFrame==10&&M.bReviewing);
            Press(TEXT("ResultsFollow"));Next();break;
        case 11:
            Test->TestTrue(TEXT("Follow restores ongoing playback"),!M.bReviewing&&M.SelectedFrame==M.PlaybackFrame);
            M.Pause();Capture(TEXT("following.png"));
            StudioFileDialog::SetNextRecordingFolderForAutomation(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture"));Press(TEXT("ResultsImport"));Next();break;
        case 12:
            Test->TestEqual(TEXT("Import exposes original nonuniform point timeline"),M.Frames.Num(),3);
            Test->TestTrue(TEXT("Point data is described without invented cells"),HasText(TEXT("2D original points"))&&HasText(TEXT("3 snapshots"))&&HasText(TEXT("No cell connectivity supplied")));
            Type(TEXT("ResultsFrame"),TEXT("2"));Next();break;
        case 13:
            Test->TestTrue(TEXT("Ordinal is distinct from original source step and time"),M.SelectedFrame==1&&M.DisplayFrame().Index==5001&&M.DisplayFrame().Time==12.5025&&HasText(TEXT("Source step 5001")));
            CheckRetained();Capture(TEXT("point-recording.png"));
            StaticCastSharedPtr<SScrollBox>(Find(TEXT("ResultsDetailsScroll")))->ScrollToEnd();Next();break;
        case 14:
            Test->TestTrue(TEXT("Provenance contains exact source hash"),HasText(M.Solver->Descriptor().MetadataSHA256));
            Capture(TEXT("source-identity.png"));
            StaticCastSharedPtr<SScrollBox>(Find(TEXT("ResultsDetailsScroll")))->ScrollToStart();
            SavedSource=M.Project.Dataset;SavedFrame=M.SelectedFrame;
            StudioFileDialog::SetNextRecordingFolderForAutomation(Work/TEXT("missing-recording"));Press(TEXT("ResultsImport"));Next();break;
        case 15:
            Test->TestTrue(TEXT("Failed import retains exact source and frame"),M.Project.Dataset==SavedSource&&M.SelectedFrame==SavedFrame);
            Test->TestFalse(TEXT("Import failure has a readable reason"),M.Notice.IsEmpty());
            Capture(TEXT("failed-import.png"));Press(TEXT("ResultsDataset_MeshGraphNets_Airfoil_test009"));Press(TEXT("CancelRecording"));Next();break;
        case 16:
            Test->TestTrue(TEXT("Cancel cannot publish an already-finished worker"),M.Project.Dataset==SavedSource&&M.SelectedFrame==SavedFrame);
            Capture(TEXT("cancelled.png"));
            Test->TestTrue(TEXT("Select real control-harness backend for captured configuration"),M.SetControlHarness(true));
            M.EditCase(TEXT("Captured harness case"),[](auto& C){C.Name=TEXT("Captured wing case");C.Setup.MaxSteps=4000;});
            M.Project.Runs.Add(FStudioRunRecord::Capture(TEXT("Command lifecycle check"),M.Project.Draft,EStudioRunOrigin::ControlHarness));Run=M.Project.Runs.Last().GetId();++M.CatalogRevision;
            M.EditCase(TEXT("Later case edit"),[](auto& C){C.Name=TEXT("Later editable case");C.Setup.MaxSteps=9000;});
            Test->TestTrue(TEXT("Return toolbar to recorded playback"),M.SetControlHarness(false));
            {
                FStudioProject Validated;FString Error;
                Test->TestTrue(TEXT("Captured run passes production document validation: ")+Error,StudioProjectIO::Parse(StudioProjectIO::Serialize(M.SnapshotProject()),Validated,Error));
            }
            Case=StudioCaseIO::Serialize(M.Project.Draft);Press(TEXT("ResultsRuns"));Next();break;
        case 17:Capture(TEXT("run-history.png"));Press(FName(*(TEXT("ResultsRun_")+Run.ToString())));Next();break;
        case 18:
            Test->TestTrue(TEXT("Harness run has no CFD claim"),HasText(TEXT("Control harness · no CFD output"))&&HasText(TEXT("No job lifecycle is attached")));
            Test->TestTrue(TEXT("Run inspection names the active toolbar source"),HasText(TEXT("Toolbar playback and field export use ")+M.Solver->Descriptor().Title));
            Test->TestTrue(TEXT("Captured case is immutable"),HasText(TEXT("Captured wing case"))&&HasText(TEXT("4000"))&&!HasText(TEXT("Later editable case")));
            Test->TestFalse(TEXT("Harness configuration has no recording action"),Find(TEXT("ResultsRunRecording")).IsValid());
            CheckRetained();Capture(TEXT("harness-run.png"));
            for(const auto& R:M.Project.Runs)if(R.GetDatasetId()==TEXT("MeshGraphNets_Airfoil_test010")){Press(FName(*(TEXT("ResultsRun_")+R.GetId().ToString())));break;}
            Next();break;
        case 19:
            Test->TestTrue(TEXT("Recording does not borrow authoring case"),HasText(TEXT("Case settings were not supplied with this recording.")));
            Test->TestTrue(TEXT("Saved run inspection retains active export source"),HasText(TEXT("Toolbar playback and field export use ")+M.Solver->Descriptor().Title));
            Capture(TEXT("recorded-run.png"));Press(TEXT("ResultsRunRecording"));Next();break;
        case 20:
            Test->TestEqual(TEXT("Run link selects its own recording"),M.Project.Dataset,FString(TEXT("MeshGraphNets_Airfoil_test010")));
            Press(TEXT("Workspace0"));Next();break;
        case 21:
            Test->TestTrue(TEXT("Dashboard refers to one Results browser"),HasText(TEXT("captured run settings in Results.")));
            Capture(TEXT("dashboard-summary.png"));Press(TEXT("Workspace7"));Next();break;
        case 22:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestFalse(TEXT("Solve has no duplicate recording chooser"),Find(TEXT("RecordingSelector")).IsValid());
            CheckRetained();Test->TestTrue(TEXT("Restore original session"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 23:return true;
        }
        return false;
    }
private:
    void Next(){++Phase;Changed=GFrameCounter;}
    TSharedPtr<SWidget> FindIn(const TSharedRef<SWidget>& W,FName Tag)
    {
        // Immediate cancellation follows opening in the same input turn. Update
        // the footer's visibility attribute before traversing its new control.
        W->UpdateAllAttributes();
        if(!W->GetVisibility().IsVisible())return {};if(W->GetTag()==Tag)return W;
        auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto Found=FindIn(C->GetChildAt(I),Tag))return Found;return {};
    }
    TSharedPtr<SWidget> Find(FName Tag){return FindIn(GEngine->GameViewport->GetWindow().ToSharedRef(),Tag);}
    bool HasTextIn(const TSharedRef<SWidget>& W,const FString& Text)
    {
        if(!W->GetVisibility().IsVisible())return false;
        if(W->GetType()==TEXT("STextBlock")&&StaticCastSharedRef<STextBlock>(W)->GetText().ToString().Contains(Text))return true;
        if(W->GetType()==TEXT("SEditableTextBox")&&StaticCastSharedRef<SEditableTextBox>(W)->GetText().ToString().Contains(Text))return true;
        auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(HasTextIn(C->GetChildAt(I),Text))return true;return false;
    }
    bool HasText(const FString& Text){return HasTextIn(GEngine->GameViewport->GetWindow().ToSharedRef(),Text);}
    TSharedPtr<SWidget> Focusable(const TSharedRef<SWidget>& W)
    {
        if(!W->GetVisibility().IsVisible()||!W->IsEnabled())return {};if(W->SupportsKeyboardFocus())return W;
        auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto Found=Focusable(C->GetChildAt(I)))return Found;return {};
    }
    void Key(FKey K,const FModifierKeysState& Mods=FModifierKeysState())
    {auto& A=FSlateApplication::Get();A.ProcessKeyDownEvent(FKeyEvent(K,Mods,0,false,0,0));A.ProcessKeyUpEvent(FKeyEvent(K,Mods,0,false,0,0));}
    TSharedPtr<SWidget> Focus(FName Tag)
    {
        const auto W=Find(Tag);if(!Test->TestTrue(TEXT("Results widget found: ")+Tag.ToString(),W.IsValid()))return {};
        W->UpdateAllAttributes();if(!Test->TestTrue(TEXT("Results widget enabled: ")+Tag.ToString(),W->IsEnabled()))return {};
        const auto Target=Focusable(W.ToSharedRef());if(!Test->TestTrue(TEXT("Results focus target"),Target.IsValid()))return {};
        FSlateApplication::Get().SetKeyboardFocus(Target,EFocusCause::Navigation);
        Test->TestTrue(TEXT("Results focus reaches intended control or its native editor: ")+Tag.ToString(),
            Target->HasKeyboardFocus()||Target->HasFocusedDescendants());return Target;
    }
    void Press(FName Tag){if(Focus(Tag))Key(EKeys::Enter);}
    void Type(FName Tag,const FString& Value)
    {
        if(!Focus(Tag))return;
        Key(EKeys::A,FModifierKeysState(false,false,true,false,false,false,false,false,false));
        if(Value.IsEmpty())Key(EKeys::BackSpace);
        else for(TCHAR C:Value)FSlateApplication::Get().ProcessKeyCharEvent(FCharacterEvent(C,FModifierKeysState(),0,false));
        Key(EKeys::Enter);
    }
    void CheckRetained()
    {
        Test->TestTrue(TEXT("Results preserves arbitrary camera"),StudioView::CameraEquals(Scene->Model->Project.Camera,Camera));
        Test->TestEqual(TEXT("Results preserves editable case"),StudioCaseIO::Serialize(Scene->Model->Project.Draft),Case);
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture native Results window"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Save native Results capture"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FStudioCameraState Camera;FGuid Run;
    FString Root,Work,Case,SavedSource;int32 Phase=0,SavedFrame=0;uint64 Changed=0;
    double Started=0,LastActivation=0;bool bCaptured=false,bTooltips=true;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioResultsUI,"Studio.ResultsUI.RecordingsFramesAndRunHistory",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioResultsUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioResultsUICommand(this));return true;}
#endif
