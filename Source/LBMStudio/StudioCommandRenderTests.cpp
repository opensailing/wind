#include "StudioScene.h"
#include "StudioAuthoringTestCapture.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformApplicationMisc.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioCommandUICommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioCommandUICommand(FAutomationTestBase* InTest):Test(InTest){}
    ~FStudioCommandUICommand(){if(bCaptured&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>100){Test->AddError(FString::Printf(TEXT("Command UI timed out at phase %d"),Phase));return true;}
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
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/CommandUI");Work=Root/FGuid::NewGuid().ToString();
            PreviousWorkspace=M.Workspace;PreviousExpanded=M.bViewportExpanded;FString Error;
            Test->TestTrue(TEXT("Save prior session"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error));
            bTooltips=App.GetAllowTooltips();bCaptured=true;App.SetAllowTooltips(false);StudioAuthoringTestCapture::DismissTooltips();
            M.NewProject(TEXT("Wing · Commands"));M.Stop();M.SetControlHarness(false);M.bViewportExpanded=false;M.Navigate(EStudioWorkspace::Solve);Next();break;
        }
        case 1:
            if(!Scene->HasCurrentFrame())return false;
            Case=StudioCaseIO::Serialize(M.Project.Draft);Camera=Scene->SavedCameraState();Press(TEXT("LogExpand"));Next();break;
        case 2:
            if(GuardPhase==0)
            {
                Capture(TEXT("empty.png"));Sequence=M.ActivityLog().LastSequence();
                StaticCastSharedPtr<SEditableTextBox>(FindTag(TEXT("CommandInput")))->SetText(FText::FromString(TEXT("status")+FString::ChrN(250,TEXT(' '))+TEXT("unexpected argument")));
                Press(TEXT("CommandInput"),EKeys::Down);Press(TEXT("CommandInput"));
                GuardPhase=1;Changed=GFrameCounter;return false;
            }
            // Visibility attributes are cached by Slate. Let the error panel
            // appear before asserting its text through the visible widget tree.
            Test->TestTrue(TEXT("Empty history navigation preserves oversize rejection"),Response().Contains(TEXT("exceeded 256")));
            Test->TestEqual(TEXT("Unchanged truncated input cannot dispatch"),M.ActivityLog().LastSequence(),Sequence);
            if(GuardPhase==1)
            {Press(TEXT("CommandInput"),EKeys::Up);Press(TEXT("CommandInput"));GuardPhase=2;Changed=GFrameCounter;return false;}
            Type(TEXT("help"));Press(TEXT("CommandInput"));Next();break;
        case 3:
            Test->TestTrue(TEXT("Help remains visible beside input"),Response().Contains(TEXT("job reconnect")));
            Test->TestTrue(TEXT("Success clears input"),InputText().IsEmpty());Capture(TEXT("help.png"));Type(TEXT("replay p"));Next();break;
        case 4:Capture(TEXT("completion.png"));Press(TEXT("CommandInput"),EKeys::Tab);Next();break;
        case 5:
            Test->TestEqual(TEXT("Tab fills exact command"),InputText(),FString(TEXT("replay pause")));
            Test->TestTrue(TEXT("Completion did not execute"),M.State!=EStudioRunState::Running);
            Press(TEXT("CommandInput"));Next();break;
        case 6:
            Test->TestEqual(TEXT("Invalid-state command retained"),InputText(),FString(TEXT("replay pause")));
            Test->TestTrue(TEXT("Unavailable action explained"),Response().Contains(TEXT("unavailable")));
            Type(TEXT("status"));Press(TEXT("CommandInput"));Next();break;
        case 7:
            Press(TEXT("CommandInput"),EKeys::Up);Test->TestEqual(TEXT("Up recalls prior command"),InputText(),FString(TEXT("status")));
            Press(TEXT("CommandInput"),EKeys::Down);Test->TestTrue(TEXT("Down restores blank draft"),InputText().IsEmpty());
            Type(TEXT("replay run"));Press(TEXT("CommandInput"));Next();break;
        case 8:
            Test->TestTrue(TEXT("Replay command starts playback"),M.State==EStudioRunState::Running);
            Type(TEXT("replay pause"));Press(TEXT("CommandInput"));Next();break;
        case 9:
            Test->TestTrue(TEXT("Replay command pauses playback"),M.State==EStudioRunState::Paused);Frame=M.PlaybackFrame;
            Press(TEXT("CommandInput"),EKeys::Up);Press(TEXT("CommandInput"));Next();break;
        case 10:
            Test->TestTrue(TEXT("Recalled pause cannot resume"),M.State==EStudioRunState::Paused);
            Capture(TEXT("state-error.png"));Press(TEXT("LogFollow"));Type(TEXT("replay step"));Press(TEXT("CommandSend"));Next();break;
        case 11:
            Test->TestEqual(TEXT("Send advances exactly one frame"),M.PlaybackFrame,Frame+1);
            Test->TestTrue(TEXT("Response visible even with paused log"),Response().Contains(TEXT("Result · replay step")));
            Capture(TEXT("paused-response.png"));Type(TEXT("quit"));Press(TEXT("CommandInput"));Next();break;
        case 12:
            Test->TestTrue(TEXT("Unsupported command explained"),Response().Contains(TEXT("Unsupported command")));
            Test->TestEqual(TEXT("Unsupported text retained"),InputText(),FString(TEXT("quit")));
            Test->TestEqual(TEXT("Commands preserve physical case"),StudioCaseIO::Serialize(M.Project.Draft),Case);
            Test->TestTrue(TEXT("Commands preserve camera"),StudioView::CameraEquals(Camera,Scene->SavedCameraState()));
            Capture(TEXT("unsupported.png"));Sequence=M.ActivityLog().LastSequence();Press(TEXT("CommandMenu"));Next();break;
        case 13:Capture(TEXT("menu.png"));Press(TEXT("CommandChoice_status"));Next();break;
        case 14:
            Test->TestEqual(TEXT("Menu fills field without executing"),InputText(),FString(TEXT("status")));
            Test->TestEqual(TEXT("Menu selection creates no command event"),M.ActivityLog().LastSequence(),Sequence);
            Test->TestTrue(TEXT("Menu returns focus to command field"),StaticCastSharedPtr<SEditableTextBox>(FindTag(TEXT("CommandInput")))->HasKeyboardFocus());
            Type(TEXT("draft"));Press(TEXT("CommandInput"),EKeys::Up);Press(TEXT("CommandInput"),EKeys::Down);
            Test->TestEqual(TEXT("History restores unfinished text"),InputText(),FString(TEXT("draft")));
            M.SetControlHarness(true);Type(TEXT("job submit"));Press(TEXT("CommandInput"));Next();break;
        case 15:
            if(M.Job().State()!=EStudioJobState::Running)return false;
            Test->TestTrue(TEXT("Dispatch feedback distinguishes send from acknowledged state"),Response().Contains(TEXT("Sent · job submit"))&&Response().Contains(TEXT("Use status")));
            Capture(TEXT("job.png"));Type(TEXT("job pause"));Press(TEXT("CommandInput"));Next();break;
        case 16:
            if(M.Job().State()!=EStudioJobState::Paused)return false;
            Press(TEXT("CommandInput"),EKeys::Up);Press(TEXT("CommandInput"));
            Test->TestTrue(TEXT("Job pause recall is literal"),M.Job().State()==EStudioJobState::Paused&&!M.Job().IsPending());
            Type(TEXT("job stop"));Press(TEXT("CommandInput"));Next();break;
        case 17:
            if(M.Job().State()!=EStudioJobState::Stopped)return false;
            Sequence=M.ActivityLog().LastSequence();
            StaticCastSharedPtr<SEditableTextBox>(FindTag(TEXT("CommandInput")))->SetText(FText::FromString(TEXT("status")+FString::ChrN(300,TEXT(' '))));
            Press(TEXT("CommandInput"));
            Test->TestTrue(TEXT("Oversize paste cannot execute a truncated command"),Response().Contains(TEXT("exceeded 256")));
            Test->TestEqual(TEXT("Oversize input stays bounded"),InputText().Len(),256);
            Test->TestEqual(TEXT("Oversize submit creates no command event"),M.ActivityLog().LastSequence(),Sequence);
            Press(TEXT("CommandInput"),EKeys::Down);Press(TEXT("CommandInput"));
            Test->TestTrue(TEXT("Down outside recall preserves oversize rejection with retained history"),Response().Contains(TEXT("exceeded 256")));
            Test->TestEqual(TEXT("Unchanged history text still cannot dispatch"),M.ActivityLog().LastSequence(),Sequence);
            Test->TestTrue(TEXT("Create known save destination"),M.SaveProject(Work/TEXT("command.lbms")));
            M.Project.Name=TEXT("Saved through command");Type(TEXT("project save"));Press(TEXT("CommandInput"));Next();break;
        case 18:
        {
            FStudioProject Saved;FString Error;Test->TestTrue(TEXT("Saved project readable"),StudioProjectIO::Load(Work/TEXT("command.lbms"),Saved,Error));
            Test->TestEqual(TEXT("Command uses real save handler"),Saved.Name,M.Project.Name);
            Capture(TEXT("saved.png"));Press(TEXT("LogRestore"));Next();break;
        }
        case 19:
            if(!Scene->HasCurrentFrame())return false;
            Press(TEXT("InspectorTab0"));Next();break;
        case 20:
        {
            const auto Field=StaticCastSharedPtr<SEditableTextBox>(FindTag(TEXT("RunParameter0")));
            Field->SetText(FText::FromString(TEXT("777")));Press(TEXT("LogExpand"));Next();break;
        }
        case 21:Type(TEXT("job submit"));Press(TEXT("CommandInput"));Next();break;
        case 22:
            Test->TestFalse(TEXT("Guard reveals retained settings"),M.bActivityLogExpanded);
            Test->TestTrue(TEXT("Shared submission guard explains action"),M.Notice.Contains(TEXT("Apply or revert run parameters")));
            Test->TestEqual(TEXT("Unresolved settings never submit another job"),M.Project.JobHistory.Num(),1);
            Capture(TEXT("settings-guard.png"));Press(TEXT("RunParametersRevert"));Next();break;
        case 23:Press(TEXT("LogExpand"));Next();break;
        case 24:
            Test->TestEqual(TEXT("Rejected submit draft survives guard"),InputText(),FString(TEXT("job submit")));
            M.SetControlHarness(false);Type(TEXT("job submit"));Press(TEXT("CommandInput"));Next();break;
        case 25:
            Test->TestTrue(TEXT("Changed mode revalidated"),Response().Contains(TEXT("Select Control harness")));
            Capture(TEXT("mode-error.png"));Type(TEXT("unfinished draft"));M.NewProject(TEXT("Replacement · Commands"));M.Stop();Next();break;
        case 26:
            if(!Scene->HasCurrentFrame())return false;Press(TEXT("LogExpand"));Next();break;
        case 27:
            Test->TestTrue(TEXT("Project replacement clears draft"),InputText().IsEmpty());
            Press(TEXT("CommandInput"),EKeys::Up);Test->TestEqual(TEXT("Session history survives replacement"),InputText(),FString(TEXT("job submit")));
            Press(TEXT("CommandInput"));Next();break;
        case 28:
            Test->TestTrue(TEXT("Recalled command checks replacement context"),Response().Contains(TEXT("Select Control harness")));
            Test->TestTrue(TEXT("No job created by recall"),M.Project.JobHistory.IsEmpty());
            Press(TEXT("CommandInput"),EKeys::Escape);Test->TestTrue(TEXT("Escape clears only input"),InputText().IsEmpty());
            Test->TestTrue(TEXT("Restore prior session"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 29:M.bViewportExpanded=PreviousExpanded;M.Navigate(PreviousWorkspace);return true;
        }
        return false;
    }
