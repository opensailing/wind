#include "StudioPerformance.h"
#include "Misc/AutomationTest.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPerformanceMeasurements,"Studio.Performance.MeasuredIntervalsAndMissingCounters",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioPerformanceMeasurements::RunTest(const FString&)
{
    FStudioPerformanceHistory H;int32 Reads=0;FStudioApplicationCounters C;
    C.ProcessCPUSeconds=10.;C.FootprintBytes=uint64(1024);C.Captures=uint64(20);
    auto Read=[&]{++Reads;return C;};
    H.Observe(0,.2,Read);for(int32 I=1;I<=100;++I)H.Observe(double(I)/100.,.2,Read);
    TestEqual(TEXT("Platform read is limited to one per second"),Reads,1);
    TestEqual(TEXT("Real tick intervals establish cadence"),H.Samples().Last().UICadenceMs,10.);
    TestTrue(TEXT("Update work is independent from cadence"),FMath::IsNearlyEqual(H.Samples().Last().UIUpdateMs,.2,1.e-8));
    TestFalse(TEXT("First CPU counter needs a baseline"),H.Samples().Last().ProcessCPUPercent.IsSet());
    C.ProcessCPUSeconds=12.5;C.Captures=uint64(25);
    for(int32 I=101;I<=200;++I)H.Observe(double(I)/100.,.3,Read);
    TestEqual(TEXT("Multicore CPU may exceed 100 percent"),H.Samples().Last().ProcessCPUPercent.Get(-1.),250.);
    TestEqual(TEXT("Scene captures use measured count delta"),H.Samples().Last().CapturesPerSecond.Get(-1.),5.);
    C.ProcessCPUSeconds.Reset();C.FootprintBytes.Reset();C.Captures.Reset();H.Observe(3,.1,Read);
    TestFalse(TEXT("Missing platform values are not carried forward"),H.Samples().Last().ProcessCPUPercent.IsSet() || H.Samples().Last().Counters.FootprintBytes.IsSet() || H.Samples().Last().CapturesPerSecond.IsSet());
    C.ProcessCPUSeconds=15.;C.Captures=uint64(30);H.Observe(4,.1,Read);
    TestFalse(TEXT("CPU rate never bridges a missing read"),H.Samples().Last().ProcessCPUPercent.IsSet());
    H.Observe(5,.1,Read);TestEqual(TEXT("Stationary actual capture count reports zero"),H.Samples().Last().CapturesPerSecond.Get(-1.),0.);
    const auto Actual=StudioPerformance::ReadCounters(nullptr);
#if PLATFORM_MAC
    TestTrue(TEXT("macOS reports the real current process footprint"),Actual.FootprintBytes.IsSet()&&*Actual.FootprintBytes>0);
    TestTrue(TEXT("macOS reports real cumulative CPU time"),Actual.ProcessCPUSeconds.IsSet()&&*Actual.ProcessCPUSeconds>=0);
#endif
    TestFalse(TEXT("Absent renderer cannot supply capture counts"),Actual.Captures.IsSet());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPerformanceBounds,"Studio.Performance.GapsValidationAndBoundedRetention",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioPerformanceBounds::RunTest(const FString&)
{
    FStudioPerformanceHistory H;double CPU=0;int32 Reads=0;
    auto Read=[&]{FStudioApplicationCounters C;C.ProcessCPUSeconds=CPU++;++Reads;return C;};
    H.Observe(0,0,Read);for(int32 I=1;I<=240;++I)H.Observe(I,.1,Read);
    TestEqual(TEXT("Retention is bounded to 120 actual readings"),H.Samples().Num(),120);
    TestEqual(TEXT("Oldest retained timestamp is exact"),H.Samples()[0].At,121.);
    H.Observe(240,.1,Read);H.Observe(239,.1,Read);H.Observe(241,-1,Read);
    H.Observe(std::numeric_limits<double>::quiet_NaN(),.1,Read);
    TestEqual(TEXT("Invalid ticks never read the platform or alter history"),Reads,240);
    H.Observe(250,.1,Read);TestEqual(TEXT("Long gap starts a fresh interval"),Reads,240);
    H.Observe(251,.1,Read);TestFalse(TEXT("No CPU throughput across minimized/suspended gap"),H.Samples().Last().ProcessCPUPercent.IsSet());
    H.Observe(252,.1,Read);TestEqual(TEXT("Measurement resumes from new interval"),H.Samples().Last().ProcessCPUPercent.Get(-1.),100.);
    CPU=0;H.Observe(253,.1,Read);TestFalse(TEXT("Regressing CPU counter cannot report negative utilization"),H.Samples().Last().ProcessCPUPercent.IsSet());
    return true;
}
#endif
