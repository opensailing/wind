#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto ResultFrameFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FString ResultFrameRoot()
{return FPaths::ProjectDir()/TEXT("tmp/debug/result-frame-tests")/FGuid::NewGuid().ToString();}
bool FinishResultFrame(FStudioModel& Model)
{
    const double Deadline=FPlatformTime::Seconds()+10;
    while(Model.IsRecordingLoadPending()&&FPlatformTime::Seconds()<Deadline)
    {Model.Tick(0);FPlatformProcess::SleepNoStats(.001f);}
    return !Model.IsRecordingLoadPending();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioResultExactReview,"Studio.Results.ExactReviewAndPlaybackCursor",ResultFrameFlags)
bool FStudioResultExactReview::RunTest(const FString&)
{
    FStudioModel M(ResultFrameRoot());M.Run();M.Tick(.1);
    const auto Source=M.Solver;
    const auto Camera=M.Project.Camera.Position;
    const auto Draft=StudioCaseIO::Serialize(M.Project.Draft);
    const auto Intent=M.RenderIntentRevision;
    TestTrue(TEXT("Last original frame can be reviewed exactly"),M.ReviewRecordedFrame(600));
    TestEqual(TEXT("Exact ordinal without fractional round trip"),M.SelectedFrame,600);
    TestEqual(TEXT("Review keeps playback cursor"),M.PlaybackFrame,2);
    TestEqual(TEXT("Review invalidates obsolete render work"),M.RenderIntentRevision,Intent+1);
    M.Tick(.05);
    TestEqual(TEXT("Playback continues separately"),M.PlaybackFrame,3);
    TestEqual(TEXT("Reviewed frame remains fixed"),M.SelectedFrame,600);
    TestTrue(TEXT("Review does not replace reader or camera"),M.Solver==Source&&M.Project.Camera.Position==Camera);
    TestEqual(TEXT("Review does not edit case"),StudioCaseIO::Serialize(M.Project.Draft),Draft);
    const auto Before=StudioProjectIO::Serialize(M.SnapshotProject());
    const auto ReviewIntent=M.RenderIntentRevision;
    for(const int32 Ordinal:{-1,601,MAX_int32})
        TestFalse(TEXT("Invalid ordinals are rejected without clamping"),M.ReviewRecordedFrame(Ordinal));
    M.Scrub(std::numeric_limits<double>::quiet_NaN());
    M.Scrub(std::numeric_limits<double>::infinity());
    TestEqual(TEXT("Invalid input retains whole document"),StudioProjectIO::Serialize(M.SnapshotProject()),Before);
    TestEqual(TEXT("Invalid input cannot invalidate current render"),M.RenderIntentRevision,ReviewIntent);
    M.ReturnToLive();
    TestTrue(TEXT("Return to live restores real playback cursor"),!M.bReviewing&&M.SelectedFrame==3);
    M.Scrub(std::numeric_limits<double>::max());
    TestEqual(TEXT("Finite slider input clamps before integer conversion"),M.SelectedFrame,600);
    M.Scrub(-std::numeric_limits<double>::max());
    TestEqual(TEXT("Negative slider input clamps to first frame"),M.SelectedFrame,0);
    M.Solver=MakeShared<FRecordedSolver,ESPMode::ThreadSafe>(ResultFrameRoot()/TEXT("missing/flow.bin"));
    TestFalse(TEXT("Placeholder metadata is never a selectable CFD frame"),M.ReviewRecordedFrame(0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioResultExactOpen,"Studio.Results.TransactionalFrameOpen",ResultFrameFlags)
bool FStudioResultExactOpen::RunTest(const FString&)
{
    const FString Root=ResultFrameRoot(),Second=TEXT("MeshGraphNets_Airfoil_test010");
    FStudioModel M(Root/TEXT("session"));M.ReviewRecordedFrame(37);
    M.Project.Camera.Position=FVector(7,8,9);
    const auto Original=M.Solver;
    const auto Before=StudioProjectIO::Serialize(M.SnapshotProject());
    TestFalse(TEXT("Negative ordinal rejected before opening"),M.RequestRecording(Second,-1));
    TestFalse(TEXT("Rejected request has no worker"),M.IsRecordingLoadPending());
    TestTrue(TEXT("Unavailable target checked by source reader"),M.RequestRecording(Second,601));
    if(!TestTrue(TEXT("Invalid frame read drains"),FinishResultFrame(M)))return false;
    TestTrue(TEXT("Invalid frame retains exact source and document"),M.Solver==Original&&StudioProjectIO::Serialize(M.SnapshotProject())==Before);
    TestFalse(TEXT("Unavailable frame reports a reason"),M.Notice.IsEmpty());
    TestTrue(TEXT("Exact second-source frame requested"),M.RequestRecording(Second,420));
    TestFalse(TEXT("A second request cannot overwrite the pending ordinal"),M.RequestRecording(Second,3));
    TestTrue(TEXT("Current source and frame retained until publication"),M.Solver==Original&&M.SelectedFrame==37);
    // Camera and case edits made while the reader is active must survive.
    M.Project.Camera.Position=FVector(4,5,6);
    M.EditCase(TEXT("Keep authoring during result opening"),[](auto& Draft){Draft.Name=TEXT("Later case edit");});
    const auto Draft=StudioCaseIO::Serialize(M.Project.Draft);
    if(!TestTrue(TEXT("Selected original frame becomes ready"),FinishResultFrame(M)))return false;
    TestEqual(TEXT("Verified recording published"),M.Project.Dataset,Second);
    TestTrue(TEXT("Both cursors start at requested ordinal"),M.SelectedFrame==420&&M.PlaybackFrame==420);
    TestTrue(TEXT("Opened frame paused for inspection"),M.bReviewing&&M.State==EStudioRunState::Paused);
    TestEqual(TEXT("Later camera retained"),M.Project.Camera.Position,FVector(4,5,6));
    TestEqual(TEXT("Later case retained"),StudioCaseIO::Serialize(M.Project.Draft),Draft);
    const auto Identity=M.Solver->CaptureField(420)->Identity();
    TestTrue(TEXT("Prepared field has requested original frame identity"),Identity.IsSet()&&Identity->Ordinal==420&&Identity->Frame.Index==M.DisplayFrame().Index&&Identity->Frame.Time==M.DisplayFrame().Time);
    TestEqual(TEXT("Recording adds one run, never a run per frame"),M.Project.Runs.Num(),2);
    TestTrue(TEXT("Result position saves"),M.SaveProject(Root/TEXT("selected.lbms")));
    FStudioModel Reopened(Root/TEXT("reopened"));
    TestTrue(TEXT("Exact result position reopens"),Reopened.LoadProject(Root/TEXT("selected.lbms"))&&Reopened.SelectedFrame==420&&Reopened.Project.Dataset==Second);
    const auto Committed=StudioProjectIO::Serialize(M.SnapshotProject());
    TestTrue(TEXT("Another exact frame requested"),M.RequestRecording(TEXT("MeshGraphNets_Airfoil_test009"),600));
    M.CancelRecording();
    if(!TestTrue(TEXT("Cancelled frame read drains"),FinishResultFrame(M)))return false;
    TestEqual(TEXT("Cancellation retains selected result"),StudioProjectIO::Serialize(M.SnapshotProject()),Committed);
    TestTrue(TEXT("Ordinary source selection still defaults to frame zero"),M.RequestRecording(TEXT("MeshGraphNets_Airfoil_test009")));
    if(!TestTrue(TEXT("Default selection drains"),FinishResultFrame(M)))return false;
    TestEqual(TEXT("Cancelled ordinal cannot leak into next request"),M.SelectedFrame,0);
    TestEqual(TEXT("Revisiting recording does not duplicate run"),M.Project.Runs.Num(),2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioResultSourceSteps,"Studio.Results.OriginalStepIDsAreNotOrdinals",ResultFrameFlags)
bool FStudioResultSourceSteps::RunTest(const FString&)
{
    FStudioModel M(ResultFrameRoot());
    TestTrue(TEXT("Import original NACA snapshots"),M.RequestExternalRecording(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json")));
    if(!TestTrue(TEXT("Original recording imported"),FinishResultFrame(M)&&M.Frames.Num()==3))return false;
    const FString Dataset=M.Project.Dataset;
    TestTrue(TEXT("Review second ordinal"),M.ReviewRecordedFrame(1));
    TestEqual(TEXT("Original nonordinal step retained"),M.DisplayFrame().Index,5001);
    TestEqual(TEXT("Original nonuniform physical time retained"),M.DisplayFrame().Time,12.5025);
    TestFalse(TEXT("Source step is not accepted as an ordinal"),M.ReviewRecordedFrame(5001));
    TestTrue(TEXT("Load final exact ordinal using saved source identity"),M.RequestRecording(Dataset,2));
    if(!TestTrue(TEXT("Saved source read drains"),FinishResultFrame(M)))return false;
    TestTrue(TEXT("Final source step and time preserved"),M.SelectedFrame==2&&M.DisplayFrame().Index==9000&&M.DisplayFrame().Time==22.5);
    TestTrue(TEXT("Reconstruction-independent source frame remains readable"),M.Solver->CaptureField(2)->IsValid());
    TestTrue(TEXT("Pending result requested before document replacement"),M.RequestRecording(Dataset,1));
    M.NewProject(TEXT("New document"));const auto NewId=M.Project.Id;
    if(!TestTrue(TEXT("Old document frame read drains"),FinishResultFrame(M)))return false;
    TestTrue(TEXT("Old completion cannot move new document cursor"),M.Project.Id==NewId&&M.SelectedFrame==0&&M.Project.Dataset!=Dataset&&M.Project.Runs.Num()==1);
    return true;
}
#endif
