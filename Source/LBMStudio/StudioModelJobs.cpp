#include "StudioModel.h"

void FStudioModel::ResetJobSession()
{
    auto Adapter=MakeUnique<FStudioControlHarness>(); ControlHarness=Adapter.Get();
    JobController=MakeUnique<FStudioJobController>(MoveTemp(Adapter));
    JobClock=JobStartedAt=0;LastJobState=EStudioJobState::Idle;LastJobNotice.Empty();LastLoggedJobSequence=0;
}
bool FStudioModel::HasActiveJob() const
{return JobController&&JobController->Run().IsSet()&&!FStudioJobController::IsTerminal(JobController->State());}
bool FStudioModel::CanReplaceProject()
{
    if(!HasActiveJob())return true;
    Notice=JobController->State()==EStudioJobState::Disconnected?
        TEXT("Reconnect the control job and stop it before closing or replacing this project."):
        TEXT("Stop the control job and wait for confirmation before closing or replacing this project.");
    return false;
}
bool FStudioModel::SetControlHarness(bool bEnabled)
{
    if(HasActiveJob()&&bEnabled!=Project.bControlHarness)
    {Notice=TEXT("Stop the control job before changing control mode. The recording can still be inspected.");return false;}
    if(IsProjectOpenPending()) {Notice=TEXT("Finish or cancel project opening before changing control mode.");return false;}
    if(bEnabled&&Project.Draft.Setup.BackendId!=Job().Capabilities().BackendId)
    {
        const FString Backend=Job().Capabilities().BackendId;
        if(!EditCase(TEXT("Choose control harness"),[&](auto& D){D.Setup.BackendId=Backend;}))return false;
    }
    Project.bControlHarness=bEnabled;bDirty=true;
    Notice=bEnabled?TEXT("Control harness selected for the case. Run tests commands; no CFD is computed."):
        TEXT("Toolbar controls recorded playback. Saved case settings are retained.");
    return true;
}
bool FStudioModel::CanControl(EStudioJobCommand Command) const
{
    if(Project.bControlHarness)
    {
        if(Command==EStudioJobCommand::Submit)
            return !IsProjectOpenPending()&&(Job().Can(EStudioJobCommand::Resume)||
                (Job().Can(Command)&&Project.Runs.Num()<256&&Project.Draft.Setup.BackendId==Job().Capabilities().BackendId));
        if(Command==EStudioJobCommand::Pause)return Job().Can(Command)||Job().Can(EStudioJobCommand::Resume);
        return Job().Can(Command);
    }
    switch(Command)
    {
    case EStudioJobCommand::Submit:return Solver->FrameCount()>0&&State!=EStudioRunState::Running;
    case EStudioJobCommand::Pause:return State==EStudioRunState::Running||State==EStudioRunState::Paused;
    case EStudioJobCommand::Stop:return State==EStudioRunState::Running||State==EStudioRunState::Paused;
    case EStudioJobCommand::Step:return Solver->FrameCount()>0&&State!=EStudioRunState::Running&&State!=EStudioRunState::Complete;
    default:return false;
    }
}
bool FStudioModel::Control(EStudioJobCommand Command)
{
    if(!CanControl(Command)) {Notice=TEXT("This control is unavailable in the current mode or state.");return false;}
    if(!Project.bControlHarness)
    {
        switch(Command)
        {
        case EStudioJobCommand::Submit:Run();break;
        case EStudioJobCommand::Pause:Pause();break;
        case EStudioJobCommand::Stop:Stop();break;
        case EStudioJobCommand::Step:Step();break;
        default:return false;
        }
        return true;
    }
    if((Command==EStudioJobCommand::Submit||Command==EStudioJobCommand::Pause)&&Job().Can(EStudioJobCommand::Resume))
        Command=EStudioJobCommand::Resume;
    bool OK;
    if(Command==EStudioJobCommand::Submit)
    {
        const FString Name=Project.Name.Left(96)+FString::Printf(TEXT(" · control %d"),Project.JobHistory.Num()+1);
        // Reserve room for status changes before dispatching an irreversible command.
        auto Candidate=SnapshotProject();
        Candidate.Runs.Add(FStudioRunRecord::Capture(Name,Project.Draft,EStudioRunOrigin::ControlHarness));
        if(StudioProjectIO::Serialize(Candidate).Len()>4*1024*1024-8192)
        {Notice=TEXT("This project has reached its run-history size limit. Create a new project for another run.");return false;}
        OK=JobController->Submit(Name,Project.Draft,JobClock);
        if(OK)
        {
            Project.Runs.Add(Job().Run().GetValue());
            FStudioJobHistory H;H.RunId=Job().Run()->GetId();Project.JobHistory.Add(H);
            JobStartedAt=JobClock;LastJobNotice.Empty();LastLoggedJobSequence=0;++CatalogRevision;
        }
    }
    else OK=JobController->Command(Command,JobClock);
    SyncJob();Notice=Job().Notice();return OK;
}
void FStudioModel::SyncJob()
{
    if(!Job().Run())return;
    auto* H=Project.JobHistory.FindByPredicate([this](const auto& Entry){return Entry.RunId==Job().Run()->GetId();});
    if(!H)return;
    H->LastState=Job().State();H->StepCommands=Job().CompletedStepCommands();
    H->CheckpointCommands=Job().CompletedCheckpointCommands();H->Notice=Job().Notice();
    bool bLoggedCurrentNotice=false;
    // Drain every accepted event, even when a single tick observes several
    // transitions. Sequence filtering excludes duplicates and rejected owners.
    for(const auto& Event:Job().Events())
    {
        if(Event.Sequence<=LastLoggedJobSequence)continue;
        const auto Severity=Event.Kind==EStudioJobEventKind::Rejected||Event.State==EStudioJobState::Failed?
            EStudioLogSeverity::Error:Event.State==EStudioJobState::Disconnected?EStudioLogSeverity::Warning:EStudioLogSeverity::Info;
        AddLog(StudioJobs::StateName(Event.State)+TEXT(" · ")+Event.Message,Severity,
            EStudioLogSource::ControlHarness,Event.RunId,Job().Capabilities().BackendId);
        LastLoggedJobSequence=Event.Sequence;bLoggedCurrentNotice=Event.Message==H->Notice;
    }
    if(LastJobState!=H->LastState||LastJobNotice!=H->Notice)
    {
        if(Notice==LastJobNotice)Notice=H->Notice;
        if(!bLoggedCurrentNotice)
        {
            const auto Severity=H->LastState==EStudioJobState::Failed?EStudioLogSeverity::Error:
                H->LastState==EStudioJobState::Disconnected?EStudioLogSeverity::Warning:EStudioLogSeverity::Info;
            AddLog(StudioJobs::StateName(H->LastState)+TEXT(" · ")+H->Notice,Severity,
                EStudioLogSource::ControlHarness,H->RunId,Job().Capabilities().BackendId);
        }
        LastJobState=H->LastState;LastJobNotice=H->Notice;bDirty=true;
    }
}
void FStudioModel::TickJob(double Delta)
{
    JobClock+=Delta;
    const bool WasActive=HasActiveJob();
    JobController->Tick(JobClock);
    if(WasActive&&Job().Run())
        if(auto* H=Project.JobHistory.FindByPredicate([this](const auto& Entry){return Entry.RunId==Job().Run()->GetId();}))
            H->ElapsedSeconds=JobClock-JobStartedAt;
    SyncJob();
}
const FStudioJobHistory* FStudioModel::CurrentJobHistory() const
{
    return Job().Run()?Project.JobHistory.FindByPredicate([this](const auto& H){return H.RunId==Job().Run()->GetId();}):nullptr;
}
FString FStudioModel::RunStatus(const FGuid& Id) const
{
    const auto* H=Project.JobHistory.FindByPredicate([&](const auto& Entry){return Entry.RunId==Id;});
    if(!H)return {};
    const bool Attached=Job().Run()&&Job().Run()->GetId()==Id;
    return (Attached?TEXT(""):TEXT("Saved: "))+StudioJobs::StateName(H->LastState)+
        (!Attached&&!FStudioJobController::IsTerminal(H->LastState)?TEXT(" · session not attached"):TEXT(""));
}
bool FStudioModel::CanSimulateJobEvent(EStudioJobState StateToInject) const
{
    if(!Project.bControlHarness||!HasActiveJob()||!ControlHarness||Job().State()==EStudioJobState::Disconnected)return false;
    if(StateToInject==EStudioJobState::Completed)
        return Job().State()==EStudioJobState::Running||Job().State()==EStudioJobState::Paused;
    return StateToInject==EStudioJobState::Disconnected||StateToInject==EStudioJobState::Failed;
}
bool FStudioModel::SimulateJobEvent(EStudioJobState StateToInject)
{
    if(!CanSimulateJobEvent(StateToInject))return false;
    switch(StateToInject)
    {
    case EStudioJobState::Completed:ControlHarness->InjectCompletion(JobClock);break;
    case EStudioJobState::Disconnected:ControlHarness->InjectDisconnect(JobClock);break;
    case EStudioJobState::Failed:ControlHarness->InjectFailure(JobClock);break;
    default:return false;
    }
    JobController->Tick(JobClock);SyncJob();Notice=Job().Notice();return true;
}
