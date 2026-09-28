#include "StudioProbeMonitor.h"
#include "StudioModel.h"
#include "StudioMonitorExport.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto ProbeMonitorFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FStudioProbeHistoryRequest MonitorProbeRequest(const FStudioRecordingLoadResult& Source)
{
    FStudioProbeHistoryRequest R;R.ProjectId=FGuid::NewGuid();R.Source=Source.Source;R.LastOrdinal=2;R.Scalar=TEXT("pressure");
    R.Probe.Name=TEXT("Trailing \"edge\", pressure");R.Probe.Source={Source.Reference->Id,Source.Reference->MetadataSHA256,Source.Reference->PayloadSHA256};
    R.Probe.Method=EStudioProbeMethod::OriginalPoint;R.Probe.PointId=0;return R;
}
FStudioRecordingLoadResult MonitorProbeSource()
{return StudioRecordings::Import(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json"),0,{});}
bool FinishProbeMonitor(FStudioProbeMonitorSession& S,const FStudioProbeHistoryRequest& R,const FStudioInspectionObjects& Objects)
{
    const double End=FPlatformTime::Seconds()+10;
    do {S.Tick(R.ProjectId,R.Source,Objects);if(!S.IsBusy())return true;FPlatformProcess::SleepNoStats(.001f);}while(FPlatformTime::Seconds()<End);
    return false;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProbeMonitorChart,"Studio.ProbeMonitor.OriginalValuesGapsAndPlotting",ProbeMonitorFlags)
bool FProbeMonitorChart::RunTest(const FString&)
{
    const auto Source=MonitorProbeSource();if(!TestTrue(*Source.Error,Source.Source.IsValid()))return false;
    auto R=MonitorProbeRequest(Source);FString Error;
    auto Samples=MakeShared<FStudioProbeHistoryResult,ESPMode::ThreadSafe>(StudioProbeHistory::Evaluate(R));
    const auto H=StudioProbeMonitor::MakeHistory(Samples,Error);
    if(!TestTrue(*Error,H.IsValid()))return false;
    auto S=StudioMonitor::Defaults(*H);const auto Plot=StudioMonitor::BuildPlot(*H,S,10);
    TestTrue(TEXT("Source pressure supports a normal chart"),Plot.Error.IsEmpty()&&Plot.Traces.Num()==1&&Plot.Traces[0].Samples.Num()==3);
    for(int32 I=0;I<3;++I)
    {TestEqual(TEXT("Chart retains exact source values"),H->Columns[0].Values[I],Samples->Frames[I].Samples[0].Value.GetValue());TestEqual(TEXT("Chart retains original times"),H->Times[I],Samples->Frames[I].Frame.Time);}
    TestTrue(TEXT("History is explicitly bound to its recording"),H->FieldRecordingId==TOptional<FString>(R.Probe.Source.Dataset));
    // Gap control uses a genuinely absent original ID, not generated numbers.
    R.Probe.PointId=MAX_int64;const auto Missing=StudioProbeMonitor::MakeHistory(MakeShared<FStudioProbeHistoryResult,ESPMode::ThreadSafe>(StudioProbeHistory::Evaluate(R)),Error);
    if(!TestTrue(*Error,Missing.IsValid()))return false;
    const auto Empty=StudioMonitor::BuildPlot(*Missing,StudioMonitor::Defaults(*Missing),200);
    TestTrue(TEXT("Missing source values are explicit plot gaps"),Empty.Error.Contains(TEXT("No available samples"))&&Empty.Traces[0].MissingSamples==3&&Empty.Traces[0].Samples.IsEmpty());
    TestTrue(TEXT("Gap never becomes zero in chart storage"),FMath::IsNaN(Missing->Columns[0].Values[0]));
    // Mix original-valued and original-missing snapshots to exercise gap geometry only.
    auto Mixed=MakeShared<FStudioProbeHistoryResult,ESPMode::ThreadSafe>(*Samples);
    Mixed->Frames[1].Samples[0]=Missing->ProbeHistory->Frames[1].Samples[0];Mixed->Frames[1].Samples[0].PointId=R.Probe.PointId=Samples->Probe.PointId;
    const auto GapHistory=StudioProbeMonitor::MakeHistory(Mixed,Error);
    if(!TestTrue(*Error,GapHistory.IsValid()))return false;
    const auto GapPlot=StudioMonitor::BuildPlot(*GapHistory,StudioMonitor::Defaults(*GapHistory),1);
    TestTrue(TEXT("Pixel reduction cannot connect across a missing sample"),GapPlot.Error.IsEmpty()&&GapPlot.Traces[0].Samples==TArray<int32>{0,INDEX_NONE,2});
    Mixed->Status=EStudioProbeHistoryStatus::ReadFailed;TestFalse(TEXT("Partial/failed results cannot enter Monitors"),StudioProbeMonitor::MakeHistory(Mixed,Error).IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProbeMonitorExport,"Studio.ProbeMonitor.FrozenCSVAndFrameLinks",ProbeMonitorFlags)
bool FProbeMonitorExport::RunTest(const FString&)
{
    const auto Source=MonitorProbeSource();if(!TestTrue(*Source.Error,Source.Source.IsValid()))return false;
    auto R=MonitorProbeRequest(Source);FString Error,CSV;int32 Rows=-1;
    const auto H=StudioProbeMonitor::MakeHistory(MakeShared<FStudioProbeHistoryResult,ESPMode::ThreadSafe>(StudioProbeHistory::Evaluate(R)),Error);
    if(!TestTrue(*Error,H.IsValid()))return false;
    auto S=StudioMonitor::Defaults(*H);S.bManualTime=true;S.TimeMinimum=H->Times[1];S.TimeMaximum=H->Times[2];
    S.bLogY=true;
    if(!TestTrue(*Error,StudioMonitorExport::CSV(*H,S,CSV,Rows,Error)))return false;
    TestEqual(TEXT("Inclusive range exports every selected original time"),Rows,2);
    TestTrue(TEXT("CSV preserves quoted probe names"),CSV.Contains(TEXT("\"Trailing \"\"edge\"\", pressure\"")));
    TestTrue(TEXT("CSV identifies original frames and source positions"),CSV.Contains(TEXT("frame_ordinal,source_step,time_s,sample_index"))&&CSV.Contains(TEXT("source_x_m,source_y_m,source_z_m,original_point_id,status,value")));
    TestTrue(TEXT("CSV pins the original source identity"),CSV.Contains(R.Probe.Source.MetadataSHA256));
    for(int32 I=1;I<=2;++I)
    {
        const auto& F=H->ProbeHistory->Frames[I];
        TestTrue(TEXT("CSV carries original frame, step and physical time"),CSV.Contains(FString::Printf(TEXT("%d,%d,%.17g,0,"),F.Ordinal,F.Frame.Index,F.Frame.Time)));
        TestTrue(TEXT("Log display does not remove nonpositive exported samples"),CSV.Contains(FString::Printf(TEXT(",\"value\",%.17g\n"),F.Samples[0].Value.GetValue())));
    }
    R.Probe.PointId=MAX_int64;const auto Missing=StudioProbeMonitor::MakeHistory(MakeShared<FStudioProbeHistoryResult,ESPMode::ThreadSafe>(StudioProbeHistory::Evaluate(R)),Error);
    if(!TestTrue(*Error,StudioMonitorExport::CSV(*Missing,StudioMonitor::Defaults(*Missing),CSV,Rows,Error)))return false;
    TestTrue(TEXT("Missing values export empty numeric cells with an explicit reason"),CSV.Contains(TEXT(",\"original_point_not_found\",\n"))&&!CSV.Contains(TEXT("nan")));
    S.Series.Reset();CSV=TEXT("unchanged");Rows=73;
    TestFalse(TEXT("Empty selection cannot create an empty claimed export"),StudioMonitorExport::CSV(*H,S,CSV,Rows,Error));
    TestTrue(TEXT("Failed export preserves caller output"),CSV==TEXT("unchanged")&&Rows==73);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProbeMonitorLifetime,"Studio.ProbeMonitor.SessionInvalidationAndExplicitRegeneration",ProbeMonitorFlags)
bool FProbeMonitorLifetime::RunTest(const FString&)
{
    const auto Source=MonitorProbeSource();if(!TestTrue(*Source.Error,Source.Source.IsValid()))return false;
    auto R=MonitorProbeRequest(Source);FStudioInspectionObjects Objects;Objects.Probes.Add(R.Probe);FStudioProbeMonitorSession Session;
    Session.Select(R);TestFalse(TEXT("Selecting a probe performs no background read"),Session.IsBusy());
    TestTrue(TEXT("Explicit generation starts"),Session.Generate());
    if(!TestTrue(TEXT("Original history finishes"),FinishProbeMonitor(Session,R,Objects)&&Session.History().IsValid()))return false;
    const auto Original=Session.History();auto S=Session.Settings();S.bLogY=true;TestTrue(TEXT("Session chart edits apply"),Session.UpdateSettings(S));
    for(int32 I=0;I<10;++I)Session.Tick(R.ProjectId,R.Source,Objects);
    TestTrue(TEXT("Unchanged recording/probe retains values across unrelated activity"),Session.History()==Original&&Session.Settings().bLogY);
    Session.SetRange(1,2);TestFalse(TEXT("Changing generation range removes old values immediately"),Session.History().IsValid());
    TestTrue(TEXT("New range requires explicit generation"),!Session.IsBusy()&&Session.Generate());
    if(!TestTrue(TEXT("New range finishes without decimation"),FinishProbeMonitor(Session,R,Objects)&&Session.History().IsValid()&&Session.History()->Times.Num()==2))return false;
    Objects.Probes[0].A.X+=1;Session.Tick(R.ProjectId,R.Source,Objects);
    TestTrue(TEXT("Same-ID edits invalidate data and request"),!Session.History()&&!Session.Selection().IsSet());
    Objects.Probes[0]=R.Probe;Session.Select(R);Session.Generate();Session.Tick(FGuid::NewGuid(),R.Source,Objects);
    TestTrue(TEXT("Project replacement cancels and removes selection"),!Session.Selection().IsSet()&&!Session.History());
    TestTrue(TEXT("Old project work drains without publication"),FinishProbeMonitor(Session,R,Objects)&&!Session.History());
    Session.Select(R);Session.Generate();Session.Cancel();
    TestTrue(TEXT("Cancellation does not publish a completed race"),FinishProbeMonitor(Session,R,Objects)&&!Session.History());
    Session.Select(R);Session.SetRange(0,1000000);TestFalse(TEXT("Invalid range reports a recoverable error"),Session.Generate());
    TestTrue(TEXT("Invalid generation remains idle"),!Session.IsBusy()&&!Session.Notice.IsEmpty());
    return true;
}
#endif
