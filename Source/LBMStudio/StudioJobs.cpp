#include "StudioJobs.h"

FString StudioJobs::StateName(EStudioJobState State)
{
    static const TCHAR* Names[]={TEXT("Idle"),TEXT("Validating"),TEXT("Preparing"),TEXT("Queued"),TEXT("Running"),TEXT("Pausing"),
        TEXT("Paused"),TEXT("Stopping"),TEXT("Completed"),TEXT("Stopped"),TEXT("Failed"),TEXT("Disconnected")};
    return uint8(State)<UE_ARRAY_COUNT(Names)?Names[uint8(State)]:TEXT("Unknown");
}
bool StudioJobs::ParseState(const FString& Name,EStudioJobState& Out)
{
    for(uint8 I=0;I<=uint8(EStudioJobState::Disconnected);++I)
        if(StateName(EStudioJobState(I))==Name){Out=EStudioJobState(I);return true;}
    return false;
}

FStudioJobController::FStudioJobController(TUniquePtr<IStudioJobAdapter> InAdapter):Adapter(MoveTemp(InAdapter))
{
    check(Adapter); Caps=Adapter->Capabilities();
    if(!FMath::IsFinite(Caps.AcknowledgementTimeout)||Caps.AcknowledgementTimeout<=0)Caps.AcknowledgementTimeout=5.;
    if(!FMath::IsFinite(Caps.CompletionTimeout)||Caps.CompletionTimeout<=0)Caps.CompletionTimeout=30.;
    if(!FMath::IsFinite(Caps.TelemetryStaleSeconds)||Caps.TelemetryStaleSeconds<=0)Caps.TelemetryStaleSeconds=5.;
    Status=Caps.bControlHarness?TEXT("Control harness ready. No CFD is computed."):TEXT("Solver adapter ready.");
}
bool FStudioJobController::IsTerminal(EStudioJobState S)
{ return S==EStudioJobState::Completed||S==EStudioJobState::Stopped||S==EStudioJobState::Failed; }
bool FStudioJobController::AdvanceClock(double Now)
{
    if(!FMath::IsFinite(Now)||Now<0||Now<LastClock) {Status=TEXT("Job clock must be finite and monotonic.");return false;}
    LastClock=Now;return true;
}
bool FStudioJobController::Can(EStudioJobCommand C) const
{
    if(C==EStudioJobCommand::Stop)
        return (!Pending||Pending->Command!=EStudioJobCommand::Stop)&&
            (Current==EStudioJobState::Validating||Current==EStudioJobState::Preparing||Current==EStudioJobState::Queued||
             Current==EStudioJobState::Running||Current==EStudioJobState::Pausing||Current==EStudioJobState::Paused);
    if(Pending.IsSet())return false;
    switch(C)
    {
    case EStudioJobCommand::Submit:return Current==EStudioJobState::Idle||IsTerminal(Current);
    case EStudioJobCommand::Pause:return Caps.bPause&&Current==EStudioJobState::Running;
    case EStudioJobCommand::Resume:return Caps.bPause&&Current==EStudioJobState::Paused;
    case EStudioJobCommand::Step:return Caps.bStep&&Current==EStudioJobState::Paused;
    case EStudioJobCommand::RunToDimensionless:return Caps.bControlHarness&&Caps.bRunToDimensionless&&Current==EStudioJobState::Paused;
    case EStudioJobCommand::Checkpoint:return Caps.bCheckpoint&&(Current==EStudioJobState::Running||Current==EStudioJobState::Paused);
    case EStudioJobCommand::Stop:return Current==EStudioJobState::Running||Current==EStudioJobState::Paused||Current==EStudioJobState::Queued;
    case EStudioJobCommand::Reconnect:return Caps.bReconnect&&Current==EStudioJobState::Disconnected;
    }
    return false;
}
bool FStudioJobController::Submit(const FString& Name,const FStudioCaseDraft& Draft,double Now)
{
    if(!Can(EStudioJobCommand::Submit)) {Status=TEXT("Finish or reconnect the current job before starting another.");return false;}
    FString Error;
    if(Name.TrimStartAndEnd().IsEmpty()||Name.Len()>120) {Status=TEXT("Enter a run name of 1–120 characters.");return false;}
    if(!StudioCaseIO::Validate(Draft,Error)) {Status=Error;return false;}
    if(Draft.Setup.BackendId!=Caps.BackendId) {Status=TEXT("Case backend does not match the selected job adapter.");return false;}
    if(!AdvanceClock(Now))return false;
    Record=FStudioRunRecord::Capture(Name,Draft,Caps.bControlHarness?EStudioRunOrigin::ControlHarness:EStudioRunOrigin::Solver);
    History.Reset();LastSequence=Steps=Checkpoints=0;
    Schedule.Reset();SimulatedSteps=0;
    Measurements.Reset();StepHighWater.Reset();PhysicalHighWater.Reset();StateSequence=RateAfterSequence=0;
    FStudioJobRequest R;R.RunId=Record->GetId();R.Command=EStudioJobCommand::Submit;R.Configuration=Draft;
    Dispatch(MoveTemp(R),Now);return true;
}
bool FStudioJobController::Command(EStudioJobCommand C,double Now)
{
    if(C==EStudioJobCommand::Step&&Caps.bControlHarness&&Caps.bStepN)return StepN(1,Now);
    if(C==EStudioJobCommand::RunToDimensionless) {Status=TEXT("Supply an explicit dimensionless target.");return false;}
    if(C==EStudioJobCommand::Submit||!Can(C)) {Status=TEXT("Command is unavailable in the current job state.");return false;}
    if(!AdvanceClock(Now))return false;
    FStudioJobRequest R;R.RunId=Record->GetId();R.Command=C;Dispatch(MoveTemp(R),Now);return true;
}
TOptional<int64> FStudioJobController::DimensionlessTargetStep(double Target) const
{
    if(!Caps.bControlHarness||!Caps.bRunToDimensionless||!FMath::IsFinite(Target)||Target<=0||!Record)return {};
    const auto* Configuration=Record->GetConfiguration();
    if(!Configuration||!Configuration->Home4)return {};
    const auto StepsTarget=StudioHome4Config::ConvertUnits(Target,EStudioHome4Quantity::Time,
        EStudioHome4UnitDisplay::Nondimensional,EStudioHome4UnitDisplay::Lattice,*Configuration->Home4);
    if(!StepsTarget||*StepsTarget<=double(SimulatedSteps)||*StepsTarget>double(MaxSimulatedControlSteps))return {};
    const double Nearest=FMath::RoundToDouble(*StepsTarget);
    const double Roundoff=8.881784197001252e-16*FMath::Max(1.,FMath::Abs(*StepsTarget));
    const double Rounded=FMath::Abs(*StepsTarget-Nearest)<=Roundoff?Nearest:FMath::CeilToDouble(*StepsTarget);
    const int64 Limit=Configuration->Home4->Run.Steps.Get(Configuration->Setup.MaxSteps);
    if(Rounded>double(Limit)||Rounded<=double(SimulatedSteps))return {};
    return int64(Rounded);
}
bool FStudioJobController::DispatchStepRange(EStudioJobCommand C,int64 Count,double Now,TOptional<double> Target)
{
    if(!Caps.bControlHarness||!Caps.bStepN||!Can(C)||Count<=0||Count>MaxSimulatedControlSteps-SimulatedSteps||!Record)
    {Status=TEXT("Development step range is unavailable or exceeds its bounded limit.");return false;}
    const auto* Configuration=Record->GetConfiguration();
    const int64 Limit=Configuration->Home4?Configuration->Home4->Run.Steps.Get(Configuration->Setup.MaxSteps):Configuration->Setup.MaxSteps;
    if(Count>Limit-SimulatedSteps) {Status=TEXT("Development range exceeds the frozen run step limit.");return false;}
    if(!AdvanceClock(Now))return false;
    FStudioJobRequest R;R.RunId=Record->GetId();R.Command=C;R.StepCount=Count;
    R.SimulatedTargetStep=SimulatedSteps+Count;R.RequestedDimensionlessTime=Target;
    Dispatch(MoveTemp(R),Now);return true;
}
bool FStudioJobController::StepN(int64 Count,double Now)
{return DispatchStepRange(EStudioJobCommand::Step,Count,Now);}
bool FStudioJobController::RunToDimensionless(double Target,double Now)
{
    const auto Step=DimensionlessTargetStep(Target);
    if(!Step) {Status=TEXT("Run to t* requires a known frozen reference time and a later target within the frozen step limit.");return false;}
    return DispatchStepRange(EStudioJobCommand::RunToDimensionless,*Step-SimulatedSteps,Now,Target);
}
void FStudioJobController::AddScheduledOutputs(int64 Before,int64 After,uint64 CommandId)
{
    if(!Caps.bControlHarness||!Caps.bOutputSchedule||!Record||After<=Before)return;
    const auto* Configuration=Record->GetConfiguration();
    if(!Configuration||!Configuration->Home4)return;
    const auto& Run=Configuration->Home4->Run;
    const TOptional<int64> Intervals[]={Run.MeasureEvery,Run.SaveEvery,Run.VizEvery,Run.RestartEvery};
    for(int32 I=0;I<4;++I)
    {
        if(!Intervals[I]||*Intervals[I]<=0)continue;
        const int64 Interval=*Intervals[I],FirstIndex=Before/Interval+1,LastIndex=After/Interval;
        if(LastIndex<FirstIndex)continue;
        FStudioJobScheduleNotice N;N.RunId=Record->GetId();N.CommandId=CommandId;N.Kind=EStudioJobScheduledOutputKind(I);
        N.FirstStep=FirstIndex*Interval;N.LastStep=LastIndex*Interval;N.Crossings=LastIndex-FirstIndex+1;N.Interval=Interval;
        if(Schedule.Num()>=64)Schedule.RemoveAt(0,Schedule.Num()-63,EAllowShrinking::No);
        Schedule.Add(N);
    }
}
void FStudioJobController::Dispatch(FStudioJobRequest R,double Now)
{
    R.CommandId=NextCommand++;
    // Rejected interruption leaves the superseded command's outcome unknown.
    BeforeCommand=Pending?EStudioJobState::Disconnected:Current;Pending=R;Deadline=Now+Caps.AcknowledgementTimeout;
    if(R.Command==EStudioJobCommand::Submit)Current=EStudioJobState::Validating;
    else if(R.Command==EStudioJobCommand::Pause)Current=EStudioJobState::Pausing;
    else if(R.Command==EStudioJobCommand::Stop)Current=EStudioJobState::Stopping;
    if(Current!=BeforeCommand)BreakTelemetryRates();
    Status=TEXT("Waiting for adapter confirmation.");Adapter->Send(R,Now);
}
bool FStudioJobController::Accept(const FStudioJobEvent& E)
{
    if(!Record||E.RunId!=Record->GetId()||E.Sequence<=LastSequence)return false;
    const bool Matches=Pending&&E.CommandId==Pending->CommandId;
    if(E.CommandId!=0&&!Matches)return false;
    if(Current==EStudioJobState::Disconnected&&(!Matches||Pending->Command!=EStudioJobCommand::Reconnect))return false;
    if(IsTerminal(Current))return false;
    bool Complete=false;
    if(E.Kind==EStudioJobEventKind::Accepted)
    {if(!Matches)return false;Deadline=LastClock+Caps.CompletionTimeout;}
    else if(E.Kind==EStudioJobEventKind::Rejected)
    {
        if(!Matches)return false;
        Current=Pending->Command==EStudioJobCommand::Submit?EStudioJobState::Failed:BeforeCommand;Complete=true;
        BreakTelemetryRates();
    }
    else if(E.Kind==EStudioJobEventKind::StepCompleted||E.Kind==EStudioJobEventKind::CheckpointCompleted)
    {
        if(!Matches)return false;
        const bool Step=E.Kind==EStudioJobEventKind::StepCompleted;
        if(Step?Pending->Command!=EStudioJobCommand::Step&&Pending->Command!=EStudioJobCommand::RunToDimensionless:
            Pending->Command!=EStudioJobCommand::Checkpoint)return false;
        if(Step&&Caps.bControlHarness&&Caps.bStepN)
        {
            if(!E.SimulatedSteps||!Pending->SimulatedTargetStep||*E.SimulatedSteps!=*Pending->SimulatedTargetStep||
                *E.SimulatedSteps<=SimulatedSteps||*E.SimulatedSteps>MaxSimulatedControlSteps)return false;
            AddScheduledOutputs(SimulatedSteps,*E.SimulatedSteps,E.CommandId);SimulatedSteps=*E.SimulatedSteps;
        }
        if(Step)++Steps;else ++Checkpoints;Complete=true;
    }
    else if(E.Kind==EStudioJobEventKind::State)
    {
        bool Allowed=false;
        if(E.State==EStudioJobState::Failed||E.State==EStudioJobState::Disconnected)Allowed=true;
        else if(Matches)
        {
            switch(Pending->Command)
            {
            case EStudioJobCommand::Submit:
                Allowed=(Current==EStudioJobState::Validating&&E.State==EStudioJobState::Preparing)||
                    (Current==EStudioJobState::Preparing&&E.State==EStudioJobState::Queued)||
                    (Current==EStudioJobState::Queued&&E.State==EStudioJobState::Running);
                Complete=E.State==EStudioJobState::Running;break;
            case EStudioJobCommand::Pause:Allowed=E.State==EStudioJobState::Paused;Complete=Allowed;break;
            case EStudioJobCommand::Resume:Allowed=E.State==EStudioJobState::Running;Complete=Allowed;break;
            case EStudioJobCommand::Stop:Allowed=E.State==EStudioJobState::Stopped;Complete=Allowed;break;
            case EStudioJobCommand::Reconnect:
                Allowed=E.State!=EStudioJobState::Idle&&E.State!=EStudioJobState::Disconnected;
                Complete=Allowed;break;
            default:break;
            }
        }
        else if(E.CommandId==0)
            Allowed=(Current==EStudioJobState::Validating&&E.State==EStudioJobState::Preparing)||
                (Current==EStudioJobState::Preparing&&E.State==EStudioJobState::Queued)||
                (Current==EStudioJobState::Queued&&E.State==EStudioJobState::Running)||
                (Current==EStudioJobState::Pausing&&E.State==EStudioJobState::Paused)||
                (Current==EStudioJobState::Stopping&&E.State==EStudioJobState::Stopped)||
                (E.State==EStudioJobState::Completed&&(Current==EStudioJobState::Running||Current==EStudioJobState::Pausing||
                    Current==EStudioJobState::Stopping||Current==EStudioJobState::Paused));
        if(!Allowed)return false;
        if(Matches&&Pending->Command==EStudioJobCommand::Reconnect&&Caps.bControlHarness&&Caps.bStepN)
        {
            if(!E.SimulatedSteps)return false;
            const auto* Configuration=Record->GetConfiguration();
            const int64 Limit=Configuration->Home4?Configuration->Home4->Run.Steps.Get(Configuration->Setup.MaxSteps):Configuration->Setup.MaxSteps;
            if(*E.SimulatedSteps<SimulatedSteps||*E.SimulatedSteps>FMath::Min(Limit,MaxSimulatedControlSteps))return false;
        }
        BreakTelemetryRates();StateSequence=E.Sequence;
        Current=E.State;
        if(Matches&&Pending->Command==EStudioJobCommand::Reconnect&&Caps.bControlHarness&&Caps.bStepN&&E.SimulatedSteps)
        {AddScheduledOutputs(SimulatedSteps,*E.SimulatedSteps,E.CommandId);SimulatedSteps=*E.SimulatedSteps;}
        if(Matches)Deadline=LastClock+Caps.CompletionTimeout;
        Complete|=IsTerminal(Current)||Current==EStudioJobState::Disconnected;
    }
    else return false;
    LastSequence=E.Sequence;Status=E.Message.Left(2048);
    auto Bounded=E;Bounded.Message=Status;History.Add(MoveTemp(Bounded));
    if(History.Num()>256)History.RemoveAt(0,History.Num()-256,EAllowShrinking::No);
    if(Complete)Pending.Reset();return true;
}
void FStudioJobController::Tick(double Now)
{
    if(!AdvanceClock(Now))return;
    // A late acknowledgement is ambiguous: require an authoritative reconnect,
    // never silently repeat Start/Step or permit a second job after a timeout.
    if(Pending&&Now>Deadline)
    {Current=EStudioJobState::Disconnected;BreakTelemetryRates();Pending.Reset();Status=TEXT("Adapter confirmation timed out. Reconnect to determine job state.");}
    TArray<FStudioJobEvent> Incoming;Adapter->Poll(Now,64,Incoming);
    for(int32 I=0;I<FMath::Min(64,Incoming.Num());++I)Accept(Incoming[I]);
    if(Caps.bTelemetry&&!Caps.bControlHarness)
    {
        TArray<FStudioJobMeasurement> Samples;Adapter->PollTelemetry(Now,64,Samples);
        for(int32 I=0;I<FMath::Min(64,Samples.Num());++I)AcceptTelemetry(Samples[I]);
    }
}

