#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
FString LoadingTestRoot(const TCHAR* Name)
{
    return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/ProjectLoadingTests")/Name/FGuid::NewGuid().ToString());
}
bool FinishOpening(FStudioModel& Model)
{
    const double Deadline=FPlatformTime::Seconds()+10;
    while(Model.IsProjectOpenPending()&&FPlatformTime::Seconds()<Deadline)
    { Model.Tick(0); FPlatformProcess::Sleep(.001f); }
    return !Model.IsProjectOpenPending();
}
FStudioProject LoadingTarget()
{
    FStudioProject Project;
    Project.Name=TEXT("Verified second recording");
    Project.Dataset=TEXT("MeshGraphNets_Airfoil_test010");
    Project.SelectedFrame=420;
    Project.Camera.Position=FVector(7,8,9);
    Project.View.bVectors=false;
    Project.Runs={FStudioRunRecord::Recording(Project.Name,Project.Dataset)};
    return Project;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioAsyncProjectOpen,"Studio.ProjectLoading.OpenKeepsCurrentViewUntilVerified",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioAsyncProjectOpen::RunTest(const FString&)
{
    const FString Root=LoadingTestRoot(TEXT("open")),Path=Root/TEXT("target.lbms"); FString Error;
    const auto Target=LoadingTarget(); TestTrue(TEXT("Target saved"),StudioProjectIO::Save(Path,Target,Error));
    FStudioModel Model(Root/TEXT("session")); const auto PreviousId=Model.Project.Id;
    Model.Scrub(.5); Model.Navigate(EStudioWorkspace::Geometry);
    TestTrue(TEXT("Opening accepted"),Model.RequestProjectOpen(Path));
    TestTrue(TEXT("Work remains pending until publication"),Model.IsProjectOpenPending());
    TestFalse(TEXT("Another open cannot accumulate a worker"),Model.RequestProjectOpen(Path));
    TestFalse(TEXT("Recording change cannot race project opening"),Model.RequestRecording(Target.Dataset));
    TestEqual(TEXT("Old document is retained during I/O"),Model.Project.Id,PreviousId);
    TestEqual(TEXT("Old selected frame remains"),Model.SelectedFrame,300);
    Model.Project.Camera.Position=FVector(3,5,7); Model.bVectors=false;
    Model.Run(); // These interactions must not invalidate the requested open.
    TestTrue(TEXT("Candidate completed"),FinishOpening(Model));
    TestEqual(TEXT("Verified candidate replaces project"),Model.Project.Id,Target.Id);
    TestEqual(TEXT("Original recording identity"),Model.Solver->Descriptor().Id,Target.Dataset);
    TestEqual(TEXT("Saved frame restored"),Model.SelectedFrame,420);
    TestEqual(TEXT("Saved camera restored"),Model.Project.Camera.Position,Target.Camera.Position);
    TestEqual(TEXT("Loaded view routes to Solve"),Model.Workspace,EStudioWorkspace::Solve);
    TestTrue(TEXT("Saved frame is actually readable"),Model.Solver->CaptureField(420)->IsValid());
    TestFalse(TEXT("Opening is clean"),Model.HasUnsavedChanges());
    TestTrue(TEXT("Recent entry added after success"),Model.RecentProjects.Contains(Path));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioAsyncProjectStale,"Studio.ProjectLoading.CancelAndProtectLaterEdits",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioAsyncProjectStale::RunTest(const FString&)
{
    const FString Root=LoadingTestRoot(TEXT("stale")),Path=Root/TEXT("target.lbms"); FString Error;
    TestTrue(TEXT("Target saved"),StudioProjectIO::Save(Path,LoadingTarget(),Error));
    FStudioModel Model(Root/TEXT("session")); Model.Scrub(.25);
    const FString Before=StudioProjectIO::Serialize(Model.SnapshotProject());
    TestTrue(TEXT("Open begins"),Model.RequestProjectOpen(Path)); Model.CancelProjectOpen();
    TestTrue(TEXT("Cancelled worker drains"),FinishOpening(Model));
    TestEqual(TEXT("Cancellation retains complete document"),StudioProjectIO::Serialize(Model.SnapshotProject()),Before);
    TestFalse(TEXT("Cancelled file not added to recents"),Model.RecentProjects.Contains(Path));
    TestTrue(TEXT("Open again"),Model.RequestProjectOpen(Path));
    TestTrue(TEXT("Authoring remains available"),Model.EditCase(TEXT("Edit while loading"),[](auto& Draft){Draft.Name=TEXT("Keep this edit");}));
    TestTrue(TEXT("Stale candidate drains"),FinishOpening(Model));
    TestEqual(TEXT("New authoring edit wins"),Model.Project.Draft.Name,FString(TEXT("Keep this edit")));
    TestTrue(TEXT("Conflict explained"),Model.Notice.Contains(TEXT("changed while opening")));
    TestTrue(TEXT("Edit undo history preserved"),Model.UndoCase());
    TestTrue(TEXT("Open before rename"),Model.RequestProjectOpen(Path)); Model.RenameProject(TEXT("Keep this name"));
    TestTrue(TEXT("Rename conflict drains"),FinishOpening(Model));
    TestEqual(TEXT("New project name survives"),Model.Project.Name,FString(TEXT("Keep this name")));
    TestTrue(TEXT("Open before replacement"),Model.RequestProjectOpen(Path)); Model.NewProject(TEXT("Replacement"));
    const auto Replacement=Model.Project.Id;
    TestTrue(TEXT("Replaced project's worker drains"),FinishOpening(Model));
    TestEqual(TEXT("New project wins"),Model.Project.Id,Replacement);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioAsyncProjectFailure,"Studio.ProjectLoading.FailureRelocationAndRecovery",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioAsyncProjectFailure::RunTest(const FString&)
{
    const FString Root=LoadingTestRoot(TEXT("failure")),Path=Root/TEXT("target.lbms"),Old=Root/TEXT("old-location.lbms"); FString Error;
    FStudioModel Model(Root/TEXT("session")); const auto Id=Model.Project.Id;
    Model.RecentProjects.Add(Old);
    TestTrue(TEXT("Missing file starts bounded check"),Model.RequestProjectOpen(Path,Old));
    TestTrue(TEXT("Missing file finishes"),FinishOpening(Model));
    TestEqual(TEXT("Missing file cannot replace current project"),Model.Project.Id,Id);
    TestTrue(TEXT("Failed Locate keeps original recent entry"),Model.RecentProjects.Contains(Old));
    auto Target=LoadingTarget(); Target.SelectedFrame=999;
    TestTrue(TEXT("Structurally valid project saved"),StudioProjectIO::Save(Path,Target,Error));
    TestTrue(TEXT("Invalid source frame checked off-thread"),Model.RequestProjectOpen(Path));
    TestTrue(TEXT("Invalid frame finishes"),FinishOpening(Model));
    TestEqual(TEXT("Invalid frame retains project"),Model.Project.Id,Id);
    TestTrue(TEXT("Frame problem explained"),Model.Notice.Contains(TEXT("source frame")));
    Target.SelectedFrame=5;
    TestTrue(TEXT("Target repaired"),StudioProjectIO::Save(Path,Target,Error));
    TestTrue(TEXT("Locate retry starts"),Model.RequestProjectOpen(Path,Old));
    TestTrue(TEXT("Locate completes"),FinishOpening(Model));
    TestFalse(TEXT("Old recent removed only after success"),Model.RecentProjects.Contains(Old));
    TestTrue(TEXT("New recent retained"),Model.RecentProjects.Contains(Path));
    TestTrue(TEXT("Unsaved recovery edit"),Model.RenameProject(TEXT("Recovered edit"))); Model.WriteRecovery();
    const FString Recovery=Root/TEXT("session/Recovery/StudioRecovery.lbms");
    Model.PendingRecovery=Recovery; FPaths::MakePathRelativeTo(Model.PendingRecovery,FPlatformProcess::BaseDir());
    TestTrue(TEXT("Recovery preparation begins"),Model.RequestRecoveryOpen());
    TestTrue(TEXT("Recovery prepared"),FinishOpening(Model));
    TestEqual(TEXT("Recovery original save location retained"),Model.ProjectPath,Path);
    TestEqual(TEXT("Recovery content retained"),Model.Project.Name,FString(TEXT("Recovered edit")));
    TestTrue(TEXT("Recovered work requires saving"),Model.HasUnsavedChanges());
    TestTrue(TEXT("Recovery prompt cleared"),Model.PendingRecovery.IsEmpty());
    // A discarded recovery may never be resurrected by a late completion.
    Model.WriteRecovery(); Model.PendingRecovery=Recovery;
    TestTrue(TEXT("Second recovery preparation begins"),Model.RequestRecoveryOpen()); Model.DiscardRecovery();
    TestTrue(TEXT("Discarded recovery drains"),FinishOpening(Model));
    TestEqual(TEXT("Discard leaves current project"),Model.Project.Name,FString(TEXT("Recovered edit")));
    const FString LegacySession=Root/TEXT("legacy-session");
    TestTrue(TEXT("Old preference file prepared"),StudioProjectIO::Save(LegacySession/TEXT("StudioProject.json"),Target,Error));
    FStudioModel Legacy(LegacySession); Legacy.OpenSession();
    TestTrue(TEXT("Legacy opening also runs off-thread"),Legacy.IsProjectOpenPending());
    TestTrue(TEXT("Legacy candidate finishes"),FinishOpening(Legacy));
    TestEqual(TEXT("Legacy source selection retained"),Legacy.SelectedFrame,Target.SelectedFrame);
    TestTrue(TEXT("Legacy import needs a new destination"),Legacy.ProjectPath.IsEmpty());
    TestTrue(TEXT("Legacy import remains unsaved"),Legacy.HasUnsavedChanges());
    TestFalse(TEXT("Legacy preferences not listed as a normal project"),Legacy.RecentProjects.Contains(LegacySession/TEXT("StudioProject.json")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioCancelledRecordingPreparation,"Studio.ProjectLoading.InterruptibleRecordingPreparation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioCancelledRecordingPreparation::RunTest(const FString&)
{
    auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    FRecordedSolver Abandoned(FString(),8LL*1024*1024,Cancel);
    TestEqual(TEXT("Cancelled indexing publishes no frames"),Abandoned.FrameCount(),0);
    TestTrue(TEXT("Cancellation is explained"),Abandoned.LoadError().Contains(TEXT("cancelled")));
    FRecordedSolver Source;
    TestFalse(TEXT("Cancelled preparation reads no frame"),Source.PrepareFrame(42,Cancel));
    TestEqual(TEXT("Cancelled frame does not enter cache"),Source.CacheStats().ResidentFrames,0);
    Cancel->store(false);
    TestTrue(TEXT("Fresh preparation succeeds"),Source.PrepareFrame(42,Cancel));
    const auto Frame=Source.CaptureField(42); Cancel->store(true);
    TestTrue(TEXT("Cancelling request does not poison published fields"),Frame->IsValid());
    TestTrue(TEXT("Future independent frame remains readable"),Source.CaptureField(43)->IsValid());
    TestTrue(TEXT("No spurious data integrity error"),Source.LoadError().IsEmpty());
    return true;
}
#endif
