#include "SStudioPipelineWorkspace.h"
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
#include "Widgets/Input/SMenuAnchor.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"
#include "ImageUtils.h"

#if WITH_DEV_AUTOMATION_TESTS
/** Export through routed Slate input, retaining direct evaluated buffers for an
 * independent VTK/CSV reader. Injected paths do not test native panel permissions. */
class FStudioPipelineExportUICommand final : public IAutomationLatentCommand
{
    struct FPublishGate
    {
        FEvent* Reached=FPlatformProcess::GetSynchEventFromPool(true);
        FEvent* Release=FPlatformProcess::GetSynchEventFromPool(true);
        ~FPublishGate(){FPlatformProcess::ReturnSynchEventToPool(Reached);FPlatformProcess::ReturnSynchEventToPool(Release);}
    };
public:
    explicit FStudioPipelineExportUICommand(FAutomationTestBase* In):Test(In){}
    ~FStudioPipelineExportUICommand()
    {if(Gate)Gate->Release->Trigger();if(bCaptured&&FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllowTooltips(bTooltips);}
    bool Update() override
    {
        const double Now=FPlatformTime::Seconds();if(!Started)Started=Now;
        if(Test->HasAnyErrors())return true;
        if(Now-Started>180){Test->AddError(FString::Printf(TEXT("Pipeline export timed out at phase %d"),Phase));return true;}
        if(!Scene.IsValid())for(const auto& C:GEngine->GetWorldContexts())if(C.World()&&C.World()->IsGameWorld())
            for(TActorIterator<AStudioScene> It(C.World());It;++It)if(It->Model&&!It->Model->IsSnapshotView())Scene=*It;
        if(!Scene.IsValid()||GFrameCounter<Changed+8)return false;
        auto& App=FSlateApplication::Get();auto& M=*Scene->Model;
        if(M.IsProjectOpenPending()||M.IsRecordingLoadPending())return false;
        if(!App.IsActive())
        {if(Now-LastActivation>1){LastActivation=Now;FPlatformApplicationMisc::ActivateApplication();GEngine->GameViewport->GetWindow()->BringToFront(true);}Changed=GFrameCounter;return false;}
        if(bOpenExport)
        {
            bOpenExport=false;Press(TEXT("ExportMenu"));
            const auto Anchor=StaticCastSharedPtr<SMenuAnchor>(Find(TEXT("ExportMenu")));
            Test->TestTrue(TEXT("Routed Export input opens its anchor"),Anchor&&Anchor->IsOpen());Changed=GFrameCounter;return false;
        }
        switch(Phase)
        {
        case 0:
        {
            if(!Scene->HasCurrentFrame())return false;
            Root=FPaths::ProjectSavedDir()/TEXT("Automation/PipelineExportUI");IFileManager::Get().MakeDirectory(*Root,true);
            Work=Root/FGuid::NewGuid().ToString();FString Error;
            Test->TestTrue(TEXT("Preserve prior project"),StudioProjectIO::Save(Work/TEXT("prior.lbms"),M.SnapshotProject(),Error));
            bTooltips=App.GetAllowTooltips();bCaptured=true;App.SetAllowTooltips(false);StudioAuthoringTestCapture::DismissTooltips();
            M.NewProject(TEXT("Evaluated flow exports"));Press(TEXT("Workspace10"));Open();Next();break;
        }
        case 1:
            Test->TestTrue(TEXT("Empty workspace cannot export Solve data"),!Enabled(TEXT("SaveFieldVTK"))&&HasText(TEXT("Choose or create a pipeline")));
            Test->TestFalse(TEXT("Empty export does not claim a frozen result"),HasText(TEXT("This evaluated result is frozen")));
            Capture(TEXT("empty.png"));App.DismissAllMenus();
            if(!Fixture(false))return true;Baseline=SolveState();Open();Next();break;
        case 2:
            Test->TestTrue(TEXT("Unevaluated pipeline requires Evaluate: ")+MenuInfo(),!Enabled(TEXT("SaveFieldVTK"))&&HasText(TEXT("Evaluate the current pipeline")));
            Capture(TEXT("unevaluated.png"));App.DismissAllMenus();Press(TEXT("PipelineEvaluate"));Next();break;
        case 3:
            if(!Ready())return false;Type(TEXT("PipelineOperationName"),TEXT("Unapplied pressure"));Open();Next();break;
        case 4:
            Test->TestTrue(TEXT("Unapplied text blocks stale output"),!Enabled(TEXT("SaveFieldVTK"))&&HasText(TEXT("Apply or Revert")));
            Capture(TEXT("unapplied.png"));App.DismissAllMenus();Press(TEXT("PipelineRevertOperation"));
            {Test->TestTrue(TEXT("Rename evaluated analysis"),M.RenamePipeline(M.Project.Pipelines[0].Id,TEXT("Wing \"analysis\" · α")));
             auto C=View()->SavedCameraState();C.Position.X+=.025;View()->RestoreCamera(C,TEXT("Export camera"));}
            Open();Next();break;
        case 5:
            Test->TestTrue(TEXT("Pipeline form names evaluated geometry"),HasText(TEXT("Export pipeline output"))&&HasText(TEXT("Frozen evaluation · frame 2"))&&Enabled(TEXT("SaveFieldVTK")));
            Test->TestFalse(TEXT("Pipeline has no duplicate original-field selections"),Find(TEXT("VTKSelectAll")).IsValid()||Find(TEXT("ExportScopeRange")).IsValid());
            Capture(TEXT("surface.png"));StudioFileDialog::SetNextFieldVTKForAutomation(FString());Press(TEXT("SaveFieldVTK"));Next();break;
        case 6:
            Test->TestTrue(TEXT("Native selection cancellation reported"),M.Notice.Contains(TEXT("file selection cancelled")));Open();Next();break;
        case 7:
            if(!Oracle(TEXT("surface")))return true;
            Press(TEXT("VTKCoordinatesSource"));
            {auto P=M.Project.Pipelines[0];P.Operations.Last().B.X=.3;Test->TestTrue(TEXT("Change recipe after snapshot"),M.UpdatePipeline(P.Id,P));
             Test->TestTrue(TEXT("Change name after snapshot"),M.RenamePipeline(P.Id,TEXT("Changed after opening Export")));}
            Test->TestTrue(TEXT("Advance independent Solve cursor"),M.ReviewRecordedFrame(421));Next();break;
        case 8:
            Test->TestTrue(TEXT("Menu retains earlier output after recipe and Solve cursor change: ")+MenuInfo(),HasText(TEXT("Wing \"analysis\" · α"))&&HasText(TEXT("Frozen evaluation · frame 2"))&&M.SelectedFrame==421);
            Capture(TEXT("frozen.png"));Save(TEXT("surface-source.vtp"));Next();break;
        case 9:
            if(!Saved(TEXT("surface-source.vtp")))return false;
            Test->TestTrue(TEXT("Completion uses frozen pipeline name"),M.Notice.Contains(TEXT("Wing \"analysis\" · α")));Open();Next();break;
        case 10:
            Test->TestTrue(TEXT("Changed numerical recipe requires reevaluation"),!Enabled(TEXT("SaveFieldVTK"))&&HasText(TEXT("Evaluate the current pipeline")));
            Capture(TEXT("stale.png"));App.DismissAllMenus();
            {auto P=M.Project.Pipelines[0];M.RenamePipeline(P.Id,TEXT("Velocity magnitude"));auto O=Op(EStudioPipelineOperation::Magnitude,TEXT("Speed"));
             O.Field=TEXT("derived.speed");O.Unit=TEXT("m/s");O.Components={TEXT("velocity_u"),TEXT("velocity_v")};P.Operations.Insert(O,1);M.UpdatePipeline(P.Id,P);}
            Baseline=SolveState();Next();break;
        case 11:Press(TEXT("PipelineEvaluate"));Next();break;
        case 12:if(!Ready())return false;if(!Oracle(TEXT("magnitude")))return true;CheckSolve();Open();Next();break;
        case 13:
            Press(TEXT("ExportFormatCSV"));Press(TEXT("VTKCoordinatesScene"));
            Test->TestTrue(TEXT("Derived scalar expression is visible"),HasText(TEXT("interpolated before magnitude")));Capture(TEXT("magnitude-csv.png"));Save(TEXT("magnitude-scene.csv"),true);Next();break;
        case 14:if(!Saved(TEXT("magnitude-scene.csv")))return false;CheckSolve();Open();Next();break;
        case 15:
            Test->TestTrue(TEXT("Saved output offers Finder reveal"),Enabled(TEXT("RevealFieldVTK")));Capture(TEXT("saved.png"));
            Test->TestTrue(TEXT("Create failure sentinel"),FFileHelper::SaveStringToFile(TEXT("Keep destination"),*(Root/TEXT("blocked-file"))));
            Save(TEXT("blocked-file/child.csv"),true);Next();break;
        case 16:if(!M.Notice.StartsWith(TEXT("Field export failed")))return false;Open();Next();break;
        case 17:
            Test->TestTrue(TEXT("Failure has visible recovery"),HasText(TEXT("Field export failed")));Capture(TEXT("failed.png"));App.DismissAllMenus();
            {auto P=M.Project.Pipelines[0];M.RenamePipeline(P.Id,TEXT("Probe with coverage gaps"));P.Operations.SetNum(1);
             auto O=Op(EStudioPipelineOperation::Probe,TEXT("Across the wing"));O.bLine=true;O.A=FVector(-.6,0,.02);O.B=FVector(.5,0,.02);O.Samples=17;P.Operations.Add(O);M.UpdatePipeline(P.Id,P);}
            Next();break;
        case 18:Press(TEXT("PipelineEvaluate"));Next();break;
        case 19:if(!Find(TEXT("PipelineProbeTable")))return false;if(!Oracle(TEXT("probe")))return true;Open();Next();break;
        case 20:
            Test->TestTrue(TEXT("Probe CSV preserves missing rows; VTK disabled with explanation"),!Enabled(TEXT("ExportFormatVTK"))&&Enabled(TEXT("ExportFormatCSV"))&&HasText(TEXT("every probe row")));
            Press(TEXT("VTKCoordinatesSource"));Capture(TEXT("probe-csv.png"));Save(TEXT("probe-source.csv"),true);Next();break;
        case 21:
            if(!Saved(TEXT("probe-source.csv")))return false;CheckSolve();App.DismissAllMenus();
            {auto P=M.Project.Pipelines[0];M.RenamePipeline(P.Id,TEXT("Empty clip"));P.Operations.SetNum(1);auto O=Op(EStudioPipelineOperation::ClipBox,TEXT("Outside source"));O.A=FVector(5,5,5);O.B=FVector(6,6,6);P.Operations.Add(O);M.UpdatePipeline(P.Id,P);}
            Next();break;
        case 22:Press(TEXT("PipelineEvaluate"));Next();break;
        case 23:
            if(!Snapshot())return false;if(!Oracle(TEXT("empty")))return true;Open();Next();break;
        case 24:
            Press(TEXT("ExportFormatVTK"));Press(TEXT("VTKCoordinatesScene"));Test->TestTrue(TEXT("Empty evaluated output remains exportable"),Enabled(TEXT("SaveFieldVTK"))&&HasText(TEXT("0 evaluated vertices")));
            Capture(TEXT("empty-output.png"));Save(TEXT("empty-scene.vtp"));Next();break;
        case 25:
            if(!Saved(TEXT("empty-scene.vtp")))return false;CheckSolve();App.DismissAllMenus();
            Test->TestTrue(TEXT("Remove old selected pipeline"),M.DeletePipeline(M.Project.Pipelines[0].Id));if(!Fixture(true))return true;Baseline=SolveState();Next();break;
        case 26:Press(TEXT("PipelineEvaluate"));Next();break;
        case 27:if(!Ready())return false;if(!Oracle(TEXT("volume-slice")))return true;Open();Next();break;
        case 28:
            Capture(TEXT("volume-slice.png"));Press(TEXT("VTKCoordinatesSource"));Save(TEXT("volume-slice-source.vtp"));Next();break;
        case 29:if(!Saved(TEXT("volume-slice-source.vtp")))return false;CheckSolve();Open();Next();break;
        case 30:
            Gate=MakeShared<FPublishGate,ESPMode::ThreadSafe>();
            Test->TestTrue(TEXT("Create cancellation sentinel"),FFileHelper::SaveStringToFile(TEXT("Keep existing destination"),*(Root/TEXT("cancelled.vtp"))));
            FStudioFieldExportUI::SetBeforeNextPublishForAutomation([G=Gate]{G->Reached->Trigger();G->Release->Wait(20000);});Save(TEXT("cancelled.vtp"));Next();break;
        case 31:
            if(!Gate->Reached->Wait(0))return false;Press(TEXT("Workspace7"));Open();Next();break;
        case 32:
            Test->TestTrue(TEXT("One pending export retains pipeline context from Solve"),M.Workspace==EStudioWorkspace::Solve&&HasText(TEXT("Export pipeline output"))&&Enabled(TEXT("CancelFieldVTK"))&&!Enabled(TEXT("SaveFieldVTK")));
            Capture(TEXT("progress-in-solve.png"));App.DismissAllMenus();M.NewProject(TEXT("Replacement during pipeline export"));Open();Next();break;
        case 33:
            Test->TestTrue(TEXT("Pending old-project task stays cancellable without a second write"),HasText(TEXT("Volume slice"))&&Enabled(TEXT("CancelFieldVTK"))&&!Enabled(TEXT("SaveFieldVTK")));
            Capture(TEXT("progress-new-project.png"));Press(TEXT("CancelFieldVTK"));Gate->Release->Trigger();Next();break;
        case 34:
            if(Find(TEXT("CancelFieldVTK")))return false;
            Test->TestEqual(TEXT("Cancellation preserves existing destination"),Read(TEXT("cancelled.vtp")),FString(TEXT("Keep existing destination")));
            Test->TestFalse(TEXT("Old task does not adopt completion into replacement project"),M.Notice.Contains(TEXT("Field export cancelled")));
            App.DismissAllMenus();Open();Next();break;
        case 35:
            if(!Scene->HasCurrentFrame())return false;
            Test->TestTrue(TEXT("Original Solve export restored after pending task drains"),HasText(TEXT("Export original field data"))&&Find(TEXT("ExportScopeRange"))&&Find(TEXT("VTKSelectAll"))&&Enabled(TEXT("SaveFieldVTK")));
            Capture(TEXT("original-solve.png"));App.DismissAllMenus();Test->TestTrue(TEXT("Restore prior project"),M.RequestProjectOpen(Work/TEXT("prior.lbms")));Next();break;
        case 36:return true;
        }
        return false;
    }
private:
    static FStudioPipelineOperation Op(EStudioPipelineOperation Kind,const TCHAR* Name)
    {FStudioPipelineOperation O;O.Kind=Kind;O.Name=Name;return O;}
    bool Fixture(bool Volume)
    {
        const auto Samples=FPaths::ProjectContentDir()/TEXT("Samples");auto& M=*Scene->Model;
        auto R=StudioRecordings::Import(Samples/(Volume?TEXT("Cylinder3D_ReaderFixture"):TEXT("NACA0018_ReaderFixture"))/TEXT("recording.json"),1,{});
        if(!Test->TestTrue(*R.Error,R.Reference.IsSet()))return false;
        R=StudioRecordings::ImportReconstruction(*R.Reference,Samples/(Volume?TEXT("Cylinder3D_VolumeFixture"):TEXT("NACA0018_SurfaceFixture"))/TEXT("reconstruction.json"),1,{});
        if(!Test->TestTrue(*R.Error,R.Source&&R.Reference.IsSet()))return false;
        M.Project.Recordings.Add(*R.Reference);auto F=R.Source->ReadScalarFrame(1,TEXT("pressure"));if(!Test->TestTrue(*F.Error,F.Field.IsValid()))return false;
        FStudioSavedPipeline P;P.Name=Volume?TEXT("Volume slice"):TEXT("Wing clip");P.Source.Reference=R.Reference;P.Source.Title=R.Source->Descriptor().Title;P.Source.Identity=*F.Field->Identity();
        P.Source.Camera=StudioView::FitBounds(P.Source.Camera,R.Source->Descriptor().DisplayBounds,4./3.,.001);
        auto Select=Op(EStudioPipelineOperation::Field,TEXT("Pressure"));Select.Field=TEXT("pressure");Select.Unit=TEXT("Pa");P.Operations={Select};
        auto O=Op(Volume?EStudioPipelineOperation::Slice:EStudioPipelineOperation::ClipBox,Volume?TEXT("Oblique plane"):TEXT("Wing window"));
        O.A=Volume?FVector(.04,.04,0):FVector(-.1,-.01,-.1);O.B=Volume?FVector(1,2,3).GetSafeNormal():FVector(.4,.01,.1);P.Operations.Add(O);
        return Test->TestTrue(TEXT("Install authentic pinned pipeline fixture"),M.AddPipeline(P));
    }
    TOptional<FStudioPipelineEvaluationResult> Snapshot()
    {const auto W=Find(TEXT("PipelineWorkspace"));FString Error;return W?StaticCastSharedPtr<SStudioPipelineWorkspace>(W)->ExportSnapshot(Error):TOptional<FStudioPipelineEvaluationResult>();}
    bool Oracle(const FString& Name)
    {
        const auto E=Snapshot();if(!Test->TestTrue(TEXT("Capture evaluated oracle"),E.IsSet()))return false;const auto& O=*E->Output;
        auto SaveBytes=[&](const TCHAR* Suffix,const void* Data,int64 Bytes)
        {return FFileHelper::SaveArrayToFile(TArrayView<const uint8>(static_cast<const uint8*>(Data),Bytes),*(Root/(Name+Suffix)));};
        TArray<double> Vertices;TArray<int64> IDs;
        for(const auto& V:O.Vertices){Vertices.Append({V.PositionMeters.X,V.PositionMeters.Y,V.PositionMeters.Z,V.Scalar});IDs.Append({V.OriginalRow,V.OriginalPointId});}
        if(!Test->TestTrue(TEXT("Retain direct evaluation bytes"),SaveBytes(TEXT("-vertices.f64"),Vertices.GetData(),Vertices.Num()*8LL)&&SaveBytes(TEXT("-identity.i64"),IDs.GetData(),IDs.Num()*8LL)&&
            SaveBytes(TEXT("-triangles.i32"),O.Triangles.GetData(),O.Triangles.Num()*12LL)&&SaveBytes(TEXT("-lines.i32"),O.Lines.GetData(),O.Lines.Num()*8LL)))return false;
        auto Truth=MakeShared<FJsonObject>();Truth->SetArrayField(TEXT("recipes"),StudioPipelines::ToJSON({E->Prepared.Recipe}));
        Truth->SetStringField(TEXT("field"),E->Prepared.Field->SelectedScalar().Id);Truth->SetStringField(TEXT("method"),O.Method);Truth->SetNumberField(TEXT("kind"),int32(O.Kind));
        TArray<TSharedPtr<FJsonValue>> Rows;
        if(O.Probe)for(const auto& V:O.Probe->Samples)
        {
            auto J=MakeShared<FJsonObject>();if(V.ScenePosition){TArray<TSharedPtr<FJsonValue>> XYZ;for(int32 K=0;K<3;++K)XYZ.Add(MakeShared<FJsonValueNumber>((*V.ScenePosition)[K]));J->SetArrayField(TEXT("position"),XYZ);}
            if(V.Value)J->SetNumberField(TEXT("value"),*V.Value);if(V.PointId)J->SetStringField(TEXT("point_id"),LexToString(*V.PointId));
            J->SetNumberField(TEXT("distance"),V.DistanceAlongLineMeters);J->SetNumberField(TEXT("status"),int32(V.Status));Rows.Add(MakeShared<FJsonValueObject>(J));
        }
        Truth->SetArrayField(TEXT("probe"),Rows);FString JSON;FJsonSerializer::Serialize(Truth,TJsonWriterFactory<>::Create(&JSON));
        return Test->TestTrue(TEXT("Retain recipe and probe oracle"),FFileHelper::SaveStringToFile(JSON,*(Root/(Name+TEXT(".json"))),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    }
    AStudioScene* View(){for(TActorIterator<AStudioScene> It(Scene->GetWorld());It;++It)if(It->ActorHasTag(TEXT("StudioPipelineView")))return *It;return nullptr;}
    bool Ready(){return View()&&View()->HasCurrentFrame()&&Enabled(TEXT("PipelineEvaluate"));}
    void Next(){++Phase;Changed=GFrameCounter;}
    void Open(){bOpenExport=true;}
    void Save(const TCHAR* Name,bool CSV=false)
    {if(CSV)StudioFileDialog::SetNextProbeCSVForAutomation(Root/Name);else StudioFileDialog::SetNextFieldVTKForAutomation(Root/Name);Press(TEXT("SaveFieldVTK"));}
    bool Saved(const TCHAR* Name){return Scene->Model->Notice.StartsWith(FString(TEXT("Saved "))+Name);}
    FString Read(const TCHAR* Name){FString S;FFileHelper::LoadFileToString(S,*(Root/Name));return S;}
    FString SolveState(){auto P=Scene->Model->SnapshotProject();P.Pipelines.Reset();return StudioProjectIO::Serialize(P);}
    void CheckSolve(){Test->TestEqual(TEXT("Export leaves Solve state and cursor unchanged"),SolveState(),Baseline);}
    TSharedPtr<SWidget> FindIn(const TSharedRef<SWidget>& W,FName Tag)
    {W->UpdateAllAttributes();if(!W->GetVisibility().IsVisible())return {};if(W->GetTag()==Tag)return W;auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)if(auto F=FindIn(C->GetChildAt(I),Tag))return F;return {};}
    TSharedPtr<SWidget> FindWindow(const TSharedRef<SWindow>& W,FName Tag)
    {if(auto Found=FindIn(W,Tag))return Found;for(const auto& Child:W->GetChildWindows())if(auto Found=FindWindow(Child,Tag))return Found;return {};}
    TSharedPtr<SWidget> Find(FName Tag)
    {for(const auto& W:FSlateApplication::Get().GetInteractiveTopLevelWindows())if(auto Found=FindWindow(W,Tag))return Found;return {};}
    FString MenuInfo()
    {
        auto& App=FSlateApplication::Get();const auto Anchor=StaticCastSharedPtr<SMenuAnchor>(Find(TEXT("ExportMenu")));const auto Focus=App.GetKeyboardFocusedWidget();
        return FString::Printf(TEXT("phase=%d menus=%d anchor=%d panel=%d windows=%d focus=%s/%s frame=%d notice=%s"),Phase,App.AnyMenusVisible(),Anchor&&Anchor->IsOpen(),Find(TEXT("FieldExportPanel")).IsValid(),
            App.GetInteractiveTopLevelWindows().Num(),Focus?*Focus->GetTypeAsString():TEXT("none"),Focus?*Focus->GetTag().ToString():TEXT("none"),Scene->Model->SelectedFrame,*Scene->Model->Notice);
    }
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
    {const auto W=Find(Tag);if(!Test->TestTrue(TEXT("Export widget available: ")+Tag.ToString(),W&&W->IsEnabled()))return false;
        const auto Target=Focusable(W.ToSharedRef());if(!Test->TestTrue(TEXT("Focusable export widget"),Target.IsValid()))return false;
        FSlateApplication::Get().SetKeyboardFocus(Target,EFocusCause::Navigation);return Test->TestTrue(TEXT("Export keyboard focus"),Target->HasKeyboardFocus()||Target->HasFocusedDescendants());}
    void Press(FName Tag){if(Focus(Tag))Key(EKeys::Enter);}
    void Type(FName Tag,const FString& Text)
    {if(!Focus(Tag))return;Key(EKeys::A,FModifierKeysState(false,false,true,false,false,false,false,false,false));for(const TCHAR C:Text)FSlateApplication::Get().ProcessKeyCharEvent(FCharacterEvent(C,FModifierKeysState(),0,false));Key(EKeys::Tab);}
    void Capture(const TCHAR* Name)
    {
        const auto Panel=Find(TEXT("FieldExportPanel"));Test->TestTrue(TEXT("Export panel visible: ")+MenuInfo(),Panel.IsValid());
        if(Panel)
        {
            const auto Bounds=Panel->GetCachedGeometry().GetLayoutBoundingRect();
            for(const TCHAR* Tag:{TEXT("VTKFrozenFrame"),TEXT("SaveFieldVTK"),TEXT("CancelFieldVTK"),TEXT("VTKNotice"),TEXT("RevealFieldVTK")})if(const auto W=Find(Tag))
            {const auto R=W->GetCachedGeometry().GetLayoutBoundingRect();Test->TestTrue(FString(Name)+TEXT(" keeps ")+Tag+TEXT(" visible"),R.Top>=Bounds.Top&&R.Bottom<=Bounds.Bottom&&R.Left>=Bounds.Left&&R.Right<=Bounds.Right);}
        }
        TArray<FColor> Pixels;FIntVector Size;if(!Test->TestTrue(TEXT("Capture pipeline export"),FSlateApplication::Get().TakeScreenshot(GEngine->GameViewport->GetWindow().ToSharedRef(),Pixels,Size)))return;
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);Test->TestTrue(TEXT("Save export capture"),FFileHelper::SaveArrayToFile(PNG,*(Root/Name)));
    }
    FAutomationTestBase* Test;TWeakObjectPtr<AStudioScene> Scene;TSharedPtr<FPublishGate,ESPMode::ThreadSafe> Gate;
    FString Root,Work,Baseline;int32 Phase=0;uint64 Changed=0;double Started=0,LastActivation=0;bool bTooltips=true,bCaptured=false,bOpenExport=false;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPipelineExportUI,"Studio.PipelineExportUI.FrozenOutputModesAndLifecycle",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioPipelineExportUI::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(FStudioPipelineExportUICommand(this));return true;}
#endif
