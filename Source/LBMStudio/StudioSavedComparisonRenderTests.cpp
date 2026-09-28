#include "StudioScene.h"
#include "StudioSavedComparison.h"
#include "StudioFileDialog.h"
#include "StudioAuthoringTestCapture.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioSavedComparisonUICommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioSavedComparisonUICommand(FAutomationTestBase* In):Test(In){}
    ~FStudioSavedComparisonUICommand(){if(bCaptured&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>180){Test->AddError(FString::Printf(TEXT("Saved comparison UI timed out at phase %d"),Phase));return true;}
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
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/SavedComparisonUI");IFileManager::Get().MakeDirectory(*Root,true);
            Work=Root/FGuid::NewGuid().ToString();FString Error;
            Test->TestTrue(TEXT("Preserve prior project"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error));
            bTooltips=App.GetAllowTooltips();bCaptured=true;App.SetAllowTooltips(false);StudioAuthoringTestCapture::DismissTooltips();
            M.NewProject(TEXT("Saved wing comparisons"));M.ReviewRecordedFrame(420);Next();break;
        }
        case 1:if(!Scene->HasCurrentFrame())return false;Baseline=SolveState();Press(TEXT("Workspace9"));Next();break;
        case 2:if(!Enabled(TEXT("ResultsCompare")))return false;Press(TEXT("ResultsCompare"));Press(TEXT("CompareSavedMenu"));Next();break;
        case 3:
            Test->TestTrue(TEXT("Empty saved library has concrete next action"),HasText(TEXT("No saved comparisons.")));
            Capture(TEXT("empty-library.png"));App.DismissAllMenus();Press(TEXT("CompareSourceB"));Next();break;
        case 4:Press(TEXT("CompareB_MeshGraphNets_Airfoil_test010"));Next();break;
        case 5:if(!Enabled(TEXT("CompareSourceB")))return false;Press(TEXT("CompareAlignment"));Next();break;
        case 6:Press(TEXT("CompareAlignment1"));Press(TEXT("CompareScalar"));Next();break;
        case 7:Press(TEXT("CompareScalar_pressure"));Press(TEXT("CompareApply"));Next();break;
        case 8:if(!PairReady())return false;Press(TEXT("CompareCameraA"));Next();break;
        case 9:Type(TEXT("CompareCamera0Axis0"),TEXT("-0.125"));App.DismissAllMenus();Press(TEXT("CompareCameraB"));Next();break;
        case 10:Press(TEXT("CompareOrthoB"));App.DismissAllMenus();Press(TEXT("CompareCommonRange"));Next();break;
        case 11:if(!PairReady())return false;Press(TEXT("CompareSaveMenu"));Next();break;
        case 12:Capture(TEXT("save-dialog.png"));Type(TEXT("CompareSaveName"),TEXT("Wake camera comparison"));Next();break;
        case 13:
            if(!Test->TestEqual(TEXT("Saved through native input"),M.Project.Comparisons.Num(),1))return true;
            Saved=M.Project.Comparisons[0];SavedId=Saved.Id;
            Test->TestTrue(TEXT("Both exact cameras and source ranges captured"),StudioView::CameraEquals(Saved.Primary.Camera,Side(false)->SavedCameraState())&&
                StudioView::CameraEquals(Saved.Secondary.Camera,Side(true)->SavedCameraState())&&!Saved.bSharedRange&&Saved.Secondary.Camera.bOrthographic);
            CheckSolve();Press(TEXT("CompareSaveMenu"));Next();break;
        case 14:Type(TEXT("CompareSaveName"),TEXT("Wake camera comparison"));Next();break;
        case 15:
            Test->TestTrue(TEXT("Duplicate name leaves draft and field intact"),HasText(TEXT("already has that name"))&&M.Project.Comparisons.Num()==1&&PairReady());
            {const auto Input=Find(TEXT("CompareSaveName"));Test->TestTrue(TEXT("Rejected name keeps editor open and focused"),
                Input&&(Input->HasKeyboardFocus()||Input->HasFocusedDescendants()));}
            Capture(TEXT("duplicate-name.png"));App.DismissAllMenus();Next();break;
        case 16:
            if(!bDraftReopened){Press(TEXT("CompareSaveMenu"));bDraftReopened=true;Changed=GFrameCounter;return false;}
            Test->TestTrue(TEXT("Rejected name draft survives dismiss/reopen"),HasText(TEXT("Wake camera comparison")));App.DismissAllMenus();
            Type(TEXT("CompareFrame"),TEXT("422"));Press(TEXT("CompareApply"));Next();break;
        case 17:if(!PairReady())return false;Press(TEXT("CompareSavedMenu"));Next();break;
        case 18:Capture(TEXT("saved-library.png"));Press(SavedTag(TEXT("CompareUpdate_")));Next();break;
        case 19:
            Test->TestEqual(TEXT("Update retains stable ID and new original frame"),M.FindComparison(SavedId)->Primary.Identity.Ordinal,421);
            Press(TEXT("CompareSavedMenu"));Next();break;
        case 20:Press(TEXT("CompareUndo"));Next();break;
        case 21:
            Test->TestEqual(TEXT("Undo restores saved frame"),M.FindComparison(SavedId)->Primary.Identity.Ordinal,420);
            Test->TestEqual(TEXT("Collection undo leaves displayed frame alone"),Side(false)->PresentedField()->Identity()->Ordinal,421);
            Press(TEXT("CompareSavedMenu"));Next();break;
        case 22:Press(TEXT("CompareRedo"));Next();break;
        case 23:
            Test->TestEqual(TEXT("Redo restores updated saved frame"),M.FindComparison(SavedId)->Primary.Identity.Ordinal,421);
            Press(TEXT("CompareSavedMenu"));Press(TEXT("CompareUndo"));Next();break;
        case 24:Press(TEXT("CompareSavedMenu"));Press(SavedTag(TEXT("CompareRename_")));Next();break;
        case 25:Type(TEXT("CompareRenameName"),TEXT("Pressure and camera comparison across the two original wing recordings"));Next();break;
        case 26:
            Saved=*M.FindComparison(SavedId);CheckSolve();Test->TestTrue(TEXT("Persist project with saved comparison"),M.SaveProject(Work/TEXT("comparison.lbms")));
            Press(TEXT("CompareBack"));Next();break;
        case 27:
            M.NewProject(TEXT("Another document"));Test->TestTrue(TEXT("Reopen persisted comparison project"),M.RequestProjectOpen(Work/TEXT("comparison.lbms")));Next();break;
        case 28:
            Test->TestTrue(TEXT("Full saved definition survives project reopen"),M.FindComparison(SavedId)&&StudioSavedComparisons::Equals(Saved,*M.FindComparison(SavedId)));
            Test->TestFalse(TEXT("Reopen starts a new collection history"),M.CanUndoComparisons());CheckSolve();Press(TEXT("Workspace9"));Next();break;
        case 29:if(!Enabled(TEXT("ResultsCompare")))return false;Press(TEXT("ResultsCompare"));Press(TEXT("CompareSavedMenu"));Next();break;
        case 30:Press(SavedTag(TEXT("CompareOpen_")));Press(TEXT("CompareCancel"));Next();break;
        case 31:
            if(!Enabled(TEXT("CompareSavedMenu")))return false;
            Test->TestTrue(TEXT("Cancellation publishes no saved pair"),HasText(TEXT("Opening comparison cancelled"))&&!Side(false)&&!Side(true));
            Capture(TEXT("cancelled-open.png"));Press(TEXT("CompareSavedMenu"));Next();break;
        case 32:Press(SavedTag(TEXT("CompareOpen_")));Next();break;
        case 33:
            if(!PairReady())return false;
            Test->TestTrue(TEXT("Reopened original frames and cameras are exact"),Side(false)->PresentedField()->Identity()->Ordinal==Saved.Primary.Identity.Ordinal&&
                Side(true)->PresentedField()->Identity()->Ordinal==Saved.Secondary.Identity.Ordinal&&StudioView::CameraEquals(Side(false)->SavedCameraState(),Saved.Primary.Camera)&&
                StudioView::CameraEquals(Side(true)->SavedCameraState(),Saved.Secondary.Camera));
            Test->TestTrue(TEXT("Independent source color mappings reopen"),Side(false)->PresentedColorMapping().Minimum!=Side(true)->PresentedColorMapping().Minimum);
            CheckSolve();Capture(TEXT("reopened.png"));
            // Fault only the saved metadata. Published CFD files remain untouched.
            {auto Bad=Saved;Bad.Name=TEXT("Unavailable original frame");++Bad.Secondary.Identity.Frame.Index;
                Test->TestTrue(TEXT("Stage stale original-frame reference"),M.AddComparison(Bad));BrokenId=M.Project.Comparisons.Last().Id;}
            OldSceneA=Side(false);OldSceneB=Side(true);Press(TEXT("CompareSavedMenu"));Next();break;
        case 34:Press(FName(TEXT("CompareOpen_")+BrokenId.ToString()));Next();break;
        case 35:
            if(!Enabled(TEXT("CompareSavedMenu")))return false;
            Test->TestTrue(TEXT("Failed restore preserves both existing native views"),HasText(TEXT("differs from the saved comparison"))&&
                Side(false)==OldSceneA.Get()&&Side(true)==OldSceneB.Get()&&PairReady());
            Capture(TEXT("missing-frame.png"));Press(TEXT("CompareSavedMenu"));Next();break;
        case 36:Press(FName(TEXT("CompareDelete_")+BrokenId.ToString()));Next();break;
        case 37:Press(TEXT("CompareSavedMenu"));Next();break;
        case 38:Press(SavedTag(TEXT("CompareDelete_")));Next();break;
        case 39:Press(TEXT("CompareSavedMenu"));Next();break;
        case 40:
            Test->TestTrue(TEXT("Delete leaves open comparison and reversible library state"),M.Project.Comparisons.IsEmpty()&&PairReady()&&Enabled(TEXT("CompareUndo")));
            Capture(TEXT("undo-delete.png"));Press(TEXT("CompareUndo"));Next();break;
        case 41:
            Test->TestTrue(TEXT("Undo restores exact saved comparison"),M.FindComparison(SavedId)&&StudioSavedComparisons::Equals(Saved,*M.FindComparison(SavedId)));
            CheckSolve();OldModel=Side(false)->Model;OldField=Side(false)->PresentedField();Press(TEXT("CompareBack"));Next();break;
        case 42:
            Test->TestTrue(TEXT("Closing restored comparison releases models and immutable fields"),!OldModel.IsValid()&&!OldField.IsValid()&&!Side(false)&&!Side(true));
            StudioFileDialog::SetNextRecordingFolderForAutomation(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_ReaderFixture"));Press(TEXT("ResultsImport"));Next();break;
        case 43:
            Test->TestTrue(TEXT("Prepare verified3D reconstruction for saved view"),M.RequestReconstruction(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_VolumeFixture/reconstruction.json")));Next();break;
        case 44:Baseline=SolveState();Press(TEXT("ResultsCompare"));Press(TEXT("CompareSourceB"));Next();break;
        case 45:Press(FName(TEXT("CompareB_")+M.Project.Dataset));Next();break;
        case 46:if(!Enabled(TEXT("CompareSourceB")))return false;Press(TEXT("CompareAlignment"));Next();break;
        case 47:Press(TEXT("CompareAlignment1"));Press(TEXT("CompareScalar"));Next();break;
        case 48:Press(TEXT("CompareScalar_pressure"));Type(TEXT("CompareFrame"),TEXT("3"));Press(TEXT("CompareApply"));Next();break;
        case 49:if(!PairReady())return false;Press(TEXT("CompareSaveMenu"));Next();break;
        case 50:Type(TEXT("CompareSaveName"),TEXT("Cylinder volume comparison"));Next();break;
        case 51:
            if(!Test->TestEqual(TEXT("External3D comparison joins saved collection"),M.Project.Comparisons.Num(),2))return true;
            Saved=M.Project.Comparisons.Last();SavedId=Saved.Id;
            Test->TestTrue(TEXT("Both original3D and reconstruction references pinned"),Saved.Primary.Reference.IsSet()&&Saved.Secondary.Reference.IsSet()&&
                Saved.Primary.Reference->Reconstruction.IsSet()&&Saved.Secondary.Identity.Interpolation==EStudioFieldInterpolation::ReconstructedGrid);
            CheckSolve();Capture(TEXT("volume-saved.png"));Press(TEXT("CompareBack"));Next();break;
        case 52:Press(TEXT("ResultsCompare"));Press(TEXT("CompareSavedMenu"));Next();break;
        case 53:Press(SavedTag(TEXT("CompareOpen_")));Next();break;
        case 54:
            if(!PairReady())return false;
            Test->TestTrue(TEXT("Reopened real3D scenes retain exact frame and reconstruction"),Side(false)->PresentedField()->Identity()->Ordinal==2&&
                Side(true)->PresentedField()->Identity()->ReconstructionSHA256==Saved.Secondary.Identity.ReconstructionSHA256&&
                Side(false)->ResourceStats().ScalarTextureBytes>0&&Side(true)->ResourceStats().ScalarTextureBytes>0);
            CheckSolve();Capture(TEXT("volume-reopened.png"));OldModel=Side(false)->Model;OldField=Side(false)->PresentedField();Press(TEXT("CompareBack"));Next();break;
        case 55:
            Test->TestTrue(TEXT("Restored3D snapshot ownership releases"),!OldModel.IsValid()&&!OldField.IsValid());
            Test->TestTrue(TEXT("Restore prior session"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 56:return true;
        }
        return false;
    }
private:
    void Next(){++Phase;Changed=GFrameCounter;}
    FName SavedTag(const TCHAR* Prefix) const{return FName(FString(Prefix)+SavedId.ToString());}
    AStudioScene* Side(bool B)
    {for(TActorIterator<AStudioScene> It(Scene->GetWorld());It;++It)if(IsValid(*It)&&It->ActorHasTag(B?TEXT("StudioComparisonB"):TEXT("StudioComparisonA")))return *It;return nullptr;}
    bool PairReady()
    {
        auto* A=Side(false);auto* B=Side(true);
        if(!A||!B||!A->HasCurrentFrame()||!B->HasCurrentFrame()||!Find(TEXT("ComparisonViewportA")).IsValid())return false;
        // HasCurrentFrame records scene submission. Let Slate paint the new
        // render targets at their final viewport size before recording evidence.
        if(ReadyA!=A||ReadyB!=B||ReadyCaptureA!=A->GetCaptureCount()||ReadyCaptureB!=B->GetCaptureCount())
        {ReadyA=A;ReadyB=B;ReadyCaptureA=A->GetCaptureCount();ReadyCaptureB=B->GetCaptureCount();ReadyFrame=GFrameCounter;return false;}
        return GFrameCounter>=ReadyFrame+3;
    }
    bool Enabled(FName Tag){const auto W=Find(Tag);if(W)W->UpdateAllAttributes();return W&&W->IsEnabled();}
    FString SolveState(){auto P=Scene->Model->SnapshotProject();P.Comparisons.Reset();return StudioProjectIO::Serialize(P);}
    void CheckSolve(){Test->TestEqual(TEXT("Collection and restoration leave Solve state unchanged"),SolveState(),Baseline);}
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture native comparison window"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Save native comparison capture"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
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
        const auto W=Find(Tag);if(!Test->TestTrue(TEXT("Comparison widget found: ")+Tag.ToString(),W.IsValid()))return {};
        W->UpdateAllAttributes();if(!Test->TestTrue(TEXT("Comparison widget enabled: ")+Tag.ToString(),W->IsEnabled()))return {};
        const auto Target=Focusable(W.ToSharedRef());if(!Test->TestTrue(TEXT("Comparison focus target"),Target.IsValid()))return {};
        FSlateApplication::Get().SetKeyboardFocus(Target,EFocusCause::Navigation);
        Test->TestTrue(TEXT("Comparison focus reaches intended control or its native editor: ")+Tag.ToString(),
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
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene,OldSceneA,OldSceneB,ReadyA,ReadyB;
    TWeakPtr<FStudioModel> OldModel;TWeakPtr<const IStudioField,ESPMode::ThreadSafe> OldField;
    FStudioSavedComparison Saved;FGuid SavedId,BrokenId;
    FString Root,Work,Baseline;int32 Phase=0;uint64 Changed=0,ReadyCaptureA=0,ReadyCaptureB=0,ReadyFrame=0;
    double Started=0,LastActivation=0;bool bCaptured=false,bTooltips=true,bDraftReopened=false;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioSavedComparisonUI,"Studio.SavedComparisonUI.CollectionReopenAndFailureIsolation",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioSavedComparisonUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioSavedComparisonUICommand(this));return true;}
#endif
