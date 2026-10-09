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
#include "Components/SceneCaptureComponent2D.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
class FStudioComparisonUICommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioComparisonUICommand(FAutomationTestBase* In):Test(In){}
    ~FStudioComparisonUICommand(){if(bCaptured&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>180){Test->AddError(FString::Printf(TEXT("Comparison UI timed out at phase %d"),Phase));return true;}
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
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/ComparisonUI");IFileManager::Get().MakeDirectory(*Root,true);
            Work=Root/FGuid::NewGuid().ToString();FString Error;
            Test->TestTrue(TEXT("Preserve prior project"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error));
            bTooltips=App.GetAllowTooltips();bCaptured=true;App.SetAllowTooltips(false);StudioAuthoringTestCapture::DismissTooltips();
            M.NewProject(TEXT("Wing · comparison"));M.ReviewRecordedFrame(420);Next();break;
        }
        case 1:
            if(!Scene->HasCurrentFrame())return false;
            Baseline=StudioProjectIO::Serialize(M.SnapshotProject());Press(TEXT("Workspace9"));Next();break;
        case 2:if(!Enabled(TEXT("ResultsCompare")))return false;Press(TEXT("ResultsCompare"));Next();break;
        case 3:
            Test->TestFalse(TEXT("Alignment and second source must be chosen explicitly"),Enabled(TEXT("CompareApply")));
            Capture(TEXT("choose-sources.png"));Press(TEXT("CompareSourceB"));Next();break;
        case 4:Press(TEXT("CompareB_MeshGraphNets_Airfoil_test010"));Next();break;
        case 5:if(!Enabled(TEXT("CompareSourceB")))return false;Press(TEXT("CompareAlignment"));Next();break;
        case 6:Capture(TEXT("alignment-menu.png"));Press(TEXT("CompareAlignment1"));Press(TEXT("CompareScalar"));Next();break;
        case 7:Press(TEXT("CompareScalar_pressure"));Type(TEXT("CompareFrame"),TEXT("421"));Press(TEXT("CompareApply"));Next();break;
        case 8:
        {
            if(!PairReady())return false;
            auto* A=Side(false);auto* B=Side(true);
            Test->TestTrue(TEXT("Both views present exact original frame 420"),A->PresentedField()->Identity()->Ordinal==420&&B->PresentedField()->Identity()->Ordinal==420&&
                A->PresentedDatasetId()!=B->PresentedDatasetId()&&A->PresentedFrame().Time==.084&&B->PresentedFrame().Time==.084);
            Test->TestTrue(TEXT("Comparison uses pressure with shared physical color range"),A->PresentedScalar().Id==TEXT("pressure")&&B->PresentedScalar().Id==TEXT("pressure")&&
                A->PresentedColorMapping().Minimum==B->PresentedColorMapping().Minimum&&A->PresentedColorMapping().Maximum==B->PresentedColorMapping().Maximum);
            for(auto* S:{A,B})
            {
                const auto* C=S->FindComponentByClass<USceneCaptureComponent2D>();
                Test->TestTrue(TEXT("Capture is limited to its own scene components"),C&&C->PrimitiveRenderMode==ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList&&!C->ShowOnlyComponents.IsEmpty());
                if(C)for(const auto& P:C->ShowOnlyComponents)Test->TestTrue(TEXT("No other view primitive is visible"),P.IsValid()&&P->GetOwner()==S);
            }
            CheckSolve();CameraB=B->SavedCameraState();CaptureB=B->GetCaptureCount();TargetX=A->CameraPosition().X+.25;
            Capture(TEXT("two-recordings.png"));Press(TEXT("CompareCameraA"));Next();break;
        }
        case 9:Capture(TEXT("camera-menu.png"));Type(TEXT("CompareCamera0Axis0"),FString::Printf(TEXT("%.9f"),TargetX));FSlateApplication::Get().DismissAllMenus();Next();break;
        case 10:
            if(!PairReady())return false;
            Test->TestTrue(TEXT("Numeric camera placement changes only A"),FMath::IsNearlyEqual(Side(false)->PresentedCamera().Position.X,TargetX,1.e-5)&&StudioView::CameraEquals(Side(true)->SavedCameraState(),CameraB));
            Test->TestEqual(TEXT("Moving A does not capture B"),Side(true)->GetCaptureCount(),CaptureB);
            CheckSolve();Capture(TEXT("independent-camera.png"));Focus(TEXT("ComparisonViewportA"));Key(EKeys::F);Next();break;
        case 11:
            if(!PairReady())return false;
            Test->TestTrue(TEXT("Shared viewport camera shortcut fits A"),!FMath::IsNearlyEqual(Side(false)->CameraPosition().X,TargetX,1.e-5));
            Press(TEXT("CompareCommonRange"));Next();break;
        case 12:
            if(!PairReady())return false;
            Test->TestTrue(TEXT("Source ranges use each supplied scalar range"),!Side(false)->PresentedColorMapping().bManualRange&&!Side(true)->PresentedColorMapping().bManualRange);
            Capture(TEXT("source-ranges.png"));CaptureA=Side(false)->GetCaptureCount();CaptureB=Side(true)->GetCaptureCount();Press(TEXT("Workspace0"));Next();break;
        case 13:
            Test->TestTrue(TEXT("Hidden comparison drains its geometry workers"),Side(false)->ResourceStats().Workers==0&&Side(true)->ResourceStats().Workers==0);
            Test->TestTrue(TEXT("Hidden comparison submits no captures"),CaptureA==Side(false)->GetCaptureCount()&&CaptureB==Side(true)->GetCaptureCount());
            Press(TEXT("Workspace9"));Next();break;
        case 14:
            if(!PairReady())return false;
            CheckSolve();Type(TEXT("CompareFrame"),TEXT("422"));Next();break;
        case 15:
            Test->TestTrue(TEXT("Changing frame hides the obsolete pair"),!Side(false)&&!Side(true)&&!Find(TEXT("ComparisonViewportA")));
            Press(TEXT("CompareApply"));Press(TEXT("CompareCancel"));Next();break;
        case 16:
            if(!Enabled(TEXT("CompareApply")))return false;
            Test->TestTrue(TEXT("Cancellation never publishes a completed pair"),!Side(false)&&!Side(true)&&HasText(TEXT("Comparison cancelled.")));
            Capture(TEXT("cancelled.png"));Press(TEXT("CompareApply"));Next();break;
        case 17:
            if(!PairReady())return false;
            OldModel=Side(false)->Model;OldField=Side(false)->PresentedField();RetiredScene.Reset(Side(false));Press(TEXT("CompareBack"));Next();break;
        case 18:
            Test->TestTrue(TEXT("Closing comparison destroys both scenes and releases snapshots before GC"),!Side(false)&&!Side(true)&&!OldModel.IsValid()&&!OldField.IsValid());
            CheckRetired();
            CheckSolve();StudioFileDialog::SetNextRecordingFolderForAutomation(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture"));Press(TEXT("ResultsImport"));Next();break;
        case 19:Baseline=StudioProjectIO::Serialize(M.SnapshotProject());Press(TEXT("ResultsCompare"));Press(TEXT("CompareSourceB"));Next();break;
        case 20:Press(TEXT("CompareB_MeshGraphNets_Airfoil_test010"));Next();break;
        case 21:if(!Enabled(TEXT("CompareSourceB")))return false;Press(TEXT("CompareAlignment"));Next();break;
        case 22:Press(TEXT("CompareAlignment1"));Press(TEXT("CompareScalar"));Next();break;
        case 23:Press(TEXT("CompareScalar_pressure"));Press(TEXT("CompareApply"));Next();break;
        case 24:
            if(!Enabled(TEXT("CompareApply")))return false;
            Test->TestTrue(TEXT("Disjoint recorded time ranges have no rendered pair"),HasText(TEXT("outside the second recording's aligned time range"))&&!Side(false)&&!Side(true));
            Capture(TEXT("no-overlap.png"));Press(TEXT("CompareAlignment"));Next();break;
        case 25:Press(TEXT("CompareAlignment2"));Press(TEXT("CompareApply"));Next();break;
        case 26:
            if(!PairReady())return false;
            Test->TestTrue(TEXT("Elapsed alignment retains actual nonzero source time and sparse step"),Side(false)->PresentedFrame().Time==2.5025&&Side(false)->PresentedFrame().Index==1001&&Side(true)->PresentedFrame().Time==0);
            Test->TestTrue(TEXT("Point output remains original points"),Side(false)->PresentedField()->Identity()->Interpolation==EStudioFieldInterpolation::None);
            CheckSolve();Capture(TEXT("elapsed-alignment.png"));Press(TEXT("CompareAlignment"));Next();break;
        case 27:Press(TEXT("CompareAlignment3"));Type(TEXT("CompareOffset"),TEXT("2.5025"));Press(TEXT("CompareApply"));Next();break;
        case 28:
            if(!PairReady())return false;
            Test->TestTrue(TEXT("Manual B offset keeps original source timestamps"),Side(false)->PresentedFrame().Time==2.5025&&Side(true)->PresentedFrame().Time==0);
            Capture(TEXT("manual-offset.png"));Press(TEXT("CompareMatch"));Next();break;
        case 29:Press(TEXT("CompareNearest"));Type(TEXT("CompareOffset"),TEXT("2.5024"));Type(TEXT("CompareTolerance"),TEXT("0.00011"));Press(TEXT("CompareApply"));Next();break;
        case 30:
            if(!PairReady())return false;
            Test->TestTrue(TEXT("Nearest match reports its signed time difference"),HasText(TEXT("B − A"))&&Side(false)->PresentedFrame().Time!=Side(true)->PresentedFrame().Time);
            Capture(TEXT("nearest-tolerance.png"));Press(TEXT("CompareBack"));Next();break;
        case 31:
            CheckSolve();StudioFileDialog::SetNextRecordingFolderForAutomation(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_ReaderFixture"));Press(TEXT("ResultsImport"));Next();break;
        case 32:
            Test->TestTrue(TEXT("Prepare verified true 3D volume mapping"),M.RequestReconstruction(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_VolumeFixture/reconstruction.json")));Next();break;
        case 33:
            Baseline=StudioProjectIO::Serialize(M.SnapshotProject());Press(TEXT("ResultsCompare"));Press(TEXT("CompareSourceB"));Next();break;
        case 34:Press(FName(*(TEXT("CompareB_")+M.Project.Dataset)));Next();break;
        case 35:if(!Enabled(TEXT("CompareSourceB")))return false;Press(TEXT("CompareAlignment"));Next();break;
        case 36:Press(TEXT("CompareAlignment1"));Press(TEXT("CompareScalar"));Next();break;
        case 37:Press(TEXT("CompareScalar_pressure"));Type(TEXT("CompareFrame"),TEXT("3"));Press(TEXT("CompareApply"));Next();break;
        case 38:
            if(!PairReady())return false;
            Test->TestTrue(TEXT("Both native views render original 3D volume fields"),Side(false)->PresentedField()->Identity()->SpatialDimensions==3&&Side(true)->PresentedField()->Identity()->SpatialDimensions==3&&
                Side(false)->PresentedField()->Identity()->Interpolation==EStudioFieldInterpolation::ReconstructedGrid&&Side(true)->PresentedField()->Identity()->Interpolation==EStudioFieldInterpolation::ReconstructedGrid&&
                Side(false)->ResourceStats().ScalarTextureBytes>0&&Side(true)->ResourceStats().ScalarTextureBytes>0);
            CheckSolve();Capture(TEXT("three-dimensional.png"));Press(TEXT("CompareCameraB"));Next();break;
        case 39:Press(TEXT("CompareOrthoB"));FSlateApplication::Get().DismissAllMenus();Next();break;
        case 40:
            if(!PairReady())return false;
            Test->TestTrue(TEXT("Projection is independent per view"),Side(true)->PresentedCamera().bOrthographic&&!Side(false)->PresentedCamera().bOrthographic);
            Test->TestTrue(TEXT("Projection keeps the fitted physical span"),FMath::IsNearlyEqual(Side(true)->PresentedCamera().OrthoWidth,
                2*Side(true)->PresentedCamera().OrbitDistance*FMath::Tan(FMath::DegreesToRadians(Side(true)->PresentedCamera().FieldOfView*.5)),1.e-5));
            CheckSolve();Capture(TEXT("independent-projection.png"));
            OldModel=Side(true)->Model;OldField=Side(true)->PresentedField();RetiredScene.Reset(Side(true));Press(TEXT("CompareBack"));Next();break;
        case 41:
            Test->TestTrue(TEXT("3D view models and fields release before GC"),!Side(false)&&!Side(true)&&!OldModel.IsValid()&&!OldField.IsValid());
            CheckRetired();
            Test->TestTrue(TEXT("Restore original session"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 42:return true;
        }
        return false;
    }
private:
    void Next(){++Phase;Changed=GFrameCounter;}
    AStudioScene* Side(bool B)
    {for(TActorIterator<AStudioScene> It(Scene->GetWorld());It;++It)if(IsValid(*It)&&It->ActorHasTag(B?TEXT("StudioComparisonB"):TEXT("StudioComparisonA")))return *It;return nullptr;}
    bool PairReady(){auto* A=Side(false);auto* B=Side(true);return A&&B&&A->HasCurrentFrame()&&B->HasCurrentFrame()&&Find(TEXT("ComparisonViewportA")).IsValid();}
    bool Enabled(FName Tag){const auto W=Find(Tag);if(W)W->UpdateAllAttributes();return W&&W->IsEnabled();}
    void CheckSolve(){Test->TestEqual(TEXT("Comparison leaves Solve project, camera, fields and cursor unchanged"),StudioProjectIO::Serialize(Scene->Model->SnapshotProject()),Baseline);}
    void CheckRetired()
    {
        const auto S=RetiredScene->ResourceStats();
        Test->TestTrue(TEXT("Retired view has no worker, mesh, render target or scalar texture before GC"),S.Workers==0&&S.MeshBytes==0&&S.RenderTargetBytes==0&&S.ScalarTextureBytes==0);
        RetiredScene.Reset();
    }
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
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;
    TStrongObjectPtr<AStudioScene> RetiredScene;
    TWeakPtr<FStudioModel> OldModel;TWeakPtr<const IStudioField,ESPMode::ThreadSafe> OldField;
    FStudioCameraState CameraB;uint64 CaptureA=0,CaptureB=0;
    FString Root,Work,Baseline;int32 Phase=0;uint64 Changed=0;
    double Started=0,LastActivation=0,TargetX=0;bool bCaptured=false,bTooltips=true;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioComparisonUI,"Studio.ComparisonUI.OriginalFramesCamerasAndLifecycle",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioComparisonUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioComparisonUICommand(this));return true;}
#endif
