#include "StudioFieldExportUI.h"
#include "StudioScene.h"
#include "StudioFileDialog.h"
#include "StudioAuthoringTestCapture.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioFieldExportUICommand final:public IAutomationLatentCommand
{
    struct FPublishGate
    {
        FEvent* Reached=FPlatformProcess::GetSynchEventFromPool(true);
        FEvent* Release=FPlatformProcess::GetSynchEventFromPool(true);
        ~FPublishGate(){FPlatformProcess::ReturnSynchEventToPool(Reached);FPlatformProcess::ReturnSynchEventToPool(Release);}
    };
public:
    explicit FStudioFieldExportUICommand(FAutomationTestBase* In):Test(In){}
    ~FStudioFieldExportUICommand()
    {if(Gate)Gate->Release->Trigger();if(bCaptured&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>150){Test->AddError(FString::Printf(TEXT("Field export UI timed out at phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)if(It->Model&&!It->Model->IsSnapshotView())Scene=*It;
        if(!Scene.IsValid()||!Scene->Model||GFrameCounter<Changed+8)return false;
        auto& App=FSlateApplication::Get();auto& M=*Scene->Model;
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending())return false;
        if(!App.IsActive())
        {if(Now-LastActivation>1){LastActivation=Now;FPlatformApplicationMisc::ActivateApplication();GEngine->GameViewport->GetWindow()->BringToFront(true);}Changed=GFrameCounter;return false;}
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/FieldExportUI");IFileManager::Get().MakeDirectory(*Root,true);
            Work=Root/FGuid::NewGuid().ToString();FString Error;
            Test->TestTrue(TEXT("Preserve original project"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error));
            bTooltips=App.GetAllowTooltips();bCaptured=true;App.SetAllowTooltips(false);StudioAuthoringTestCapture::DismissTooltips();
            M.NewProject(TEXT("Original field export"));M.ReviewRecordedFrame(420);Next();break;
        }
        case 1:if(!Scene->HasCurrentFrame())return false;Press(TEXT("ExportMenu"));Next();break;
        case 2:
            Test->TestTrue(TEXT("Single export route offers both formats"),Find(TEXT("ExportFieldCSV")).IsValid()&&Find(TEXT("VTKExportMenu")).IsValid());
            Capture(TEXT("export-menu.png"));Press(TEXT("VTKExportMenu"));Next();break;
        case 3:
            Test->TestTrue(TEXT("Frozen original frame named"),HasText(TEXT("Frozen frame 421")));
            Capture(TEXT("original-fields.png"));Press(TEXT("VTKSelectNone"));Next();break;
        case 4:
            Test->TestFalse(TEXT("No empty scalar export"),Enabled(TEXT("SaveFieldVTK")));
            Test->TestTrue(TEXT("Missing selection has concrete recovery"),HasText(TEXT("Select at least one scalar array")));
            Capture(TEXT("empty-selection.png"));Press(TEXT("VTKField_pressure"));Press(TEXT("VTKField_density"));Press(TEXT("VTKCoordinatesScene"));
            StudioFileDialog::SetNextFieldVTKForAutomation(FString());Press(TEXT("SaveFieldVTK"));Next();break;
        case 5:
            Test->TestTrue(TEXT("File panel cancellation explicit"),M.Notice.Contains(TEXT("file selection cancelled")));
            OpenExport();Next();break;
        case 6:
            Press(TEXT("VTKSelectNone"));Press(TEXT("VTKField_pressure"));Press(TEXT("VTKField_density"));Press(TEXT("VTKCoordinatesScene"));
            M.ReviewRecordedFrame(421);
            {auto Camera=Scene->SavedCameraState();Camera.Position.X+=.1;M.EditCamera(TEXT("Camera during export selection"),Camera);}
            Next();break;
        case 7:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestTrue(TEXT("Playback cursor changes independently of frozen export"),M.SelectedFrame==421&&HasText(TEXT("Frozen frame 421")));
            Capture(TEXT("frozen-scene-selection.png"));
            StudioFileDialog::SetNextFieldVTKForAutomation(Root/TEXT("selected-scene.vtp"));Press(TEXT("SaveFieldVTK"));Next();break;
        case 8:
            if(!M.Notice.StartsWith(TEXT("Saved selected-scene.vtp")))return false;
            Test->TestTrue(TEXT("Written identity retained from before cursor change"),M.Notice.Contains(TEXT("step 420")));
            Test->TestTrue(TEXT("Only chosen arrays written"),XML(TEXT("selected-scene.vtp")).Contains(TEXT("Name=\"pressure\""))&&
                XML(TEXT("selected-scene.vtp")).Contains(TEXT("Name=\"density\""))&&!XML(TEXT("selected-scene.vtp")).Contains(TEXT("Name=\"velocity_x\"")));
            OpenExport();Next();break;
        case 9:
            Test->TestTrue(TEXT("Success and reveal survive popup dismissal"),HasText(TEXT("Saved selected-scene.vtp"))&&Enabled(TEXT("RevealFieldVTK")));
            Capture(TEXT("saved-export.png"));
            Test->TestTrue(TEXT("Create failure sentinel"),FFileHelper::SaveStringToFile(TEXT("Keep existing destination"),*(Root/TEXT("blocked-file"))));
            StudioFileDialog::SetNextFieldVTKForAutomation(Root/TEXT("blocked-file/child.vtp"));Press(TEXT("SaveFieldVTK"));Next();break;
        case 10:
            if(!M.Notice.StartsWith(TEXT("VTK export failed")))return false;
            OpenExport();Next();break;
        case 11:
            Test->TestTrue(TEXT("Write error remains inspectable"),HasText(TEXT("VTK export failed")));
            Capture(TEXT("write-error.png"));App.DismissAllMenus();
            Test->TestTrue(TEXT("Import authentic 3D source"),M.RequestExternalRecording(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_ReaderFixture/recording.json")));Next();break;
        case 12:if(!Scene->HasCurrentFrame())return false;M.ReviewRecordedFrame(2);Next();break;
        case 13:if(!Scene->HasCurrentFrame())return false;OpenExport();Next();break;
        case 14:
            Test->TestFalse(TEXT("New source clears the prior source's export error from the task panel"),ExportNotice().Contains(TEXT("VTK export failed")));
            Press(TEXT("VTKSelectAll"));Capture(TEXT("point-source-fields.png"));
            Test->TestTrue(TEXT("Create cancelled destination sentinel"),FFileHelper::SaveStringToFile(TEXT("Keep existing destination"),*(Root/TEXT("cancelled.vtp"))));
            Gate=MakeShared<FPublishGate,ESPMode::ThreadSafe>();
            FStudioFieldExportUI::SetBeforeNextPublishForAutomation([G=Gate]{G->Reached->Trigger();G->Release->Wait(10000);});
            StudioFileDialog::SetNextFieldVTKForAutomation(Root/TEXT("cancelled.vtp"));Press(TEXT("SaveFieldVTK"));Next();break;
        case 15:
            if(!Gate->Reached->Wait(0))return false;
            OpenExport();Next();break;
        case 16:
            Test->TestFalse(TEXT("Busy export has no second write"),Enabled(TEXT("SaveFieldVTK")));
            Test->TestTrue(TEXT("Progress and cancel accessible after reopening"),Find(TEXT("VTKProgress")).IsValid()&&Enabled(TEXT("CancelFieldVTK")));
            Capture(TEXT("export-progress.png"));Press(TEXT("CancelFieldVTK"));Gate->Release->Trigger();Next();break;
        case 17:
            if(!M.Notice.StartsWith(TEXT("VTK export cancelled")))return false;
            Test->TestEqual(TEXT("Cancelled write preserves existing bytes"),XML(TEXT("cancelled.vtp")),FString(TEXT("Keep existing destination")));
            Capture(TEXT("cancelled-export.png"));
            StudioFileDialog::SetNextFieldVTKForAutomation(Root/TEXT("cylinder-points.vtp"));Press(TEXT("SaveFieldVTK"));Next();break;
        case 18:
            if(!M.Notice.StartsWith(TEXT("Saved cylinder-points.vtp")))return false;
            Test->TestEqual(TEXT("Export preserves scene cursor"),M.SelectedFrame,2);
            Test->TestTrue(TEXT("Complete original point output after cancellation"),XML(TEXT("cylinder-points.vtp")).EndsWith(TEXT("</VTKFile>\n")));
            OpenExport();Next();break;
        case 19:M.NewProject(TEXT("Changed export project"));Next();break;
        case 20:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestFalse(TEXT("Project replacement dismisses the old draft"),Find(TEXT("SaveFieldVTK")).IsValid());
            OpenExport();Next();break;
        case 21:
            Test->TestTrue(TEXT("Reopening captures only the new project's source frame"),HasText(TEXT("Frozen frame 1 · step 0"))&&Enabled(TEXT("SaveFieldVTK")));
            Test->TestFalse(TEXT("New project clears old source export result"),ExportNotice().Contains(TEXT("Saved cylinder-points.vtp")));
            Capture(TEXT("changed-project.png"));App.DismissAllMenus();
            Test->TestTrue(TEXT("Restore original project"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 22:return true;
        }
        return false;
    }
private:
    void Next(){++Phase;Changed=GFrameCounter;}
    void OpenExport(){Press(TEXT("ExportMenu"));Press(TEXT("VTKExportMenu"));}
    FString XML(const TCHAR* Name){FString S;FFileHelper::LoadFileToString(S,*(Root/Name));return S;}
    TSharedPtr<SWidget> FindIn(const TSharedRef<SWidget>& W,FName Tag)
    {W->UpdateAllAttributes();if(!W->GetVisibility().IsVisible())return {};if(W->GetTag()==Tag)return W;auto* C=W->GetChildren();
        for(int32 I=0;I<C->Num();++I)if(auto F=FindIn(C->GetChildAt(I),Tag))return F;return {};}
    TSharedPtr<SWidget> Find(FName Tag){return FindIn(GEngine->GameViewport->GetWindow().ToSharedRef(),Tag);}
    bool HasTextIn(const TSharedRef<SWidget>& W,const FString& Text)
    {if(!W->GetVisibility().IsVisible())return false;
        if(W->GetType()==TEXT("STextBlock")&&StaticCastSharedRef<STextBlock>(W)->GetText().ToString().Contains(Text))return true;
        auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(HasTextIn(C->GetChildAt(I),Text))return true;return false;}
    bool HasText(const FString& Text){return HasTextIn(GEngine->GameViewport->GetWindow().ToSharedRef(),Text);}
    FString ExportNotice(){const auto W=Find(TEXT("VTKNotice"));return W?StaticCastSharedPtr<STextBlock>(W)->GetText().ToString():FString();}
    bool Enabled(FName Tag){const auto W=Find(Tag);return W&&W->IsEnabled();}
    TSharedPtr<SWidget> Focusable(const TSharedRef<SWidget>& W)
    {
        if(!W->GetVisibility().IsVisible()||!W->IsEnabled())return {};if(W->SupportsKeyboardFocus())return W;
        auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto F=Focusable(C->GetChildAt(I)))return F;return {};
    }
    void Press(FName Tag)
    {
        const auto W=Find(Tag);if(!Test->TestTrue(TEXT("Field export widget exists: ")+Tag.ToString(),W.IsValid()))return;
        if(!Test->TestTrue(TEXT("Field export widget enabled: ")+Tag.ToString(),W->IsEnabled()))return;
        const auto Target=Focusable(W.ToSharedRef());if(!Test->TestTrue(TEXT("Focusable field export control"),Target.IsValid()))return;
        auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Target,EFocusCause::Navigation);
        Test->TestTrue(TEXT("Field export keyboard focus: ")+Tag.ToString(),Target->HasKeyboardFocus()||Target->HasFocusedDescendants());
        const FKey Key=Target->GetType()==TEXT("SCheckBox")?EKeys::SpaceBar:EKeys::Enter;
        App.ProcessKeyDownEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(Key,FModifierKeysState(),0,false,0,0));
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture field export window"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Save field export capture"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;TSharedPtr<FPublishGate,ESPMode::ThreadSafe> Gate;
    FString Root,Work;int32 Phase=0;uint64 Changed=0;double Started=0,LastActivation=0;bool bTooltips=true,bCaptured=false;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioFieldExportUIRender,"Studio.FieldExportUI.FrozenFieldsCoordinatesAndRecovery",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioFieldExportUIRender::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioFieldExportUICommand(this));return true;}
#endif
