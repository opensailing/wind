#include "StudioCommands.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioCommandParseTest,"Studio.Commands.ExactVocabularyAndCompletion",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioCommandParseTest::RunTest(const FString&)
{
    FString Error;EStudioCommand Id=EStudioCommand::Status;TSet<FString> Names;TSet<uint8> Ids;
    for(const auto& S:StudioCommands::Registry())
    {
        TestFalse(TEXT("Names unique"),Names.Contains(S.Name));Names.Add(S.Name);
        TestFalse(TEXT("Identifiers unique"),Ids.Contains(uint8(S.Id)));Ids.Add(uint8(S.Id));
        TestTrue(TEXT("Every advertised command parses"),StudioCommands::Parse(S.Name,Id,Error));
        TestTrue(TEXT("Exact registry binding"),Id==S.Id);
        TestTrue(TEXT("Every command described"),FCString::Strlen(S.Description)>0);
    }
    TestTrue(TEXT("Ordinary whitespace and case normalized"),StudioCommands::Parse(TEXT("  REPLAY   PAUSE  "),Id,Error));
    TestTrue(TEXT("Normalized binding"),Id==EStudioCommand::ReplayPause);
    for(const FString& Bad:TArray<FString>{TEXT(""),TEXT("pause"),TEXT("job"),TEXT("job pause now"),TEXT("replay\tpause"),TEXT("replay pause\n"),
        TEXT("replay pause; job stop"),TEXT("!ls"),TEXT("$(whoami)"),TEXT("quit"),TEXT("stat fps"),FString::ChrN(257,TEXT('x'))})
    {
        Id=EStudioCommand::Status;
        TestFalse(TEXT("Unsupported syntax rejected"),StudioCommands::Parse(Bad,Id,Error));
        TestFalse(TEXT("Reason supplied"),Error.IsEmpty());TestTrue(TEXT("Failure leaves output unchanged"),Id==EStudioCommand::Status);
    }
    const auto Matches=StudioCommands::Complete(TEXT("  RePlay P"));
    TestEqual(TEXT("Prefix finds only literal matches"),Matches.Num(),1);
    TestEqual(TEXT("Canonical completion"),Matches[0],FString(TEXT("replay pause")));
    TestTrue(TEXT("Unknown prefix empty"),StudioCommands::Complete(TEXT("shell")).IsEmpty());
    TestTrue(TEXT("Control characters never complete"),StudioCommands::Complete(TEXT("job\n")).IsEmpty());
    TestEqual(TEXT("Empty prefix exposes registry"),StudioCommands::Complete(TEXT("")).Num(),StudioCommands::Registry().Num());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioCommandHistoryTest,"Studio.Commands.BoundedRecallDraftAndCompletion",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioCommandHistoryTest::RunTest(const FString&)
{
    FStudioCommandHistory H;
    TestEqual(TEXT("Empty history preserves draft"),H.Previous(TEXT("draft")),FString(TEXT("draft")));
    H.Remember(EStudioCommand::Status);H.Remember(EStudioCommand::ReplayPause);H.Remember(EStudioCommand::ReplayPause);
    TestEqual(TEXT("Consecutive duplicates collapsed"),H.Num(),2);
    TestEqual(TEXT("Up selects newest"),H.Previous(TEXT("repl")),FString(TEXT("replay pause")));
    TestEqual(TEXT("Up selects previous"),H.Previous(TEXT("replay pause")),FString(TEXT("status")));
    TestEqual(TEXT("Up clamps oldest"),H.Previous(TEXT("status")),FString(TEXT("status")));
    TestEqual(TEXT("Down selects next"),H.Next(TEXT("status")),FString(TEXT("replay pause")));
    TestEqual(TEXT("Down restores unfinished draft"),H.Next(TEXT("replay pause")),FString(TEXT("repl")));
    H.Edited();H.Previous(TEXT("new draft"));H.Edited();
    TestEqual(TEXT("Editing exits recall"),H.Next(TEXT("edited")),FString(TEXT("edited")));
    for(int32 I=0;I<100;++I)H.Remember(I%2?EStudioCommand::Status:EStudioCommand::Help);
    TestEqual(TEXT("Session memory bounded"),H.Num(),FStudioCommandHistory::Capacity);
    TestEqual(TEXT("First completion"),H.Complete(TEXT("replay r")),FString(TEXT("replay run")));
    TestEqual(TEXT("Repeated Tab cycles original prefix"),H.Complete(TEXT("replay run")),FString(TEXT("replay resume")));
    TestEqual(TEXT("Reverse completion"),H.Complete(TEXT("replay resume"),true),FString(TEXT("replay run")));
    TestEqual(TEXT("Changed prefix starts new cycle"),H.Complete(TEXT("job recon")),FString(TEXT("job reconnect")));
    TestEqual(TEXT("Unknown completion preserves input"),H.Complete(TEXT("unknown")),FString(TEXT("unknown")));
    TestEqual(TEXT("History unaffected by completion"),H.Num(),FStudioCommandHistory::Capacity);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioCommandReplayTest,"Studio.Commands.LiteralReplayStateAndIsolation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioCommandReplayTest::RunTest(const FString&)
{
    FStudioModel M(FPaths::ProjectSavedDir()/TEXT("Automation/Commands")/FGuid::NewGuid().ToString());
    FString Response;const FString Case=StudioProjectIO::Serialize(M.SnapshotProject());
    TestTrue(TEXT("Help works"),StudioCommands::ExecuteModel(M,EStudioCommand::Help,Response));
    for(const auto& S:StudioCommands::Registry())TestTrue(TEXT("Help covers registry"),Response.Contains(S.Name));
    TestTrue(TEXT("Status works"),StudioCommands::ExecuteModel(M,EStudioCommand::Status,Response));
    TestEqual(TEXT("Informational commands don't change project"),StudioProjectIO::Serialize(M.SnapshotProject()),Case);
    TestFalse(TEXT("Job command cannot silently change mode"),StudioCommands::ExecuteModel(M,EStudioCommand::JobSubmit,Response));
    TestFalse(TEXT("Resume requires pause"),StudioCommands::ExecuteModel(M,EStudioCommand::ReplayResume,Response));
    TestTrue(TEXT("Start replay"),StudioCommands::ExecuteModel(M,EStudioCommand::ReplayRun,Response));
    M.Tick(.1);TestTrue(TEXT("Pause literal"),StudioCommands::ExecuteModel(M,EStudioCommand::ReplayPause,Response));
    const auto Frame=M.PlaybackFrame;
    TestFalse(TEXT("Repeated pause never resumes"),StudioCommands::ExecuteModel(M,EStudioCommand::ReplayPause,Response));
    TestTrue(TEXT("Still paused"),M.State==EStudioRunState::Paused);
    TestFalse(TEXT("Run cannot act as resume"),StudioCommands::ExecuteModel(M,EStudioCommand::ReplayRun,Response));
    TestTrue(TEXT("Resume literal"),StudioCommands::ExecuteModel(M,EStudioCommand::ReplayResume,Response));
    TestEqual(TEXT("Resume retains cursor"),M.PlaybackFrame,Frame);
    TestFalse(TEXT("Step blocked while running"),StudioCommands::ExecuteModel(M,EStudioCommand::ReplayStep,Response));
    StudioCommands::ExecuteModel(M,EStudioCommand::ReplayPause,Response);
    TestTrue(TEXT("Step advances one actual recorded frame"),StudioCommands::ExecuteModel(M,EStudioCommand::ReplayStep,Response));
    TestEqual(TEXT("Single frame"),M.PlaybackFrame,Frame+1);
    TestTrue(TEXT("Stop literal"),StudioCommands::ExecuteModel(M,EStudioCommand::ReplayStop,Response));
    TestFalse(TEXT("Repeated stop rejected"),StudioCommands::ExecuteModel(M,EStudioCommand::ReplayStop,Response));
    TestTrue(TEXT("No control job created"),M.Project.JobHistory.IsEmpty());
    TestFalse(TEXT("Model cannot bypass workspace save guard"),StudioCommands::ExecuteModel(M,EStudioCommand::ProjectSave,Response));
    TestFalse(TEXT("Invalid identifier rejected"),StudioCommands::Validate(M,static_cast<EStudioCommand>(255),Response));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioCommandJobTest,"Studio.Commands.ExactJobDispatchAndRecallRevalidation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioCommandJobTest::RunTest(const FString&)
{
    FStudioModel M(FPaths::ProjectSavedDir()/TEXT("Automation/Commands")/FGuid::NewGuid().ToString());
    M.SetControlHarness(true);FString Response;const auto Frame=M.PlaybackFrame;
    TestFalse(TEXT("Replay cannot silently change mode"),StudioCommands::ExecuteModel(M,EStudioCommand::ReplayRun,Response));
    TestTrue(TEXT("Submit"),StudioCommands::ExecuteModel(M,EStudioCommand::JobSubmit,Response));M.Tick(.1);
    const auto Run=M.Job().Run()->GetId();
    TestTrue(TEXT("Pause requested"),StudioCommands::ExecuteModel(M,EStudioCommand::JobPause,Response));
    TestFalse(TEXT("Pending dispatch rejected"),StudioCommands::ExecuteModel(M,EStudioCommand::JobPause,Response));M.Tick(.1);
    TestFalse(TEXT("Pause cannot resume a paused job"),StudioCommands::ExecuteModel(M,EStudioCommand::JobPause,Response));
    TestFalse(TEXT("Submit cannot resume a paused job"),StudioCommands::ExecuteModel(M,EStudioCommand::JobSubmit,Response));
    TestTrue(TEXT("Step supported"),StudioCommands::ExecuteModel(M,EStudioCommand::JobStep,Response));M.Tick(.1);
    TestEqual(TEXT("Control step acknowledged"),M.Job().CompletedStepCommands(),uint64(1));
    TestTrue(TEXT("Checkpoint supported"),StudioCommands::ExecuteModel(M,EStudioCommand::JobCheckpoint,Response));M.Tick(.1);
    TestEqual(TEXT("Checkpoint acknowledged"),M.Job().CompletedCheckpointCommands(),uint64(1));
    TestTrue(TEXT("Resume supported"),StudioCommands::ExecuteModel(M,EStudioCommand::JobResume,Response));M.Tick(.1);
    TestEqual(TEXT("Resume retains run"),M.Job().Run()->GetId(),Run);
    M.SimulateJobEvent(EStudioJobState::Disconnected);
    TestFalse(TEXT("Disconnected pause blocked"),StudioCommands::ExecuteModel(M,EStudioCommand::JobPause,Response));
    TestTrue(TEXT("Reconnect supported"),StudioCommands::ExecuteModel(M,EStudioCommand::JobReconnect,Response));M.Tick(.1);
    TestTrue(TEXT("Stop supported"),StudioCommands::ExecuteModel(M,EStudioCommand::JobStop,Response));M.Tick(.1);
    TestEqual(TEXT("No scientific frame advanced by job controls"),M.PlaybackFrame,Frame);
    M.SetControlHarness(false);
    FStudioCommandHistory H;H.Remember(EStudioCommand::JobSubmit);EStudioCommand Recalled;
    TestTrue(TEXT("Recall recognized"),StudioCommands::Parse(H.Previous(TEXT("")),Recalled,Response));
    TestFalse(TEXT("Recalled command revalidates changed mode"),StudioCommands::ExecuteModel(M,Recalled,Response));
    TestEqual(TEXT("Revalidation cannot create another run"),M.Project.JobHistory.Num(),1);
    return true;
}
#endif
