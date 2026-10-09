#include "StudioNotifications.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
namespace StudioNotificationTests
{
FString Session(){return FPaths::ProjectDir()/TEXT("tmp/debug/notification-sessions")/FGuid::NewGuid().ToString();}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioNotificationBounds,"Studio.Notifications.BoundedContextAndAcknowledgement",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioNotificationBounds::RunTest(const FString&)
{
    FStudioLogJournal Log;FStudioNotificationJournal Journal;const FGuid Project=FGuid::NewGuid(),Run=FGuid::NewGuid();
    const FDateTime Original(2026,1,2,3,4,5);
    auto Add=[&](EStudioLogSeverity Severity,const FString& Message,bool Completion=false)
    {Log.Append(Message,Severity,EStudioLogSource::ControlHarness,Project,Run,TEXT("real-source-reference"),Original);return Journal.Observe(*Log.Latest(),Completion);};
    TestFalse(TEXT("Info is not a notification"),Add(EStudioLogSeverity::Info,TEXT("Preparing")));
    TestTrue(TEXT("Warnings observed"),Add(EStudioLogSeverity::Warning,TEXT("Disconnected")));
    const uint64 First=Journal.LastSequence();
    TestFalse(TEXT("Repeated accepted log event is not duplicated"),Journal.Observe(*Log.Latest()));
    const auto Item=*Journal.Find(First);
    TestEqual(TEXT("Observed UTC retained independently of solver time"),Item.ObservedUTC,Original);
    TestEqual(TEXT("Exact project identity"),Item.ProjectId,Project);TestEqual(TEXT("Exact run identity"),Item.RunId,Run);
    TestTrue(TEXT("Harness title does not imply a numerical solve"),StudioNotifications::Title(Item).StartsWith(TEXT("Control harness")));
    TestTrue(TEXT("Explicit completion observed"),Add(EStudioLogSeverity::Info,TEXT("Completed; no CFD generated"),true));
    const auto Through=Journal.LastSequence();
    TestTrue(TEXT("Later arrival observed"),Add(EStudioLogSeverity::Error,TEXT("Failed")));
    Journal.MarkReadThrough(Through);
    TestEqual(TEXT("Bulk acknowledgement does not read later arrivals"),Journal.UnreadCount(),1);
    TestTrue(TEXT("Individual unread state reversible"),Journal.SetRead(First,false));
    TestEqual(TEXT("Unread count reflects manual undo"),Journal.UnreadCount(),2);
    for(int32 I=0;I<300;++I)Add(EStudioLogSeverity::Warning,FString::ChrN(3000,TCHAR('x')));
    const auto Entries=Journal.Snapshot();
    TestEqual(TEXT("History hard bounded"),Entries.Num(),FStudioNotificationJournal::Capacity);
    TestFalse(TEXT("Evicted sequence no longer markable"),Journal.SetRead(First,true));
    TestEqual(TEXT("Eviction count exact"),Journal.EvictedCount(),Journal.LastSequence()-uint64(Entries.Num()));
    for(int32 I=1;I<Entries.Num();++I)TestTrue(TEXT("Ring order monotonic"),Entries[I].Sequence>Entries[I-1].Sequence);
    TestTrue(TEXT("Long messages explicitly truncated"),Entries.Last().bTruncated&&Entries.Last().Message.Len()==FStudioLogJournal::MessageLimit);
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioNotificationLifecycle,"Studio.Notifications.OriginalReplayAndHarnessLifecycle",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioNotificationLifecycle::RunTest(const FString&)
{
    const FString Root=StudioNotificationTests::Session();FStudioModel M(Root);
    ON_SCOPE_EXIT{IFileManager::Get().DeleteDirectory(*Root,false,true);};
    if(!TestTrue(TEXT("Original recording available"),M.Solver&&M.Solver->FrameCount()>1))return false;
    const auto Original=M.Project.Id;const FString Case=StudioCaseIO::Serialize(M.Project.Draft);
    M.bLoopPlayback=false;M.State=EStudioRunState::Paused;M.PlaybackFrame=M.Solver->FrameCount()-2;M.Step();
    auto Entries=M.Notifications().Snapshot();
    if(!TestEqual(TEXT("Original playback completion notified once"),Entries.Num(),1))return false;
    TestTrue(TEXT("Replay completion distinct from job"),Entries[0].Source==EStudioLogSource::Playback&&!Entries[0].RunId.IsValid());
    TestEqual(TEXT("Original dataset retained"),Entries[0].SourceReference,M.Project.Dataset);
    const uint64 ReplayId=Entries[0].Sequence;FString Reason;TestTrue(TEXT("Current original completion can be inspected"),M.CanRevealNotification(ReplayId,Reason));
    TestEqual(TEXT("Observation does not edit case"),StudioCaseIO::Serialize(M.Project.Draft),Case);
    M.SetControlHarness(true);M.Control(EStudioJobCommand::Submit);M.Tick(.1);
    const auto Run=M.Job().Run()->GetId();M.SimulateJobEvent(EStudioJobState::Completed);M.Tick(.1);M.Tick(.1);
    Entries=M.Notifications().Snapshot();
    TestEqual(TEXT("Completion is not duplicated on idle polls"),Entries.Num(),2);
    TestEqual(TEXT("Completion belongs to exact accepted run"),Entries.Last().RunId,Run);
    TestTrue(TEXT("Harness origin explicit"),Entries.Last().Source==EStudioLogSource::ControlHarness);
    TestTrue(TEXT("Current run can be inspected"),M.CanRevealNotification(Entries.Last().Sequence,Reason));
    M.Control(EStudioJobCommand::Submit);M.Tick(.1);M.SimulateJobEvent(EStudioJobState::Disconnected);
    TestTrue(TEXT("Disconnect warning notified"),M.Notifications().Snapshot().Last().Kind==EStudioNotificationKind::Warning);
    M.Control(EStudioJobCommand::Reconnect);M.Tick(.1);M.SimulateJobEvent(EStudioJobState::Failed);
    TestTrue(TEXT("Accepted failure notified"),M.Notifications().Snapshot().Last().Kind==EStudioNotificationKind::Error);
    M.NewProject(TEXT("New notification context"));
    TestFalse(TEXT("Earlier project cannot be revealed into current state"),M.CanRevealNotification(ReplayId,Reason));
    TestTrue(TEXT("Reason explains project boundary"),Reason.Contains(TEXT("project")));
    TestEqual(TEXT("Retained context never reowned"),M.Notifications().Snapshot()[0].ProjectId,Original);
    M.AddLog(TEXT("retained issue"),EStudioLogSeverity::Error);const uint64 ErrorId=M.Notifications().LastSequence();
    for(int32 I=0;I<FStudioLogJournal::Capacity;++I)M.AddLog(TEXT("ordinary info"));
    TestFalse(TEXT("Expired log link disabled"),M.CanRevealNotification(ErrorId,Reason));
    TestNotNull(TEXT("Notification text outlives log link"),M.Notifications().Find(ErrorId));
    const uint64 Before=M.Notifications().LastSequence();M.AddLog(FString(),EStudioLogSeverity::Info,EStudioLogSource::Playback,{}, {},true);
    TestEqual(TEXT("Empty completion cannot reclassify previous event"),M.Notifications().LastSequence(),Before);
    return !HasAnyErrors();
}
#endif
