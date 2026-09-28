#include "StudioModel.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
    FString JobTestDirectory()
    {return FPaths::ProjectSavedDir()/TEXT("Automation/JobModel")/FGuid::NewGuid().ToString();}
    FString JobJSON(const TSharedPtr<FJsonObject>& O)
    {FString S;FJsonSerializer::Serialize(O.ToSharedRef(),TJsonWriterFactory<>::Create(&S));return S;}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioJobModelLifecycle,"Studio.Jobs.ModelControlAndSavedHistory",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioJobModelLifecycle::RunTest(const FString&)
{
    const FString Dir=JobTestDirectory();FStudioModel M(Dir);
    M.Scrub(.5);const int32 Frame=M.SelectedFrame;const auto Source=M.Solver;const auto Camera=M.Project.Camera;
    TestTrue(TEXT("Explicit mode selects harness case backend"),M.SetControlHarness(true));
    M.EditCase(TEXT("Set run limit"),[](auto& D){D.Setup.MaxSteps=123;});
    TestTrue(TEXT("Toolbar dispatch submits"),M.Control(EStudioJobCommand::Submit));
    const FGuid RunId=M.Job().Run()->GetId();
    TestTrue(TEXT("Submit adds frozen configuration and lifecycle"),M.Project.Runs.Num()==2&&M.Project.JobHistory.Num()==1);
    M.Tick(.1);TestTrue(TEXT("Model advances job while replay paused"),M.Job().State()==EStudioJobState::Running);
    M.EditCase(TEXT("Next run limit"),[](auto& D){D.Setup.MaxSteps=456;});
    TestEqual(TEXT("Submitted settings never change with draft"),M.Project.Runs.Last().GetConfiguration()->Setup.MaxSteps,int64(123));
    TestTrue(TEXT("Pause dispatch"),M.Control(EStudioJobCommand::Pause));M.Tick(.1);
    TestEqual(TEXT("Footer advances after acknowledgement"),M.Notice,M.Job().Notice());
    TestTrue(TEXT("Paused job can step"),M.Control(EStudioJobCommand::Step));M.Tick(.1);
    TestTrue(TEXT("Checkpoint acknowledgement dispatch"),M.Control(EStudioJobCommand::Checkpoint));M.Tick(.1);
    TestEqual(TEXT("Step command history"),M.CurrentJobHistory()->StepCommands,uint64(1));
    TestEqual(TEXT("Checkpoint command history"),M.CurrentJobHistory()->CheckpointCommands,uint64(1));
    TestTrue(TEXT("Checkpoint does not claim a restart file"),M.CurrentJobHistory()->Notice.Contains(TEXT("No restart file")));
    TestTrue(TEXT("Pause button resumes"),M.Control(EStudioJobCommand::Pause));M.Tick(.1);
    TestTrue(TEXT("Active project can save"),M.SaveProject(Dir/TEXT("active.lbms")));
    FStudioModel Reopened(Dir/TEXT("reopened"));TestTrue(TEXT("Saved active history reopens"),Reopened.LoadProject(Dir/TEXT("active.lbms")));
    TestFalse(TEXT("Saved in-memory job is never treated as connected"),Reopened.HasActiveJob());
    TestTrue(TEXT("Reopened history explains attachment"),Reopened.RunStatus(RunId).Contains(TEXT("session not attached")));
    TestTrue(TEXT("Reopened toolbar mode retained"),Reopened.Project.bControlHarness);
    TestEqual(TEXT("Frozen run persists"),Reopened.Project.Runs.Last().GetConfiguration()->Setup.MaxSteps,int64(123));
    TestTrue(TEXT("Stop dispatch"),M.Control(EStudioJobCommand::Stop));M.Tick(.1);
    TestFalse(TEXT("Acknowledged stop releases active guard"),M.HasActiveJob());
    const double Elapsed=M.CurrentJobHistory()->ElapsedSeconds;M.Tick(.1);
    TestEqual(TEXT("Stopped elapsed duration freezes"),M.CurrentJobHistory()->ElapsedSeconds,Elapsed);
    TestEqual(TEXT("Controls never advance source frame"),M.SelectedFrame,Frame);
    TestTrue(TEXT("Controls keep recorded data identity"),M.Solver==Source);
    TestTrue(TEXT("Controls keep exact camera"),StudioView::CameraEquals(M.Project.Camera,Camera));
    TestTrue(TEXT("Replay mode can be restored"),M.SetControlHarness(false));
    TestTrue(TEXT("Shared Step now dispatches to replay"),M.Control(EStudioJobCommand::Step));
    TestEqual(TEXT("Replay step advances one source frame"),M.SelectedFrame,Frame+1);
    TestEqual(TEXT("Replay does not create more jobs"),M.Project.JobHistory.Num(),1);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioJobModelGuards,"Studio.Jobs.ProjectGuardsAndFaultRecovery",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioJobModelGuards::RunTest(const FString&)
{
    const FString Dir=JobTestDirectory();FStudioModel M(Dir);M.SetControlHarness(true);
    TestTrue(TEXT("Initial document saved"),M.SaveProject(Dir/TEXT("original.lbms")));
    const auto Id=M.Project.Id;
    M.Control(EStudioJobCommand::Submit);
    TestFalse(TEXT("Close blocked during startup"),M.CanReplaceProject());
    TestFalse(TEXT("Mode cannot hide an active job"),M.SetControlHarness(false));
    TestFalse(TEXT("Create blocked before writing"),M.CreateProject(Dir/TEXT("forbidden.lbms"),TEXT("New")));
    TestFalse(TEXT("Duplicate blocked before writing"),M.DuplicateProject(Dir/TEXT("copy.lbms"),TEXT("Copy")));
    TestFalse(TEXT("Load blocked before replacement"),M.LoadProject(Dir/TEXT("original.lbms")));
    TestFalse(TEXT("Async load also blocked"),M.RequestProjectOpen(Dir/TEXT("original.lbms")));
    M.PendingRecovery=Dir/TEXT("original.lbms");
    TestFalse(TEXT("Recovery cannot orphan job"),M.RestoreRecovery());
    TestFalse(TEXT("Async recovery cannot orphan job"),M.RequestRecoveryOpen());M.PendingRecovery.Empty();
    M.NewProject(TEXT("Blocked"));TestEqual(TEXT("Direct New preserves identity"),M.Project.Id,Id);
    TestFalse(TEXT("No refused file created"),IFileManager::Get().FileExists(*(Dir/TEXT("forbidden.lbms"))));
    TestFalse(TEXT("No refused duplicate created"),IFileManager::Get().FileExists(*(Dir/TEXT("copy.lbms"))));
    TestTrue(TEXT("Can stop pending startup"),M.Control(EStudioJobCommand::Stop));M.Tick(.1);
    TestTrue(TEXT("Stopped state authoritative"),M.Job().State()==EStudioJobState::Stopped);
    M.Control(EStudioJobCommand::Submit);M.Tick(.1);
    TestTrue(TEXT("Connection loss can be exercised"),M.SimulateJobEvent(EStudioJobState::Disconnected));
    TestFalse(TEXT("Ambiguous state still protects project"),M.CanReplaceProject());
    TestFalse(TEXT("Ambiguous state blocks new submit"),M.CanControl(EStudioJobCommand::Submit));
    TestTrue(TEXT("Reconnect enabled"),M.Control(EStudioJobCommand::Reconnect));M.Tick(.1);
    TestTrue(TEXT("Reconnect observes running backend"),M.Job().State()==EStudioJobState::Running);
    TestTrue(TEXT("Failure can be exercised"),M.SimulateJobEvent(EStudioJobState::Failed));
    TestTrue(TEXT("Failed job releases project guard"),M.CanReplaceProject());
    M.Control(EStudioJobCommand::Submit);M.Tick(.1);
    TestTrue(TEXT("Completion can be exercised"),M.SimulateJobEvent(EStudioJobState::Completed));
    TestTrue(TEXT("Completed state persisted"),M.CurrentJobHistory()->LastState==EStudioJobState::Completed);
    TestTrue(TEXT("Replacement succeeds after terminal event"),M.LoadProject(Dir/TEXT("original.lbms")));
    TestTrue(TEXT("Loaded project has its own idle controller"),M.Job().State()==EStudioJobState::Idle&&!M.Job().Run());
    TestTrue(TEXT("History of replaced project cleared"),M.Project.JobHistory.IsEmpty());
    TestFalse(TEXT("Opening clears previous authoring history"),M.CanUndoCase());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioJobHistorySchema,"Studio.Jobs.HistoryValidationAndMigration",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioJobHistorySchema::RunTest(const FString&)
{
    FStudioProject P;P.Draft.Setup.BackendId=TEXT("studio-control-harness");
    const auto Run=FStudioRunRecord::Capture(TEXT("Test"),P.Draft,EStudioRunOrigin::ControlHarness);P.Runs.Add(Run);
    FStudioJobHistory H;H.RunId=Run.GetId();H.LastState=EStudioJobState::Paused;H.StepCommands=3;H.ElapsedSeconds=12.5;H.Notice=TEXT("Paused");P.JobHistory.Add(H);
    FStudioProject Loaded;FString Error;
    TestTrue(TEXT("History round trip"),StudioProjectIO::Parse(StudioProjectIO::Serialize(P),Loaded,Error));
    TestEqual(TEXT("Exact observed duration retained"),Loaded.JobHistory[0].ElapsedSeconds,12.5);
    const FString Before=StudioProjectIO::Serialize(Loaded);
    auto Invalid=P;Invalid.JobHistory[0].RunId=FGuid::NewGuid();
    TestFalse(TEXT("Orphan history rejected"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Invalid),Loaded,Error));
    TestEqual(TEXT("Rejected history leaves current destination"),StudioProjectIO::Serialize(Loaded),Before);
    Invalid=P;Invalid.JobHistory.Add(H);TestFalse(TEXT("Duplicate history rejected"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Invalid),Loaded,Error));
    Invalid=P;Invalid.JobHistory[0].RunId=P.Runs[0].GetId();TestFalse(TEXT("Published recording cannot acquire a fake job"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Invalid),Loaded,Error));
    Invalid=P;Invalid.JobHistory[0].LastState=EStudioJobState::Idle;TestFalse(TEXT("Unsubmitted history rejected"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Invalid),Loaded,Error));
    TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Before),O);
    O->GetArrayField(TEXT("jobHistory"))[0]->AsObject()->SetNumberField(TEXT("stepCommands"),1.5);
    TestFalse(TEXT("Fractional acknowledgement count rejected"),StudioProjectIO::Parse(JobJSON(O),Loaded,Error));
    O->SetNumberField(TEXT("version"),4);O->RemoveField(TEXT("jobHistory"));O->RemoveField(TEXT("controlHarness"));
    TestTrue(TEXT("Version4 migrates without claiming jobs ran"),StudioProjectIO::Parse(JobJSON(O),Loaded,Error));
    TestTrue(TEXT("Old configuration snapshots have no invented lifecycle"),Loaded.JobHistory.IsEmpty()&&!Loaded.bControlHarness);
    FStudioJobController J(MakeUnique<FStudioControlHarness>());
    TestFalse(TEXT("Unsaveable overlong run name rejected before dispatch"),J.Submit(FString::ChrN(121,TEXT('x')),P.Draft,0));
    TestFalse(TEXT("Invalid name creates no run"),J.Run().IsSet());
    return true;
}
#endif
