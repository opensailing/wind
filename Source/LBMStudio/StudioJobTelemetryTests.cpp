#include "StudioJobs.h"
#include "Misc/AutomationTest.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
    // Synthetic protocol values exercise validation/math only. This adapter is
    // never registered with the application and supplies no CFD fixture/data.
    class FMeasurementAdapter final : public IStudioJobAdapter
    {
    public:
        FStudioJobRequest Request;
        TArray<FStudioJobEvent> Events;
        TArray<FStudioJobMeasurement> Samples;
        uint64 EventSequence=0,LatestState=0;
        int32 TelemetryPolls=0;
        FStudioJobCapabilities Caps;
        FMeasurementAdapter()
        {
            Caps.BackendId=TEXT("telemetry-protocol-test");
            Caps.bPause=Caps.bReconnect=Caps.bTelemetry=true;
        }
        FStudioJobCapabilities Capabilities() const override {return Caps;}
        void State(EStudioJobState Value,uint64 Command=0)
        {
            FStudioJobEvent E;E.RunId=Request.RunId;E.Sequence=++EventSequence;E.CommandId=Command;E.State=Value;
            LatestState=E.Sequence;Events.Add(E);
        }
        void Send(const FStudioJobRequest& R,double) override
        {
            Request=R;
            if(R.Command==EStudioJobCommand::Submit)
            {
                EventSequence=0;Events.Reset();Samples.Reset();
                State(EStudioJobState::Preparing,R.CommandId);State(EStudioJobState::Queued,R.CommandId);
                State(EStudioJobState::Running,R.CommandId);
            }
            else if(R.Command==EStudioJobCommand::Pause)State(EStudioJobState::Paused,R.CommandId);
            else if(R.Command==EStudioJobCommand::Resume || R.Command==EStudioJobCommand::Reconnect)State(EStudioJobState::Running,R.CommandId);
            else if(R.Command==EStudioJobCommand::Stop)State(EStudioJobState::Stopped,R.CommandId);
        }
        void Poll(double,int32 Max,TArray<FStudioJobEvent>& Out) override
        {const int32 N=FMath::Min(Max,Events.Num());Out.Append(Events.GetData(),N);Events.RemoveAt(0,N,EAllowShrinking::No);}
        void PollTelemetry(double,int32 Max,TArray<FStudioJobMeasurement>& Out) override
        {++TelemetryPolls;const int32 N=FMath::Min(Max,Samples.Num());Out.Append(Samples.GetData(),N);Samples.RemoveAt(0,N,EAllowShrinking::No);}
        FStudioJobMeasurement Sample(uint64 Sequence,double Wall,int64 Steps=100) const
        {
            FStudioJobMeasurement S;S.RunId=Request.RunId;S.Sequence=Sequence;S.StateSequence=LatestState;
            S.Reporter=TEXT("protocol fixture");S.Host=TEXT("test host");S.Device=TEXT("test device");
            S.WallSeconds=Wall;S.CompletedSteps=Steps;S.PhysicalSeconds=double(Steps)/1000.;
            S.StopAfterSteps=1000;S.StopAtPhysicalSeconds=2.;S.StopRule=EStudioJobStopRule::FirstReportedLimit;
            S.HostResidentBytes=uint64(1024);S.DeviceUsedBytes=uint64(2048);S.DeviceTotalBytes=uint64(4096);S.DeviceUtilizationPercent=25.;
            return S;
        }
    };
    FStudioCaseDraft Draft()
    {FStudioCaseDraft D;D.Setup.BackendId=TEXT("telemetry-protocol-test");D.Setup.MaxSteps=9999;return D;}
    struct FFixture
    {
        FMeasurementAdapter* Adapter=nullptr;
        FStudioJobController Job;
        TUniquePtr<IStudioJobAdapter> Create()
        {auto A=MakeUnique<FMeasurementAdapter>();Adapter=A.Get();return A;}
        FFixture():Job(Create()) {Job.Submit(TEXT("Protocol test"),Draft(),0);Job.Tick(.1);}
        void Push(const FStudioJobMeasurement& S,double At)
        {Adapter->Samples.Add(S);Job.Tick(At);}
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioTelemetryRates,"Studio.Jobs.Telemetry.ReportedLimitsAndMeasuredRates",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioTelemetryRates::RunTest(const FString&)
{
    FFixture F;auto& J=F.Job;auto& A=*F.Adapter;
    TestTrue(TEXT("No numerical telemetry inferred from Running"),J.Telemetry().Status==EStudioTelemetryStatus::Unavailable);
    auto S=A.Sample(1,1);S.PhysicalSeconds=.25;F.Push(S,1);
    auto V=J.Telemetry();TestFalse(TEXT("One sample cannot establish rate or ETA"),V.StepsPerSecond.IsSet() || V.EstimatedRemainingSeconds.IsSet());
    S=A.Sample(2,3,300);S.PhysicalSeconds=.75;F.Push(S,3);V=J.Telemetry();
    TestEqual(TEXT("Backend source retained"),V.BackendId,FString(TEXT("telemetry-protocol-test")));
    TestEqual(TEXT("Authoritative count retained"),V.Sample->CompletedSteps.Get(-1),int64(300));
    TestEqual(TEXT("Requested case limit not used as actual limit"),V.StepProgress.Get(-1.),.3);
    TestEqual(TEXT("Physical progress uses reported physical limit"),V.PhysicalProgress.Get(-1.),.375);
    TestEqual(TEXT("First reported limit controls progress"),V.Progress.Get(-1.),.375);
    TestEqual(TEXT("Measured solver steps per elapsed second"),V.StepsPerSecond.Get(-1.),100.);
    TestEqual(TEXT("Measured physical seconds per elapsed second"),V.PhysicalSecondsPerSecond.Get(-1.),.25);
    TestEqual(TEXT("Earliest measured limit controls ETA"),V.EstimatedRemainingSeconds.Get(-1.),5.);
    TestEqual(TEXT("Command completion count remains independent"),J.CompletedStepCommands(),uint64(0));
    TestEqual(TEXT("Numerical samples do not flood command/event log"),J.Events().Num(),3);
    TestEqual(TEXT("Resource byte identity is exact"),V.Sample->DeviceUsedBytes.Get(0),uint64(2048));
    TestEqual(TEXT("Device attribution retained"),V.Sample->Device,FString(TEXT("test device")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioTelemetryValidation,"Studio.Jobs.Telemetry.RejectInvalidAndForeignMeasurements",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioTelemetryValidation::RunTest(const FString&)
{
    FFixture F;auto& A=*F.Adapter;F.Push(A.Sample(1,1),1);
    auto Reject=[&](const TCHAR* Label,TFunction<void(FStudioJobMeasurement&)> Change)
    {auto S=A.Sample(2,2,200);Change(S);F.Push(S,2);TestEqual(Label,F.Job.TelemetryHistory().Num(),1);};
    Reject(TEXT("Foreign run"),[](auto& S){S.RunId=FGuid::NewGuid();});
    Reject(TEXT("Previous state"),[](auto& S){S.StateSequence=1;});
    Reject(TEXT("Duplicate sequence"),[](auto& S){S.Sequence=1;});
    Reject(TEXT("Nonincreasing solver clock"),[](auto& S){S.WallSeconds=1;});
    Reject(TEXT("Negative age"),[](auto& S){S.SampleAgeSeconds=-1;});
    Reject(TEXT("Older delivery timestamp"),[](auto& S){S.SampleAgeSeconds=2;});
    Reject(TEXT("Regressing count"),[](auto& S){S.CompletedSteps=99;});
    Reject(TEXT("Regressing physical time"),[](auto& S){S.PhysicalSeconds=.01;});
    Reject(TEXT("Nonfinite physical time"),[](auto& S){S.PhysicalSeconds=std::numeric_limits<double>::quiet_NaN();});
    Reject(TEXT("Nonfinite elapsed clock"),[](auto& S){S.WallSeconds=std::numeric_limits<double>::infinity();});
    Reject(TEXT("Missing reporter"),[](auto& S){S.Reporter=TEXT(" ");});
    Reject(TEXT("Unbounded reporter"),[](auto& S){S.Reporter=FString::ChrN(257,TEXT('x'));});
    Reject(TEXT("Host memory without host"),[](auto& S){S.Host.Empty();});
    Reject(TEXT("Device usage without device"),[](auto& S){S.Device.Empty();});
    Reject(TEXT("Impossible utilization"),[](auto& S){S.DeviceUtilizationPercent=101.;});
    Reject(TEXT("Used exceeds total memory"),[](auto& S){S.DeviceUsedBytes=uint64(4097);});
    Reject(TEXT("Empty stop criteria"),[](auto& S){S.StopAfterSteps.Reset();S.StopAtPhysicalSeconds.Reset();});
    Reject(TEXT("Invalid limit"),[](auto& S){S.StopAfterSteps=0;});
    Reject(TEXT("Unknown enum value"),[](auto& S){S.StopRule=EStudioJobStopRule(99);});
    auto Missing=A.Sample(2,2,200);Missing.CompletedSteps.Reset();Missing.PhysicalSeconds.Reset();
    Missing.HostResidentBytes.Reset();Missing.DeviceUtilizationPercent.Reset();F.Push(Missing,2);
    auto V=F.Job.Telemetry();
    TestFalse(TEXT("Missing fields never inherit previous measurements"),V.Sample->CompletedSteps.IsSet() || V.Sample->HostResidentBytes.IsSet() || V.Sample->DeviceUtilizationPercent.IsSet());
    TestFalse(TEXT("Missing counters make total progress indeterminate"),V.Progress.IsSet());
    F.Push(A.Sample(3,3,99),3);TestEqual(TEXT("Regression rejected across missing counter"),F.Job.TelemetryHistory().Num(),2);
    F.Push(A.Sample(3,3,300),3);TestFalse(TEXT("A missing value interrupts rate history"),F.Job.Telemetry().StepsPerSecond.IsSet());
    F.Push(A.Sample(4,4,400),4);TestEqual(TEXT("Valid measurements recover without rejected samples poisoning sequence"),F.Job.Telemetry().StepsPerSecond.Get(-1.),100.);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioTelemetryLifecycle,"Studio.Jobs.Telemetry.PauseReconnectAndTerminalFreshness",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioTelemetryLifecycle::RunTest(const FString&)
{
    FFixture F;auto& A=*F.Adapter;auto& J=F.Job;
    F.Push(A.Sample(1,1),1);F.Push(A.Sample(2,2,200),2);
    J.Command(EStudioJobCommand::Pause,2.1);
    TestTrue(TEXT("In-flight state change makes old telemetry stale"),J.Telemetry().Status==EStudioTelemetryStatus::Stale);
    F.Push(A.Sample(3,2.2,200),2.2);
    TestTrue(TEXT("Paused measurement remains current"),J.Telemetry().Status==EStudioTelemetryStatus::Current);
    TestFalse(TEXT("Paused measurement has no rate/ETA"),J.Telemetry().StepsPerSecond.IsSet() || J.Telemetry().EstimatedRemainingSeconds.IsSet());
    const auto PausedSample=A.Sample(4,3.1,300);
    J.Command(EStudioJobCommand::Resume,3);J.Tick(3.1);F.Push(PausedSample,3.1);
    TestEqual(TEXT("Queued sample from prior state rejected after resume"),J.TelemetryHistory().Num(),3);
    F.Push(A.Sample(4,3.1,300),3.1);TestFalse(TEXT("Resume needs new rate window"),J.Telemetry().StepsPerSecond.IsSet());
    F.Push(A.Sample(5,4.2,410),4.2);TestTrue(TEXT("Resume obtains a new rate"),J.Telemetry().StepsPerSecond.IsSet());
    A.State(EStudioJobState::Disconnected);J.Tick(4.3);
    TestTrue(TEXT("Disconnected measurements explicitly identified"),J.Telemetry().Status==EStudioTelemetryStatus::Disconnected);
    F.Push(A.Sample(6,5,500),5);TestEqual(TEXT("Disconnected measurements ignored"),J.TelemetryHistory().Num(),5);
    J.Command(EStudioJobCommand::Reconnect,10);J.Tick(10.1);
    TestTrue(TEXT("Reconnect state alone cannot refresh numerical data"),J.Telemetry().Status==EStudioTelemetryStatus::Stale);
    F.Push(A.Sample(6,10.2,500),10.2);TestFalse(TEXT("Reconnect rate cannot bridge outage"),J.Telemetry().StepsPerSecond.IsSet());
    F.Push(A.Sample(7,11.3,610),11.3);TestTrue(TEXT("New measurements recover rate"),J.Telemetry().StepsPerSecond.IsSet());
    A.State(EStudioJobState::Completed);J.Tick(12);
    TestTrue(TEXT("Completion alone does not certify old sample"),J.Telemetry().Status==EStudioTelemetryStatus::Stale);
    F.Push(A.Sample(8,12,610),12);
    TestTrue(TEXT("Explicit terminal measurement retained as final"),J.Telemetry().Status==EStudioTelemetryStatus::Final);
    TestEqual(TEXT("Completed event cannot fabricate 100 percent"),J.Telemetry().Progress.Get(-1.),.61);
    TestFalse(TEXT("Final snapshot has no current throughput or ETA"),J.Telemetry().StepsPerSecond.IsSet() || J.Telemetry().EstimatedRemainingSeconds.IsSet());
    F.Push(A.Sample(9,13,900),13);TestEqual(TEXT("Final measurement is immutable"),J.Telemetry().Sample->CompletedSteps.Get(-1),int64(610));
    J.Tick(100);TestTrue(TEXT("Final remains explicitly historical"),J.Telemetry().Status==EStudioTelemetryStatus::Final);
    J.Submit(TEXT("Next run"),Draft(),100);TestTrue(TEXT("New run clears all prior numerical history"),J.TelemetryHistory().IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioTelemetryAge,"Studio.Jobs.Telemetry.DelayedSamplesAndSourceChanges",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioTelemetryAge::RunTest(const FString&)
{
    FFixture F;auto& A=*F.Adapter;auto& J=F.Job;
    auto Delayed=A.Sample(1,1);Delayed.SampleAgeSeconds=6;F.Push(Delayed,10);
    TestTrue(TEXT("Receipt of old data does not make it current"),J.Telemetry().Status==EStudioTelemetryStatus::Stale);
    TestEqual(TEXT("Adapter transport age exposed"),J.Telemetry().AgeSeconds,6.);
    F.Push(A.Sample(2,2,200),11);TestFalse(TEXT("Fresh data cannot reuse stale rate baseline"),J.Telemetry().StepsPerSecond.IsSet());
    F.Push(A.Sample(3,3,300),12);TestTrue(TEXT("Fresh pair establishes throughput"),J.Telemetry().StepsPerSecond.IsSet());
    J.Tick(17.1);auto V=J.Telemetry();
    TestTrue(TEXT("Silent stream expires without new events"),V.Status==EStudioTelemetryStatus::Stale);
    TestFalse(TEXT("Expired sample exposes no current progress or ETA"),V.Progress.IsSet() || V.EstimatedRemainingSeconds.IsSet());
    TestEqual(TEXT("Original stale value remains inspectable"),V.Sample->CompletedSteps.Get(-1),int64(300));
    F.Push(A.Sample(4,9,400),18);TestFalse(TEXT("No throughput across a silent gap"),J.Telemetry().StepsPerSecond.IsSet());
    auto Migrated=A.Sample(5,10,500);Migrated.Reporter=TEXT("new solver process");F.Push(Migrated,19);
    TestFalse(TEXT("Producer change starts a new rate window"),J.Telemetry().StepsPerSecond.IsSet());
    Migrated.Sequence=6;Migrated.WallSeconds=11;Migrated.CompletedSteps=600;Migrated.PhysicalSeconds=.6;F.Push(Migrated,20);
    TestEqual(TEXT("New source rate measured independently"),J.Telemetry().StepsPerSecond.Get(-1.),100.);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioTelemetryIndeterminate,"Studio.Jobs.Telemetry.PartialCriteriaAndStationaryCounters",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioTelemetryIndeterminate::RunTest(const FString&)
{
    FFixture F;auto& A=*F.Adapter;auto& J=F.Job;
    auto S=A.Sample(1,1);S.StopRule=EStudioJobStopRule::Unknown;F.Push(S,1);
    TestFalse(TEXT("Unknown stop relationship has no aggregate progress"),J.Telemetry().Progress.IsSet());
    TestTrue(TEXT("Individual known criterion remains inspectable"),J.Telemetry().StepProgress.IsSet());
    S=A.Sample(2,2);S.PhysicalSeconds.Reset();F.Push(S,2);
    TestFalse(TEXT("Unknown counter for an active limit keeps total indeterminate"),J.Telemetry().Progress.IsSet());
    TestEqual(TEXT("Stationary measured count is honestly zero per second"),J.Telemetry().StepsPerSecond.Get(-1.),0.);
    TestFalse(TEXT("Zero measured rate cannot predict a finish"),J.Telemetry().EstimatedRemainingSeconds.IsSet());
    S=A.Sample(3,3,1000);S.PhysicalSeconds.Reset();F.Push(S,3);
    TestEqual(TEXT("A reached first limit proves progress even with another counter missing"),J.Telemetry().Progress.Get(-1.),1.);
    TestEqual(TEXT("Reached limit has zero remaining time"),J.Telemetry().EstimatedRemainingSeconds.Get(-1.),0.);
    TestTrue(TEXT("Numerical progress never invents completion state"),J.State()==EStudioJobState::Running);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioTelemetryBounds,"Studio.Jobs.Telemetry.BoundedWorkAndHarnessExclusion",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioTelemetryBounds::RunTest(const FString&)
{
    FFixture F;auto& A=*F.Adapter;auto& J=F.Job;
    for(uint64 I=1;I<=300;++I)A.Samples.Add(A.Sample(I,double(I)/10.,int64(I)));
    J.Tick(40);TestEqual(TEXT("One poll accepts at most 64 measurements"),J.TelemetryHistory().Num(),64);
    for(int32 I=0;I<4;++I)J.Tick(40);
    TestEqual(TEXT("Retained measurement history bounded"),J.TelemetryHistory().Num(),240);
    TestEqual(TEXT("Most recent original sample retained"),J.Telemetry().Sample->Sequence,uint64(300));
    TestEqual(TEXT("Oldest retained original sample is exact"),J.TelemetryHistory()[0].Measurement.Sequence,uint64(61));
    for(bool Harness:{false,true})
    {
        auto Adapter=MakeUnique<FMeasurementAdapter>();auto* Ptr=Adapter.Get();
        Ptr->Caps.bControlHarness=Harness;Ptr->Caps.bTelemetry=Harness;
        FStudioJobController Other(MoveTemp(Adapter));Other.Submit(TEXT("No telemetry"),Draft(),0);Other.Tick(.1);
        Ptr->Samples.Add(Ptr->Sample(1,1));Other.Tick(1);
        TestEqual(TEXT("Unsupported or harness adapter cannot poll numerical telemetry"),Ptr->TelemetryPolls,0);
        TestTrue(TEXT("Unsupported or harness telemetry remains unavailable"),Other.Telemetry().Status==EStudioTelemetryStatus::Unavailable);
    }
    return true;
}
#endif
