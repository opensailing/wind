#include "StudioSnapshotUI.h"
#include "StudioScene.h"
#include "StudioFileDialog.h"
#include "StudioAuthoringTestCapture.h"
#include "StudioAutomationForeground.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#if WITH_DEV_AUTOMATION_TESTS
class FStudioSnapshotUICommand final:public IAutomationLatentCommand
{
    struct FGate
    {
        FEvent* Reached=FPlatformProcess::GetSynchEventFromPool(true);
        FEvent* Release=FPlatformProcess::GetSynchEventFromPool(true);
        ~FGate(){FPlatformProcess::ReturnSynchEventToPool(Reached);FPlatformProcess::ReturnSynchEventToPool(Release);}
    };
public:
    explicit FStudioSnapshotUICommand(FAutomationTestBase* In,bool Movie=false):Test(In),bMovie(Movie){}
    ~FStudioSnapshotUICommand(){if(Gate)Gate->Release->Trigger();if(bCaptured&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Foreground.WasInterrupted()){Test->AddError(Foreground.Describe(Phase));return true;}
        if(Now-Started>180){Test->AddError(FString::Printf(TEXT("Snapshot UI timeout phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)if(It->Model&&!It->Model->IsSnapshotView())Scene=*It;
        if(!Scene.IsValid()||GFrameCounter<Changed+8)return false;
        auto& App=FSlateApplication::Get();auto& M=*Scene->Model;
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending())return false;
        if(!App.IsActive())
        {if(Now-LastActivation>1){LastActivation=Now;FPlatformApplicationMisc::ActivateApplication();GEngine->GameViewport->GetWindow()->BringToFront(true);}Changed=GFrameCounter;return false;}
        if(bOpen)
        {bOpen=false;Press(TEXT("SnapshotOptions"));const auto A=StaticCastSharedPtr<SMenuAnchor>(Find(TEXT("SnapshotOptions")));Test->TestTrue(TEXT("Snapshot opens through routed input"),A&&A->IsOpen());Changed=GFrameCounter;return false;}
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            Root=FPaths::ProjectSavedDir()/(bMovie?TEXT("Automation/MovieUI"):TEXT("Automation/SnapshotUI"));IFileManager::Get().MakeDirectory(*Root,true);Work=Root/FGuid::NewGuid().ToString();
            FString Error;Test->TestTrue(TEXT("Preserve prior project"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error));
            bTooltips=App.GetAllowTooltips();bCaptured=true;App.SetAllowTooltips(false);StudioAuthoringTestCapture::DismissTooltips();Foreground.Begin();
            M.NewProject(TEXT("Image exports"));M.ReviewRecordedFrame(420);M.EditView(TEXT("Snapshot test field"),[](auto& S){S.Display.ScalarField=TEXT("pressure");});Next();break;
        }
        case 1:if(!Scene->HasCurrentFrame())return false;Open();Next();break;
        case 2:
            Test->TestTrue(TEXT("One image defaults to existing PNG action"),Enabled(TEXT("SaveSnapshotPNG"))&&HasText(TEXT("Single PNG"))&&!Find(TEXT("SnapshotFirst")));
            Type(TEXT("SnapshotWidth"),TEXT("640"));Press(TEXT("SnapshotAspect1"));Capture(TEXT("single.png"));Next();break;
        case 3:
            Oracle(TEXT("single"));StudioFileDialog::SetNextSnapshotPNGForAutomation(Work/TEXT("single.png"));Press(TEXT("SaveSnapshotPNG"));Next();break;
        case 4:if(!M.Notice.StartsWith(TEXT("Saved PNG")))return false;Open();Next();break;
        case 5:
            Test->TestTrue(TEXT("Completed PNG has reveal action"),Enabled(TEXT("RevealSnapshot")));Press(TEXT("SnapshotSequence"));Next();break;
        case 6:
            if(bMovie)Press(TEXT("SnapshotMovie"));
            Type(TEXT("SnapshotFirst"),TEXT("1"));Type(TEXT("SnapshotLast"),TEXT("602"));Type(TEXT("SnapshotStride"),TEXT("300"));Next();break;
        case 7:
            Test->TestTrue(TEXT("Out-of-range endpoint disables export"),!Enabled(TEXT("SaveSnapshotPNG"))&&HasText(TEXT("within frames 1–601")));Capture(TEXT("invalid-range.png"));Type(TEXT("SnapshotLast"),TEXT("601"));Type(TEXT("SnapshotStride"),TEXT("0"));Next();break;
        case 8:
            Test->TestTrue(TEXT("Zero stride is actionable"),!Enabled(TEXT("SaveSnapshotPNG"))&&HasText(TEXT("positive whole numbers")));Capture(TEXT("invalid-stride.png"));Type(TEXT("SnapshotStride"),TEXT("300"));Type(TEXT("SnapshotFolder"),TEXT("../bad"));Next();break;
        case 9:
            Test->TestTrue(TEXT("Unsafe folder rejected"),!Enabled(TEXT("SaveSnapshotPNG"))&&HasText(TEXT("new folder name")));Capture(TEXT("invalid-folder.png"));Type(TEXT("SnapshotFolder"),TEXT("range"));
            if(bMovie){Type(TEXT("SnapshotWidth"),TEXT("642"));Phase=50;Changed=GFrameCounter;}else Next();break;
        case 50:
            Test->TestTrue(TEXT("Movie refuses odd height without silent resizing"),!Enabled(TEXT("SaveSnapshotPNG"))&&HasText(TEXT("even image dimensions")));
            Capture(TEXT("invalid-movie-size.png"));Type(TEXT("SnapshotWidth"),TEXT("640"));Type(TEXT("SnapshotMovieRate"),TEXT("24"));Next();break;
        case 51:
            Test->TestTrue(TEXT("Movie rate and duration are separate from physical time"),HasText(TEXT("Playback: 24 fps · 0.125 s"))&&HasText(TEXT("0–0.12 s"))&&Enabled(TEXT("SaveSnapshotPNG")));
            Capture(TEXT("movie-options.png"));Phase=10;Changed=GFrameCounter;break;
        case 10:
            Test->TestTrue(TEXT("Selection states exact sampled count and original times"),HasText(bMovie?TEXT("3 PNGs + MP4 · frames 1–601"):TEXT("3 PNGs · frames 1–601"))&&HasText(TEXT("0–0.12 s"))&&Enabled(TEXT("SaveSnapshotPNG")));
            Capture(TEXT("range.png"));StudioFileDialog::SetNextExportFolderForAutomation(FString());Press(TEXT("SaveSnapshotPNG"));Next();break;
        case 11:Test->TestTrue(TEXT("Folder chooser cancellation explicit"),M.Notice.Contains(TEXT("folder selection cancelled")));Open();Next();break;
        case 12:
            Oracle(TEXT("range"));Gate=MakeShared<FGate,ESPMode::ThreadSafe>();FStudioSnapshotUI::SetBeforeNextPublishForAutomation([G=Gate]{G->Reached->Trigger();G->Release->Wait(30000);});
            StudioFileDialog::SetNextExportFolderForAutomation(Work);Press(TEXT("SaveSnapshotPNG"));M.Run();Scene->SetCameraPosition(Scene->SavedCameraState().Position+FVector(.2,0,0));Next();break;
        case 13:if(!Gate->Reached->Wait(0))return false;Open();Next();break;
        case 14:
            Test->TestTrue(TEXT("Frozen sequence remains owned after live edits"),HasText(TEXT("Frozen view · 640 × 360"))&&HasText(bMovie?TEXT("Finalizing MP4"):TEXT("3 of 3 images saved"))&&!Enabled(TEXT("SaveSnapshotPNG"))&&Enabled(TEXT("CancelSnapshotExport"))&&!Enabled(TEXT("SnapshotDraft")));
            Capture(TEXT("progress.png"));App.DismissAllMenus();M.Pause();Gate->Release->Trigger();Next();break;
        case 15:if(!M.Notice.StartsWith(bMovie?TEXT("Saved MP4 + 3 PNGs"):TEXT("Saved 3 PNGs")))return false;Open();Next();break;
        case 16:Test->TestTrue(TEXT("Sequence saved with reveal"),Enabled(TEXT("RevealSnapshot")));Capture(TEXT("saved.png"));StudioFileDialog::SetNextExportFolderForAutomation(Work);Press(TEXT("SaveSnapshotPNG"));Next();break;
        case 17:if(!M.Notice.StartsWith(TEXT("Image sequence failed")))return false;Open();Next();break;
        case 18:
            Test->TestTrue(TEXT("Existing export has recovery"),HasText(TEXT("already exists"))&&Enabled(TEXT("SaveSnapshotPNG")));Capture(TEXT("existing.png"));Type(TEXT("SnapshotFolder"),TEXT("cancelled"));Next();break;
        case 19:
            Gate=MakeShared<FGate,ESPMode::ThreadSafe>();FStudioSnapshotUI::SetBeforeNextPublishForAutomation([G=Gate]{G->Reached->Trigger();G->Release->Wait(30000);});
            StudioFileDialog::SetNextExportFolderForAutomation(Work);Press(TEXT("SaveSnapshotPNG"));Next();break;
        case 20:if(!Gate->Reached->Wait(0))return false;Press(TEXT("Workspace1"));Next();break;
        case 21:
            Test->TestTrue(TEXT("Global status points back to sole Snapshot owner"),HasText(TEXT("Open Snapshot in Solve")));M.NewProject(TEXT("Replacement while exporting images"));Press(TEXT("Workspace7"));Open();Next();break;
        case 22:
            Test->TestTrue(TEXT("Old export cancellable from replacement project"),Enabled(TEXT("CancelSnapshotExport"))&&!Enabled(TEXT("SaveSnapshotPNG"))&&HasText(TEXT("Frozen view")));
            Capture(TEXT("replacement-progress.png"));Press(TEXT("CancelSnapshotExport"));Gate->Release->Trigger();Next();break;
        case 23:
            if(Find(TEXT("CancelSnapshotExport")))return false;
            Test->TestFalse(TEXT("Cancelled sequence never published"),IFileManager::Get().DirectoryExists(*(Work/TEXT("cancelled"))));
            Test->TestFalse(TEXT("Old completion cannot enter new project"),M.Notice.Contains(TEXT("Image sequence cancelled")));
            Capture(TEXT("changed-source.png"));App.DismissAllMenus();M.RequestExternalRecording(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_ReaderFixture/recording.json"));Next();break;
        case 24:M.RequestReconstruction(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_VolumeFixture/reconstruction.json"));Next();break;
        case 25:if(!Scene->HasCurrentFrame())return false;Scene->FitCamera();Open();Next();break;
        case 26:Press(TEXT("SnapshotAll"));Type(TEXT("SnapshotStride"),TEXT("1"));Type(TEXT("SnapshotFolder"),TEXT("all-volume"));Next();break;
        case 27:
            Test->TestTrue(TEXT("All uses current authentic source without editable endpoints"),HasText(bMovie?TEXT("3 PNGs + MP4 · frames 1–3"):TEXT("3 PNGs · frames 1–3"))&&!Find(TEXT("SnapshotFirst"))&&Enabled(TEXT("SaveSnapshotPNG")));
            Capture(TEXT("all.png"));Oracle(TEXT("all-volume"));StudioFileDialog::SetNextExportFolderForAutomation(Work);Press(TEXT("SaveSnapshotPNG"));Next();break;
        case 28:if(!M.Notice.StartsWith(bMovie?TEXT("Saved MP4 + 3 PNGs"):TEXT("Saved 3 PNGs")))return false;Open();Next();break;
        case 29:Capture(TEXT("all-saved.png"));App.DismissAllMenus();Test->TestTrue(TEXT("Restore prior project"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 30:return true;
        }
        return false;
    }
private:
    void Oracle(const TCHAR* Name)
    {
        FStudioSnapshot S;S.Options.Size=FIntPoint(640,360);FString Error;
        if(!Test->TestTrue(TEXT("Freeze direct clicked camera oracle"),Scene->CaptureSnapshot(S,nullptr,Error)))return;
        Test->TestTrue(TEXT("Save camera and original source oracle"),FFileHelper::SaveStringToFile(StudioSnapshot::Metadata(S),*(Work/(FString(Name)+TEXT("-anchor.json"))),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    }
    void Next(){++Phase;Changed=GFrameCounter;}
    void Open(){bOpen=true;}
    TSharedPtr<SWidget> FindIn(const TSharedRef<SWidget>& W,FName Tag)
    {W->UpdateAllAttributes();if(!W->GetVisibility().IsVisible())return {};if(W->GetTag()==Tag)return W;auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto F=FindIn(C->GetChildAt(I),Tag))return F;return {};}
    TSharedPtr<SWidget> FindWindow(const TSharedRef<SWindow>& W,FName Tag)
    {if(auto Found=FindIn(W,Tag))return Found;for(const auto& Child:W->GetChildWindows())if(auto Found=FindWindow(Child,Tag))return Found;return {};}
    TSharedPtr<SWidget> Find(FName Tag)
    {for(const auto& W:FSlateApplication::Get().GetInteractiveTopLevelWindows())if(auto Found=FindWindow(W,Tag))return Found;return {};}
    FString MenuInfo(){return FString::Printf(TEXT("Snapshot phase %d"),Phase);}
    bool Enabled(FName Tag){const auto W=Find(Tag);return W&&W->IsEnabled();}
    bool HasTextIn(const TSharedRef<SWidget>& W,const FString& Text)
    {if(!W->GetVisibility().IsVisible())return false;if(W->GetType()==TEXT("STextBlock")&&StaticCastSharedRef<STextBlock>(W)->GetText().ToString().Contains(Text))return true;
        auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(HasTextIn(C->GetChildAt(I),Text))return true;return false;}
    bool HasWindowText(const TSharedRef<SWindow>& W,const FString& Text)
    {if(HasTextIn(W,Text))return true;for(const auto& Child:W->GetChildWindows())if(HasWindowText(Child,Text))return true;return false;}
    bool HasText(const FString& Text)
    {for(const auto& W:FSlateApplication::Get().GetInteractiveTopLevelWindows())if(HasWindowText(W,Text))return true;return false;}
    TSharedPtr<SWidget> Focusable(const TSharedRef<SWidget>& W)
    {if(!W->GetVisibility().IsVisible()||!W->IsEnabled())return {};if(W->SupportsKeyboardFocus())return W;auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto F=Focusable(C->GetChildAt(I)))return F;return {};}
    void Key(FKey K,const FModifierKeysState& Mod={})
    {auto& A=FSlateApplication::Get();A.ProcessKeyDownEvent(FKeyEvent(K,Mod,0,false,0,0));A.ProcessKeyUpEvent(FKeyEvent(K,Mod,0,false,0,0));}
    bool Focus(FName Tag)
    {const auto W=Find(Tag);if(!Test->TestTrue(TEXT("Export widget available: ")+Tag.ToString()+TEXT(" · ")+MenuInfo(),W&&W->IsEnabled()))return false;
        const auto Target=Focusable(W.ToSharedRef());if(!Test->TestTrue(TEXT("Focusable export widget"),Target.IsValid()))return false;
        FSlateApplication::Get().SetKeyboardFocus(Target,EFocusCause::Navigation);return Test->TestTrue(TEXT("Export keyboard focus"),Target->HasKeyboardFocus()||Target->HasFocusedDescendants());}
    void Press(FName Tag){if(Focus(Tag))Key(EKeys::Enter);}
    void Type(FName Tag,const FString& Text)
    {if(!Focus(Tag))return;Key(EKeys::A,FModifierKeysState(false,false,true,false,false,false,false,false,false));for(const TCHAR C:Text)FSlateApplication::Get().ProcessKeyCharEvent(FCharacterEvent(C,FModifierKeysState(),0,false));Key(EKeys::Tab);}
    void Capture(const TCHAR* Name)
    {
        const auto Panel=Find(TEXT("SnapshotPanel"));Test->TestTrue(TEXT("Snapshot panel visible"),Panel.IsValid());
        if(Panel)
        {
            const auto Bounds=Panel->GetCachedGeometry().GetLayoutBoundingRect();
            for(const TCHAR* Tag:{TEXT("SnapshotSource"),TEXT("SaveSnapshotPNG"),TEXT("CancelSnapshotExport"),TEXT("SnapshotNotice"),TEXT("RevealSnapshot")})if(const auto W=Find(Tag))
            {const auto R=W->GetCachedGeometry().GetLayoutBoundingRect();Test->TestTrue(FString(Name)+TEXT(" keeps ")+Tag+TEXT(" visible"),R.Top>=Bounds.Top&&R.Bottom<=Bounds.Bottom&&R.Left>=Bounds.Left&&R.Right<=Bounds.Right);}
        }
        TArray<FColor> Pixels;FIntVector Size;if(!Test->TestTrue(TEXT("Capture Snapshot controls"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);Test->TestTrue(TEXT("Save UI capture"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FStudioAutomationForeground Foreground;
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;TSharedPtr<FGate,ESPMode::ThreadSafe> Gate;
    FString Root,Work;int32 Phase=0;uint64 Changed=0;double Started=0,LastActivation=0;bool bTooltips=true,bCaptured=false,bOpen=false,bMovie=false;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioSnapshotUIAcceptance,"Studio.SnapshotUI.OriginalSequencesAndLifecycle",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioSnapshotUIAcceptance::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioSnapshotUICommand(this));return true;}
#if PLATFORM_MAC
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioMovieUIAcceptance,"Studio.MovieUI.ControlsAndOriginalOutputs",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioMovieUIAcceptance::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioSnapshotUICommand(this,true));return true;}
#endif
#endif
