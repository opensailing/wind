#include "StudioJobs.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
    FStudioCaseDraft JobDraft()
    {FStudioCaseDraft D;D.Setup.BackendId=TEXT("studio-control-harness");return D;}
    class FEventAdapter final : public IStudioJobAdapter
    {
    public:
        FStudioJobRequest Request;
        TArray<FStudioJobEvent> Replies;
        FStudioJobCapabilities Capabilities() const override
        {FStudioJobCapabilities C;C.BackendId=TEXT("studio-control-harness");C.bControlHarness=C.bReconnect=true;return C;}
        void Send(const FStudioJobRequest& R,double) override {Request=R;}
        void Poll(double,int32 Max,TArray<FStudioJobEvent>& Out) override
        {int32 Count=FMath::Min(Max,Replies.Num());Out.Append(Replies.GetData(),Count);Replies.RemoveAt(0,Count,EAllowShrinking::No);}
        void Reply(uint64 Sequence,EStudioJobState State,uint64 Command=MAX_uint64)
        {FStudioJobEvent E;E.RunId=Request.RunId;E.CommandId=Command==MAX_uint64?Request.CommandId:Command;E.Sequence=Sequence;E.State=State;Replies.Add(E);}
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioJobLifecycle,"Studio.Jobs.HarnessLifecycleAndFrozenConfiguration",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioJobLifecycle::RunTest(const FString&)
{
    auto Adapter=MakeUnique<FStudioControlHarness>();auto* Harness=Adapter.Get();FStudioJobController Job(MoveTemp(Adapter));
    auto Draft=JobDraft();Draft.Setup.MaxSteps=123;
    TestTrue(TEXT("Submit control run"),Job.Submit(TEXT("Control test"),Draft,0));
    Draft.Setup.MaxSteps=456;
    TestEqual(TEXT("Run settings frozen at submission"),Job.Run()->GetConfiguration()->Setup.MaxSteps,int64(123));
    TestTrue(TEXT("Harness origin explicit"),Job.Run()->GetOrigin()==EStudioRunOrigin::ControlHarness);
    TestTrue(TEXT("No recording falsely attached"),Job.Run()->GetDatasetId().IsEmpty());
    TestFalse(TEXT("Second submission blocked"),Job.Submit(TEXT("Second"),Draft,0));
    Job.Tick(.1);TestTrue(TEXT("Validated prepared queued running"),Job.State()==EStudioJobState::Running);
    TestEqual(TEXT("All startup acknowledgements recorded"),Job.Events().Num(),4);
    TestFalse(TEXT("Step requires pause"),Job.Command(EStudioJobCommand::Step,.1));
    TestTrue(TEXT("Pause sent"),Job.Command(EStudioJobCommand::Pause,.1));
    TestTrue(TEXT("Pause awaits authoritative state"),Job.State()==EStudioJobState::Pausing&&Job.IsPending());
    Job.Tick(.2);TestTrue(TEXT("Pause acknowledged"),Job.State()==EStudioJobState::Paused&&!Job.IsPending());
    TestTrue(TEXT("Single step sent"),Job.Command(EStudioJobCommand::Step,.2));Job.Tick(.3);
    TestEqual(TEXT("One command completion, not a CFD time step"),Job.CompletedStepCommands(),uint64(1));
    TestTrue(TEXT("Step stays paused"),Job.State()==EStudioJobState::Paused);
    Job.Command(EStudioJobCommand::Checkpoint,.3);Job.Tick(.4);
    TestEqual(TEXT("Checkpoint control acknowledged"),Job.CompletedCheckpointCommands(),uint64(1));
    TestTrue(TEXT("No checkpoint file claim"),Job.Notice().Contains(TEXT("No restart file")));
    Job.Command(EStudioJobCommand::Resume,.4);Job.Tick(.5);
    Harness->InjectCompletion(.6);Job.Tick(.6);TestTrue(TEXT("Completion event terminal"),Job.State()==EStudioJobState::Completed);
    const FGuid First=Job.Run()->GetId();Job.Submit(TEXT("Next"),Draft,.7);
    TestTrue(TEXT("New run gets independent identity"),First!=Job.Run()->GetId());
    TestTrue(TEXT("Stop allowed while preparing"),Job.Command(EStudioJobCommand::Stop,.7));Job.Tick(.8);
    TestTrue(TEXT("Pending startup cancelled before running"),Job.State()==EStudioJobState::Stopped);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioJobFaults,"Studio.Jobs.TimeoutReconnectAndFailure",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioJobFaults::RunTest(const FString&)
{
    auto Adapter=MakeUnique<FStudioControlHarness>();auto* H=Adapter.Get();FStudioJobController J(MoveTemp(Adapter));
    J.Submit(TEXT("Control"),JobDraft(),0);J.Tick(.1);
    H->DropNextCommand();J.Command(EStudioJobCommand::Pause,.2);J.Tick(5.3);
    TestTrue(TEXT("Missing reply makes state unknown"),J.State()==EStudioJobState::Disconnected);
    TestFalse(TEXT("No new run after ambiguous timeout"),J.Can(EStudioJobCommand::Submit));
    TestTrue(TEXT("Reconnect queries actual state"),J.Command(EStudioJobCommand::Reconnect,5.3));J.Tick(5.4);
    TestTrue(TEXT("Backend still running after lost pause"),J.State()==EStudioJobState::Running);
    H->RejectNextCommand();J.Command(EStudioJobCommand::Pause,5.4);J.Tick(5.5);
    TestTrue(TEXT("Rejected pause retains running state"),J.State()==EStudioJobState::Running);
    H->InjectDisconnect(5.6);J.Tick(5.6);J.Command(EStudioJobCommand::Reconnect,5.7);J.Tick(5.8);
    TestTrue(TEXT("Explicit connection loss reconnects"),J.State()==EStudioJobState::Running);
    H->InjectFailure(5.9);J.Tick(5.9);TestTrue(TEXT("Backend failure terminal"),J.State()==EStudioJobState::Failed);
    H->DropNextCommand();J.Submit(TEXT("No launch"),JobDraft(),6);J.Tick(11.1);
    J.Command(EStudioJobCommand::Reconnect,11.2);J.Tick(11.3);
    TestTrue(TEXT("Dropped launch reconnect confirms no active job"),J.State()==EStudioJobState::Stopped);
    TestFalse(TEXT("Backwards clock rejected"),J.Submit(TEXT("Bad clock"),JobDraft(),10));
    TestTrue(TEXT("Invalid clock does not replace state"),J.State()==EStudioJobState::Stopped);
    J.Submit(TEXT("Disconnect during preparation"),JobDraft(),12);J.Tick(12.021);
    H->InjectDisconnect(12.022);J.Tick(12.022);J.Tick(12.1);
    TestTrue(TEXT("Late start events do not clear disconnected state"),J.State()==EStudioJobState::Disconnected);
    J.Command(EStudioJobCommand::Reconnect,12.1);J.Tick(12.2);
    TestTrue(TEXT("Reconnect discovers backend advanced during outage"),J.State()==EStudioJobState::Running);
    J.Command(EStudioJobCommand::Stop,12.3);J.Tick(12.4);
    J.Submit(TEXT("Reconnect races preparation"),JobDraft(),13);J.Tick(13.021);
    H->InjectDisconnect(13.022);J.Tick(13.022);J.Command(EStudioJobCommand::Reconnect,13.022);J.Tick(13.1);
    TestTrue(TEXT("Reconnect response observes changes occurring while query was in flight"),J.State()==EStudioJobState::Running);
    J.Command(EStudioJobCommand::Stop,13.2);J.Tick(13.3);
    J.Submit(TEXT("Dropped stop during preparation"),JobDraft(),14);J.Tick(14.021);
    H->DropNextCommand();J.Command(EStudioJobCommand::Stop,14.022);J.Tick(14.1);J.Tick(19.1);
    J.Command(EStudioJobCommand::Reconnect,19.1);J.Tick(19.2);
    TestTrue(TEXT("Dropped stop cannot cancel backend work"),J.State()==EStudioJobState::Running);
    J.Command(EStudioJobCommand::Stop,19.3);J.Tick(19.4);
    J.Submit(TEXT("Rejected stop during preparation"),JobDraft(),20);J.Tick(20.021);
    H->RejectNextCommand();J.Command(EStudioJobCommand::Stop,20.022);J.Tick(20.1);
    TestTrue(TEXT("Rejected interruption requires state query"),J.State()==EStudioJobState::Disconnected);
    J.Command(EStudioJobCommand::Reconnect,20.1);J.Tick(20.2);
    TestTrue(TEXT("Rejected stop cannot cancel backend work"),J.State()==EStudioJobState::Running);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioJobEvents,"Studio.Jobs.CorrelationCapabilitiesAndBounds",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioJobEvents::RunTest(const FString&)
{
    auto Adapter=MakeUnique<FEventAdapter>();auto* A=Adapter.Get();FStudioJobController J(MoveTemp(Adapter));
    auto D=JobDraft();D.Setup.BackendId=TEXT("other");
    TestFalse(TEXT("Wrong backend rejected before submission"),J.Submit(TEXT("Wrong"),D,0));
    J.Submit(TEXT("Events"),JobDraft(),0);
    A->Reply(1,EStudioJobState::Preparing);A->Replies.Last().RunId=FGuid::NewGuid();
    A->Reply(2,EStudioJobState::Running); // illegal transition
    A->Reply(3,EStudioJobState::Preparing,900); // wrong command
    J.Tick(.1);TestEqual(TEXT("Foreign and impossible events ignored"),J.Events().Num(),0);
    A->Reply(4,EStudioJobState::Preparing);A->Reply(3,EStudioJobState::Queued);
    A->Reply(5,EStudioJobState::Queued);A->Reply(6,EStudioJobState::Running);J.Tick(.2);
    TestTrue(TEXT("Ordered matching states accepted"),J.State()==EStudioJobState::Running);
    TestEqual(TEXT("Out-of-order reply ignored"),J.Events().Num(),3);
    TestFalse(TEXT("Unsupported pause disabled"),J.Can(EStudioJobCommand::Pause));
    TestFalse(TEXT("Unsupported step disabled"),J.Can(EStudioJobCommand::Step));
    TestFalse(TEXT("Unsupported checkpoint disabled"),J.Can(EStudioJobCommand::Checkpoint));
    J.Command(EStudioJobCommand::Stop,.3);const uint64 Command=J.PendingCommandId();
    A->Reply(7,EStudioJobState::Stopped,Command-1);J.Tick(.4);
    TestTrue(TEXT("Old command cannot complete stop"),J.IsPending());
    // Acceptance messages can be numerous; work and retained log are bounded.
    for(uint64 I=8;I<408;++I)
    {A->Reply(I,EStudioJobState::Stopping);A->Replies.Last().Kind=EStudioJobEventKind::Accepted;A->Replies.Last().Message=FString::ChrN(4096,TEXT('x'));}
    J.Tick(.5);TestTrue(TEXT("One tick processes at most64"),J.Events().Num()<=67);
    for(int32 I=0;I<6;++I)J.Tick(.6+I*.1);
    TestEqual(TEXT("Retained event history bounded"),J.Events().Num(),256);
    TestEqual(TEXT("Retained message bounded"),J.Events().Last().Message.Len(),2048);
    A->Reply(408,EStudioJobState::Stopped);J.Tick(1.3);
    TestTrue(TEXT("Matching stop completes"),J.State()==EStudioJobState::Stopped);
    J.Submit(TEXT("Slow preparation"),JobDraft(),2);
    A->Reply(1,EStudioJobState::Validating);A->Replies.Last().Kind=EStudioJobEventKind::Accepted;J.Tick(2.1);
    J.Tick(8);TestTrue(TEXT("Accepted work gets completion deadline beyond acknowledgement timeout"),J.IsPending());
    J.Tick(33);TestTrue(TEXT("Accepted but stalled operation still times out"),J.State()==EStudioJobState::Disconnected);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioJobDeterminism,"Studio.Jobs.RepeatableControlTrace",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioJobDeterminism::RunTest(const FString&)
{
    auto Trace=[](bool Fine)
    {
        FStudioJobController J(MakeUnique<FStudioControlHarness>());J.Submit(TEXT("Script"),JobDraft(),0);
        if(Fine)for(int32 I=1;I<10;++I)J.Tick(I*.01);
        J.Tick(.1);J.Command(EStudioJobCommand::Pause,.1);J.Tick(.2);
        J.Command(EStudioJobCommand::Step,.2);J.Tick(.3);J.Command(EStudioJobCommand::Stop,.3);J.Tick(.4);
        FString Result;
        for(const auto& E:J.Events())Result+=FString::Printf(TEXT("%llu/%llu/%d/%d/%s\n"),E.Sequence,E.CommandId,int32(E.Kind),int32(E.State),*E.Message);
        return Result;
    };
    TestEqual(TEXT("Same scripted commands produce identical event trace across polling cadence"),Trace(false),Trace(true));
    return true;
}
#endif
