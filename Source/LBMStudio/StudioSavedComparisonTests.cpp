#include "StudioSavedComparison.h"
#include "StudioModel.h"
#include "StudioAssetPaths.h"
#include "StudioSnapshotSource.h"
#include "StudioPointRecording.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioSavedComparisonTestsPrivate
{
constexpr auto Flags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FString Fixture(const TCHAR* Name){return FPaths::ProjectContentDir()/TEXT("Samples")/Name;}
FString Work(){return FPaths::ProjectSavedDir()/TEXT("Automation/SavedComparisons")/FGuid::NewGuid().ToString();}
struct FFixture
{
    TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> A,B;
    TArray<FStudioRecordingReference> References;
    FStudioComparisonResult Pair;
    FStudioSavedComparison Saved;
    FString Error;
    bool Load(bool Surface=false)
    {
        auto Raw=StudioRecordings::Import(Fixture(TEXT("NACA0018_ReaderFixture"))/TEXT("recording.json"),1,{});
        if(!Raw.Source||!Raw.Reference.IsSet()){Error=Raw.Error;return false;}
        A=Raw.Source;References.Add(*Raw.Reference);
        auto Other=Surface?StudioRecordings::ImportReconstruction(*Raw.Reference,Fixture(TEXT("NACA0018_SurfaceFixture"))/TEXT("reconstruction.json"),1,{}):
            StudioRecordings::Import(Fixture(TEXT("NACA0018_ReaderFixture"))/TEXT("recording.json"),1,{});
        if(!Other.Source||!Other.Reference.IsSet()){Error=Other.Error;return false;}B=Other.Source;References.Add(*Other.Reference);
        FStudioComparisonRequest R{FGuid::NewGuid(),A,B,1,TEXT("pressure"),{}};R.Alignment.Mode=EStudioTimeAlignment::ElapsedFromStart;
        Pair=StudioComparison::Evaluate(R);if(!Pair.Matches(R)){Error=Pair.Frames.Error;return false;}
        FStudioCameraState CA,CB;CA.Position=FVector(1.1234567890123,2,3);CA.Orientation=FRotator(43,71,89).Quaternion();CA.bFreeCamera=true;
        CB.Position=FVector(-4,5,2);CB.bOrthographic=true;CB.OrthoWidth=4.125;CB.bDepthClipping=true;CB.NearClipMeters=.04;CB.FarClipMeters=900;
        return StudioSavedComparisons::Create(TEXT("  Pressure study  "),Pair,CA,CB,false,References,Saved,Error);
    }
};
TSharedPtr<FJsonObject> Document(const FStudioProject& P)
{TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StudioProjectIO::Serialize(P)),O);return O;}
FString Text(const TSharedPtr<FJsonObject>& O)
{FString S;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&S));return S;}
TOptional<FStudioComparisonRestoreResult> Finish(FStudioComparisonRestoreTask& Task)
{
    const double End=FPlatformTime::Seconds()+15;
    do{if(auto R=Task.Poll())return R;FPlatformProcess::SleepNoStats(.001f);}while(FPlatformTime::Seconds()<End);
    return {};
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSavedComparisonSchema,"Studio.SavedComparisons.Schema20MigrationAndValidation",StudioSavedComparisonTestsPrivate::Flags)
bool FSavedComparisonSchema::RunTest(const FString&)
{
    using namespace StudioSavedComparisonTestsPrivate;
    FFixture F;if(!TestTrue(TEXT("Original comparison fixture loads"),F.Load(true)))return false;
    FStudioProject P;P.Comparisons={F.Saved};FStudioProject Loaded;FString Error;
    if(!TestTrue(TEXT("Schema20 project round trip"),StudioProjectIO::Parse(StudioProjectIO::Serialize(P),Loaded,Error)))return false;
    TestTrue(TEXT("All source, original frame, camera and alignment values are exact"),StudioSavedComparisons::Equals(P.Comparisons,Loaded.Comparisons));
    TestEqual(TEXT("Trimmed saved name"),Loaded.Comparisons[0].Name,FString(TEXT("Pressure study")));
    auto Old=Document(P);Old->SetNumberField(TEXT("version"),19);Old->RemoveField(TEXT("comparisons"));
    TestTrue(TEXT("Schema19 migrates without inventing a saved comparison"),StudioProjectIO::Parse(Text(Old),Loaded,Error)&&Loaded.Comparisons.IsEmpty());
    Old->SetNumberField(TEXT("version"),20);TestFalse(TEXT("Schema20 requires explicit collection"),StudioProjectIO::Parse(Text(Old),Loaded,Error));
    const FString Before=StudioProjectIO::Serialize(Loaded);
    const TArray<TFunction<void(FStudioSavedComparison&)>> Corrupt={
        [](auto& S){S.Name=TEXT("bad\nname");},[](auto& S){S.Alignment.Mode=EStudioTimeAlignment::Unset;},
        [](auto& S){S.Alignment.MaximumMismatchSeconds=-1;},[](auto& S){S.Primary.Identity.MetadataSHA256=TEXT("wrong");},
        [](auto& S){S.Secondary.Identity.Ordinal=-1;},[](auto& S){S.Primary.Camera.Orientation=FQuat(0,0,0,0);},
        [](auto& S){S.Unit=TEXT("unknown");},[](auto& S){S.Primary.Reference->MetadataSHA256=FString::ChrN(64,'a');}};
    for(const auto& Change:Corrupt)
    {
        auto Bad=P;Change(Bad.Comparisons[0]);TestFalse(TEXT("Corruption rejected transactionally"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Bad),Loaded,Error));
        TestEqual(TEXT("Rejected comparison cannot replace document"),StudioProjectIO::Serialize(Loaded),Before);
    }
    auto Fractional=Document(P);Fractional->GetArrayField(TEXT("comparisons"))[0]->AsObject()->GetObjectField(TEXT("secondary"))->SetNumberField(TEXT("ordinal"),1.5);
    TestFalse(TEXT("Fractional original ordinal rejected"),StudioProjectIO::Parse(Text(Fractional),Loaded,Error));
    auto Duplicate=P;Duplicate.Comparisons.Add(F.Saved);TestFalse(TEXT("Duplicate IDs rejected"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Duplicate),Loaded,Error));
    Duplicate.Comparisons.Last().Id=FGuid::NewGuid();Duplicate.Comparisons.Last().Name=TEXT("pressure STUDY");
    TestFalse(TEXT("Case-insensitive duplicate names rejected"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Duplicate),Loaded,Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSavedComparisonRestore,"Studio.SavedComparisons.OriginalSourcesAndExactRestoration",StudioSavedComparisonTestsPrivate::Flags)
