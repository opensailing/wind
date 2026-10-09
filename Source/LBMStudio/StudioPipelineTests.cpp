#include "StudioPipeline.h"
#include "StudioSavedFieldView.h"
#include "StudioModel.h"
#include "StudioSnapshotSource.h"
#include "StudioAssetPaths.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioPipelineTestsPrivate
{
constexpr auto Flags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FString Fixture(const TCHAR* Name){return FPaths::ProjectContentDir()/TEXT("Samples")/Name;}
FString Work(){return FPaths::ProjectSavedDir()/TEXT("Automation/Pipelines")/FGuid::NewGuid().ToString();}
FStudioPipelineOperation Operation(EStudioPipelineOperation Kind,const TCHAR* Name)
{FStudioPipelineOperation O;O.Kind=Kind;O.Name=Name;return O;}
struct FFixture
{
    TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> Source;
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> Field;
    FStudioSavedPipeline Saved;
    FString Error;
    bool Load(bool Surface=true)
    {
        auto R=StudioRecordings::Import(Fixture(TEXT("NACA0018_ReaderFixture"))/TEXT("recording.json"),1,{});
        if(!R.Source||!R.Reference.IsSet()){Error=R.Error;return false;}
        if(Surface)R=StudioRecordings::ImportReconstruction(*R.Reference,Fixture(TEXT("NACA0018_SurfaceFixture"))/TEXT("reconstruction.json"),1,{});
        if(!R.Source||!R.Reference.IsSet()){Error=R.Error;return false;}Source=R.Source;
        auto Read=Source->ReadScalarFrame(1,TEXT("pressure"));if(!Read.Field||!Read.Field->Identity().IsSet()){Error=Read.Error;return false;}Field=Read.Field;
        Saved.Name=TEXT("Wing study");Saved.Source.Identity=*Field->Identity();Saved.Source.Title=Source->Descriptor().Title;Saved.Source.Reference=R.Reference;
        Saved.Source.Camera.Position=FVector(1.1234567890123,2,3);Saved.Source.Camera.Orientation=FRotator(43,71,89).Quaternion();Saved.Source.Camera.bFreeCamera=true;
        auto M=Operation(EStudioPipelineOperation::Magnitude,TEXT("Speed"));M.Field=TEXT("derived.speed");M.Unit=TEXT("m/s");M.Components={TEXT("velocity_u"),TEXT("velocity_v")};
        auto F=Operation(EStudioPipelineOperation::Field,TEXT("Pressure"));F.Field=TEXT("pressure");F.Unit=TEXT("Pa");
        auto C=Operation(EStudioPipelineOperation::ClipBox,TEXT("Wing region"));C.A=FVector(-.2,-1,-.2);C.B=FVector(.4,1,.3);
        auto S=Operation(EStudioPipelineOperation::Slice,TEXT("Original plane"));S.A=Saved.Source.Identity.SourceOffset;S.B=FVector::RightVector;
        auto Iso=Operation(EStudioPipelineOperation::Contour,TEXT("Pressure contour"));Iso.Value=12.1234567890123;
        auto P=Operation(EStudioPipelineOperation::Probe,TEXT("Wake line"));P.bLine=true;P.A=FVector(.2,0,.02);P.B=FVector(.4,0,.02);P.Samples=127;P.bEnabled=false;
        Saved.Operations={M,F,C,S,Iso,P};return StudioPipelines::IsValid(Saved,Error);
    }
};
TSharedPtr<FJsonObject> Document(const FStudioProject& P)
{TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StudioProjectIO::Serialize(P)),O);return O;}
FString JSONText(const TSharedPtr<FJsonObject>& O)
{FString S;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&S));return S;}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineSchema,"Studio.Pipelines.Schema21MigrationAndTransactionalValidation",StudioPipelineTestsPrivate::Flags)
bool FPipelineSchema::RunTest(const FString&)
{
    using namespace StudioPipelineTestsPrivate;
    FFixture F;if(!TestTrue(TEXT("Published original source and reconstruction available"),F.Load()))return false;
    FStudioProject P;P.Pipelines={F.Saved};FStudioProject Loaded;FString Error;
    if(!TestTrue(TEXT("Schema21 saves every operation and pinned view"),StudioProjectIO::Parse(StudioProjectIO::Serialize(P),Loaded,Error)))return false;
    TestTrue(TEXT("Exact IDs, fields, disabled parameters, source and camera survive"),StudioPipelines::Equals(P.Pipelines,Loaded.Pipelines));
    TestTrue(TEXT("Source hashes, original ordinal, step, time and reconstruction survive"),StudioSavedFieldViews::SameIdentity(F.Saved.Source.Identity,Loaded.Pipelines[0].Source.Identity));
    auto Old=Document(P);Old->SetNumberField(TEXT("version"),20);Old->RemoveField(TEXT("pipelines"));
    TestTrue(TEXT("Older document starts with no invented pipelines"),StudioProjectIO::Parse(JSONText(Old),Loaded,Error)&&Loaded.Pipelines.IsEmpty());
    Old->SetNumberField(TEXT("version"),21);TestFalse(TEXT("Current schema requires explicit collection"),StudioProjectIO::Parse(JSONText(Old),Loaded,Error));
    const FString Before=StudioProjectIO::Serialize(Loaded);
    const TArray<TFunction<void(FStudioSavedPipeline&)>> Corrupt={
        [](auto& V){V.Name=TEXT("bad\nname");},[](auto& V){V.Operations[0].Kind=EStudioPipelineOperation(255);},
        [](auto& V){V.Operations[0].Components={TEXT("velocity_u"),TEXT("velocity_u")};},
        [](auto& V){V.Operations[0].Unit=TEXT("unknown");},[](auto& V){V.Operations[0].Field=TEXT("derived.");},
        [](auto& V){V.Operations[2].B=V.Operations[2].A;},[](auto& V){V.Operations[3].B*=2;},
        [](auto& V){V.Operations[5].Samples=1025;},[](auto& V){V.Operations[5].B=V.Operations[5].A;},
        [](auto& V){V.Operations[4].Value=std::numeric_limits<double>::quiet_NaN();},
        [](auto& V){V.Operations[2].Id=V.Operations[0].Id;},[](auto& V){V.Operations[2].Name=TEXT("sPEED");},
        [](auto& V){V.Source.Reference->MetadataSHA256=FString::ChrN(64,'a');},
        [](auto& V){V.Source.Camera.Orientation=FQuat(0,0,0,0);},[](auto& V){V.Operations.Reset();}};
    for(const auto& Change:Corrupt)
    {
        auto Bad=P;Change(Bad.Pipelines[0]);TestFalse(TEXT("Invalid recipe rejected"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Bad),Loaded,Error));
        TestEqual(TEXT("Failure cannot replace project"),StudioProjectIO::Serialize(Loaded),Before);
    }
    for(const auto* Key:{TEXT("kind"),TEXT("samples")})
    {
        auto J=Document(P);J->GetArrayField(TEXT("pipelines"))[0]->AsObject()->GetArrayField(TEXT("operations"))[5]->AsObject()->SetNumberField(Key,2.5);
        TestFalse(TEXT("Fractional enum or sample count rejected"),StudioProjectIO::Parse(JSONText(J),Loaded,Error));
    }
    auto Duplicate=P;Duplicate.Pipelines.Add(F.Saved);TestFalse(TEXT("Duplicate pipeline IDs rejected"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Duplicate),Loaded,Error));
    Duplicate.Pipelines.Last().Id=FGuid::NewGuid();Duplicate.Pipelines.Last().Name=TEXT("wing STUDY");
    TestFalse(TEXT("Case-insensitive duplicate names rejected"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Duplicate),Loaded,Error));
    auto Many=P;Many.Pipelines.Reset();
    for(int32 I=0;I<64;++I){auto V=F.Saved;V.Id=FGuid::NewGuid();V.Name=FString::Printf(TEXT("Study %d"),I);Many.Pipelines.Add(V);}
    TestTrue(TEXT("64 saved recipes supported"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Many),Loaded,Error));
    auto Extra=F.Saved;Extra.Id=FGuid::NewGuid();Extra.Name=TEXT("Overflow");Many.Pipelines.Add(Extra);
    TestFalse(TEXT("65th recipe rejected"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Many),Loaded,Error));
    auto TooMany=F.Saved;while(TooMany.Operations.Num()<33){auto Op=Operation(EStudioPipelineOperation::Contour,*FString::Printf(TEXT("Disabled %d"),TooMany.Operations.Num()));Op.bEnabled=false;TooMany.Operations.Add(Op);}
    TestFalse(TEXT("Operation bound enforced before loading"),StudioPipelines::FromJSON(StudioPipelines::ToJSON({TooMany}),Loaded.Pipelines,Error));
    // Shared source decoding is independently transactional as well.
    auto View=F.Saved.Source;auto BadView=StudioSavedFieldViews::ToJSON(View);BadView->SetNumberField(TEXT("ordinal"),1.5);
    TestFalse(TEXT("Fractional source frame rejected"),StudioSavedFieldViews::FromJSON(BadView,View));
    TestTrue(TEXT("Shared source retains prior identity"),StudioSavedFieldViews::SameIdentity(View.Identity,F.Saved.Source.Identity));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineOrder,"Studio.Pipelines.OrderDependenciesAndSourceCapabilities",StudioPipelineTestsPrivate::Flags)
