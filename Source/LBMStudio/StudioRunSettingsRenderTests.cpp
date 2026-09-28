#include "StudioScene.h"
#include "StudioAuthoringTestCapture.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SWindow.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioRunSettingsUICommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioRunSettingsUICommand(FAutomationTestBase* InTest):Test(InTest){}
    ~FStudioRunSettingsUICommand()
    {if(bCapturedTooltips&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>100){Test->AddError(FString::Printf(TEXT("Run parameters timed out at phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model||GFrameCounter<Changed+4)return false;
        auto& App=FSlateApplication::Get();
        if(!App.IsActive())
        {
            if(Now-LastActivation>1.)
            {LastActivation=Now;Test->AddInfo(TEXT("Activating native app for run-parameter input."));FPlatformApplicationMisc::ActivateApplication();GEngine->GameViewport->GetWindow()->BringToFront(true);}
            Changed=GFrameCounter;return false;
        }
        auto& M=*Scene->Model;if(M.IsProjectOpenPending()||M.IsRecordingLoadPending())return false;
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/RunSettingsUI");Work=Root/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Work,true);
            FString Error;PreviousWorkspace=M.Workspace;PreviousTab=M.InspectorTab;PreviousExpanded=M.bViewportExpanded;
            if(!Test->TestTrue(TEXT("Preserve prior project"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error)))return true;
            bTooltips=App.GetAllowTooltips();bCapturedTooltips=true;App.SetAllowTooltips(false);StudioAuthoringTestCapture::DismissTooltips();
            M.NewProject(TEXT("Wing · Run parameters"));M.Pause();M.Navigate(EStudioWorkspace::Solve);M.bViewportExpanded=false;M.InspectorTab=0;
            Before=StudioCaseIO::Serialize(M.Project.Draft);Next();break;
        }
        case 1:
            if(!Scene->HasCurrentFrame())return false;
            Camera=Scene->SavedCameraState();Frame=M.SelectedFrame;RenderRevision=M.RenderIntentRevision;
            Capture(TEXT("applied-defaults.png"));
            Type(TEXT("RunParameter0"),TEXT("120000"));Type(TEXT("RunParameter1"),TEXT("2.345678901234567"));
            Type(TEXT("RunParameter2"),TEXT("250"));Type(TEXT("RunParameter3"),TEXT("1000"));Press(TEXT("RunCheckpoints"),EKeys::SpaceBar);Next();break;
        case 2:
            Test->TestEqual(TEXT("Typing and focus changes leave the case unchanged"),StudioCaseIO::Serialize(M.Project.Draft),Before);
            Capture(TEXT("retained-draft.png"));Press(TEXT("InspectorTab1"));Next();break;
        case 3:Press(TEXT("Workspace4"));Next();break;
        case 4:
            if(M.Workspace!=EStudioWorkspace::Solve){Press(TEXT("Workspace7"));Changed=GFrameCounter;return false;}
            Press(TEXT("InspectorTab0"));Next();break;
        case 5:
            Test->TestEqual(TEXT("Exact draft survives sidebar and category changes"),Text(TEXT("RunParameter1")),FString(TEXT("2.345678901234567")));
            Type(TEXT("RunParameter2"),TEXT("1.5"));Press(TEXT("RunParametersApply"));Next();break;
        case 6:
            Test->TestEqual(TEXT("Fractional count is retained for repair"),Text(TEXT("RunParameter2")),FString(TEXT("1.5")));
            Test->TestEqual(TEXT("Invalid apply is atomic"),StudioCaseIO::Serialize(M.Project.Draft),Before);
            Capture(TEXT("invalid-count.png"));Press(TEXT("InspectorTab3"));SaveShortcut();Next();break;
        case 7:
            Test->TestTrue(TEXT("Save exposes unresolved Setup parameters"),M.InspectorTab==0&&M.Workspace==EStudioWorkspace::Solve&&M.Notice.Contains(TEXT("Apply or revert run parameters")));
            Capture(TEXT("save-guard.png"));Type(TEXT("RunParameter2"),TEXT("250"));Press(TEXT("RunParametersApply"));Next();break;
        case 8:
            Applied=M.Project.Draft;
            Test->TestEqual(TEXT("Maximum steps applied"),Applied.Setup.MaxSteps,int64(120000));
            Test->TestTrue(TEXT("Physical seconds and checkpoint request applied exactly"),Applied.Setup.MaxPhysicalTime.Get(0)==2.345678901234567&&Applied.Setup.bCheckpoints&&Applied.Setup.CheckpointInterval==1000);
            Test->TestFalse(TEXT("Apply clears its own save guard"),M.Notice.Contains(TEXT("Apply or revert run parameters")));
            VerifyIsolation();Capture(TEXT("applied-parameters.png"));Press(TEXT("RunParametersUndo"));Next();break;
        case 9:
        {
            auto Restored=M.Project.Draft;Restored.Revision=0;
            Test->TestEqual(TEXT("One undo restores default limit"),Restored.Setup.MaxSteps,int64(2000000));
            Test->TestFalse(TEXT("Undo restores blank time and checkpoint state"),Restored.Setup.MaxPhysicalTime.IsSet()||Restored.Setup.bCheckpoints);
            Test->TestEqual(TEXT("Undo resynchronizes retained fields"),Text(TEXT("RunParameter0")),FString(TEXT("2000000")));
            Press(TEXT("RunParametersRedo"));Next();break;
        }
        case 10:
            Test->TestEqual(TEXT("Redo resynchronizes all applied values"),Text(TEXT("RunParameter1")),FString(TEXT("2.345678901234567")));
            Type(TEXT("RunParameter0"),TEXT("500"));M.EditCase(TEXT("External run limit"),[](auto& Case){Case.Setup.OutputInterval=75;});Next();break;
        case 11:
        {
            const auto Apply=FindTag(TEXT("RunParametersApply"));if(!Apply)return true;
            Test->TestFalse(TEXT("Conflicting form cannot apply"),Apply->IsEnabled());
            Test->TestEqual(TEXT("Conflict retains typed limit"),Text(TEXT("RunParameter0")),FString(TEXT("500")));
            Capture(TEXT("conflict.png"));Press(TEXT("RunParametersRevert"));Next();break;
        }
        case 12:
            Test->TestEqual(TEXT("Revert adopts external output interval"),Text(TEXT("RunParameter2")),FString(TEXT("75")));
            Test->TestTrue(TEXT("Select control harness"),M.SetControlHarness(true));
            Type(TEXT("RunParameter0"),TEXT("600"));
            Test->TestTrue(TEXT("Save placement target"),M.AddCamera(TEXT("Run setup camera"),Scene->SavedCameraState()));
            Test->TestTrue(TEXT("Prepare pending camera placement"),M.BeginCameraPlacement(M.Project.Cameras.Last().Id));
            Press(TEXT("RunControl"));Next();break;
        case 13:
            Test->TestFalse(TEXT("New control run cannot silently use stale applied parameters"),M.HasActiveJob());
            if(!bPlacementChecked)
            {
                Test->TestTrue(TEXT("Run preserves and exposes pending camera placement"),M.CameraPlacement()&&M.Notice.Contains(TEXT("Apply or cancel camera placement")));
                M.CancelCameraPlacement();bPlacementChecked=true;Changed=GFrameCounter;return false;
            }
            Press(TEXT("RunControl"));
            Test->TestTrue(TEXT("Run explains unresolved parameters"),M.Notice.Contains(TEXT("Apply or revert run parameters")));
            Press(TEXT("RunParametersApply"));Next();break;
        case 14:Press(TEXT("RunControl"));Next();break;
        case 15:
            if(M.Job().State()!=EStudioJobState::Running)return false;
            Test->TestTrue(TEXT("Run captured the applied request"),M.Job().Run()->GetConfiguration()->Setup.MaxSteps==600);
            Frozen=StudioCaseIO::Serialize(*M.Job().Run()->GetConfiguration());
            Type(TEXT("RunParameter0"),TEXT("1"));Type(TEXT("RunParameter1"),TEXT("0.0001"));Next(22);break;
        case 22:Press(TEXT("RunParametersApply"));Next(16);break;
        case 16:
            Test->TestEqual(TEXT("Editing the next case preserves the active frozen run"),StudioCaseIO::Serialize(*M.Job().Run()->GetConfiguration()),Frozen);
            Test->TestTrue(TEXT("Harness does not execute physical limits"),M.Job().State()==EStudioJobState::Running);
            Test->TestEqual(TEXT("Scheduled checkbox does not invent restart acknowledgements"),M.Job().CompletedCheckpointCommands(),uint64(0));
            VerifyIsolation();Capture(TEXT("active-run.png"));M.Control(EStudioJobCommand::Stop);Next();break;
        case 17:
            if(M.HasActiveJob())return false;
            Type(TEXT("RunParameter1"),TEXT(""));Next(23);break;
        case 23:Press(TEXT("RunParametersApply"));Next(18);break;
        case 18:
            Test->TestFalse(TEXT("Blank clears the physical-time request"),M.Project.Draft.Setup.MaxPhysicalTime.IsSet());
            Saved=StudioCaseIO::Serialize(M.Project.Draft);
            Test->TestTrue(TEXT("Save applied settings"),M.SaveProject(Work/TEXT("parameters.lbms")));
            M.NewProject(TEXT("Before reopen"));Test->TestTrue(TEXT("Reopen parameter case"),M.RequestProjectOpen(Work/TEXT("parameters.lbms")));Next();break;
        case 19:
            Test->TestEqual(TEXT("Entire case survives reopening"),StudioCaseIO::Serialize(M.Project.Draft),Saved);
            if(M.Workspace!=EStudioWorkspace::Solve){Press(TEXT("Workspace7"));Changed=GFrameCounter;return false;}
            Press(TEXT("InspectorTab0"));Next();break;
        case 20:
            Test->TestEqual(TEXT("Reopened count exact"),Text(TEXT("RunParameter0")),FString(TEXT("1")));
            Test->TestEqual(TEXT("Reopened optional time stays blank"),Text(TEXT("RunParameter1")),FString());
            Capture(TEXT("reopened-parameters.png"));Test->TestTrue(TEXT("Restore previous session"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 21:M.InspectorTab=PreviousTab;M.bViewportExpanded=PreviousExpanded;M.Navigate(PreviousWorkspace);return true;
        }
        return false;
    }
private:
    void Next(){++Phase;Changed=GFrameCounter;}
    void Next(int32 Value){Phase=Value;Changed=GFrameCounter;}
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& Widget,FName Tag)
    {
        if(!Widget->GetVisibility().IsVisible())return {};
        if(Widget->GetTag()==Tag)return Widget;
        auto* Children=Widget->GetChildren();for(int32 Index=0;Index<Children->Num();++Index)if(auto Match=Find(Children->GetChildAt(Index),Tag))return Match;
        return {};
    }
    TSharedPtr<SWidget> FindTag(FName Tag)
    {
        TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& Window:Windows)if(auto Match=Find(Window,Tag))return Match;
        Test->AddError(TEXT("Missing run-parameter control: ")+Tag.ToString());return {};
    }
    void Press(FName Tag,FKey Key=EKeys::Enter)
    {
        const auto Widget=FindTag(Tag);if(!Widget)return;
        if(!Test->TestTrue(FString::Printf(TEXT("Run parameter control %s is enabled at phase %d"),*Tag.ToString(),Phase),Widget->IsEnabled()))return;
        auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Widget,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));
    }
    FString Text(FName Tag)
    {const auto Widget=FindTag(Tag);return Widget?StaticCastSharedPtr<SEditableTextBox>(Widget)->GetText().ToString():FString();}
    void Type(FName Tag,const FString& Value)
    {
        const auto Widget=FindTag(Tag);if(!Widget)return;auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Widget,EFocusCause::Navigation);
        const FModifierKeysState All(false,false,true,false,false,false,false,false,false);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::A,All,0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::A,All,0,false,0,0));
        if(Value.IsEmpty())
        {App.ProcessKeyDownEvent(FKeyEvent(EKeys::BackSpace,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::BackSpace,FModifierKeysState(),0,false,0,0));}
        else for(TCHAR Character:Value)App.ProcessKeyCharEvent(FCharacterEvent(Character,FModifierKeysState(),0,false));
    }
    void SaveShortcut()
    {
        auto& App=FSlateApplication::Get();const FModifierKeysState Command(false,false,false,false,false,false,true,false,false);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::S,Command,0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::S,Command,0,false,0,0));
    }
    void VerifyIsolation()
    {
        const auto& M=*Scene->Model;Test->TestEqual(TEXT("Source frame unchanged"),M.SelectedFrame,Frame);
        Test->TestEqual(TEXT("No computed field request"),M.RenderIntentRevision,RenderRevision);
        Test->TestTrue(TEXT("Solve camera unchanged"),StudioView::CameraEquals(Scene->SavedCameraState(),Camera));
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture native run parameters"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Save run-parameter evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;
    FString Root,Work,Before,Saved,Frozen;FStudioCaseDraft Applied;FStudioCameraState Camera;
    int32 Frame=0,Phase=0,PreviousTab=0;uint64 RenderRevision=0,Changed=0;
    double Started=0,LastActivation=0;bool bTooltips=true,bCapturedTooltips=false,PreviousExpanded=false,bPlacementChecked=false;
    EStudioWorkspace PreviousWorkspace=EStudioWorkspace::Solve;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioRunSettingsUI,"Studio.RunSettingsUI.ControlsPersistenceAndRunIsolation",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioRunSettingsUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioRunSettingsUICommand(this));return true;}
#endif
