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
class FStudioGeometryEditUICommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioGeometryEditUICommand(FAutomationTestBase* InTest):Test(InTest){}
    ~FStudioGeometryEditUICommand()
    {if(bCapturedTooltips&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>100){Test->AddError(FString::Printf(TEXT("Geometry editor timed out at phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model||GFrameCounter<Changed+4)return false;
        auto& App=FSlateApplication::Get();
        if(!App.IsActive())
        {
            if(Now-LastActivation>1.)
            {LastActivation=Now;Test->AddInfo(TEXT("Activating the native application before geometry keyboard input."));FPlatformApplicationMisc::ActivateApplication();GEngine->GameViewport->GetWindow()->BringToFront(true);}
            Changed=GFrameCounter;return false;
        }
        auto& M=*Scene->Model;
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending()||M.IsReadingGeometry())return false;
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/GeometryEdit");Work=Root/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Work,true);
            FString Error;PreviousWorkspace=M.Workspace;
            if(!Test->TestTrue(TEXT("Preserve prior project"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error)))return true;
            bTooltips=App.GetAllowTooltips();bCapturedTooltips=true;App.SetAllowTooltips(false);StudioAuthoringTestCapture::DismissTooltips();
            M.NewProject(TEXT("Geometry transforms"));M.Pause();
            const FString Path=Work/TEXT("transform-fixture.obj");
            // Authoring fixture only, with no generated CFD arrays.
            FFileHelper::SaveStringToFile(TEXT("# Structural geometry fixture\nv 0 0 0\nv 1 0 0\nv 0 .5 0\nv 0 0 1\ng Surface\nf 1 3 2\nf 1 2 4\nf 2 3 4\nf 3 1 4\n"),*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            const auto Source=StudioMeshImport::Read(Path,MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false));
            FStudioMeshImportOptions Options;Options.Name=TEXT("Transform fixture A");Options.MetersPerUnit=1.;
            if(!Test->TestTrue(TEXT("Parse original structural fixture"),StudioMeshImport::MakeAsset(Source,Options,First,Error)))return true;
            auto Second=First;Second.Id=FGuid::NewGuid();Second.Name=TEXT("Transform fixture B");Second.Translation.X=2;
            for(auto& Patch:Second.Patches)Patch.Id=FGuid::NewGuid();SecondId=Second.Id;
            Test->TestTrue(TEXT("Add independent case objects"),M.EditCase(TEXT("Transform fixtures"),[this,Second](auto& Case)
                {Case.Geometry={First,Second};Case.Setup.BackendId=TEXT("test-control-harness");}));
            M.Project.Runs.Add(FStudioRunRecord::Capture(TEXT("Frozen transform fixture"),M.Project.Draft,EStudioRunOrigin::ControlHarness));
            Frozen=StudioCaseIO::Serialize(*M.Project.Runs.Last().GetConfiguration());
            Before=StudioCaseIO::Serialize(M.Project.Draft);Camera=M.Project.Camera;Frame=M.SelectedFrame;RenderRevision=M.RenderIntentRevision;
            Test->TestTrue(TEXT("Select original object"),M.SelectGeometry(First.Id));Press(TEXT("Workspace2"));Next();break;
        }
        case 1:
            if(!Scene->HasGeometryPreview())return false;
            Test->TestTrue(TEXT("Sidebar opens Geometry"),M.Workspace==EStudioWorkspace::Geometry);
            Capture(TEXT("applied-original.png"));
            Type(TEXT("GeometryEditName"),TEXT("Positioned object"));Type(TEXT("GeometryEditValue0"),TEXT("0.25"));
            Type(TEXT("GeometryEditValue2"),TEXT("0.12"));Type(TEXT("GeometryEditValue5"),TEXT("15"));Type(TEXT("GeometryEditValue8"),TEXT("2"));Next();break;
        case 2:
            Test->TestEqual(TEXT("Typing and changing focus leave the applied case unchanged"),StudioCaseIO::Serialize(M.Project.Draft),Before);
            Test->TestTrue(TEXT("Unapplied text leaves original rendered bounds"),FMath::IsNearlyEqual(Scene->GetPreviewBounds().Max.Z,1.,1.e-9));
            Capture(TEXT("retained-draft.png"));Press(TEXT("Workspace4"));Next();break;
        case 3:Press(TEXT("Workspace2"));Next();break;
        case 4:
            if(!Scene->HasGeometryPreview())return false;
            Test->TestEqual(TEXT("Navigation retains the unapplied name"),Text(TEXT("GeometryEditName")),FString(TEXT("Positioned object")));
            Press(FName(*(TEXT("GeometryRow_")+SecondId.ToString())));
            Test->TestTrue(TEXT("Object selection starts a source read"),M.IsReadingGeometry());
            SaveShortcut();Next();break;
        case 5:
            if(!Scene->HasGeometryPreview())return false;
            if(!bSaveRedirectChecked)
            {
                Test->TestEqual(TEXT("Save during another source read returns to the pending object"),M.SelectedGeometry,First.Id);
                Test->TestEqual(TEXT("Save redirection retains the draft"),Text(TEXT("GeometryEditValue0")),FString(TEXT("0.25")));
                const auto ApplyButton=FindTag(TEXT("ApplyGeometryEdits"));if(!ApplyButton)return true;
                Test->TestTrue(TEXT("Redirected form has its verified source ready"),ApplyButton->IsEnabled());
                Capture(TEXT("save-redirect.png"));Press(FName(*(TEXT("GeometryRow_")+SecondId.ToString())));
                bSaveRedirectChecked=true;Changed=GFrameCounter;return false;
            }
            Test->TestEqual(TEXT("Another object has its own form"),Text(TEXT("GeometryEditName")),FString(TEXT("Transform fixture B")));
            Press(FName(*(TEXT("GeometryRow_")+First.Id.ToString())));Next();break;
        case 6:
            if(!Scene->HasGeometryPreview())return false;
            Test->TestEqual(TEXT("Object switching retains the exact position draft"),Text(TEXT("GeometryEditValue0")),FString(TEXT("0.25")));
            Press(TEXT("ApplyGeometryEdits"));Next();break;
        case 7:
            if(!Scene->HasGeometryPreview())return false;
            Applied=M.Project.Draft.Geometry[0];
            Test->TestEqual(TEXT("One apply commits the name"),Applied.Name,FString(TEXT("Positioned object")));
            Test->TestEqual(TEXT("One apply commits the scale"),Applied.Scale.Z,2.);
            Test->TestTrue(TEXT("Rendered physical bounds include applied translation and scale"),
                FMath::IsNearlyEqual(Scene->GetPreviewBounds().Min.Z,.12,1.e-9)&&FMath::IsNearlyEqual(Scene->GetPreviewBounds().Max.Z,2.12,1.e-9));
            Test->TestEqual(TEXT("Source identity is preserved"),Applied.SourceSHA256,First.SourceSHA256);
            Test->TestEqual(TEXT("Patch identity is preserved"),Applied.Patches[0].Id,First.Patches[0].Id);
            VerifyIsolation();Capture(TEXT("applied-transform.png"));Type(TEXT("GeometryEditValue0"),TEXT("not meters"));Next();break;
        case 8:Press(TEXT("ApplyGeometryEdits"));Next();break;
        case 9:
            Test->TestEqual(TEXT("Rejected text is retained"),Text(TEXT("GeometryEditValue0")),FString(TEXT("not meters")));
            Test->TestEqual(TEXT("Rejected input does not move the object"),M.Project.Draft.Geometry[0].Translation,Applied.Translation);
            Capture(TEXT("invalid-transform.png"));SaveShortcut();Next();break;
        case 10:
            Test->TestTrue(TEXT("Save routes to unapplied object edits"),M.Notice.Contains(TEXT("Apply or revert object edits"))&&M.Workspace==EStudioWorkspace::Geometry);
            Press(TEXT("RevertGeometryEdits"));Next();break;
        case 11:
            Test->TestFalse(TEXT("Revert clears the matching save guard"),M.Notice.Contains(TEXT("Apply or revert object edits")));
            Test->TestEqual(TEXT("Revert restores the applied position"),Text(TEXT("GeometryEditValue0")),FString(TEXT("0.25")));
            Type(TEXT("GeometryEditValue0"),TEXT("1.5"));
            M.EditCase(TEXT("External transform"),[](auto& Case){Case.Geometry[0].Translation.Y=.5;});Next();break;
        case 12:
        {
            if(!Scene->HasGeometryPreview())return false;
            const auto ApplyButton=FindTag(TEXT("ApplyGeometryEdits"));if(!ApplyButton)return true;
            Test->TestFalse(TEXT("Conflicting object form cannot apply"),ApplyButton->IsEnabled());
            Test->TestEqual(TEXT("Conflict retains local text"),Text(TEXT("GeometryEditValue0")),FString(TEXT("1.5")));
            Capture(TEXT("conflicting-transform.png"));Press(TEXT("RevertGeometryEdits"));Next();break;
        }
        case 13:
            Test->TestEqual(TEXT("Revert adopts the external position"),Text(TEXT("GeometryEditValue1")),FString(TEXT("0.5")));
            Press(TEXT("GeometryUndo"));Next();break;
        case 14:
            if(!Scene->HasGeometryPreview())return false;
            Press(TEXT("GeometryUndo"));Next();break;
        case 15:
            if(!Scene->HasGeometryPreview())return false;
            Test->TestEqual(TEXT("A single edit undo restores the original name"),M.Project.Draft.Geometry[0].Name,First.Name);
            Test->TestTrue(TEXT("Undo restores the original transform"),M.Project.Draft.Geometry[0].Translation==First.Translation&&M.Project.Draft.Geometry[0].Rotation==First.Rotation&&M.Project.Draft.Geometry[0].Scale==First.Scale);
            Press(TEXT("GeometryRedo"));Next();break;
        case 16:
            if(!Scene->HasGeometryPreview())return false;
            VerifyIsolation();Saved=StudioCaseIO::Serialize(M.Project.Draft);
            Test->TestTrue(TEXT("Save applied object edits"),M.SaveProject(Work/TEXT("edited.lbms")));
            M.NewProject(TEXT("Before geometry reopen"));Test->TestTrue(TEXT("Reopen edited case"),M.RequestProjectOpen(Work/TEXT("edited.lbms")));Next();break;
        case 17:
            Test->TestEqual(TEXT("Complete case reopens exactly"),StudioCaseIO::Serialize(M.Project.Draft),Saved);
            Press(TEXT("Workspace2"));Next();break;
        case 18:
            if(!bReopenedSelection)
            {Press(FName(*(TEXT("GeometryRow_")+First.Id.ToString())));bReopenedSelection=true;Changed=GFrameCounter;return false;}
            if(!Scene->HasGeometryPreview())return false;
            Test->TestEqual(TEXT("Reopened form shows the applied name"),Text(TEXT("GeometryEditName")),Applied.Name);
            Test->TestEqual(TEXT("Reopened form shows applied scale"),Text(TEXT("GeometryEditValue8")),FString(TEXT("2")));
            Capture(TEXT("reopened-transform.png"));Type(TEXT("GeometryEditName"),TEXT("Uncommitted removal draft"));
            M.EditCase(TEXT("Remove object while editing"),[this](auto& Case){Case.Geometry.RemoveAll([this](const auto& Asset){return Asset.Id==First.Id;});});Next();break;
        case 19:SaveShortcut();Next();break;
        case 20:
        {
            Test->TestTrue(TEXT("Removed object's draft names its available save recovery"),M.Notice.Contains(TEXT("Discard the removed object's edits")));
            const auto Discard=FindTag(TEXT("DiscardRemovedGeometryDraft"));if(!Discard)return true;
            Test->TestTrue(TEXT("Save focuses the enabled removed-object recovery action"),Discard->IsEnabled()&&App.GetKeyboardFocusedWidget()==Discard);
            Capture(TEXT("removed-object-draft.png"));Press(TEXT("DiscardRemovedGeometryDraft"));Next();break;
        }
        case 21:
            Test->TestFalse(TEXT("Explicit discard resolves the removed-object save guard"),M.Notice.Contains(TEXT("Discard the removed object's edits")));
            Test->TestTrue(TEXT("Discard does not resurrect the removed object"),M.Project.Draft.Geometry.Num()==1&&M.Project.Draft.Geometry[0].Id==SecondId);
            Test->TestTrue(TEXT("Restore prior session"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 22:M.Navigate(PreviousWorkspace);return true;
        }
        return false;
    }
private:
    void Next(){++Phase;Changed=GFrameCounter;}
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& Widget,FName Tag)
    {
        if(!Widget->GetVisibility().IsVisible())return {};
        if(Widget->GetTag()==Tag)return Widget;
        auto* Children=Widget->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Match=Find(Children->GetChildAt(I),Tag))return Match;
        return {};
    }
    TSharedPtr<SWidget> FindTag(FName Tag)
    {
        TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& Window:Windows)if(auto Match=Find(Window,Tag))return Match;
        Test->AddError(TEXT("Missing geometry editor control: ")+Tag.ToString());return {};
    }
    void Press(FName Tag)
    {
        const auto Widget=FindTag(Tag);if(!Widget)return;
        if(!Test->TestTrue(TEXT("Geometry control is enabled"),Widget->IsEnabled()))return;
        auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Widget,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
    }
    FString Text(FName Tag)
    {const auto Widget=FindTag(Tag);return Widget?StaticCastSharedPtr<SEditableTextBox>(Widget)->GetText().ToString():FString();}
    void Type(FName Tag,const FString& Value)
    {
        const auto Widget=FindTag(Tag);if(!Widget)return;auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Widget,EFocusCause::Navigation);
        const FModifierKeysState All(false,false,true,false,false,false,false,false,false);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::A,All,0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::A,All,0,false,0,0));
        for(TCHAR Character:Value)App.ProcessKeyCharEvent(FCharacterEvent(Character,FModifierKeysState(),0,false));
    }
    void SaveShortcut()
    {
        auto& App=FSlateApplication::Get();const FModifierKeysState Command(false,false,false,false,false,false,true,false,false);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::S,Command,0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::S,Command,0,false,0,0));
    }
    void VerifyIsolation()
    {
        const auto& M=*Scene->Model;
        Test->TestEqual(TEXT("Recorded frame remains selected"),M.SelectedFrame,Frame);
        Test->TestEqual(TEXT("Object editing does not request computed CFD"),M.RenderIntentRevision,RenderRevision);
        Test->TestTrue(TEXT("Saved Solve camera remains exact"),StudioView::CameraEquals(M.Project.Camera,Camera));
        Test->TestEqual(TEXT("Frozen run configuration stays unchanged"),StudioCaseIO::Serialize(*M.Project.Runs.Last().GetConfiguration()),Frozen);
    }
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture native Geometry editor"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Save Geometry evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;
    FString Root,Work,Before,Frozen,Saved;FStudioGeometryAsset First,Applied;FGuid SecondId;
    FStudioCameraState Camera;int32 Frame=0,Phase=0;uint64 RenderRevision=0,Changed=0;
    double Started=0,LastActivation=0;bool bTooltips=true,bCapturedTooltips=false,bSaveRedirectChecked=false,bReopenedSelection=false;
    EStudioWorkspace PreviousWorkspace=EStudioWorkspace::Solve;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioGeometryEditUI,"Studio.GeometryEditUI.ControlsDraftsAndPersistence",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioGeometryEditUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioGeometryEditUICommand(this));return true;}
#endif
