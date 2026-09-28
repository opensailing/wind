#include "StudioScene.h"
#include "StudioAuthoringTestCapture.h"
#include "StudioDomain.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioDomainControlsCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioDomainControlsCommand(FAutomationTestBase* InTest):Test(InTest){}
    ~FStudioDomainControlsCommand(){if(bCapturedTooltips&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        if(!Started)Started=FPlatformTime::Seconds();
        if(Test->HasAnyErrors()){FSlateApplication::Get().DismissAllMenus();return true;}
        if(FPlatformTime::Seconds()-Started>100){Test->AddError(FString::Printf(TEXT("Domain workflow timed out at phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& Context:GEngine->GetWorldContexts())if(Context.World()&&Context.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(Context.World());It;++It)Scene=*It;
        if(!Scene.IsValid()||!Scene->Model||GFrameCounter-ChangedFrame<4)return false;
        auto& M=*Scene->Model;
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending())return false;
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/Domain");Work=Root/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Work,true);
            FString Error;PreviousWorkspace=M.Workspace;
            Test->TestTrue(TEXT("Save prior session"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error));
            bTooltips=FSlateApplication::Get().GetAllowTooltips();bCapturedTooltips=true;FSlateApplication::Get().SetAllowTooltips(false);
            // Retired native tooltip windows must leave the capture tree before the next frame.
            StudioAuthoringTestCapture::DismissTooltips();
            M.NewProject(TEXT("Wing domain"));M.Pause();Camera=M.Project.Camera;Frame=M.SelectedFrame;Revision=M.RenderIntentRevision;
            const FString Path=Work/TEXT("domain-fixture.obj");
            // Geometry fixture only. CFD remains the application's authentic recording.
            FFileHelper::SaveStringToFile(TEXT("# Domain test shape; no CFD\nv 0 0 0\nv 2 0 0\nv 0 1 0\nv 0 0 1\nf 1 3 2\nf 1 2 4\nf 2 3 4\nf 3 1 4\n"),*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            Test->TestTrue(TEXT("Read original geometry fixture"),M.RequestGeometryImport(Path));Next();break;
        }
        case 1:
            if(M.IsReadingGeometry())return false;
            M.ImportOptions.MetersPerUnit=1.;Test->TestTrue(TEXT("Commit explicit case geometry"),M.CommitGeometryImport());
            Faces=M.Project.Draft.Domain.Faces;Press(TEXT("Workspace3"));Next();break;
        case 2:
            if(M.IsReadingDomainGeometry()||!Scene->HasDomainPreview())return false;
            Test->TestTrue(TEXT("Sidebar opens domain"),M.Workspace==EStudioWorkspace::Domain);
            if(!Test->TestTrue(TEXT("Original geometry verified"),M.DomainGeometry&&M.DomainGeometry->Complete()))return true;
            Test->TestFalse(TEXT("Outside object identified"),StudioDomain::Contains(M.Project.Draft.Domain,M.DomainGeometry->Bounds));
            Test->TestFalse(TEXT("Domain preview is not labelled as recorded CFD"),Scene->HasPresentedFrame());
            if(!bBoundsTyped)
            {
                Capture(TEXT("outside.png"));
                Type(TEXT("DomainValue0"),TEXT("-2"),false);Type(TEXT("DomainValue1"),TEXT("4"),false);Type(TEXT("DomainValue6"),TEXT("Inlet"),false);
                bBoundsTyped=true;ChangedFrame=GFrameCounter;return false;
            }
            Capture(TEXT("bounds-draft.png"));Press(TEXT("DomainApply"));Next();break;
        case 3:
            if(!Scene->HasDomainPreview())return false;
            Test->TestEqual(TEXT("Minimum applied through Slate"),M.Project.Draft.Domain.Min.X,-2.);
            Test->TestEqual(TEXT("Face name applied through Slate"),M.Project.Draft.Domain.FaceNames[0],FString(TEXT("Inlet")));
            Test->TestTrue(TEXT("Face identity survives bounds/name edit"),M.Project.Draft.Domain.Faces==Faces);
            Test->TestTrue(TEXT("Applied box contains case geometry"),StudioDomain::Contains(M.Project.Draft.Domain,M.DomainGeometry->Bounds));
            Test->TestTrue(TEXT("Read actual domain pixels"),Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(BeforeFace));
            Capture(TEXT("applied.png"));Press(TEXT("DomainFace5"));Next();break;
        case 4:
        {
            if(!Scene->HasDomainPreview())return false;TArray<FColor> After;
            Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(After);
            Test->TestTrue(TEXT("Selecting a face changes rendered pixels"),BeforeFace!=After);BeforeFace.Reset();
            Test->TestEqual(TEXT("Positive Z selected"),M.SelectedDomainFace,5);Capture(TEXT("selected-face.png"));
            Scene->Orbit(25,-12);Type(TEXT("DomainValue0"),TEXT("nan"),true);Next();break;
        }
        case 5:
            Test->TestEqual(TEXT("Invalid bounds do not mutate case"),M.Project.Draft.Domain.Min.X,-2.);
            Capture(TEXT("invalid.png"));Press(TEXT("Workspace7"));Next();break;
        case 6:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestTrue(TEXT("Returning to Solve restores actual CFD"),Scene->HasPresentedFrame());Press(TEXT("Workspace3"));Next();break;
        case 7:
            if(!Scene->HasDomainPreview())return false;
            Test->TestEqual(TEXT("Invalid draft survives route change"),Text(TEXT("DomainValue0")),FString(TEXT("nan")));
            SaveShortcut();Next();break;
        case 8:
            Test->TestTrue(TEXT("Save protects unapplied draft"),M.Notice.Contains(TEXT("Apply or revert")));
            Press(TEXT("DomainRevert"));for(int32 I=12;I<18;++I)Type(FName(*FString::Printf(TEXT("DomainValue%d"),I)),TEXT("0.5"),false);
            Press(TEXT("DomainFitBounds"));Next();break;
        case 9:
            Test->TestFalse(TEXT("Reverting bounds resolves the footer save warning"),M.Notice.Contains(TEXT("Apply or revert")));
            Test->TestEqual(TEXT("Fit changes draft only until applied"),M.Project.Draft.Domain.Min.X,-2.);
            Test->TestEqual(TEXT("Fit uses all verified geometry"),Text(TEXT("DomainValue1")),FString(TEXT("2.5")));
            Capture(TEXT("fit-draft.png"));Press(TEXT("DomainApply"));Next();break;
        case 10:
            if(!Scene->HasDomainPreview())return false;
            Test->TestEqual(TEXT("Fit minimum applied"),M.Project.Draft.Domain.Min,FVector(-.5,-.5,-.5));
            Test->TestEqual(TEXT("Fit maximum applied"),M.Project.Draft.Domain.Max,FVector(2.5,1.5,1.5));
            Capture(TEXT("fitted.png"));Press(TEXT("DomainUndo"));Next();break;
        case 11:
            Test->TestEqual(TEXT("Fit is one undoable transaction"),M.Project.Draft.Domain.Min.X,-2.);Press(TEXT("DomainRedo"));Next();break;
        case 12:
        {
            Test->TestEqual(TEXT("Redo restores fit"),M.Project.Draft.Domain.Min.X,-.5);
            Type(TEXT("DomainValue0"),TEXT("-3"),false);auto External=M.Project.Draft.Domain;External.Max.X=3.;M.UpdateDomain(External);Next();break;
        }
        case 13:
            Test->TestEqual(TEXT("Conflict retains user text"),Text(TEXT("DomainValue0")),FString(TEXT("-3")));
            if(auto Apply=FindTag(TEXT("DomainApply")))Test->TestFalse(TEXT("Conflicting draft cannot apply"),Apply->IsEnabled());
            Capture(TEXT("conflict.png"));Press(TEXT("DomainRevert"));Next();break;
        case 14:
            Test->TestEqual(TEXT("Revert loads current bounds"),Text(TEXT("DomainValue1")),FString(TEXT("3")));
            Test->TestTrue(TEXT("Save authored domain"),M.SaveProject(Work/TEXT("domain.lbms")));Saved=StudioCaseIO::Serialize(M.Project.Draft);
            Test->TestTrue(TEXT("Domain camera leaves saved flow camera exact"),StudioView::CameraEquals(Camera,M.Project.Camera));
            Test->TestEqual(TEXT("Source frame retained"),M.SelectedFrame,Frame);Test->TestEqual(TEXT("No new CFD intent from domain edits"),M.RenderIntentRevision,Revision);
            M.NewProject(TEXT("Empty domain"));M.Navigate(EStudioWorkspace::Domain);Next();break;
        case 15:
            if(M.IsReadingDomainGeometry()||!Scene->HasDomainPreview())return false;
            Test->TestTrue(TEXT("Empty case has no old source"),M.DomainGeometry&&M.DomainGeometry->Objects.IsEmpty());Capture(TEXT("empty.png"));
            Test->TestTrue(TEXT("Open saved domain"),M.RequestProjectOpen(Work/TEXT("domain.lbms")));Next();break;
        case 16:
            Test->TestEqual(TEXT("Exact authored domain reopens"),StudioCaseIO::Serialize(M.Project.Draft),Saved);M.Navigate(EStudioWorkspace::Domain);Next();break;
        case 17:
            if(M.IsReadingDomainGeometry()||!Scene->HasDomainPreview())return false;
            Capture(TEXT("reopened.png"));M.CancelDomainGeometry();Next();break;
        case 18:
            Capture(TEXT("unverified.png"));Test->TestTrue(TEXT("Restore prior session"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 19:M.Navigate(PreviousWorkspace);return true;
        }
        return false;
    }
private:
    void Next(){++Phase;ChangedFrame=GFrameCounter;}
    TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& Widget,FName Tag,bool bFocusable=false)
    {
        if(!Widget->GetVisibility().IsVisible())return {};
        if((bFocusable&&Widget->SupportsKeyboardFocus()&&Widget->IsEnabled())||(!bFocusable&&Widget->GetTag()==Tag))return Widget;
        auto* Children=Widget->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Result=Find(Children->GetChildAt(I),Tag,bFocusable))return Result;
        return {};
    }
    TSharedPtr<SWidget> FindTag(FName Tag)
    {
        TArray<TSharedRef<SWindow>> Windows;FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
        for(const auto& Window:Windows)if(auto Result=Find(Window,Tag))return Result;
        Test->AddError(TEXT("Missing domain control: ")+Tag.ToString());return {};
    }
    void Enter(const TSharedPtr<SWidget>& Widget)
    {
        if(!Widget)return;auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Widget,EFocusCause::Navigation);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));
    }
    void Press(FName Tag){Enter(FindTag(Tag));}
    void Open(FName Tag){if(auto Widget=FindTag(Tag))Enter(Find(Widget.ToSharedRef(),NAME_None,true));}
    FString Text(FName Tag){auto Widget=FindTag(Tag);return Widget?StaticCastSharedPtr<SEditableTextBox>(Widget)->GetText().ToString():FString();}
    void Type(FName Tag,const FString& Value,bool bCommit)
    {
        const auto Widget=FindTag(Tag);if(!Widget)return;auto& App=FSlateApplication::Get();App.SetKeyboardFocus(Widget,EFocusCause::Navigation);
        const FModifierKeysState All(false,false,true,false,false,false,false,false,false);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::A,All,0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::A,All,0,false,0,0));
        for(TCHAR C:Value)App.ProcessKeyCharEvent(FCharacterEvent(C,FModifierKeysState(),0,false));if(bCommit)Enter(Widget);
    }
    void SaveShortcut()
    {
        auto& App=FSlateApplication::Get();const FModifierKeysState Command(false,false,false,false,false,false,true,false,false);
        App.ProcessKeyDownEvent(FKeyEvent(EKeys::S,Command,0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::S,Command,0,false,0,0));
    }
    void Capture(const TCHAR* Name)
    {
        FSlateApplication::Get().CloseToolTip();TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture native domain workspace"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Retain domain evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FString Root,Work,Saved;
    TArray<FGuid> Faces;TArray<FColor> BeforeFace;FStudioCameraState Camera;EStudioWorkspace PreviousWorkspace=EStudioWorkspace::Solve;
    bool bTooltips=true,bCapturedTooltips=false,bBoundsTyped=false;int32 Phase=0,Frame=0;uint64 ChangedFrame=0,Revision=0;double Started=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioDomainControlsTest,"Studio.Domain.ControlsPreviewDraftsAndReopen",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioDomainControlsTest::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioDomainControlsCommand(this));return true;}
#endif
