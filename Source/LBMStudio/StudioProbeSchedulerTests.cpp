#include "StudioProbeScheduler.h"
#include "StudioModel.h"
#include "StudioPointRecording.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto ProbeSchedulerFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
bool FinishProbe(FStudioProbeScheduler& Scheduler,bool bExpectResult)
{
    const double Deadline=FPlatformTime::Seconds()+5.;
    do
    {
        Scheduler.Tick();
        if(bExpectResult?Scheduler.Result()!=nullptr:!Scheduler.HasPendingWork())return true;
        FPlatformProcess::SleepNoStats(.001f);
    }while(FPlatformTime::Seconds()<Deadline);
    return false;
}
FStudioProbeRequest ScheduledProbe(const FStudioRecordingLoadResult& Source,int32 Frame)
{
    FStudioProbeRequest R;R.ProjectId=FGuid::NewGuid();R.PresentationId=1;
    R.Probe.Name=TEXT("Source point zero");R.Probe.Method=EStudioProbeMethod::OriginalPoint;R.Probe.PointId=0;
    R.Probe.Source={Source.Reference->Id,Source.Reference->MetadataSHA256,Source.Reference->PayloadSHA256};
    R.Probe.Field=TEXT("pressure");R.DisplayedScalar=TEXT("velocity_magnitude");R.Field=Source.Source->CaptureViewField(Frame,R.DisplayedScalar,false);return R;
}
TOptional<double> ExpectedProbePressure(int32 Frame)
{
    FString Text;TSharedPtr<FJsonObject> JSON;
    if(!FFileHelper::LoadFileToString(Text,*(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/expected.json")))||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),JSON))return {};
    for(const auto& Value:JSON->GetArrayField(TEXT("samples")))
    {
        const auto E=Value->AsObject();
        if(E->GetStringField(TEXT("field"))==TEXT("pressure")&&E->GetNumberField(TEXT("point"))==0&&E->GetNumberField(TEXT("frame"))==Frame)
            return E->GetNumberField(TEXT("value"));
    }
    return {};
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioProbeLatest,"Studio.Inspection.LatestProbeWorkerAndCameraReuse",ProbeSchedulerFlags)
bool FStudioProbeLatest::RunTest(const FString&)
{
    const auto Source=StudioRecordings::Import(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json"),0,{});
    if(!TestTrue(*Source.Error,Source.Source.IsValid()&&Source.Reference.IsSet()))return false;
    FStudioProbeScheduler Scheduler;auto Request=ScheduledProbe(Source,0);Scheduler.Submit(Request);
    for(int32 I=2;I<=100;++I){Request.PresentationId=I;Scheduler.Submit(Request);}
    if(!TestTrue(TEXT("Camera motion does not starve the current field query"),FinishProbe(Scheduler,true)))return false;
    TestEqual(TEXT("Camera-only captures reuse a single scientific query"),Scheduler.StartedRequests(),uint64(1));
    TestTrue(TEXT("Reused answer matches the latest capture"),Scheduler.Result()->Matches(Request));
    const auto Expected0=ExpectedProbePressure(0);
    if(!TestTrue(TEXT("Independent original pressure expectation and sample exist"),Expected0.IsSet()&&
        Scheduler.Result()->Samples.Num()==1&&Scheduler.Result()->Samples[0].Value.IsSet()))return false;
    TestEqual(TEXT("Worker returns the original published value"),Scheduler.Result()->Samples[0].Value.GetValue(),Expected0.GetValue());

    // Scrubs replace one pending request. No queue grows with the number of UI events.
    const TSharedPtr<const IStudioField,ESPMode::ThreadSafe> Frame0=Request.Field,
        Frame2=Source.Source->CaptureViewField(2,TEXT("velocity_magnitude"),false);
    for(int32 I=0;I<128;++I)
    {
        Request.PresentationId=101+I;Request.Field=(I&1)?Frame0:Frame2;
        Scheduler.Submit(Request);
        if(const auto* Visible=Scheduler.Result())TestTrue(TEXT("No stale frame appears during scrubbing"),Visible->Matches(Request));
    }
    Request.PresentationId=300;Request.Field=Frame2;Scheduler.Submit(Request);
    if(!TestTrue(TEXT("Latest scrub eventually supplies a current answer"),FinishProbe(Scheduler,true)))return false;
    const auto Expected2=ExpectedProbePressure(2);
    if(!TestTrue(TEXT("Independent final-frame expectation and sample exist"),Expected2.IsSet()&&
        Scheduler.Result()->Samples.Num()==1&&Scheduler.Result()->Samples[0].Value.IsSet()&&Scheduler.Result()->Identity.IsSet()))return false;
    TestEqual(TEXT("Latest original frame is sampled"),Scheduler.Result()->Identity->Ordinal,2);
    TestEqual(TEXT("Latest pressure agrees with original-HDF5 extraction"),Scheduler.Result()->Samples[0].Value.GetValue(),Expected2.GetValue());
    TestTrue(TEXT("Obsolete work is cancelled"),Scheduler.CancelledRequests()>0);
    TestTrue(TEXT("Published result matches the entire latest request"),Scheduler.Result()->Matches(Request));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioProbeLifetime,"Studio.Inspection.ProbeWorkerClearAndShutdown",ProbeSchedulerFlags)
bool FStudioProbeLifetime::RunTest(const FString&)
{
    const auto Source=StudioRecordings::Import(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json"),0,{});
    if(!TestTrue(*Source.Error,Source.Source.IsValid()&&Source.Reference.IsSet()))return false;
    auto Request=ScheduledProbe(Source,0);FStudioProbeScheduler Scheduler;Scheduler.Submit(Request);
    if(!TestTrue(TEXT("Initial probe can finish"),FinishProbe(Scheduler,true)))return false;
    Scheduler.Clear();TestNull(TEXT("Deleting selection removes its visible value immediately"),Scheduler.Result());
    TestTrue(TEXT("Clear drains existing work without starting another task"),FinishProbe(Scheduler,false));
    const uint64 Started=Scheduler.StartedRequests();
    for(int32 I=0;I<20;++I)Scheduler.Tick();
    TestEqual(TEXT("No selection means no background sampling"),Scheduler.StartedRequests(),Started);
    Request.ProjectId=FGuid::NewGuid();++Request.PresentationId;Scheduler.Submit(Request);
    if(!TestTrue(TEXT("A new project can request the same immutable recording"),FinishProbe(Scheduler,true)))return false;
    TestTrue(TEXT("New result belongs to the new project"),Scheduler.Result()->Matches(Request));
    Request.Probe.PointId=MAX_int64;++Request.PresentationId;Scheduler.Submit(Request);
    Scheduler.Shutdown();TestFalse(TEXT("Shutdown joins the only owned worker"),Scheduler.HasPendingWork());
    TestNull(TEXT("Shutdown publishes no stale values"),Scheduler.Result());
    const uint64 Stopped=Scheduler.StartedRequests();Scheduler.Submit(Request);Scheduler.Tick();
    TestEqual(TEXT("A destroyed viewport cannot restart sampling"),Scheduler.StartedRequests(),Stopped);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioProbeOptionalBudget,"Studio.Inspection.OptionalScalarBudgetAndRetry",ProbeSchedulerFlags)
bool FStudioProbeOptionalBudget::RunTest(const FString&)
{
    FStudioPointReadOptions Options;Options.CacheBytes=0;Options.LiveArrayBytes=2LL*18706*int64(sizeof(double));
    const auto Open=StudioPointRecordings::Open(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json"),Options);
    if(!TestTrue(*Open.Error,Open.Recording.IsValid()))return false;
    const auto Solver=MakeShared<FPointRecordedSolver,ESPMode::ThreadSafe>(Open.Recording.ToSharedRef());
    FStudioProbeRequest Request;Request.ProjectId=FGuid::NewGuid();Request.PresentationId=1;
    Request.Probe.Name=TEXT("Independent pressure");Request.Probe.Method=EStudioProbeMethod::OriginalPoint;Request.Probe.PointId=0;
    Request.Probe.Source={Solver->Descriptor().Id,Solver->Descriptor().MetadataSHA256,FString()};Request.Probe.Field=TEXT("pressure");
    Request.DisplayedScalar=TEXT("velocity_magnitude");Request.Field=Solver->CaptureViewField(0,Request.DisplayedScalar,false);
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> Other=Solver->CaptureViewField(1,TEXT("velocity_u"),false);
    if(!TestTrue(TEXT("Both budget slots are held by real source arrays"),Request.Field->IsValid()&&Other->IsValid()))return false;
    FStudioProbeScheduler Scheduler;Scheduler.Submit(Request);
    if(!TestTrue(TEXT("Budget failure completes without blocking UI"),FinishProbe(Scheduler,true)))return false;
    TestTrue(TEXT("Budget exhaustion is a load failure with no substitute values"),Scheduler.Result()->Status==EStudioProbeStatus::FieldLoadFailed&&Scheduler.Result()->Samples.IsEmpty());
    TestTrue(TEXT("Probe failure does not poison viewport solver state"),Solver->LoadError().IsEmpty());
    const uint64 Started=Scheduler.StartedRequests();
    for(int32 I=0;I<32;++I)Scheduler.Submit(Request);
    TestEqual(TEXT("Failed load does not retry on every UI tick"),Scheduler.StartedRequests(),Started);
    Other.Reset();Scheduler.Clear();Scheduler.Submit(Request);
    if(!TestTrue(TEXT("Explicit retry finishes after a pinned array releases"),FinishProbe(Scheduler,true)))return false;
    const auto Expected=ExpectedProbePressure(0);
    if(!TestTrue(TEXT("Retried pressure has a source value"),Expected.IsSet()&&Scheduler.Result()->Status==EStudioProbeStatus::Ready&&
        Scheduler.Result()->Samples.Num()==1&&Scheduler.Result()->Samples[0].Value.IsSet()))return false;
    TestEqual(TEXT("Retry reads the independent published pressure"),Scheduler.Result()->Samples[0].Value.GetValue(),Expected.GetValue());
    TestTrue(TEXT("Returned result matches original displayed snapshot"),Scheduler.Result()->Matches(Request));
    TestEqual(TEXT("Result retains copied samples, not its optional array"),Open.Recording->Stats().LiveArrayBytes,int64(18706*sizeof(double)));
    TestTrue(TEXT("Optional reads honor the existing hard live-array budget"),Open.Recording->Stats().PeakLiveArrayBytes<=Options.LiveArrayBytes);
    Scheduler.Shutdown();Request.Field.Reset();
    TestEqual(TEXT("Shutdown and release return live values to zero"),Open.Recording->Stats().LiveArrayBytes,int64(0));
    return true;
}
#endif
