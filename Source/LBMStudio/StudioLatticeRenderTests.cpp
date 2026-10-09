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
class FStudioLatticeControlsCommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioLatticeControlsCommand(FAutomationTestBase* InTest):Test(InTest){}
    ~FStudioLatticeControlsCommand(){if(bCapturedTooltips&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        if(!Started)Started=FPlatformTime::Seconds();
        if(Test->HasAnyErrors()){FSlateApplication::Get().DismissAllMenus();return true;}
        if(FPlatformTime::Seconds()-Started>100){Test->AddError(FString::Printf(TEXT("Lattice workflow timed out at phase %d"),Phase));return true;}
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
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/Lattice");Work=Root/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Work,true);
            FString Error;PreviousWorkspace=M.Workspace;
            Test->TestTrue(TEXT("Save prior session"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error));
            bTooltips=FSlateApplication::Get().GetAllowTooltips();bCapturedTooltips=true;FSlateApplication::Get().SetAllowTooltips(false);
            // Retired native tooltip windows must leave the capture tree before the next frame.
            StudioAuthoringTestCapture::DismissTooltips();
            M.NewProject(TEXT("Lattice authoring"));M.Pause();Camera=M.Project.Camera;Frame=M.SelectedFrame;Revision=M.RenderIntentRevision;
            const FString Path=Work/TEXT("cube.obj");
            // Original geometric fixture only. Scientific playback stays authentic.
            FFileHelper::SaveStringToFile(TEXT("# Lattice test shape; no CFD\nv -0.6 -0.6 -0.6\nv 0.6 -0.6 -0.6\nv 0.6 0.6 -0.6\nv -0.6 0.6 -0.6\nv -0.6 -0.6 0.6\nv 0.6 -0.6 0.6\nv 0.6 0.6 0.6\nv -0.6 0.6 0.6\nf 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 4 8 7 3\nf 1 5 8 4\nf 2 3 7 6\n"),*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            Test->TestTrue(TEXT("Read original shape"),M.RequestGeometryImport(Path));Next();break;
        }
        case 1:
        {
            if(M.IsReadingGeometry())return false;
            M.ImportOptions.MetersPerUnit=1.;Test->TestTrue(TEXT("Commit geometric fixture"),M.CommitGeometryImport());
            auto Domain=M.Project.Draft.Domain;Domain.Min=FVector(-1.25);Domain.Max=FVector(1.25);M.UpdateDomain(Domain);
            Press(TEXT("Workspace6"));Next();break;
        }
        case 2:
            if(M.IsReadingDomainGeometry()||!Scene->HasLatticePreview())return false;
            Test->TestTrue(TEXT("Sidebar owns Meshing route"),M.Workspace==EStudioWorkspace::Meshing);
            if(!Test->TestTrue(TEXT("Original geometry is verified"),M.DomainGeometry&&M.DomainGeometry->Complete()))return true;
            Test->TestFalse(TEXT("Authoring preview is not recorded CFD"),Scene->HasPresentedFrame());Capture(TEXT("domain.png"));
            if(!bCountsTyped)
            {
                for(int32 I=0;I<3;++I)Type(FName(*FString::Printf(TEXT("LatticeValue%d"),I)),TEXT("5"),false);
                bCountsTyped=true;ChangedFrame=GFrameCounter;return false;
            }
            Press(TEXT("LatticeApply"));Next();break;
        case 3:
            Test->TestEqual(TEXT("Counts apply through actual Slate fields"),M.Project.Draft.Setup.LatticeResolution,FIntVector(5));
            Type(TEXT("LatticeValue0"),TEXT("0"),true);Next();break;
        case 4:
            Test->TestEqual(TEXT("Invalid draft cannot mutate case"),M.Project.Draft.Setup.LatticeResolution,FIntVector(5));Capture(TEXT("invalid.png"));
            Press(TEXT("Workspace7"));Next();break;
        case 5:
            if(!Scene->HasCurrentFrame())return false;Press(TEXT("Workspace6"));Next();break;
        case 6:
            if(!Scene->HasLatticePreview())return false;
            Test->TestEqual(TEXT("Invalid exact text survives navigation"),Text(TEXT("LatticeValue0")),FString(TEXT("0")));
            SaveShortcut();Next();break;
        case 7:
            if(!bDraftReverted)
            {
                Test->TestTrue(TEXT("Save protects lattice draft before showing a file dialog"),M.Notice.Contains(TEXT("Apply or revert")));
                Press(TEXT("LatticeRevert"));bDraftReverted=true;ChangedFrame=GFrameCounter;return false;
            }
            Test->TestFalse(TEXT("Reverting counts resolves the footer save warning"),M.Notice.Contains(TEXT("Apply or revert")));
            Press(TEXT("LatticeAxis2"));Press(TEXT("LatticePreview"));Next();break;
        case 8:
            if(M.IsBuildingLatticePreview()||!Scene->HasLatticePreview())return false;
            if(!Test->TestTrue(TEXT("Current layer preview available"),M.LatticePreview&&M.LatticePreview->Complete()))return true;
            Test->TestEqual(TEXT("Every original layer cell retained"),M.LatticePreview->Samples.Num(),25);
            Test->TestEqual(TEXT("Layer surface occupancy is exact"),M.LatticePreview->Surface,8);
            Test->TestEqual(TEXT("Layer closed interior occupancy is exact"),M.LatticePreview->Inside,1);
            Test->TestTrue(TEXT("Read actual layer pixels"),Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(Before));
            Capture(TEXT("layer.png"));Scene->Orbit(31,-12);Next();break;
        case 9:
        {
            if(!Scene->HasLatticePreview())return false;TArray<FColor> After;Scene->GetRenderTarget()->GameThread_GetRenderTargetResource()->ReadPixels(After);
            Test->TestTrue(TEXT("Camera changes rendered perspective"),Before!=After);Before.Reset();Capture(TEXT("orbit.png"));Press(TEXT("LatticeProjection"));Next();break;
        }
        case 10:
            if(!Scene->HasLatticePreview())return false;
            Test->TestTrue(TEXT("Projection control changes authoring camera"),Scene->CameraState().bOrthographic);Capture(TEXT("orthographic.png"));
            if(!bPreviewRestarted){Press(TEXT("LatticePreview"));bPreviewRestarted=true;ChangedFrame=GFrameCounter;return false;}
            Press(TEXT("LatticeCancel"));Next();break;
        case 11:
            if(M.IsBuildingLatticePreview())return false;
            Test->TestFalse(TEXT("Native cancel or completed-preview clearing removes cells"),M.LatticePreview.IsValid());
            Press(TEXT("LatticeAxis3"));Press(TEXT("LatticePreview"));Next();break;
        case 12:
            if(M.IsBuildingLatticePreview()||!Scene->HasLatticePreview())return false;
            if(!Test->TestTrue(TEXT("Whole preview available"),M.LatticePreview&&M.LatticePreview->Complete()))return true;
            Test->TestEqual(TEXT("All original cells classified"),M.LatticePreview->Samples.Num(),125);
            Test->TestEqual(TEXT("Whole closed surface occupancy"),M.LatticePreview->Surface,26);
            Test->TestEqual(TEXT("Whole closed interior occupancy"),M.LatticePreview->Inside,1);
            Test->TestEqual(TEXT("Whole outside occupancy"),M.LatticePreview->Outside,98);Capture(TEXT("whole.png"));
            Test->TestTrue(TEXT("Authoring camera preserves saved flow camera"),StudioView::CameraEquals(Camera,M.Project.Camera));
            Test->TestEqual(TEXT("Preview preserves source frame"),M.SelectedFrame,Frame);Test->TestEqual(TEXT("Preview does not request CFD"),M.RenderIntentRevision,Revision);
            Press(TEXT("LatticeUndo"));Next();break;
        case 13:
            Test->TestTrue(TEXT("Counts are undoable"),M.Project.Draft.Setup.LatticeResolution!=FIntVector(5));
            Test->TestFalse(TEXT("Undo invalidates old cells"),M.LatticePreview.IsValid());Press(TEXT("LatticeRedo"));Next();break;
        case 14:
            Test->TestEqual(TEXT("Counts redo exactly"),M.Project.Draft.Setup.LatticeResolution,FIntVector(5));
            Test->TestTrue(TEXT("Save authoring project"),M.SaveProject(Work/TEXT("lattice.lbms")));Saved=StudioCaseIO::Serialize(M.Project.Draft);
            M.NewProject(TEXT("Empty lattice"));M.Navigate(EStudioWorkspace::Meshing);Next();break;
        case 15:
            if(M.IsReadingDomainGeometry()||!Scene->HasLatticePreview())return false;
            Test->TestFalse(TEXT("New project retains no earlier occupancy"),M.LatticePreview.IsValid());Capture(TEXT("empty.png"));
            Test->TestTrue(TEXT("Reopen authored lattice"),M.RequestProjectOpen(Work/TEXT("lattice.lbms")));Next();break;
        case 16:
            Test->TestEqual(TEXT("Exact authoring case reopens"),StudioCaseIO::Serialize(M.Project.Draft),Saved);
            M.Navigate(EStudioWorkspace::Meshing);Next();break;
        case 17:
            if(M.IsReadingDomainGeometry()||!Scene->HasLatticePreview())return false;
            Test->TestFalse(TEXT("Preview is recomputed from source after reopen"),M.LatticePreview.IsValid());Capture(TEXT("reopened.png"));
            Test->TestTrue(TEXT("Restore prior session"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 18:M.Navigate(PreviousWorkspace);return true;
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
        Test->AddError(TEXT("Missing lattice control: ")+Tag.ToString());return {};
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
        if(!Test->TestTrue(TEXT("Capture native lattice workspace"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Retain lattice evidence"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;FString Root,Work,Saved;
    TArray<FColor> Before;FStudioCameraState Camera;EStudioWorkspace PreviousWorkspace=EStudioWorkspace::Solve;
    bool bTooltips=true,bCapturedTooltips=false,bCountsTyped=false,bPreviewRestarted=false,bDraftReverted=false;int32 Phase=0,Frame=0;uint64 ChangedFrame=0,Revision=0;double Started=0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioLatticeControlsTest,"Studio.LatticeUI.ControlsOccupancyCancellationAndReopen",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioLatticeControlsTest::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioLatticeControlsCommand(this));return true;}
#endif