private:
    FString InputText(){const auto W=FindTag(TEXT("CommandInput"));return W?StaticCastSharedPtr<SEditableTextBox>(W)->GetText().ToString():FString();}
    FString Response(){const auto W=FindTag(TEXT("CommandResponse"));return W?StaticCastSharedPtr<STextBlock>(W)->GetText().ToString():FString();}
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
        Test->AddError(TEXT("Missing command widget: ")+Tag.ToString());return {};
    }
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
        const auto Target=Focusable(W.ToSharedRef());if(!Test->TestTrue(TEXT("Command input has a focusable target"),Target.IsValid()))return;
        auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Target,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));
    }
    void Type(const FString& Value)
    {
        const auto W=FindTag(TEXT("CommandInput"));if(!W)return;
        auto Search=StaticCastSharedPtr<SEditableTextBox>(W);auto& App=FSlateApplication::Get();
        Search->SetText(FText::GetEmpty());App.SetKeyboardFocus(Search,EFocusCause::Navigation);
        for(const TCHAR C:Value)App.ProcessKeyCharEvent(FCharacterEvent(C,FModifierKeysState(),0,false));
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;Test->TestTrue(TEXT("Capture command interface"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size));
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);Test->TestTrue(TEXT("Save command evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FStudioCameraState Camera;
    FString Root,Work,Case;EStudioWorkspace PreviousWorkspace=EStudioWorkspace::Solve;
    int32 Phase=0,Frame=0,GuardPhase=0;uint64 Changed=0,Sequence=0;double Started=0,LastActivation=0;
    bool bTooltips=true,bCaptured=false,PreviousExpanded=false;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioCommandUI,"Studio.CommandUI.EntryRecallCompletionGuardsAndSave",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioCommandUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioCommandUICommand(this));return true;}
#endif
