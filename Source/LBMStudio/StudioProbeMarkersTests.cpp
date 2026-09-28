#include "StudioProbeMarkers.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioProbeMarkersTest,"Studio.Inspection.OriginalPointMarkers",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioProbeMarkersTest::RunTest(const FString&)
{
    const auto Source=StudioRecordings::Import(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json"),0,{});
    if(!TestTrue(*Source.Error,Source.Source.IsValid()&&Source.Reference.IsSet()))return false;
    const auto Field=Source.Source->CaptureViewField(0,TEXT("pressure"),false);
    const auto Points=Field->OriginalPoints();if(!TestTrue(TEXT("Published point geometry available"),Points.IsValid()))return false;
    FStudioProbeMarkerRequest R;R.Project=FGuid::NewGuid();R.Geometry=Points->Geometry;
    R.Source={Source.Reference->Id,Source.Reference->MetadataSHA256,Source.Reference->PayloadSHA256};
    R.Offset=FVector(1,2,3);
    FStudioProbeObject A;A.Source=R.Source;A.Method=EStudioProbeMethod::OriginalPoint;A.PointId=17;
    auto B=A;B.Id=FGuid::NewGuid();auto Missing=A;Missing.Id=FGuid::NewGuid();Missing.PointId=MAX_int64;
    R.Queries={{A.Id,17},{B.Id,17},{Missing.Id,MAX_int64}};
    const auto Result=StudioProbeMarkers::Resolve(R);
    const FVector Expected(.1636374593+1,2,-.2851103544+3); // Independently decoded original HDF5 row 17.
    TestTrue(TEXT("Recorded coordinates plus source offset are exact"),Result.Position(R.Project,A).IsSet()&&Result.Position(R.Project,A).GetValue()==Expected);
    TestTrue(TEXT("Duplicate probes resolve the same source ID"),Result.Position(R.Project,B).IsSet()&&Result.Position(R.Project,B).GetValue()==Expected);
    TestFalse(TEXT("Unknown 64-bit ID has no substitute marker"),Result.Position(R.Project,Missing).IsSet());
    TestFalse(TEXT("Different project rejects previous markers"),Result.Position(FGuid::NewGuid(),A).IsSet());
    auto Changed=A;Changed.PointId=100;TestFalse(TEXT("Edited ID immediately rejects prior position"),Result.Position(R.Project,Changed).IsSet());
    Changed=A;Changed.Source.Dataset+=TEXT("other");TestFalse(TEXT("Changed source rejects marker"),Result.Position(R.Project,Changed).IsSet());
    Changed=A;Changed.bVisible=false;TestFalse(TEXT("Hidden marker is unavailable"),Result.Position(R.Project,Changed).IsSet());
    const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    const auto Cancelled=StudioProbeMarkers::Resolve(R,Cancel);TestTrue(TEXT("Cancelled scan publishes no positions"),Cancelled.bCancelled&&Cancelled.Positions.IsEmpty());

    FStudioProbeMarkerScheduler Worker;Worker.Submit(R);
    auto New=R;New.Queries={{A.Id,100}};Worker.Submit(New);
    const double End=FPlatformTime::Seconds()+5;
    while(!Worker.Result()&&FPlatformTime::Seconds()<End){Worker.Tick();FPlatformProcess::Sleep(.001f);}
    if(!TestNotNull(TEXT("Latest marker request completed"),Worker.Result()))return false;
    Changed=A;Changed.PointId=100;
    const auto Position=Worker.Result()->Position(New.Project,Changed);
    TestTrue(TEXT("Only latest request can publish"),Position.IsSet()&&Position.GetValue()==FVector(.348398+1,2,-.2414756417+3));
    const auto Started=Worker.StartedRequests();
    for(int32 I=0;I<100;++I)Worker.Submit(New);
    TestEqual(TEXT("Playback/camera ticks reuse static geometry result"),Worker.StartedRequests(),Started);
    Worker.Clear();TestNull(TEXT("Clear immediately removes markers"),Worker.Result());
    return true;
}
#endif
