#include "StudioScene.h"
#include "StudioPipelineEvaluation.h"
#include "StudioAuthoringTestCapture.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
/** Routed Slate input on authentic published fixtures. This does not claim OS
 * mouse, file-picker or accessibility acceptance. */
class FStudioPipelineUICommand final : public IAutomationLatentCommand
{
public:
    explicit FStudioPipelineUICommand(FAutomationTestBase* In):Test(In){}
    ~FStudioPipelineUICommand(){if(bCaptured&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>180){Test->AddError(FString::Printf(TEXT("Pipeline UI timed out at phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)if(It->Model&&!It->Model->IsSnapshotView())Scene=*It;
        if(!Scene.IsValid()||GFrameCounter<Changed+8)return false;
        auto& App=FSlateApplication::Get();auto& M=*Scene->Model;
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending())return false;
        if(!App.IsActive())
        {if(Now-LastActivation>1){LastActivation=Now;FPlatformApplicationMisc::ActivateApplication();GEngine->GameViewport->GetWindow()->BringToFront(true);}Changed=GFrameCounter;return false;}
        // Menu anchors and focus paths are rebuilt by Slate after edits. Open
        // menus in their own input cycle, after the preceding mutation paints.
        if(!PendingMenu.IsNone())
        {
            const auto Tag=PendingMenu;PendingMenu=NAME_None;
            if(Focus(Tag))
            {
                const auto Anchor=StaticCastSharedPtr<SMenuAnchor>(Find(Tag));
                const bool Before=Anchor->IsOpen();Key(EKeys::Enter);
                const auto Focused=App.GetKeyboardFocusedWidget();
                MenuTrace=FString::Printf(TEXT("%s before=%d after=%d focus=%s/%s frame=%llu"),*Tag.ToString(),Before,Anchor->IsOpen(),
                    Focused?*Focused->GetTypeAsString():TEXT("none"),Focused?*Focused->GetTag().ToString():TEXT("none"),GFrameCounter);
                Test->TestTrue(TEXT("Routed menu input opens anchor: ")+MenuTrace,Anchor->IsOpen());
            }
            Changed=GFrameCounter;return false;
        }
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/PipelineUI");IFileManager::Get().MakeDirectory(*Root,true);Work=Root/FGuid::NewGuid().ToString();FString Error;
            Test->TestTrue(TEXT("Preserve prior project"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error));
            bTooltips=App.GetAllowTooltips();bCaptured=true;App.SetAllowTooltips(false);StudioAuthoringTestCapture::DismissTooltips();
            M.NewProject(TEXT("Wing · ordered analysis"));M.ReviewRecordedFrame(420);
            const FString Samples=FPaths::ProjectContentDir()/TEXT("Samples");
            auto R=StudioRecordings::Import(Samples/TEXT("NACA0018_ReaderFixture/recording.json"),0,{});
            if(!Test->TestTrue(*R.Error,R.Reference.IsSet()))return true;
            R=StudioRecordings::ImportReconstruction(*R.Reference,Samples/TEXT("NACA0018_SurfaceFixture/reconstruction.json"),0,{});
            if(!Test->TestTrue(*R.Error,R.Source&&R.Reference.IsSet()))return true;
            FixtureId=R.Reference->Id;M.Project.Recordings.Add(*R.Reference);
            Next();break;
        }
        case 1:
            if(!Scene->HasCurrentFrame())return false;
            Baseline=SolveState();Press(TEXT("Workspace10"));Next();break;
        case 2:
            Test->TestTrue(TEXT("Sidebar opens the native empty workspace"),M.Workspace==EStudioWorkspace::PostProcessing&&HasText(TEXT("Build an analysis")));
            Capture(TEXT("empty.png"));Press(TEXT("PipelineNew"));Next();break;
        case 3:Press(FName(*(TEXT("PipelineNew_")+FixtureId)));Next();break;
        case 4:
            if(!Ready())return false;
            Test->TestTrue(TEXT("New pipeline pins the authentic original frame"),M.Project.Pipelines.Num()==1&&View()->PresentedDatasetId()==FixtureId&&View()->PresentedField()->Identity()->Ordinal==0);
            CheckSolve();Capture(TEXT("field.png"));Press(TEXT("PipelineScalar"));Next();break;
        case 5:Press(TEXT("PipelineScalar_pressure"));Press(TEXT("PipelineApplyOperation"));Next();break;
        case 6:
            Test->TestEqual(TEXT("Changing an untouched scalar name updates its identity"),M.Project.Pipelines[0].Operations[0].Name,FString(TEXT("Pressure")));
            Test->TestTrue(TEXT("Applied parameters require explicit reevaluation"),!View()&&M.Project.Pipelines[0].Operations[0].Field==TEXT("pressure"));
            Press(TEXT("PipelineEvaluate"));Press(TEXT("PipelineCancel"));Next();break;
        case 7:
            if(!Enabled(TEXT("PipelineEvaluate")))return false;
            Test->TestTrue(TEXT("Cancelled evaluation publishes no stale scene"),!View()&&HasText(TEXT("Evaluation cancelled")));Press(TEXT("PipelineEvaluate"));Next();break;
        case 8:
            if(!Ready())return false;
            Test->TestTrue(TEXT("Selected pressure is the original source scalar"),View()->PresentedScalar().Id==TEXT("pressure"));Press(TEXT("PipelineCamera"));Next();break;
        case 9:
            TargetX=View()->CameraPosition().X+.025;Type(TEXT("PipelineCameraAxis0"),FString::Printf(TEXT("%.12g"),TargetX));App.DismissAllMenus();Next();break;
        case 10:
            if(!Ready())return false;
            Test->TestTrue(TEXT("Independent arbitrary camera persists with recipe"),FMath::IsNearlyEqual(View()->PresentedCamera().Position.X,TargetX,1.e-8)&&FMath::IsNearlyEqual(M.Project.Pipelines[0].Source.Camera.Position.X,TargetX,1.e-8));
            CheckSolve();Press(TEXT("PipelineFit"));Press(TEXT("PipelineAdd"));Next();break;
        case 11:Press(TEXT("PipelineAdd2"));Next();break;
        case 12:
            Type(TEXT("PipelineNumberA0"),TEXT("1000000"));Next();break;
        case 13:
            Test->TestTrue(TEXT("Invalid box stays in form and leaves recipe unchanged"),M.Project.Pipelines[0].Operations.Last().A.X!=1000000&&HasText(TEXT("1000000")));
            Focus(TEXT("PipelineNumberA0"));Key(EKeys::S,FModifierKeysState(false,false,false,false,false,false,true,false,false));Next();break;
        case 14:
            Test->TestTrue(*FString(TEXT("Project save requires resolving unapplied parameters: ")) .Append(M.Notice),M.Notice.Contains(TEXT("Apply or Revert pipeline"))&&HasText(TEXT("1000000")));
            Capture(TEXT("invalid-parameters.png"));Press(TEXT("PipelineRevertOperation"));Type(TEXT("PipelineNumberA0"),TEXT("-.15"));Type(TEXT("PipelineNumberB0"),TEXT(".3"));Press(TEXT("PipelineEvaluate"));Next();break;
        case 15:
            if(!Ready())return false;
            Test->TestTrue(TEXT("Box clip reaches evaluated physical geometry"),View()->PipelineOutput()->Vertices.ContainsByPredicate([](const auto& V){return V.PositionMeters.X<=.3;}));
            Capture(TEXT("clip.png"));Press(TEXT("PipelineAdd"));Next();break;
        case 16:Press(TEXT("PipelineAdd1"));Next();break;
        case 17:
            Test->TestTrue(TEXT("Magnitude precedes geometry"),M.Project.Pipelines[0].Operations[1].Kind==EStudioPipelineOperation::Magnitude);
            Press(TEXT("PipelineComponent0"));Next();break;
        case 18:Press(TEXT("PipelineScalar_velocity_u"));Press(TEXT("PipelineComponent1"));Next();break;
        case 19:Press(TEXT("PipelineScalar_velocity_v"));Press(TEXT("PipelineApplyOperation"));Press(TEXT("PipelineEvaluate"));Next();break;
        case 20:
            if(!Ready())return false;
            Test->TestTrue(TEXT("Magnitude preserves derived identity and expression"),View()->PresentedScalar().Origin==TEXT("pipeline-derived")&&HasText(TEXT("velocity_u")));
            Capture(TEXT("magnitude.png"));Press(TEXT("PipelineMoveDown"));Next();break;
        case 21:
            Test->TestTrue(TEXT("Independent magnitude may move after a clip"),M.Project.Pipelines[0].Operations[2].Kind==EStudioPipelineOperation::Magnitude);
            Press(TEXT("PipelineUndo"));Next();break;
        case 22:Press(TEXT("PipelineRedo"));Press(TEXT("PipelineUndo"));Press(TEXT("PipelineOperation1"));Press(TEXT("PipelineEnabled1"));Next();break;
        case 23:
            Test->TestFalse(TEXT("Operation can be disabled without deleting parameters"),M.Project.Pipelines[0].Operations[1].bEnabled);
            Press(TEXT("PipelineAdd"));Next();break;
        case 24:Press(TEXT("PipelineAdd4"));Next();break;
        case 25:Type(TEXT("PipelineNumberValue"),TEXT("-20"));Press(TEXT("PipelineEvaluate"));Next();break;
        case 26:
            if(!Ready())return false;
            Test->TestTrue(TEXT("Contour displays evaluated lines"),View()->PipelineOutput()->Kind==EStudioPipelineOutputKind::ContourLines&&View()->PipelineOutput()->Lines.Num()>0);
            Capture(TEXT("contour.png"));Press(TEXT("PipelineRemove"));Press(TEXT("PipelineAdd"));Next();break;
        case 27:Press(TEXT("PipelineAdd5"));Next();break;
        case 28:
            Type(TEXT("PipelineNumberA0"),TEXT(".2"));Type(TEXT("PipelineNumberA1"),TEXT(".2"));Type(TEXT("PipelineNumberA2"),TEXT(".02"));Press(TEXT("PipelineEvaluate"));Next();break;
        case 29:
            if(!Find(TEXT("PipelineProbeTable")))return false;
            Test->TestTrue(TEXT("Off-plane probe retains a missing row and truthful status"),!View()&&HasText(TEXT("Off source plane"))&&HasText(TEXT("1 unavailable")));
            Capture(TEXT("probe-gap.png"));Type(TEXT("PipelineNumberA1"),TEXT("0"));Press(TEXT("PipelineProbeLine"));
            Type(TEXT("PipelineNumberB0"),TEXT(".25"));Type(TEXT("PipelineNumberB1"),TEXT("0"));Type(TEXT("PipelineNumberB2"),TEXT(".02"));Type(TEXT("PipelineNumberSamples"),TEXT("5"));Press(TEXT("PipelineEvaluate"));Next();break;
        case 30:
            if(!Find(TEXT("PipelineProbeTable")))return false;
            Test->TestTrue(TEXT("Line probe keeps all requested rows"),HasText(TEXT("5 requested samples")));CheckSolve();Capture(TEXT("probe-line.png"));Press(TEXT("PipelineManage"));Next();break;
        case 31:Type(TEXT("PipelineRename"),TEXT("Wake pressure"));Press(TEXT("PipelineRenameApply"));Next();break;
        case 32:
            Test->TestEqual(TEXT("Rename keeps evaluated output"),M.Project.Pipelines[0].Name,FString(TEXT("Wake pressure")));
            Press(TEXT("PipelineManage"));Next();break;
        case 33:Press(TEXT("PipelineDelete"));Next();break;
        case 34:
            Test->TestTrue(TEXT("Delete retires the view"),M.Project.Pipelines.IsEmpty()&&!View());Press(TEXT("PipelineUndo"));Next();break;
        case 35:
            Test->TestTrue(TEXT("Undo restores complete recipe"),M.Project.Pipelines.Num()==1&&M.Project.Pipelines[0].Name==TEXT("Wake pressure"));
            Press(TEXT("PipelineOperation3"));Press(TEXT("PipelineRemove"));Press(TEXT("PipelineEvaluate"));Next();break;
        case 36:
            if(!Ready())return false;
            CaptureCount=View()->GetCaptureCount();Press(TEXT("Workspace0"));Next();break;
        case 37:
            Test->TestTrue(TEXT("Hidden analysis stops captures"),View()&&View()->GetCaptureCount()==CaptureCount&&View()->ResourceStats().Workers==0);
            Press(TEXT("Workspace10"));Next();break;
        case 38:
        {
            CheckSolve();SavedRecipe=M.Project.Pipelines[0];FString Error;Test->TestTrue(TEXT("Save project containing pipeline"),M.SaveProject(Work/TEXT("analysis.lbms")));
            OldView.Reset(View());OldModel=View()->Model;OldField=View()->PresentedField();
            M.NewProject(TEXT("Replacement for lifetime check"));Next();break;
        }
        case 39:
            Test->TestTrue(TEXT("Hidden replacement releases previous analysis before reopening the workspace"),!OldModel.IsValid()&&!OldField.IsValid()&&OldView->ResourceStats().MeshBytes==0);
            Test->TestTrue(TEXT("Reopen saved project"),M.RequestProjectOpen(Work/TEXT("analysis.lbms")));Next();break;
        case 40:
            Test->TestTrue(TEXT("Reopened recipe preserves operations and independent camera"),M.Project.Pipelines.Num()==1&&StudioPipelines::Equals(M.Project.Pipelines[0],SavedRecipe));
            Press(TEXT("Workspace10"));Phase=51;Changed=GFrameCounter;ReadyFrame=0;break;
        case 51:
            if(!Ready())return false;
            Test->TestTrue(TEXT("Replacement releases previous analysis resources before GC"),!OldModel.IsValid()&&!OldField.IsValid()&&OldView->ResourceStats().MeshBytes==0&&OldView->ResourceStats().RenderTargetBytes==0);
            OldView.Reset();Capture(TEXT("reopened.png"));Phase=41;Changed=GFrameCounter;ReadyFrame=0;break;
        case 41:
        {
            const FString Samples=FPaths::ProjectContentDir()/TEXT("Samples");
            auto R=StudioRecordings::Import(Samples/TEXT("Cylinder3D_ReaderFixture/recording.json"),0,{});
            if(!Test->TestTrue(*R.Error,R.Reference.IsSet()))return true;
            R=StudioRecordings::ImportReconstruction(*R.Reference,Samples/TEXT("Cylinder3D_VolumeFixture/reconstruction.json"),0,{});
            if(!Test->TestTrue(*R.Error,R.Source&&R.Reference.IsSet()))return true;
            FixtureId=R.Reference->Id;M.Project.Recordings.Add(*R.Reference);Baseline=SolveState();Press(TEXT("PipelineNew"));Next();break;
        }
        case 42:Press(FName(*(TEXT("PipelineNew_")+FixtureId)));Next();break;
        case 43:
            if(!View()||!Enabled(TEXT("PipelineEvaluate")))return false;
            Test->TestTrue(TEXT("Second saved recipe has original three-dimensional identity"),M.Project.Pipelines.Num()==2&&M.Project.Pipelines.Last().Source.Identity.SpatialDimensions==3);
            Press(TEXT("PipelineAdd"));Next();break;
        case 44:Press(TEXT("PipelineAdd3"));Next();break;
        case 45:
            Type(TEXT("PipelineNumberB0"),TEXT(".3"));Type(TEXT("PipelineNumberB1"),TEXT(".7"));Type(TEXT("PipelineNumberB2"),TEXT(".2"));Press(TEXT("PipelineEvaluate"));Next();break;
        case 46:
            if(!Ready())return false;
            Test->TestTrue(TEXT("Oblique 3D slice normal is normalized and produces a surface"),FMath::IsNearlyEqual(M.Project.Pipelines[1].Operations.Last().B.Size(),1.,1.e-10)&&
                View()->PipelineOutput()->Kind==EStudioPipelineOutputKind::Surface&&View()->PipelineOutput()->Triangles.Num()>0);
            CheckSolve();Capture(TEXT("volume-slice.png"));Press(TEXT("PipelineSourceInfo"));Next();break;
        case 47:
            Test->TestTrue(TEXT("Copyable provenance is available"),Find(TEXT("PipelineProvenance")).IsValid());Capture(TEXT("source-provenance.png"));App.DismissAllMenus();
            Press(TEXT("PipelineSaved"));Next();break;
        case 48:Press(TEXT("PipelineSaved0"));Next();break;
        case 49:
            if(!Ready())return false;
            Test->TestTrue(TEXT("Choosing saved analysis restores exact frame and independent camera"),View()->PresentedField()->Identity()->Ordinal==SavedRecipe.Source.Identity.Ordinal&&
                StudioView::CameraEquals(View()->SavedCameraState(),SavedRecipe.Source.Camera));
            Press(TEXT("PipelineOperation0"));Type(TEXT("PipelineOperationName"),TEXT("Wake sample"));Press(TEXT("PipelineApplyOperation"));Press(TEXT("PipelineScalar"));Phase=52;Changed=GFrameCounter;break;
        case 52:
            Press(TEXT("PipelineScalar_velocity_u"));Press(TEXT("PipelineApplyOperation"));
            Test->TestTrue(TEXT("Scalar selection preserves an intentional custom operation name"),M.Project.Pipelines[0].Operations[0].Name==TEXT("Wake sample")&&M.Project.Pipelines[0].Operations[0].Field==TEXT("velocity_u"));
            Test->TestTrue(TEXT("Restore original session"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Phase=50;Changed=GFrameCounter;break;
        case 50:return true;
        }
        return false;
    }
private:
    void Next(){++Phase;Changed=GFrameCounter;ReadyFrame=0;}
    AStudioScene* View(){for(TActorIterator<AStudioScene> It(Scene->GetWorld());It;++It)if(IsValid(*It)&&It->ActorHasTag(TEXT("StudioPipelineView")))return *It;return nullptr;}
    bool Ready()
    {
        const auto Viewport=Find(TEXT("PipelineViewport"));
        if(!View()||!View()->HasCurrentFrame()||!Enabled(TEXT("PipelineEvaluate"))||!Viewport||Viewport->GetCachedGeometry().GetLocalSize().X<=0)
        {ReadyFrame=0;return false;}
        if(!ReadyFrame){ReadyFrame=GFrameCounter;return false;}return GFrameCounter>=ReadyFrame+8;
    }
    bool Enabled(FName Tag){auto W=Find(Tag);if(W)W->UpdateAllAttributes();return W&&W->IsEnabled();}
    FString SolveState(){auto P=Scene->Model->SnapshotProject();P.Pipelines.Reset();return StudioProjectIO::Serialize(P);}
    void CheckSolve(){Test->TestEqual(TEXT("Pipeline editing leaves Solve document and cursor unchanged"),SolveState(),Baseline);}
    void Capture(const TCHAR* Name)
    {
        TArray<FColor> Pixels;FIntVector Size;
        if(!Test->TestTrue(TEXT("Capture native pipeline window"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        Test->TestTrue(TEXT("Save native pipeline capture"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    TSharedPtr<SWidget> FindIn(const TSharedRef<SWidget>& W,FName Tag)
    {
        // Immediate cancellation follows opening in the same input turn. Update
        // the footer's visibility attribute before traversing its new control.
        W->UpdateAllAttributes();
        if(!W->GetVisibility().IsVisible())return {};if(W->GetTag()==Tag)return W;
        auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto Found=FindIn(C->GetChildAt(I),Tag))return Found;return {};
    }
    TSharedPtr<SWidget> FindWindow(const TSharedRef<SWindow>& W,FName Tag)
    {
        if(auto Found=FindIn(W,Tag))return Found;
        for(const auto& Child:W->GetChildWindows())if(auto Found=FindWindow(Child,Tag))return Found;return {};
    }
    TSharedPtr<SWidget> Find(FName Tag)
    {for(const auto& W:FSlateApplication::Get().GetInteractiveTopLevelWindows())if(auto Found=FindWindow(W,Tag))return Found;return {};}
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
        const auto W=Find(Tag);if(!Test->TestTrue(FString::Printf(TEXT("Pipeline widget found: %s (phase %d, menus %d, operations %d, notice %s; last menu %s)"),*Tag.ToString(),Phase,FSlateApplication::Get().AnyMenusVisible(),Scene->Model->Project.Pipelines.IsEmpty()?0:Scene->Model->Project.Pipelines.Last().Operations.Num(),*Scene->Model->Notice,*MenuTrace),W.IsValid()))return {};
        W->UpdateAllAttributes();if(!Test->TestTrue(TEXT("Pipeline widget enabled: ")+Tag.ToString(),W->IsEnabled()))return {};
        const auto Target=Focusable(W.ToSharedRef());if(!Test->TestTrue(TEXT("Pipeline focus target"),Target.IsValid()))return {};
        FSlateApplication::Get().SetKeyboardFocus(Target,EFocusCause::Navigation);
        Test->TestTrue(TEXT("Pipeline focus reaches intended control or its native editor: ")+Tag.ToString(),
            Target->HasKeyboardFocus()||Target->HasFocusedDescendants());return Target;
    }
    void Press(FName Tag)
    {
        const auto Widget=Find(Tag);
        if(Widget&&Widget->GetType()==TEXT("SStudioMenuButton")){PendingMenu=Tag;return;}
        if(const auto W=Focus(Tag))Key(W->GetType()==TEXT("SCheckBox")?EKeys::SpaceBar:EKeys::Enter);
    }
    void Type(FName Tag,const FString& Value)
    {
        if(!Focus(Tag))return;
        Key(EKeys::A,FModifierKeysState(false,false,true,false,false,false,false,false,false));
        if(Value.IsEmpty())Key(EKeys::BackSpace);
        else for(TCHAR C:Value)FSlateApplication::Get().ProcessKeyCharEvent(FCharacterEvent(C,FModifierKeysState(),0,false));
        Key(EKeys::Enter);
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;TStrongObjectPtr<AStudioScene> OldView;
    TWeakPtr<FStudioModel> OldModel;TWeakPtr<const IStudioField,ESPMode::ThreadSafe> OldField;
    FString Root,Work,Baseline,FixtureId,MenuTrace;FStudioSavedPipeline SavedRecipe;FName PendingMenu;
    int32 Phase=0;uint64 Changed=0,CaptureCount=0,ReadyFrame=0;double Started=0,LastActivation=0,TargetX=0;bool bCaptured=false,bTooltips=true;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPipelineUI,"Studio.PipelineUI.RecipesParametersCameraAndLifecycle",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioPipelineUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioPipelineUICommand(this));return true;}
#endif
