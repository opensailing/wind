#include "StudioRunSettings.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioRunSettingsTests
{
constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
FString Directory()
{
    const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/RunSettings") / FGuid::NewGuid().ToString());
    IFileManager::Get().MakeDirectory(*Path, true); return Path;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioRunSettingsExact, "Studio.RunSettings.ExactLimitsAndInvalidDrafts", StudioRunSettingsTests::Flags)
bool FStudioRunSettingsExact::RunTest(const FString&)
{
    FStudioCaseDraft Case; Case.Setup.MaxPhysicalTime = .12345678901234567;
    Case.Setup.CheckpointInterval = 999999999999LL;
    FStudioRunSettingsEdit Edit; Edit.Reset(Case); FStudioCaseSetup Out;
    TestFalse(TEXT("Exact loaded settings are clean"), Edit.IsDirty());
    TestTrue(TEXT("Exact values build"), Edit.Build(Out));
    TestEqual(TEXT("Optional physical seconds remain exact"), *Out.MaxPhysicalTime, *Case.Setup.MaxPhysicalTime);
    TestEqual(TEXT("Large step interval remains exact"), Out.CheckpointInterval, Case.Setup.CheckpointInterval);
    Edit.Values[0] = TEXT("1000000000000"); Edit.Values[1] = TEXT("2.345678901234567");
    Edit.Values[2] = TEXT("250"); Edit.Values[3] = TEXT("500"); Edit.bCheckpoints = true;
    TestTrue(TEXT("All requested settings build together"), Edit.Build(Out));
    TestEqual(TEXT("Physical seconds preserve parsed double"), *Out.MaxPhysicalTime, 2.345678901234567);
    TestEqual(TEXT("Maximum exact integer supported"), Out.MaxSteps, 1000000000000LL);
    const auto Kept = Out;
    for (int32 Field : {0, 2, 3})
    for (const TCHAR* Bad : {TEXT(""), TEXT("0"), TEXT("-1"), TEXT("1.5"), TEXT("1e3"), TEXT("nan"), TEXT("inf"), TEXT("12 steps"),
        TEXT("1000000000001"), TEXT("99999999999999999999999999999"), TEXT("1000000000000.00001")})
    {
        Edit.Reset(Case); Edit.Values[Field] = Bad;
        TestFalse(TEXT("Counts require bounded whole-number digits"), Edit.Build(Out));
        TestEqual(TEXT("Error names the exact count field"), Edit.ErrorField, Field);
        TestEqual(TEXT("Rejected draft stays intact"), Edit.Values[Field], FString(Bad));
        TestTrue(TEXT("Failure leaves every output field unchanged"), Out.MaxSteps == Kept.MaxSteps && Out.MaxPhysicalTime == Kept.MaxPhysicalTime &&
            Out.OutputInterval == Kept.OutputInterval && Out.CheckpointInterval == Kept.CheckpointInterval && Out.bCheckpoints == Kept.bCheckpoints);
    }
    for (const TCHAR* Bad : {TEXT("0"), TEXT("-1"), TEXT("nan"), TEXT("inf"), TEXT("1e13"), TEXT("2 s")})
    {
        Edit.Reset(Case); Edit.Values[1] = Bad;
        TestFalse(TEXT("Physical duration must be finite positive seconds"), Edit.Build(Out));
        TestEqual(TEXT("Duration error is local"), Edit.ErrorField, 1);
    }
    Edit.Reset(Case); Edit.Values[1] = TEXT(" "); Edit.bCheckpoints = false;
    TestTrue(TEXT("Blank duration removes only the optional stop request"), Edit.Build(Out) && !Out.MaxPhysicalTime);
    TestEqual(TEXT("Disabled checkpoints retain their interval"), Out.CheckpointInterval, Case.Setup.CheckpointInterval);
    Edit.Reset(Case); Edit.Values[0] = TEXT(" 0000001 ");
    TestTrue(TEXT("Whitespace and leading zeros preserve integer meaning"), Edit.Build(Out) && Out.MaxSteps == 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioRunSettingsTransaction, "Studio.RunSettings.TransactionHistoryAndPersistence", StudioRunSettingsTests::Flags)
bool FStudioRunSettingsTransaction::RunTest(const FString&)
{
    FStudioModel Model(StudioRunSettingsTests::Directory()); Model.Pause();
    const auto Solver = Model.Solver; const auto Camera = Model.Project.Camera; const int32 Frame = Model.SelectedFrame;
    const uint64 RenderRevision = Model.RenderIntentRevision;
    FStudioRunSettingsEdit Edit; Edit.Reset(Model.Project.Draft);
    Edit.Values[0] = TEXT("123456789012"); Edit.Values[1] = TEXT("1.2345678901234567");
    Edit.Values[2] = TEXT("17"); Edit.Values[3] = TEXT("101"); Edit.bCheckpoints = true;
    // Independent ownership: do not restore old physics or lattice fields when applying.
    Model.EditCase(TEXT("Other setup owners"), [](auto& Case)
    {Case.Setup.TimeStep = .00001234567890123; Case.Setup.LatticeResolution = FIntVector(19, 27, 31); Case.Setup.BackendId = TEXT("test-control-harness");});
    const auto Before = Model.Project.Draft;
    TestTrue(TEXT("Unrelated setup edit does not conflict"), Edit.Matches(Model.Project.Draft));
    Model.Project.Runs.Add(FStudioRunRecord::Capture(TEXT("Earlier run"), Model.Project.Draft, EStudioRunOrigin::ControlHarness));
    const FString Frozen = StudioCaseIO::Serialize(*Model.Project.Runs.Last().GetConfiguration());
    TestTrue(TEXT("Apply run limits in one transaction"), Model.UpdateRunSettings(Edit));
    const auto Applied = Model.Project.Draft;
    TestEqual(TEXT("Maximum steps applied exactly"), Applied.Setup.MaxSteps, 123456789012LL);
    TestTrue(TEXT("Other owners retain their newest values"), Applied.Setup.TimeStep == Before.Setup.TimeStep &&
        Applied.Setup.LatticeResolution == Before.Setup.LatticeResolution && Applied.Setup.BackendId == Before.Setup.BackendId);
    TestEqual(TEXT("Frozen configuration is not edited"), StudioCaseIO::Serialize(*Model.Project.Runs.Last().GetConfiguration()), Frozen);
    TestTrue(TEXT("Recording and Solve camera are independent"), Model.Solver == Solver && StudioView::CameraEquals(Model.Project.Camera, Camera));
    TestEqual(TEXT("Recorded frame unchanged"), Model.SelectedFrame, Frame);
    TestEqual(TEXT("No scientific render request"), Model.RenderIntentRevision, RenderRevision);
    TestTrue(TEXT("One undo restores all run settings"), Model.UndoCase());
    auto Restored = Model.Project.Draft; Restored.Revision = Before.Revision;
    TestEqual(TEXT("Undo restores full case"), StudioCaseIO::Serialize(Restored), StudioCaseIO::Serialize(Before));
    TestTrue(TEXT("Redo reapplies the transaction"), Model.RedoCase());
    FStudioProject Loaded; FString Error;
    TestTrue(TEXT("Project reopens"), StudioProjectIO::Parse(StudioProjectIO::Serialize(Model.SnapshotProject()), Loaded, Error));
    auto Expected = Applied; Expected.Revision = Loaded.Draft.Revision;
    TestEqual(TEXT("Complete applied case survives reopening"), StudioCaseIO::Serialize(Loaded.Draft), StudioCaseIO::Serialize(Expected));
    TestEqual(TEXT("Saved prior run survives unchanged"), StudioCaseIO::Serialize(*Loaded.Runs.Last().GetConfiguration()), Frozen);
    Edit.Reset(Model.Project.Draft); const int64 CaseRevision = Model.Project.Draft.Revision;
    TestTrue(TEXT("Unchanged form may apply"), Model.UpdateRunSettings(Edit));
    TestEqual(TEXT("No-op creates no case revision"), Model.Project.Draft.Revision, CaseRevision);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioRunSettingsConflicts, "Studio.RunSettings.ConflictsAndActiveRunIsolation", StudioRunSettingsTests::Flags)
bool FStudioRunSettingsConflicts::RunTest(const FString&)
{
    FStudioModel Model(StudioRunSettingsTests::Directory()); Model.Pause();
    FStudioRunSettingsEdit Edit; Edit.Reset(Model.Project.Draft); Edit.Values[0] = TEXT("900");
    Model.EditCase(TEXT("External limit"), [](auto& Case){Case.Setup.MaxSteps = 25;});
    const FString Before = StudioCaseIO::Serialize(Model.Project.Draft);
    TestFalse(TEXT("Stale run form cannot overwrite an external edit"), Model.UpdateRunSettings(Edit));
    TestEqual(TEXT("Conflict leaves applied case intact"), StudioCaseIO::Serialize(Model.Project.Draft), Before);
    Edit.Reset(Model.Project.Draft); Edit.CaseId = FGuid::NewGuid();
    TestFalse(TEXT("Another case identity cannot apply"), Model.UpdateRunSettings(Edit));
    TestTrue(TEXT("Select real deterministic control adapter"), Model.SetControlHarness(true));
    TestTrue(TEXT("Submit applied case"), Model.Control(EStudioJobCommand::Submit)); Model.Tick(.1);
    const FString Frozen = StudioCaseIO::Serialize(*Model.Job().Run()->GetConfiguration());
    Edit.Reset(Model.Project.Draft); Edit.Values[0] = TEXT("1"); Edit.Values[1] = TEXT("0.0001");
    Edit.Values[2] = TEXT("1"); Edit.Values[3] = TEXT("1"); Edit.bCheckpoints = true;
    TestTrue(TEXT("Next-run edits are allowed while an old run is active"), Model.UpdateRunSettings(Edit));
    Model.Tick(10.);
    TestEqual(TEXT("Active run uses frozen settings"), StudioCaseIO::Serialize(*Model.Job().Run()->GetConfiguration()), Frozen);
    TestTrue(TEXT("Harness does not pretend to enforce physical/step limits"), Model.Job().State() == EStudioJobState::Running);
    TestEqual(TEXT("Schedule request does not invent checkpoint replies"), Model.Job().CompletedCheckpointCommands(), uint64(0));
    TestTrue(TEXT("Stop remains an explicit acknowledged command"), Model.Control(EStudioJobCommand::Stop)); Model.Tick(.1);
    TestTrue(TEXT("Stopped normally"), Model.Job().State() == EStudioJobState::Stopped);
    return true;
}
#endif
