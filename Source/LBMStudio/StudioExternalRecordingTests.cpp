#include "StudioModel.h"
#include "StudioAssetPaths.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
FString ExternalRoot()
{
    return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/ExternalRecordingTests")/FGuid::NewGuid().ToString());
}
bool CopyRecording(const FString& Destination,const FString& From=FString())
{
    const FString Source=From.IsEmpty()?FPaths::ProjectContentDir()/TEXT("Samples/MeshGraphNets_Airfoil_test010"):From;
    IFileManager::Get().MakeDirectory(*Destination,true);
    return IFileManager::Get().Copy(*(Destination/TEXT("flow.bin")),*(Source/TEXT("flow.bin")))==COPY_OK&&
        IFileManager::Get().Copy(*(Destination/TEXT("recording.json")),*(Source/TEXT("recording.json")))==COPY_OK;
}
bool FinishExternal(FStudioModel& M)
{
    const double Deadline=FPlatformTime::Seconds()+15;
    while((M.IsRecordingLoadPending()||M.IsProjectOpenPending())&&FPlatformTime::Seconds()<Deadline)
    {M.Tick(0);FPlatformProcess::Sleep(.001f);}
    return !M.IsRecordingLoadPending()&&!M.IsProjectOpenPending();
}
void ChangeMetadata(const FString& Directory,const FString& Key,const FString& Value)
{
    FString Text;TSharedPtr<FJsonObject> O;
    FFileHelper::LoadFileToString(Text,*(Directory/TEXT("recording.json")));
    if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O)||!O) return;
    O->SetStringField(Key,Value);Text.Empty();FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&Text));
    FFileHelper::SaveStringToFile(Text,*(Directory/TEXT("recording.json")));
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioExternalRecordingPersistence,"Studio.ExternalRecording.ImportPersistRelocateAndRecover",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioExternalRecordingPersistence::RunTest(const FString&)
{
    const FString Root=ExternalRoot(),Original=Root/TEXT("original"),Moved=Root/TEXT("moved"),File=Root/TEXT("project/case.lbms");
    TestTrue(TEXT("Use actual published CFD bytes, not synthetic flow"),CopyRecording(Original));
    FStudioModel M(Root/TEXT("session"));M.Project.Camera.Position=FVector(7,8,9);M.Scrub(.5);
    const auto Camera=M.Project.Camera;const auto Draft=StudioCaseIO::Serialize(M.Project.Draft);
    TestTrue(TEXT("External source starts asynchronously"),M.RequestExternalRecording(Original/TEXT("flow.bin")));
    TestEqual(TEXT("Previous source retained while checking"),M.Project.Dataset,FString(TEXT("MeshGraphNets_Airfoil_test009")));
    TestFalse(TEXT("One recording worker at a time"),M.RequestExternalRecording(Original/TEXT("flow.bin")));
    TestTrue(TEXT("Import completes"),FinishExternal(M));
    if(!TestEqual(TEXT("External reference saved"),M.Project.Recordings.Num(),1)) return false;
    const FString Id=M.Project.Dataset;
    TestEqual(TEXT("External import makes no publication claim"),M.Project.Runs.Last().GetOrigin(),EStudioRunOrigin::ImportedRecording);
    TestTrue(TEXT("Imported data invents no configuration"),M.Project.Runs.Last().GetConfiguration()==nullptr);
    TestEqual(TEXT("Imported source ID"),Id,FString(TEXT("MeshGraphNets_Airfoil_test010")));
    TestEqual(TEXT("Camera retained"),M.Project.Camera.Position,Camera.Position);
    TestEqual(TEXT("Draft retained"),StudioCaseIO::Serialize(M.Project.Draft),Draft);
    FStudioFieldValue Value;
    TestTrue(TEXT("Original source node available"),M.Solver->CaptureField(0)->Sample(FVector(.03250676393508911,0,.1194048523902893),Value));
    TestTrue(TEXT("Original published velocity preserved"),FMath::IsNearlyEqual(Value.Velocity.X,221.55085243305282,1e-5));
    TestEqual(TEXT("Original source duration"),M.Solver->EvaluateFrame(600).Time,.12);
    M.Scrub(.7);TestTrue(TEXT("External project saved"),M.SaveProject(File));
    FString JSON,Error;FStudioProject Stored;
    FFileHelper::LoadFileToString(JSON,*File);TestTrue(TEXT("Storage parses"),StudioProjectIO::Parse(JSON,Stored,Error));
    TestTrue(TEXT("Portable relative external path"),FPaths::IsRelative(Stored.Recordings[0].Path));
    FStudioModel Opened(Root/TEXT("opened"));
    TestTrue(TEXT("Interactive external open starts"),Opened.RequestProjectOpen(File));TestTrue(TEXT("Open finishes"),FinishExternal(Opened));
    TestEqual(TEXT("Saved frame restored"),Opened.SelectedFrame,420);TestFalse(TEXT("Normal open clean"),Opened.HasUnsavedChanges());
    TestTrue(TEXT("Duplicate into another folder"),Opened.DuplicateProject(Root/TEXT("duplicate/copy.lbms"),TEXT("Portable duplicate")));
    FStudioProject Duplicate;TestTrue(TEXT("Duplicate reloads"),StudioProjectIO::Load(Opened.ProjectPath,Duplicate,Error));
    TestTrue(TEXT("Duplicate refers to same source"),FPaths::IsSamePath(Duplicate.Recordings[0].Path,Original/TEXT("flow.bin")));
    TestTrue(TEXT("Exact relocated pair copied"),CopyRecording(Moved,Original));
    TestTrue(TEXT("Only test-owned original removed"),IFileManager::Get().DeleteDirectory(*Original,false,true));
    const auto Before=StudioProjectIO::Serialize(M.SnapshotProject());
    TestTrue(TEXT("Missing source open begins"),M.RequestProjectOpen(File));TestTrue(TEXT("Missing source check finishes"),FinishExternal(M));
    TestEqual(TEXT("Missing source cannot replace open project"),StudioProjectIO::Serialize(M.SnapshotProject()),Before);
    TestTrue(TEXT("Missing source exposes a recoverable project"),M.RecordingRepair.IsSet());
    TestTrue(TEXT("Retry with located pair"),M.RetryRecordingRepair(Moved/TEXT("flow.bin")));
    TestTrue(TEXT("Relocated candidate completes"),FinishExternal(M));
    TestTrue(TEXT("Verified replacement path applied"),FPaths::IsSamePath(M.Project.Recordings[0].Path,Moved/TEXT("flow.bin")));
    TestFalse(TEXT("Successful repair clears prompt"),M.RecordingRepair.IsSet());
    TestEqual(TEXT("Relocated open restores frame"),M.SelectedFrame,420);TestTrue(TEXT("New path needs saving"),M.HasUnsavedChanges());
    TestTrue(TEXT("Save relocated document"),M.SaveProject(File));
    TestTrue(TEXT("Unselected external result can be revisited"),M.RequestRecording(TEXT("MeshGraphNets_Airfoil_test009")));TestTrue(TEXT("Switch drains"),FinishExternal(M));
    TestTrue(TEXT("External source selection"),M.RequestRecording(Id));TestTrue(TEXT("External source reopened"),FinishExternal(M));
    M.Scrub(.4);M.RenameProject(TEXT("Unsaved recovery"));M.WriteRecovery();
    M.PendingRecovery=Root/TEXT("session/Recovery/StudioRecovery.lbms");
    TestTrue(TEXT("External recovery begins"),M.RequestRecoveryOpen());TestTrue(TEXT("External recovery drains"),FinishExternal(M));
    TestEqual(TEXT("Recovery restores source frame"),M.SelectedFrame,240);TestTrue(TEXT("Recovery dirty"),M.HasUnsavedChanges());
    TestTrue(TEXT("Recovery retains absolute location"),FPaths::IsSamePath(M.Project.Recordings[0].Path,Moved/TEXT("flow.bin")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioExternalRecordingFailures,"Studio.ExternalRecording.IntegrityCancelAndIdentity",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioExternalRecordingFailures::RunTest(const FString&)
{
    const FString Root=ExternalRoot(),Original=Root/TEXT("source"),Replacement=Root/TEXT("replacement");
    TestTrue(TEXT("Copy original CFD"),CopyRecording(Original));
    // Only catalog identity changes in this test. Every mesh/field byte remains published CFD.
    ChangeMetadata(Original,TEXT("id"),TEXT("external-copy-of-SU2-010"));
    FStudioModel M(Root/TEXT("session"));M.Scrub(.5);
    const auto Before=StudioProjectIO::Serialize(M.SnapshotProject());
    TestTrue(TEXT("Import begins"),M.RequestExternalRecording(Original/TEXT("flow.bin")));M.CancelRecording();
    TestTrue(TEXT("Cancelled verifier drains"),FinishExternal(M));
    TestEqual(TEXT("Cancellation leaves whole document"),StudioProjectIO::Serialize(M.SnapshotProject()),Before);
    TestTrue(TEXT("Retry import"),M.RequestExternalRecording(Original/TEXT("flow.bin")));TestTrue(TEXT("Retry completes"),FinishExternal(M));
    if(!TestEqual(TEXT("External source registered"),M.Project.Recordings.Num(),1)) return false;
    const FString Id=M.Project.Dataset;
    M.Scrub(.6);M.Run();const auto Runs=M.Project.Runs.Num();
    TestTrue(TEXT("Copy exact replacement"),CopyRecording(Replacement,Original));
    TestTrue(TEXT("Live relocation begins"),M.RequestRecordingRelink(Id,Replacement/TEXT("flow.bin")));
    TestTrue(TEXT("Live relocation completes"),FinishExternal(M));
    TestEqual(TEXT("Live relocation preserves frame"),M.SelectedFrame,360);TestEqual(TEXT("Playback preserved"),M.State,EStudioRunState::Running);
    TestEqual(TEXT("Relocation creates no new run"),M.Project.Runs.Num(),Runs);
    const auto Good=StudioProjectIO::Serialize(M.SnapshotProject());
    ChangeMetadata(Original,TEXT("fieldNote"),TEXT("Changed interpretation must not silently replace saved metadata."));
    TestTrue(TEXT("Changed metadata relocation begins"),M.RequestRecordingRelink(Id,Original/TEXT("flow.bin")));
    TestTrue(TEXT("Changed metadata finishes"),FinishExternal(M));
    TestEqual(TEXT("Changed metadata rejected transactionally"),StudioProjectIO::Serialize(M.SnapshotProject()),Good);
    TestTrue(TEXT("Same ID different metadata import begins"),M.RequestExternalRecording(Original/TEXT("flow.bin")));
    TestTrue(TEXT("Identity collision finishes"),FinishExternal(M));
    TestEqual(TEXT("Cannot overwrite historical result identity"),StudioProjectIO::Serialize(M.SnapshotProject()),Good);
    // Change only a valid timestamp (outside mesh/frame CRCs). Full payload SHA
    // must reject this even though the structural reader accepts the file.
    TArray<uint8> Bytes;FFileHelper::LoadFileToArray(Bytes,*(Original/TEXT("flow.bin")));
    int32 Nodes=0,Triangles=0,Boundary=0;
    FMemory::Memcpy(&Nodes,Bytes.GetData()+8,4);FMemory::Memcpy(&Triangles,Bytes.GetData()+12,4);FMemory::Memcpy(&Boundary,Bytes.GetData()+16,4);
    const int64 FirstTime=24LL+16LL*Nodes+12LL*Triangles+4LL*Boundary+4;
    const double ChangedTime=.00001;FMemory::Memcpy(Bytes.GetData()+FirstTime,&ChangedTime,sizeof(double));
    FFileHelper::SaveArrayToFile(Bytes,*(Original/TEXT("flow.bin")));Bytes.Empty();
    auto Corrupt=StudioRecordings::Import(Original/TEXT("flow.bin"),0,{});
    TestFalse(TEXT("Changed time payload rejected"),Corrupt.Source.IsValid());TestTrue(TEXT("Full hash catches header change"),Corrupt.Error.Contains(TEXT("SHA-256")));
    TestTrue(TEXT("Start import before replacing project"),M.RequestExternalRecording(Replacement/TEXT("flow.bin")));
    M.NewProject(TEXT("Replacement document"));TestTrue(TEXT("Stale import drains"),FinishExternal(M));
    TestEqual(TEXT("New document keeps its own source catalog"),M.Project.Recordings.Num(),0);
    auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    TestFalse(TEXT("Pre-cancelled verifier publishes no source"),StudioRecordings::Import(Replacement/TEXT("flow.bin"),0,Cancel).Source.IsValid());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioExternalRecordingSchema,"Studio.ExternalRecording.SchemaAndMigration",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioExternalRecordingSchema::RunTest(const FString&)
{
    FStudioProject P,Out;FString Error;
    FStudioRecordingReference Ref{TEXT("example"),TEXT("External data"),TEXT("../recording/flow.bin"),FString::ChrN(64,TCHAR('a')),FString::ChrN(64,TCHAR('b'))};
    P.Recordings.Add(Ref);
    TestTrue(TEXT("Version 6 round trip"),StudioProjectIO::Parse(StudioProjectIO::Serialize(P),Out,Error));
    TestEqual(TEXT("Payload hash retained"),Out.Recordings[0].PayloadSHA256,Ref.PayloadSHA256);
    const auto Before=StudioProjectIO::Serialize(Out);
    for(int32 Kind=0;Kind<5;++Kind)
    {
        auto Invalid=P;
        if(Kind==0) Invalid.Recordings.Add(Ref);
        if(Kind==1) Invalid.Recordings[0].MetadataSHA256=TEXT("missing");
        if(Kind==2) Invalid.Recordings[0].Path=TEXT("https://example.invalid/flow.bin");
        if(Kind==3) Invalid.Recordings[0].Path=TEXT("../other.bin");
        if(Kind==4) Invalid.Recordings[0].Id=TEXT("line\nbreak");
        TestFalse(TEXT("Invalid reference rejected"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Invalid),Out,Error));
        TestEqual(TEXT("Failed parse leaves caller intact"),StudioProjectIO::Serialize(Out),Before);
    }
    TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StudioProjectIO::Serialize(P)),O);
    O->SetNumberField(TEXT("version"),5);O->RemoveField(TEXT("recordings"));FString JSON;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&JSON));
    TestTrue(TEXT("Version 5 migrates"),StudioProjectIO::Parse(JSON,Out,Error));TestTrue(TEXT("Migration invents no external locations"),Out.Recordings.IsEmpty());
    TestFalse(TEXT("Relative path needs owner directory"),StudioAssetPaths::Resolve(P,FString(),Error));
    return true;
}
#endif
