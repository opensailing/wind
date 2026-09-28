#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Crc.h"
#include "HAL/FileManager.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Async/Async.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
// Structural parser test data only. Never staged or shown as CFD sample output.
FString WriteStructuralRecording(const FString& Name,bool bBadTime=false,bool bBadCoordinate=false,bool bBadBoundary=false)
{
    const FString Directory=FPaths::ProjectDir()/TEXT("tmp/debug/recording-tests")/Name;
    IFileManager::Get().MakeDirectory(*Directory,true);
    TArray<uint8> Bytes; FMemoryWriter W(Bytes);
    int32 Magic=0x53553246,Version=2,Nodes=3,Triangles=1,Boundary=3,Frames=3;
    W<<Magic<<Version<<Nodes<<Triangles<<Boundary<<Frames;
    for(FVector2D P:{FVector2D(0,0),FVector2D(1,0),FVector2D(0,1)})
    { if(bBadCoordinate) P.X=std::numeric_limits<double>::quiet_NaN(); W<<P.X<<P.Y; }
    for(int32 I:{0,1,2}) W<<I;
    for(int32 I:{0,1,2}) { int32 B=bBadBoundary?0:I; W<<B; }
    const uint32 MeshChecksum=FCrc::MemCrc32(Bytes.GetData(),Bytes.Num());
    TArray<TSharedPtr<FJsonValue>> CRCs;
    for(int32 I=0;I<Frames;++I)
    {
        int32 Index=42+I*17; double Time=bBadTime?0.:(I==0?.12:I==1?.4:1.5);
        W<<Index<<Time;
        const int32 Offset=Bytes.Num();
        for(int32 J=0;J<Nodes;++J) { float U=I+J+1,V=I-J,P=10+I,D=1;W<<U<<V<<P<<D; }
        CRCs.Add(MakeShared<FJsonValueNumber>(FCrc::MemCrc32(Bytes.GetData()+Offset,48)));
    }
    FFileHelper::SaveArrayToFile(Bytes,*(Directory/TEXT("flow.bin")));
    auto O=MakeShared<FJsonObject>();
    O->SetNumberField(TEXT("version"),1); O->SetNumberField(TEXT("spatialDimensions"),2);
    O->SetStringField(TEXT("id"),TEXT("parser-test-only")); O->SetStringField(TEXT("title"),TEXT("Parser structure fixture"));
    O->SetStringField(TEXT("sourceLabel"),TEXT("parser_test_only")); O->SetStringField(TEXT("sourceURL"),TEXT("https://example.invalid/parser-test-only"));
    O->SetStringField(TEXT("timeNote"),TEXT("Structural data for parser tests, not a simulation.")); O->SetStringField(TEXT("fieldNote"),TEXT("Never presented as CFD."));
    O->SetStringField(TEXT("coordinateUnit"),TEXT("m"));O->SetStringField(TEXT("velocityUnit"),TEXT("m/s"));
    O->SetStringField(TEXT("pressureUnit"),TEXT("Pa"));O->SetStringField(TEXT("densityUnit"),TEXT("kg/m3"));
    O->SetStringField(TEXT("payloadSHA256"),FString::ChrN(64,TCHAR('0')));
    auto Vector=[&](const TCHAR* Key,FVector V){TArray<TSharedPtr<FJsonValue>> A;for(int32 I=0;I<3;++I)A.Add(MakeShared<FJsonValueNumber>(V[I]));O->SetArrayField(Key,A);};
    Vector(TEXT("sourceOffset"),FVector::ZeroVector);Vector(TEXT("displayMin"),FVector(-1,-1,-1));Vector(TEXT("displayMax"),FVector(2,1,2));
    O->SetArrayField(TEXT("frameCRC32"),CRCs);
    O->SetNumberField(TEXT("meshCRC32"),MeshChecksum);
    FString Text;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&Text));
    FFileHelper::SaveStringToFile(Text,*(Directory/TEXT("recording.json")));
    return Directory/TEXT("flow.bin");
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioRecordingCache,"Studio.Data.StreamingCacheAndSecondPublishedRecording",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioRecordingCache::RunTest(const FString&)
{
    const FString Path=FPaths::ProjectContentDir()/TEXT("Samples/MeshGraphNets_Airfoil_test010/flow.bin");
    constexpr int64 FrameBytes=5233*16;
    FRecordedSolver Solver(Path,2*FrameBytes);
    TestTrue(TEXT("Second complete original trajectory loads"),Solver.LoadError().IsEmpty());
    TestEqual(TEXT("Identity from its descriptor"),Solver.Descriptor().Id,FString(TEXT("MeshGraphNets_Airfoil_test010")));
    TestEqual(TEXT("Opening does not load frame arrays"),Solver.CacheStats().ResidentFrames,0);
    TestEqual(TEXT("Original frame count"),Solver.FrameCount(),601);
    const FVector P(.03250676393508911,0,.1194048523902893);
    const auto Pinned=Solver.CaptureField(0); FStudioFieldValue V;
    TestTrue(TEXT("Independent source node available"),Pinned->Sample(P,V));
    TestTrue(TEXT("Source momentum/density X"),FMath::IsNearlyEqual(V.Velocity.X,221.55085243305282,1e-5));
    TestTrue(TEXT("Source momentum/density Y"),FMath::IsNearlyEqual(V.Velocity.Z,-2.8206439192951973,1e-5));
    TestTrue(TEXT("Source pressure"),FMath::IsNearlyEqual(V.Pressure,100223.6640625,1e-5));
    TestTrue(TEXT("Source density"),FMath::IsNearlyEqual(V.Density,1.2154428958892822,1e-8));
    const auto Before=V;
    for(int32 I=1;I<=30;++I) { auto F=Solver.CaptureField(I); TestTrue(TEXT("Streamed frame samples"),F->Sample(P,V)); }
    TestEqual(TEXT("Cache holds two frames"),Solver.CacheStats().ResidentFrames,2);
    TestEqual(TEXT("Cache meets byte budget"),Solver.CacheStats().ResidentBytes,2*FrameBytes);
    TestTrue(TEXT("Evicted immutable snapshot remains readable"),Pinned->Sample(P,V));
    TestEqual(TEXT("Pinned snapshot never changes"),V.Velocity,Before.Velocity);
    const auto Hits=Solver.CacheStats().Hits; Solver.CaptureField(30);
    TestEqual(TEXT("Repeat request uses cache"),Solver.CacheStats().Hits,Hits+1);
    TArray<TFuture<bool>> Reads;
    for(int32 I=0;I<4;++I) Reads.Add(Async(EAsyncExecution::ThreadPool,[&Solver,P,I]{FStudioFieldValue Value;return Solver.CaptureField(90+I)->Sample(P,Value);}));
    for(auto& Read:Reads) TestTrue(TEXT("Concurrent independent archives"),Read.Get());
    TestTrue(TEXT("Concurrent publication remains bounded"),Solver.CacheStats().ResidentBytes<=Solver.CacheStats().BudgetBytes);
    FRecordedSolver Original;
    FVector OriginalVelocity;
    TestTrue(TEXT("Other source vector sample available"),Original.CaptureField(0)->SampleVelocity(P,OriginalVelocity));
    TestFalse(TEXT("Second recording is different physical output"),OriginalVelocity.Equals(Before.Velocity,1e-5));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioRecordingOwnership,"Studio.Data.TrackedRecordingLifetime",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioRecordingOwnership::RunTest(const FString&)
{
    const auto Before=StudioRecordings::LiveStats();
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> Pinned;
    {
        FRecordedSolver Solver(FString(),5233*16);
        TestEqual(TEXT("Reader ownership is recorded"),StudioRecordings::LiveStats().Readers,Before.Readers+1);
        Pinned=Solver.CaptureField(0);
        Solver.CaptureField(1);
        TestEqual(TEXT("Evicted pinned and resident frames both counted"),StudioRecordings::LiveStats().Frames,Before.Frames+2);
        TestTrue(TEXT("Live allocations exceed one-frame cache when pinned"),StudioRecordings::LiveStats().FrameBytes-Before.FrameBytes>Solver.CacheStats().ResidentBytes);
    }
    TestEqual(TEXT("Pinned field retains its reader"),StudioRecordings::LiveStats().Readers,Before.Readers+1);
    Pinned.Reset();
    const auto After=StudioRecordings::LiveStats();
    TestEqual(TEXT("Reader freed after final field release"),After.Readers,Before.Readers);
    TestEqual(TEXT("All frame owners released"),After.Frames,Before.Frames);
    TestEqual(TEXT("All frame allocations released"),After.FrameBytes,Before.FrameBytes);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioRecordingSchema,"Studio.Data.GenericCountsTimesAndIntegrity",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioRecordingSchema::RunTest(const FString&)
{
    const auto Path=WriteStructuralRecording(TEXT("valid"));
    FRecordedSolver Solver(Path);
    TestTrue(TEXT("Mesh counts are not hardcoded to the airfoil"),Solver.LoadError().IsEmpty());
    TestEqual(TEXT("Arbitrary frame count"),Solver.FrameCount(),3);
    TestEqual(TEXT("Nonordinal source index"),Solver.EvaluateFrame(1).Index,59);
    TestEqual(TEXT("Nonuniform source time"),Solver.EvaluateFrame(1).Time,.4);
    FStudioFieldValue Value;
    TestTrue(TEXT("Triangle sampling"),Solver.CaptureField(1)->Sample(FVector(.25,0,.25),Value));
    TestTrue(TEXT("Barycentric scalar"),FMath::IsNearlyEqual(Value.Velocity.X,2.75,1e-8));
    TArray<uint8> Bytes; FFileHelper::LoadFileToArray(Bytes,*Path); Bytes.Last()^=0x01; FFileHelper::SaveArrayToFile(Bytes,*Path);
    const auto Invalid=Solver.CaptureField(2);
    TestFalse(TEXT("On-demand integrity check rejects changed frame"),Invalid->Sample(FVector(.25,0,.25),Value));
    TestFalse(TEXT("Corrupt frame has actionable error"),Solver.LoadError().IsEmpty());
    TestTrue(TEXT("Independent valid frame still loads"),Solver.CaptureField(0)->IsValid());
    TestFalse(TEXT("Later success cannot validate a failed immutable snapshot"),Invalid->IsValid());
    TestFalse(TEXT("Corrupt frame cannot be exported"),Solver.ExportField(2,FPaths::GetPath(Path)/TEXT("invalid.csv")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioRecordingInvalid,"Studio.Data.RejectMalformedMetadataAndMesh",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioRecordingInvalid::RunTest(const FString&)
{
    for(const auto& Path:{WriteStructuralRecording(TEXT("bad-time"),true),WriteStructuralRecording(TEXT("bad-coordinate"),false,true),WriteStructuralRecording(TEXT("bad-boundary"),false,false,true)})
    {
        FRecordedSolver Bad(Path);TestEqual(TEXT("Malformed dataset exposes no frames"),Bad.FrameCount(),0);TestFalse(TEXT("Reason supplied"),Bad.LoadError().IsEmpty());
        FStudioFieldValue V;TestFalse(TEXT("No fallback data"),Bad.CaptureField(0)->Sample(FVector(.25,0,.25),V));
    }
    FRecordedSolver TooSmall(FPaths::ProjectContentDir()/TEXT("Samples/MeshGraphNets_Airfoil/flow.bin"),16384);
    TestEqual(TEXT("Single frame must fit cache budget"),TooSmall.FrameCount(),0);
    TestFalse(TEXT("Budget failure explained"),TooSmall.LoadError().IsEmpty());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioRecordingSwitch,"Studio.Data.TransactionalSwitchCancelAndPersistence",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioRecordingSwitch::RunTest(const FString&)
{
    FStudioModel M(FPaths::ProjectDir()/TEXT("tmp/debug/recording-switch-session"));
    const auto Wait=[&]{const double Deadline=FPlatformTime::Seconds()+10;while(M.IsRecordingLoadPending()&&FPlatformTime::Seconds()<Deadline){M.Tick(0);FPlatformProcess::Sleep(.001f);}return !M.IsRecordingLoadPending();};
    const FString Second=TEXT("MeshGraphNets_Airfoil_test010");
    M.Project.Camera.Position=FVector(7,8,9); M.Scrub(.5);
    const auto Camera=M.Project.Camera; const auto Draft=StudioCaseIO::Serialize(M.Project.Draft);
    TestTrue(TEXT("Second source requested"),M.RequestRecording(Second)); M.CancelRecording();
    TestTrue(TEXT("Cancelled worker drained"),Wait());
    TestEqual(TEXT("Cancellation retains source"),M.Project.Dataset,FString(TEXT("MeshGraphNets_Airfoil_test009")));
    TestEqual(TEXT("Cancellation retains frame"),M.SelectedFrame,300);
    TestTrue(TEXT("Second source requested after cancellation"),M.RequestRecording(Second));TestTrue(TEXT("Switch completes"),Wait());
    TestEqual(TEXT("Second source committed"),M.Project.Dataset,Second);
    TestEqual(TEXT("Reader identity follows project"),M.Solver->Descriptor().Id,Second);
    TestEqual(TEXT("Camera retained"),M.Project.Camera.Position,Camera.Position);
    TestEqual(TEXT("Case draft retained"),StudioCaseIO::Serialize(M.Project.Draft),Draft);
    TestEqual(TEXT("Source starts at first actual frame"),M.SelectedFrame,0);
    TestEqual(TEXT("Original recording retained in project history"),M.Project.Runs.Num(),2);
    const FString File=FPaths::ProjectDir()/TEXT("tmp/debug/recording-tests/second.lbms");
    M.Scrub(.7);TestTrue(TEXT("Second source saves"),M.SaveProject(File));
    FStudioModel Reopened(FPaths::ProjectDir()/TEXT("tmp/debug/recording-switch-reopen"));
    TestTrue(TEXT("Project reopens its own source"),Reopened.LoadProject(File));
    TestEqual(TEXT("Recording restored"),Reopened.Solver->Descriptor().Id,Second);TestEqual(TEXT("Frame restored"),Reopened.SelectedFrame,420);
    TestEqual(TEXT("Camera restored"),Reopened.Project.Camera.Position,Camera.Position);
    const auto Before=StudioProjectIO::Serialize(Reopened.SnapshotProject());
    TestFalse(TEXT("Unknown recording rejected"),Reopened.RequestRecording(TEXT("missing-recording")));
    TestEqual(TEXT("Failure leaves document untouched"),StudioProjectIO::Serialize(Reopened.SnapshotProject()),Before);
    TestTrue(TEXT("Return to original request"),M.RequestRecording(TEXT("MeshGraphNets_Airfoil_test009")));
    M.NewProject(TEXT("Replacement during load"));TestTrue(TEXT("Old completion drained"),Wait());
    TestEqual(TEXT("New project preserved"),M.Project.Name,FString(TEXT("Replacement during load")));
    TestEqual(TEXT("No stale run inserted"),M.Project.Runs.Num(),1);
    return true;
}
#endif