bool FSavedComparisonRestore::RunTest(const FString&)
{
    using namespace StudioSavedComparisonTestsPrivate;
    FFixture F;if(!TestTrue(TEXT("Original point and reconstruction pair"),F.Load(true)))return false;
    const auto Id=FGuid::NewGuid();const auto R=StudioSavedComparisons::Restore(Id,F.Saved,{});
    if(!TestTrue(*R.Error,R.Matches(Id,F.Saved)))return false;
    TestTrue(TEXT("Same dataset can retain independent original/derived representations"),R.Pair->Primary.Identity.Interpolation==EStudioFieldInterpolation::None&&
        R.Pair->Secondary.Identity.Interpolation==EStudioFieldInterpolation::ReconstructedTriangles);
    TestTrue(TEXT("Each reference and exact camera pose restored independently"),StudioSavedComparisons::Equals(F.Saved,R.Saved));
    const auto* Original=F.Pair.Primary.Field->OriginalPoints()->FindValues(TEXT("pressure"));
    const auto* Restored=R.Pair->Primary.Field->OriginalPoints()->FindValues(TEXT("pressure"));
    TestTrue(TEXT("All pressure values remain exact published arrays"),Original&&Restored&&*Original==*Restored);
    TestTrue(TEXT("Source readers do not alias live readers"),R.PrimarySource!=F.A&&R.SecondarySource!=F.B);
    TestFalse(TEXT("Foreign project cannot adopt restored pair"),R.Matches(FGuid::NewGuid(),F.Saved));
    auto Changed=F.Saved;Changed.Primary.Camera.Position.X+=1;TestFalse(TEXT("Edited definition cannot adopt stale completion"),R.Matches(Id,Changed));
    // A source-level Locate updates only paths; an attached reconstruction must
    // not silently replace the saved original-points side.
    auto Moved=F.Saved;Moved.Primary.Reference->Path=Work()/TEXT("recording.json");Moved.Secondary.Reference->Path=Moved.Primary.Reference->Path;
    Moved.Secondary.Reference->Reconstruction->Path=Work()/TEXT("reconstruction.json");
    const auto Located=StudioSavedComparisons::Restore(Id,Moved,{F.References.Last()});
    TestTrue(TEXT("Matching current locations repair saved paths without changing representations"),Located.Matches(Id,Moved)&&Located.Pair->Primary.Identity.Interpolation==EStudioFieldInterpolation::None);
    auto Three=StudioRecordings::Import(Fixture(TEXT("Cylinder3D_ReaderFixture"))/TEXT("recording.json"),2,{});
    if(!TestTrue(TEXT("Authentic3D source loads"),Three.Source&&Three.Reference.IsSet()))return false;
    FStudioComparisonRequest Request{Id,Three.Source,Three.Source,2,TEXT("pressure"),{}};Request.Alignment.Mode=EStudioTimeAlignment::RecordedTime;
    const auto Pair=StudioComparison::Evaluate(Request);FStudioSavedComparison Saved;FString Error;
    TestTrue(TEXT("Save3D original frame pair"),StudioSavedComparisons::Create(TEXT("Cylinder"),Pair,{}, {},true,{*Three.Reference},Saved,Error));
    const auto Volume=StudioSavedComparisons::Restore(Id,Saved,{});
    TestTrue(TEXT("3D original source ordinals remain exact"),Volume.Matches(Id,Saved)&&Volume.Pair->Primary.Identity.SpatialDimensions==3&&Volume.Pair->Secondary.Identity.Ordinal==2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSavedComparisonFailures,"Studio.SavedComparisons.FailuresCancellationAndRelease",StudioSavedComparisonTestsPrivate::Flags)
bool FSavedComparisonFailures::RunTest(const FString&)
{
    using namespace StudioSavedComparisonTestsPrivate;
    FFixture F;if(!TestTrue(TEXT("Published fixtures available"),F.Load()))return false;const FGuid Id=FGuid::NewGuid();
    const TArray<TFunction<void(FStudioSavedComparison&)>> Changes={
        [](auto& S){++S.Secondary.Identity.Frame.Index;},[](auto& S){S.Secondary.Identity.Frame.Time+=.01;},
        [](auto& S){S.Secondary.Identity.Ordinal=0;},[](auto& S){S.Unit=TEXT("kPa");},
        [](auto& S){S.Secondary.Reference->Path=Work()/TEXT("recording.json");},
        [](auto& S){S.Secondary.Identity.MetadataSHA256=FString::ChrN(64,'a');S.Secondary.Reference->MetadataSHA256=S.Secondary.Identity.MetadataSHA256;}};
    for(const auto& Change:Changes)
    {
        auto Bad=F.Saved;Change(Bad);const auto R=StudioSavedComparisons::Restore(Id,Bad,{});
        TestTrue(TEXT("No substitution or partial pair on changed/missing data"),!R.Error.IsEmpty()&&!R.Pair.IsSet()&&!R.PrimarySource&&!R.SecondarySource);
    }
    auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);const auto Cancelled=StudioSavedComparisons::Restore(Id,F.Saved,{},Cancel);
    TestTrue(TEXT("Early cancellation owns no sources"),Cancelled.bCancelled&&!Cancelled.PrimarySource&&!Cancelled.Pair.IsSet());
    FStudioComparisonRestoreTask Task;FString Error;TestTrue(TEXT("Start one restore worker"),Task.Start(Id,F.Saved,{},Error));
    TestFalse(TEXT("No queued second restore"),Task.Start(Id,F.Saved,{},Error));Task.Cancel();auto Result=Finish(Task);
    if(!TestTrue(TEXT("Cancelled worker drains"),Result.IsSet()))return false;
    TestTrue(TEXT("Cancel discards all output before publication"),Result->bCancelled&&!Result->Pair.IsSet()&&!Result->PrimarySource&&!Task.IsBusy());
    TestTrue(TEXT("Task reusable after cancellation"),Task.Start(Id,F.Saved,{},Error));Result=Finish(Task);
    if(!TestTrue(TEXT("Completed restore publishes exact pair"),Result.IsSet()&&Result->Matches(Id,F.Saved)))return false;
    TWeakPtr<IStudioSolver,ESPMode::ThreadSafe> Source=Result->PrimarySource;
    TWeakPtr<const IStudioField,ESPMode::ThreadSafe> Field=Result->Pair->Primary.Field;Result.Reset();
    TestFalse(TEXT("Polled task retains no source"),Source.IsValid());TestFalse(TEXT("Polled task retains no field"),Field.IsValid());
    TestTrue(TEXT("Start pending shutdown"),Task.Start(Id,F.Saved,{},Error));Task.Shutdown();
    TestFalse(TEXT("Shutdown joins worker and clears future"),Task.IsBusy());TestFalse(TEXT("Closed task refuses another worker"),Task.Start(Id,F.Saved,{},Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSavedComparisonCommands,"Studio.SavedComparisons.CollectionHistoryAndIsolation",StudioSavedComparisonTestsPrivate::Flags)
bool FSavedComparisonCommands::RunTest(const FString&)
{
    using namespace StudioSavedComparisonTestsPrivate;
    FFixture F;if(!TestTrue(TEXT("Published comparison available"),F.Load()))return false;
    FStudioModel M(Work());M.Run();M.Tick(.2);M.Scrub(.6);const auto View=M.InspectionState();
    const auto Selected=M.SelectedFrame,Playing=M.PlaybackFrame;const auto Draft=StudioCaseIO::Serialize(M.Project.Draft);
    TestTrue(TEXT("Add saved comparison"),M.AddComparison(F.Saved));const auto Id=M.Project.Comparisons[0].Id;
    TestTrue(TEXT("Rename stable comparison"),M.RenameComparison(Id,TEXT("Wing pressure")));
    auto Update=F.Saved;Update.Primary.Camera.Position.X+=.5;TestTrue(TEXT("Update saved comparison"),M.UpdateComparison(Id,Update));
    TestEqual(TEXT("Update retains saved name"),M.FindComparison(Id)->Name,FString(TEXT("Wing pressure")));
    TestTrue(TEXT("Delete comparison"),M.DeleteComparison(Id));TestTrue(TEXT("Delete is reversible"),M.UndoComparisons()&&M.FindComparison(Id));
    TestTrue(TEXT("Redo deletion"),M.RedoComparisons()&&!M.FindComparison(Id));TestTrue(TEXT("Restore again"),M.UndoComparisons());
    TestTrue(TEXT("Solve view unchanged"),View.Equals(M.InspectionState()));TestEqual(TEXT("Review cursor unchanged"),M.SelectedFrame,Selected);
    TestEqual(TEXT("Playback cursor unchanged"),M.PlaybackFrame,Playing);TestEqual(TEXT("Case unchanged"),StudioCaseIO::Serialize(M.Project.Draft),Draft);
    auto Duplicate=F.Saved;Duplicate.Name=TEXT("wing PRESSURE");TestFalse(TEXT("Duplicate name cannot replace collection"),M.AddComparison(Duplicate));
    for(int32 I=0;I<80;++I)TestTrue(TEXT("Retain bounded rename history"),M.RenameComparison(Id,FString::Printf(TEXT("Study %d"),I)));
    int32 Undone=0;while(M.CanUndoComparisons()&&M.UndoComparisons())++Undone;
    TestEqual(TEXT("History retains only64 transactions"),Undone,64);
    const FString Before=StudioProjectIO::Serialize(M.SnapshotProject());
    FStudioSavedComparison Bad=*M.FindComparison(Id);Bad.Primary.Camera.FieldOfView=std::numeric_limits<double>::quiet_NaN();
    TestFalse(TEXT("Invalid update leaves collection unchanged"),M.UpdateComparison(Id,Bad));TestEqual(TEXT("No partial mutation"),StudioProjectIO::Serialize(M.SnapshotProject()),Before);
    auto Snapshot=MakeShared<FStudioModel>(F.Pair.Primary.Snapshot.ToSharedRef());TestFalse(TEXT("Inspection-only model cannot save comparisons"),Snapshot->AddComparison(F.Saved));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSavedComparisonPersistence,"Studio.SavedComparisons.PortableSaveRecoveryAndProjectLifetime",StudioSavedComparisonTestsPrivate::Flags)
bool FSavedComparisonPersistence::RunTest(const FString&)
{
    using namespace StudioSavedComparisonTestsPrivate;
    FFixture F;if(!TestTrue(TEXT("Source and reconstruction available"),F.Load(true)))return false;
    const FString Dir=Work();FStudioModel M(Dir);TestTrue(TEXT("Save comparison without changing active source"),M.AddComparison(F.Saved));
    const FGuid Id=M.Project.Comparisons[0].Id;const FString Path=Dir/TEXT("original/project.lbms");
    TestTrue(TEXT("Save full project"),M.SaveProject(Path));TestFalse(TEXT("Saved state clean"),M.HasUnsavedChanges());
    FStudioProject Raw,Loaded;FString Error;TestTrue(TEXT("Read saved project"),StudioProjectIO::Load(Path,Loaded,Error));
    TestTrue(TEXT("References resolve independently of active recording list"),Loaded.Recordings.IsEmpty()&&
        Loaded.Comparisons[0].Primary.Reference->Path==F.Saved.Primary.Reference->Path&&Loaded.Comparisons[0].Secondary.Reference->Reconstruction->Path==F.Saved.Secondary.Reference->Reconstruction->Path);
    TestTrue(TEXT("Rebase nested source/reconstruction paths"),StudioAssetPaths::ForStorage(M.SnapshotProject(),Dir/TEXT("elsewhere/copy.lbms"),Raw,Error));
    TestTrue(TEXT("Stored nested paths are portable"),FPaths::IsRelative(Raw.Comparisons[0].Primary.Reference->Path)&&FPaths::IsRelative(Raw.Comparisons[0].Secondary.Reference->Reconstruction->Path));
    TestTrue(TEXT("Undo after save is dirty"),M.UndoComparisons()&&M.HasUnsavedChanges());TestTrue(TEXT("Redo saved definition becomes clean"),M.RedoComparisons()&&!M.HasUnsavedChanges());
    TestFalse(TEXT("Failed project open preserves collection history"),M.LoadProject(Dir/TEXT("missing.lbms")));TestTrue(TEXT("History retained"),M.CanUndoComparisons());
    TestTrue(TEXT("Reopen project"),M.LoadProject(Path));TestFalse(TEXT("Project reopening clears comparison edit history"),M.CanUndoComparisons()||M.CanRedoComparisons());
    TestTrue(TEXT("Saved comparison survives exact reopening"),M.FindComparison(Id)&&StudioSavedComparisons::Equals(Loaded.Comparisons,M.Project.Comparisons));
    TestTrue(TEXT("Rename for recovery"),M.RenameComparison(Id,TEXT("Recovered")));M.WriteRecovery();FStudioModel Recovery(Dir);Recovery.OpenSession();
    TestTrue(TEXT("Comparison edit is offered for recovery"),Recovery.RestoreRecovery());
    if(!TestTrue(TEXT("Recovery contains saved comparison"),Recovery.FindComparison(Id)!=nullptr))return false;
    TestEqual(TEXT("Recovery retains comparison edits"),Recovery.FindComparison(Id)->Name,FString(TEXT("Recovered")));
    TestTrue(TEXT("Duplicate document rebases saved source references"),M.DuplicateProject(Dir/TEXT("duplicate/copy.lbms"),TEXT("Copy")));
    TestTrue(TEXT("Saved comparison identity retained in independent project"),M.FindComparison(Id)!=nullptr);TestFalse(TEXT("History cannot cross project ID"),M.CanUndoComparisons());
    M.NewProject(TEXT("Fresh"));TestTrue(TEXT("New project has no prior saved comparisons"),M.Project.Comparisons.IsEmpty()&&!M.CanUndoComparisons());
    return true;
}
#endif