bool FPipelineOrder::RunTest(const FString&)
{
    using namespace StudioPipelineTestsPrivate;
    FFixture F;if(!TestTrue(TEXT("Published source available"),F.Load()))return false;
    FString Error;TArray<FStudioPipelineStage> Stages;
    if(!TestTrue(TEXT("Ordered magnitude, field, clip, slice and contour compile"),StudioPipelines::Compile(F.Saved,F.Source->Descriptor(),Stages,Error)))return false;
    TestEqual(TEXT("Disabled probe emits no stage"),Stages.Num(),5);
    TestEqual(TEXT("Magnitude produces named field"),Stages[0].Field,FString(TEXT("derived.speed")));
    TestEqual(TEXT("Pressure selection governs contour"),Stages.Last().Unit,FString(TEXT("Pa")));
    TestEqual(TEXT("2D contour is a line"),Stages.Last().OutputDimensions,1);
    auto P=F.Saved;P.Operations[4].bEnabled=false;
    TestTrue(TEXT("Enable terminal line probe"),StudioPipelines::SetEnabled(P,P.Operations[5].Id,true,Error));
    TestTrue(TEXT("Probe compiles to sample table"),StudioPipelines::Compile(P,F.Source->Descriptor(),Stages,Error)&&Stages.Last().OutputDimensions==0);
    P.Operations[5].bLine=false;TestTrue(TEXT("Point probe retains valid line draft parameters"),StudioPipelines::Compile(P,F.Source->Descriptor(),Stages,Error));
    const auto Before=P;
    TestFalse(TEXT("Enabled operation after probe rejected"),StudioPipelines::Move(P,P.Operations[5].Id,2,Error));
    TestTrue(TEXT("Rejected reorder leaves recipe exact"),StudioPipelines::Equals(P,Before));
    TestFalse(TEXT("Probe after contour rejected"),StudioPipelines::SetEnabled(P,P.Operations[4].Id,true,Error));
    TestTrue(TEXT("Rejected enable is transactional"),StudioPipelines::Equals(P,Before));
    P=F.Saved;P.Operations.SetNum(2);P.Operations[1].Field=TEXT("derived.speed");P.Operations[1].Unit=TEXT("m/s");
    TestTrue(TEXT("Select earlier derived output"),StudioPipelines::Compile(P,F.Source->Descriptor(),Stages,Error));
    const auto Ordered=P;
    TestFalse(TEXT("Forward derived dependency rejected"),StudioPipelines::Move(P,P.Operations[1].Id,0,Error));
    TestFalse(TEXT("Disabling required derived input rejected"),StudioPipelines::SetEnabled(P,P.Operations[0].Id,false,Error));
    TestTrue(TEXT("Both failures preserve recipe"),StudioPipelines::Equals(P,Ordered));
    auto Derived=Operation(EStudioPipelineOperation::Magnitude,TEXT("Combined magnitude"));Derived.Field=TEXT("derived.combined");Derived.Unit=TEXT("m/s");Derived.Components={TEXT("derived.speed"),TEXT("velocity_u")};P.Operations.Add(Derived);
    TestTrue(TEXT("Derived chain compiles in order"),StudioPipelines::Compile(P,F.Source->Descriptor(),Stages,Error));
    TestFalse(TEXT("Forward derived component rejected"),StudioPipelines::Move(P,Derived.Id,0,Error));
    const auto GoodStages=Stages;
    const TArray<TFunction<void(FStudioSavedPipeline&)>> Changes={
        [](auto& V){V.Operations[0].Components[0]=TEXT("pressure");},[](auto& V){V.Operations[1].Field=TEXT("missing");},
        [](auto& V){V.Operations[1].Unit=TEXT("kPa");},[](auto& V){++V.Source.Identity.Frame.Index;},
        [](auto& V){V.Source.Identity.Frame.Time+=.01;},[](auto& V){V.Source.Identity.Ordinal=0;},
        [](auto& V){V.Source.Identity.SourceOffset.X+=1;},[](auto& V){V.Source.Identity.MetadataSHA256=FString::ChrN(64,'a');V.Source.Reference->MetadataSHA256=V.Source.Identity.MetadataSHA256;},
        [](auto& V){V.Operations[3].A.Y+=.1;},[](auto& V){V.Operations[3].B=FVector::UpVector;},
        [](auto& V){V.Operations[3].B=FVector(1.e-7,1,0);}};
    for(const auto& Change:Changes)
    {
        auto Bad=F.Saved;Change(Bad);TestFalse(TEXT("Unusable dependencies or changed original frame rejected"),StudioPipelines::Compile(Bad,F.Source->Descriptor(),Stages,Error));
        TestTrue(TEXT("Failed compilation preserves prior stages"),Stages.Num()==GoodStages.Num()&&Stages.Last().OperationId==GoodStages.Last().OperationId);
    }
    auto D=F.Source->Descriptor();auto Collision=D.Scalars[0];Collision.Id=TEXT("derived.speed");D.Scalars.Add(Collision);
    TestFalse(TEXT("Derived output cannot overwrite existing source array"),StudioPipelines::Compile(F.Saved,D,Stages,Error));
    P=F.Saved;TestTrue(TEXT("Valid independent operation reorder"),StudioPipelines::Move(P,P.Operations[2].Id,3,Error));
    FFixture Raw;if(!TestTrue(TEXT("Published source without reconstruction"),Raw.Load(false)))return false;
    TestFalse(TEXT("Interpolation is unavailable for bare original points"),StudioPipelines::Compile(Raw.Saved,Raw.Source->Descriptor(),Stages,Error));
    Raw.Saved.Operations.SetNum(3);TestTrue(TEXT("Original point field and box clip recipe remain possible"),StudioPipelines::Compile(Raw.Saved,Raw.Source->Descriptor(),Stages,Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelineHistory,"Studio.Pipelines.CollectionHistoryBoundsAndSolveIsolation",StudioPipelineTestsPrivate::Flags)
bool FPipelineHistory::RunTest(const FString&)
{
    using namespace StudioPipelineTestsPrivate;
    FFixture F;if(!TestTrue(TEXT("Original field available"),F.Load()))return false;
    FStudioModel M(Work());M.Run();M.Tick(.2);M.Scrub(.6);const auto View=M.InspectionState();
    const auto Selected=M.SelectedFrame,Playing=M.PlaybackFrame;const auto Draft=StudioCaseIO::Serialize(M.Project.Draft);
    const auto Comparisons=M.Project.Comparisons.Num();const uint64 Revision=M.PipelineRevision;
    TestTrue(TEXT("Add recipe"),M.AddPipeline(F.Saved));const auto Id=M.Project.Pipelines[0].Id;
    TestTrue(TEXT("Rename trims and retains stable ID"),M.RenamePipeline(Id,TEXT("  Wake study  ")));
    auto Update=F.Saved;Update.Source.Camera.Position.X+=.5;Update.Operations[4].Value+=1;
    TestTrue(TEXT("Update exact operation and independent camera"),M.UpdatePipeline(Id,Update));
    TestEqual(TEXT("Update preserves collection name"),M.FindPipeline(Id)->Name,FString(TEXT("Wake study")));
    TestTrue(TEXT("Delete"),M.DeletePipeline(Id));TestTrue(TEXT("Undo deletion"),M.UndoPipelines()&&M.FindPipeline(Id));
    TestTrue(TEXT("Redo deletion"),M.RedoPipelines()&&!M.FindPipeline(Id));TestTrue(TEXT("Restore definition again"),M.UndoPipelines());
    TestTrue(TEXT("Revision advances on edits"),M.PipelineRevision>Revision);
    TestTrue(TEXT("Solve scalar and camera unchanged"),View.Equals(M.InspectionState()));TestEqual(TEXT("Selected frame unchanged"),M.SelectedFrame,Selected);
    TestEqual(TEXT("Playback cursor unchanged"),M.PlaybackFrame,Playing);TestEqual(TEXT("Case unchanged"),StudioCaseIO::Serialize(M.Project.Draft),Draft);
    TestEqual(TEXT("Comparison collection unchanged"),M.Project.Comparisons.Num(),Comparisons);
    auto Duplicate=F.Saved;Duplicate.Name=TEXT("wake STUDY");TestFalse(TEXT("Duplicate name rejected"),M.AddPipeline(Duplicate));
    for(int32 I=0;I<80;++I)TestTrue(TEXT("Bounded rename transaction"),M.RenamePipeline(Id,FString::Printf(TEXT("Study %d"),I)));
    int32 Count=0;while(M.CanUndoPipelines()&&M.UndoPipelines())++Count;TestEqual(TEXT("64 transactions retained"),Count,64);
    const FString Before=StudioProjectIO::Serialize(M.SnapshotProject());auto Bad=*M.FindPipeline(Id);Bad.Source.Camera.FieldOfView=std::numeric_limits<double>::quiet_NaN();
    TestFalse(TEXT("Invalid update rejected"),M.UpdatePipeline(Id,Bad));TestEqual(TEXT("No partial document mutation"),StudioProjectIO::Serialize(M.SnapshotProject()),Before);
    TestTrue(TEXT("New edit after undo"),M.RenamePipeline(Id,TEXT("New branch")));TestFalse(TEXT("New edit clears redo"),M.CanRedoPipelines());
    const uint64 Unchanged=M.PipelineRevision;TestTrue(TEXT("Unchanged update succeeds"),M.UpdatePipeline(Id,*M.FindPipeline(Id)));
    TestEqual(TEXT("Unchanged update adds no history revision"),M.PipelineRevision,Unchanged);
    M.Project.Pipelines[0].Name=TEXT("Outside edit");TestFalse(TEXT("History cannot overwrite outside mutation"),M.UndoPipelines());
    FString Error;const auto Scalar=F.Field->Scalar(TEXT("pressure"));if(!TestTrue(TEXT("Original scalar available"),Scalar.IsSet()))return false;
    auto Snapshot=FStudioSnapshotSource::Create(*F.Source,1,*Scalar,F.Field,Error);if(!TestTrue(TEXT("Frozen source available"),Snapshot.IsValid()))return false;
    FStudioModel Frozen(Snapshot.ToSharedRef());TestFalse(TEXT("Inspection-only model cannot save recipes"),Frozen.AddPipeline(F.Saved));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPipelinePersistence,"Studio.Pipelines.PortableSaveRecoveryAndProjectLifetime",StudioPipelineTestsPrivate::Flags)
bool FPipelinePersistence::RunTest(const FString&)
{
    using namespace StudioPipelineTestsPrivate;
    FFixture F;if(!TestTrue(TEXT("Source and reconstruction available"),F.Load()))return false;
    const FString Dir=Work();FStudioModel M(Dir);TestTrue(TEXT("Save recipe independently of active source"),M.AddPipeline(F.Saved));
    const auto Id=M.Project.Pipelines[0].Id;const FString Path=Dir/TEXT("original/project.lbms");
    TestTrue(TEXT("Save project"),M.SaveProject(Path));TestFalse(TEXT("Save is clean"),M.HasUnsavedChanges());
    FStudioProject Raw,Loaded;FString Error;TestTrue(TEXT("Read saved project"),StudioProjectIO::Load(Path,Loaded,Error));
    TestTrue(TEXT("Pinned source path resolves outside active recording list"),Loaded.Recordings.IsEmpty()&&Loaded.Pipelines[0].Source.Reference->Path==F.Saved.Source.Reference->Path);
    TestEqual(TEXT("Reconstruction path resolves"),Loaded.Pipelines[0].Source.Reference->Reconstruction->Path,F.Saved.Source.Reference->Reconstruction->Path);
    TestTrue(TEXT("Rebase independent source"),StudioAssetPaths::ForStorage(M.SnapshotProject(),Dir/TEXT("elsewhere/copy.lbms"),Raw,Error));
    TestTrue(TEXT("Source and reconstruction stored relatively"),FPaths::IsRelative(Raw.Pipelines[0].Source.Reference->Path)&&FPaths::IsRelative(Raw.Pipelines[0].Source.Reference->Reconstruction->Path));
    TestTrue(TEXT("Undo saved definition is dirty"),M.UndoPipelines()&&M.HasUnsavedChanges());TestTrue(TEXT("Redo matches saved definition"),M.RedoPipelines()&&!M.HasUnsavedChanges());
    TestFalse(TEXT("Failed open preserves document"),M.LoadProject(Dir/TEXT("missing.lbms")));TestTrue(TEXT("Failed open retains history"),M.CanUndoPipelines());
    TestTrue(TEXT("Reopen project"),M.LoadProject(Path));TestFalse(TEXT("Reopen clears history"),M.CanUndoPipelines()||M.CanRedoPipelines());
    TestTrue(TEXT("All saved parameters restored"),M.FindPipeline(Id)&&StudioPipelines::Equals(Loaded.Pipelines,M.Project.Pipelines));
    TestTrue(TEXT("Rename for recovery"),M.RenamePipeline(Id,TEXT("Recovered")));M.WriteRecovery();FStudioModel Recovery(Dir);Recovery.OpenSession();
    TestTrue(TEXT("Restore recovery"),Recovery.RestoreRecovery());if(!TestTrue(TEXT("Recovery retains pipeline"),Recovery.FindPipeline(Id)!=nullptr))return false;
    TestEqual(TEXT("Recovery restores edit"),Recovery.FindPipeline(Id)->Name,FString(TEXT("Recovered")));
    TestTrue(TEXT("Duplicate rebase"),M.DuplicateProject(Dir/TEXT("duplicate/copy.lbms"),TEXT("Copy")));
    TestTrue(TEXT("Duplicate retains recipe identity"),M.FindPipeline(Id)!=nullptr);TestFalse(TEXT("History cannot cross project lifetime"),M.CanUndoPipelines());
    // Relocation repairs paths only when the pinned original scientific identity matches.
    auto Moved=F.Saved.Source;Moved.Reference->Path=Dir/TEXT("missing/recording.json");Moved.Reference->Reconstruction->Path=Dir/TEXT("missing/reconstruction.json");
    const auto Fixed=StudioSavedFieldViews::ResolvedReference(Moved,{*F.Saved.Source.Reference});
    TestTrue(TEXT("Matching hashes permit locating moved sources"),Fixed.Num()==1&&Fixed[0].Path==F.Saved.Source.Reference->Path&&Fixed[0].Reconstruction->Path==F.Saved.Source.Reference->Reconstruction->Path);
    auto Foreign=*F.Saved.Source.Reference;Foreign.MetadataSHA256=FString::ChrN(64,'a');
    const auto Kept=StudioSavedFieldViews::ResolvedReference(Moved,{Foreign});TestEqual(TEXT("Changed data never repairs source path"),Kept[0].Path,Moved.Reference->Path);
    M.NewProject(TEXT("Fresh"));TestTrue(TEXT("New project resets collection and history"),M.Project.Pipelines.IsEmpty()&&!M.CanUndoPipelines());
    return true;
}
#endif