FStudioJobCapabilities FStudioControlHarness::Capabilities() const
{ FStudioJobCapabilities C;C.BackendId=TEXT("studio-control-harness");C.bControlHarness=C.bPause=C.bStep=C.bCheckpoint=C.bReconnect=true;C.bStepN=C.bRunToDimensionless=C.bOutputSchedule=true;return C; }
void FStudioControlHarness::Queue(double At,uint64 Command,EStudioJobEventKind Kind,EStudioJobState State,const FString& Message,bool bQueryState)
{
    FStudioJobEvent E;E.RunId=ActiveRun;E.CommandId=Command;E.Kind=Kind;E.State=State;E.Message=Message;
    QueueItems.Add({At,MoveTemp(E),bQueryState});
    QueueItems.StableSort([](const FScheduled& A,const FScheduled& B){return A.At<B.At;});
}
void FStudioControlHarness::DeliverUntil(double Now,int32 MaxEvents,TArray<FStudioJobEvent>* Out)
{
    int32 Delivered=0;
    while(!QueueItems.IsEmpty()&&QueueItems[0].At<=Now&&Delivered<MaxEvents)
    {
        auto E=MoveTemp(QueueItems[0].Event);
        if(QueueItems[0].bQueryState)
        {E.State=Actual==EStudioJobState::Idle?EStudioJobState::Stopped:Actual;E.SimulatedSteps=SimulatedSteps;}
        QueueItems.RemoveAt(0,1,EAllowShrinking::No);E.Sequence=++Sequence;
        if(E.Kind==EStudioJobEventKind::State&&E.State!=EStudioJobState::Disconnected)Actual=E.State;
        if(E.Kind==EStudioJobEventKind::StepCompleted&&E.SimulatedSteps)SimulatedSteps=*E.SimulatedSteps;
        if(Out)Out->Add(MoveTemp(E));++Delivered;
    }
}
void FStudioControlHarness::Send(const FStudioJobRequest& R,double Now)
{
    if(R.Command==EStudioJobCommand::Submit) {ActiveRun=R.RunId;Actual=EStudioJobState::Idle;QueueItems.Reset();Sequence=0;SimulatedSteps=0;}
    if(R.RunId!=ActiveRun)return;
    if(R.Command==EStudioJobCommand::Reconnect)
    {
        // Transport loss does not stop the backend. Query the state at the
        // caller's clock and publish later lifecycle changes as session events.
        DeliverUntil(Now,MAX_int32,nullptr);
        for(auto& Item:QueueItems)Item.Event.CommandId=0;
    }
    if(bDropNext){bDropNext=false;return;}
    if(bRejectNext){bRejectNext=false;Queue(Now+.01,R.CommandId,EStudioJobEventKind::Rejected,Actual,TEXT("Control harness rejected the command."));return;}
    if(R.Command==EStudioJobCommand::Stop)QueueItems.Reset();
    Queue(Now+.01,R.CommandId,EStudioJobEventKind::Accepted,Actual,TEXT("Control harness accepted the command; no CFD computed."));
    switch(R.Command)
    {
    case EStudioJobCommand::Submit:
        Queue(Now+.02,R.CommandId,EStudioJobEventKind::State,EStudioJobState::Preparing,TEXT("Control harness: preparation acknowledged."));
        Queue(Now+.03,R.CommandId,EStudioJobEventKind::State,EStudioJobState::Queued,TEXT("Control harness: queued."));
        Queue(Now+.04,R.CommandId,EStudioJobEventKind::State,EStudioJobState::Running,TEXT("Control harness running. No CFD is computed."));break;
    case EStudioJobCommand::Pause:Queue(Now+.02,R.CommandId,EStudioJobEventKind::State,EStudioJobState::Paused,TEXT("Control harness paused."));break;
    case EStudioJobCommand::Resume:Queue(Now+.02,R.CommandId,EStudioJobEventKind::State,EStudioJobState::Running,TEXT("Control harness resumed."));break;
    case EStudioJobCommand::Stop:Queue(Now+.02,R.CommandId,EStudioJobEventKind::State,EStudioJobState::Stopped,TEXT("Control harness stopped."));break;
    case EStudioJobCommand::Step:
    case EStudioJobCommand::RunToDimensionless:
    {
        if(Actual!=EStudioJobState::Paused||R.StepCount<=0||R.StepCount>FStudioJobController::MaxSimulatedControlSteps-SimulatedSteps||
            !R.SimulatedTargetStep||*R.SimulatedTargetStep!=SimulatedSteps+R.StepCount)
        {Queue(Now+.02,R.CommandId,EStudioJobEventKind::Rejected,Actual,TEXT("Invalid development step range."));break;}
        Queue(Now+.02,R.CommandId,EStudioJobEventKind::StepCompleted,Actual,
            FString::Printf(TEXT("Development range acknowledged: %lld simulated control steps. No solver steps or fields generated."),R.StepCount));
        auto* Completion=QueueItems.FindByPredicate([&R](const FScheduled& E)
            {return E.Event.CommandId==R.CommandId&&E.Event.Kind==EStudioJobEventKind::StepCompleted;});
        if(Completion)Completion->Event.SimulatedSteps=*R.SimulatedTargetStep;break;
    }
    case EStudioJobCommand::Checkpoint:Queue(Now+.02,R.CommandId,EStudioJobEventKind::CheckpointCompleted,Actual,TEXT("Checkpoint control acknowledged. No restart file generated."));break;
    case EStudioJobCommand::Reconnect:Queue(Now+.02,R.CommandId,EStudioJobEventKind::State,Actual,TEXT("Control harness state confirmed."),true);break;
    }
}
void FStudioControlHarness::Poll(double Now,int32 MaxEvents,TArray<FStudioJobEvent>& Out)
{ DeliverUntil(Now,FMath::Max(0,MaxEvents),&Out); }
void FStudioControlHarness::InjectFailure(double Now)
{QueueItems.Reset();Queue(Now,0,EStudioJobEventKind::State,EStudioJobState::Failed,TEXT("Injected control-harness failure. No CFD output exists."));}
void FStudioControlHarness::InjectDisconnect(double Now)
{Queue(Now,0,EStudioJobEventKind::State,EStudioJobState::Disconnected,TEXT("Injected connection loss; reconnect to query job state."));}
void FStudioControlHarness::InjectCompletion(double Now)
{QueueItems.Reset();Queue(Now,0,EStudioJobEventKind::State,EStudioJobState::Completed,TEXT("Control harness completed. No CFD output generated."));}
