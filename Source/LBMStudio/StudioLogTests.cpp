#include "StudioLog.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioLogRetentionTest,"Studio.Log.BoundedRetentionAndObservationOrder",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioLogRetentionTest::RunTest(const FString&)
{
    FStudioLogJournal Journal;
    const FDateTime Time(2026,9,28,14,0,0);const auto Project=FGuid::NewGuid(),Run=FGuid::NewGuid();
    Journal.Append(TEXT(""),EStudioLogSeverity::Info,EStudioLogSource::Application);
    TestEqual(TEXT("Empty messages do not allocate sequence numbers"),Journal.LastSequence(),uint64(0));
    for(int32 I=0;I<FStudioLogJournal::Capacity*3+9;++I)
        Journal.Append(FString::FromInt(I),EStudioLogSeverity::Warning,EStudioLogSource::ControlHarness,Project,Run,TEXT("test adapter"),Time-FTimespan::FromSeconds(I));
    auto Rows=Journal.Snapshot();
    TestEqual(TEXT("Retention stays bounded over multiple wraps"),Rows.Num(),FStudioLogJournal::Capacity);
    TestEqual(TEXT("Eviction count exact"),Journal.EvictedCount(),uint64(FStudioLogJournal::Capacity*2+9));
    for(int32 I=1;I<Rows.Num();++I)
    {
        TestEqual(TEXT("Sequences ordered across wraps"),Rows[I].Sequence,Rows[I-1].Sequence+1);
        TestTrue(TEXT("Observation timestamps retained even when UTC moves backward"),Rows[I].ObservedUTC<Rows[I-1].ObservedUTC);
    }
    TestEqual(TEXT("First survivor correct"),Rows[0].Message,FString::FromInt(FStudioLogJournal::Capacity*2+9));
    TestEqual(TEXT("Owner and run captured"),Rows.Last().ProjectId,Project);
    TestEqual(TEXT("Run context retained"),Rows.Last().RunId,Run);
    Journal.Append(FString::ChrN(5000,TEXT('x')),EStudioLogSeverity::Error,EStudioLogSource::Application,{}, {},FString::ChrN(500,TEXT('r')),Time);
    Rows=Journal.Snapshot();
    TestEqual(TEXT("Message memory bounded"),Rows.Last().Message.Len(),FStudioLogJournal::MessageLimit);
    TestEqual(TEXT("Reference memory bounded"),Rows.Last().SourceReference.Len(),FStudioLogJournal::ReferenceLimit);
    TestTrue(TEXT("Clipping is disclosed"),Rows.Last().bTruncated);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioLogViewTest,"Studio.Log.FilterFreezeClearAndRestore",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioLogViewTest::RunTest(const FString&)
{
    FStudioLogJournal Journal;FStudioLogView View;FStudioLogQuery Query;
    const auto Project=FGuid::NewGuid(),Other=FGuid::NewGuid(),Run=FGuid::NewGuid();
    Journal.Append(TEXT("Loaded published fields"),EStudioLogSeverity::Info,EStudioLogSource::Playback,Project,{},TEXT("source-009"));
    Journal.Append(TEXT("Timeout"),EStudioLogSeverity::Warning,EStudioLogSource::ControlHarness,Project,Run);
    Journal.Append(TEXT("Failed export"),EStudioLogSeverity::Error,EStudioLogSource::Application,Other);
    View.Refresh(Journal);TestEqual(TEXT("Default selects all retained events"),View.Select(Query).Num(),3);
    Query.MinimumSeverity=EStudioLogSeverity::Warning;TestEqual(TEXT("Severity threshold includes errors"),View.Select(Query).Num(),2);
    Query.ProjectId=Project;TestEqual(TEXT("Project context filters without deleting"),View.Select(Query).Num(),1);
    Query.RunId=Run;Query.Source=EStudioLogSource::ControlHarness;Query.Search=TEXT("  TIMEOUT ");
    TestEqual(TEXT("Combined filters with trimmed case insensitive literal"),View.Select(Query).Num(),1);
    Query.Search=TEXT(".*");TestEqual(TEXT("Search is literal, not a regular expression"),View.Select(Query).Num(),0);
    Query=FStudioLogQuery();Query.Search=TEXT("SOURCE-009");TestEqual(TEXT("Recording reference searchable"),View.Select(Query).Num(),1);
    Query=FStudioLogQuery();View.SetFollowing(false,Journal);
    Journal.Append(TEXT("New observation"),EStudioLogSeverity::Info,EStudioLogSource::Application,Project);
    View.Refresh(Journal);TestEqual(TEXT("Pause freezes rows"),View.Select(Query).Num(),3);
    TestEqual(TEXT("Arrivals counted while paused"),View.PendingCount(Journal),uint64(1));
    View.ClearView();TestEqual(TEXT("Clear hides captured rows"),View.Select(Query).Num(),0);
    TestEqual(TEXT("Clear never erases journal"),Journal.Num(),4);
    View.SetFollowing(true,Journal);TestEqual(TEXT("Resume exposes arrivals after clear boundary"),View.Select(Query).Num(),1);
    View.ShowRetained();TestEqual(TEXT("Show retained restores cleared rows"),View.Select(Query).Num(),4);
    View.SetFollowing(false,Journal);
    for(int32 I=0;I<FStudioLogJournal::Capacity+1;++I)Journal.Append(TEXT("Later"),EStudioLogSeverity::Info,EStudioLogSource::Application);
    TestEqual(TEXT("Paused copy survives journal eviction"),View.Select(Query).Num(),4);
    View.SetFollowing(true,Journal);View.ShowRetained();
    TestEqual(TEXT("Resume is bounded to retained entries"),View.Select(Query).Num(),FStudioLogJournal::Capacity);
    TestTrue(TEXT("Evicted rows cannot be resurrected"),View.Select(Query)[0].Sequence>4);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioLogModelTest,"Studio.Log.ModelOriginsAndCompleteControlEvents",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioLogModelTest::RunTest(const FString&)
{
    const FString Dir=FPaths::ProjectSavedDir()/TEXT("Automation/Log")/FGuid::NewGuid().ToString();
    FStudioModel M(Dir);const auto Project=M.Project.Id;
    M.Run();M.Pause();
    auto Rows=M.ActivityLog().Snapshot();
    TestTrue(TEXT("Playback tagged explicitly"),Rows.Last().Source==EStudioLogSource::Playback);
    TestEqual(TEXT("Playback captures actual dataset reference"),Rows.Last().SourceReference,M.Project.Dataset);
    TestFalse(TEXT("Playback does not invent solver run context"),Rows.Last().RunId.IsValid());
    M.SetControlHarness(true);M.Control(EStudioJobCommand::Submit);const auto Run=M.Job().Run()->GetId();
    const uint64 Before=M.ActivityLog().LastSequence();M.Tick(.1);
    Rows=M.ActivityLog().Snapshot();int32 NewEvents=0;
    for(const auto& Entry:Rows)if(Entry.Sequence>Before)
    {
        ++NewEvents;TestEqual(TEXT("Every accepted event retains its run"),Entry.RunId,Run);
        TestTrue(TEXT("Harness distinguished from scientific output"),Entry.Source==EStudioLogSource::ControlHarness);
    }
    TestEqual(TEXT("All four accepted transitions in one tick retained"),NewEvents,4);
    const uint64 Captured=M.ActivityLog().LastSequence();M.Tick(.1);
    TestEqual(TEXT("Idle tick does not duplicate journal events"),M.ActivityLog().LastSequence(),Captured);
    M.SimulateJobEvent(EStudioJobState::Disconnected);
    TestTrue(TEXT("Disconnect is a warning"),M.ActivityLog().Snapshot().Last().Severity==EStudioLogSeverity::Warning);
    M.Control(EStudioJobCommand::Reconnect);M.Tick(.1);M.SimulateJobEvent(EStudioJobState::Failed);
    TestTrue(TEXT("Failure is an error"),M.ActivityLog().Snapshot().Last().Severity==EStudioLogSeverity::Error);
    M.Control(EStudioJobCommand::Submit);M.Tick(.1);
    TestTrue(TEXT("New run keeps old context and drains new sequences"),M.ActivityLog().Snapshot().Last().RunId!=Run);
    M.SimulateJobEvent(EStudioJobState::Completed);M.NewProject(TEXT("Second project"));
    M.AddLog(TEXT("New project observation"));Rows=M.ActivityLog().Snapshot();
    TestEqual(TEXT("New events use replacement owner"),Rows.Last().ProjectId,M.Project.Id);
    TestEqual(TEXT("Existing entries keep original owner"),Rows[0].ProjectId,Project);
    return true;
}
#endif
