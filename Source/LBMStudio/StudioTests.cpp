#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioSource,"Studio.CFD.PublishedNodeAndInterpolation",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioSource::RunTest(const FString&)
{
    FRecordedSolver Solver;
    TestTrue(TEXT("Published fixture loads"),Solver.LoadError().IsEmpty());
    TestEqual(TEXT("Complete published trajectory"),Solver.FrameCount(),601);
    const auto Field=Solver.CaptureField(0); FStudioFieldValue Value;
    // Independently decoded original MeshGraphNets source node 1000, not a generated expectation.
    const FVector P(.03250676393508911,0,.1194048523902893);
    TestTrue(TEXT("Original node is covered"),Field->Sample(P,Value));
    TestTrue(TEXT("Original momentum/density U"),FMath::IsNearlyEqual(Value.Velocity.X,82.6912444334034,1e-5));
    TestTrue(TEXT("Source y velocity maps to Unreal Z"),FMath::IsNearlyEqual(Value.Velocity.Z,18.91267044409305,1e-5));
    TestTrue(TEXT("Actual pressure preserved"),FMath::IsNearlyEqual(Value.Pressure,98699.78125,1e-5));
    TestTrue(TEXT("Actual density preserved"),FMath::IsNearlyEqual(Value.Density,1.2020570039749146,1e-8));
    FStudioFieldValue Interpolated;
    TestTrue(TEXT("Source triangle centroid is covered"),Field->Sample(FVector(.04375161727269494,0,.1260041743516922),Interpolated));
    TestTrue(TEXT("Barycentric velocity matches original nodes 1071,1000,1001"),FMath::IsNearlyEqual(Interpolated.Velocity.X,82.71352482073179,1e-5));
    TestTrue(TEXT("Pressure interpolation matches original node mean"),FMath::IsNearlyEqual(Interpolated.Pressure,98938.66666666667,1e-5));
    FStudioFieldValue Extruded;
    TestTrue(TEXT("Extruded plane available"),Field->Sample(FVector(P.X,.7,P.Z),Extruded));
    TestEqual(TEXT("Extrusion adds no spanwise variation"),Value.Velocity,Extruded.Velocity);
    TestTrue(TEXT("Boundary is the source polygon"),Field->IsSolid(FVector(0,0,0)));
    TestFalse(TEXT("No interpolated data inside solid"),Field->Sample(FVector(0,0,0),Value));
    TestFalse(TEXT("No extrapolation outside crop"),Field->Sample(FVector(3,0,0),Value));
    FVector FirstVelocity,NextVelocity;
    TestTrue(TEXT("First vector sample is available"),Field->SampleVelocity(P,FirstVelocity));
    TestTrue(TEXT("Next vector sample is available"),Solver.CaptureField(1)->SampleVelocity(P,NextVelocity));
    TestFalse(TEXT("Two actual snapshots differ"),FirstVelocity.Equals(NextVelocity,1e-7));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioLifecycle,"Studio.CFD.PlaybackLifecycle",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioLifecycle::RunTest(const FString&)
{
    FStudioModel M(FPaths::ProjectDir()/TEXT("tmp/debug/cfd-test-session"));
    TestEqual(TEXT("Full recording available immediately"),M.Frames.Num(),601);
    M.Run(); for(int32 I=0;I<4;++I) M.Tick(.25);
    TestEqual(TEXT("One wall second advances 20 snapshots"),M.DisplayFrame().Index,20);
    TestTrue(TEXT("Physical time follows published metadata"),FMath::IsNearlyEqual(M.DisplayFrame().Time,.004,1e-12));
    M.Pause(); M.Tick(.25);TestEqual(TEXT("Pause retains frame"),M.DisplayFrame().Index,20);
    M.Step();TestEqual(TEXT("Step advances one actual frame"),M.DisplayFrame().Index,21);
    M.Run();for(int32 I=0;I<116;++I)M.Tick(.25);
    TestTrue(TEXT("Complete after supplied frame600"),M.State==EStudioRunState::Complete);
    TestEqual(TEXT("Stops at source end"),M.DisplayFrame().Index,600);
    M.Step();M.Tick(5);TestEqual(TEXT("No additional output invented"),M.Frames.Num(),601);
    M.Run();TestEqual(TEXT("Run restarts source recording"),M.DisplayFrame().Index,0);
    M.PlaybackRate=2.;M.Tick(.25);TestEqual(TEXT("2x speed advances10frames perquartersecond"),M.PlaybackFrame,10);
    M.bLoopPlayback=true;for(int32 I=0;I<60;++I)M.Tick(.25);
    TestTrue(TEXT("Loop stays running"),M.State==EStudioRunState::Running);
    TestEqual(TEXT("Loop wraps to original source frames"),M.PlaybackFrame,9);
    TestEqual(TEXT("Loop does not extend source history"),M.Frames.Num(),601);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioImmutableFieldHistory,"Studio.CFD.ImmutableHistoryAndExport",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioImmutableFieldHistory::RunTest(const FString&)
{
    FStudioModel M(FPaths::ProjectDir()/TEXT("tmp/debug/cfd-test-session")); M.Run(); for(int32 I=0;I<4;++I)M.Tick(.25);
    M.Scrub(0); const auto Field=M.Solver->CaptureField(M.SelectedFrame);
    const FVector P(.03250676393508911,0,.1194048523902893); FVector Before,After;
    TestTrue(TEXT("Pinned vector sample available"),Field->SampleVelocity(P,Before));
    for(int32 I=0;I<4;++I)M.Tick(.25);
    TestEqual(TEXT("Review holds source frame while playback advances"),M.DisplayFrame().Index,0);
    TestTrue(TEXT("Pinned vector remains available"),Field->SampleVelocity(P,After));
    TestEqual(TEXT("Recorded field stays immutable"),After,Before);
    M.ReturnToLive();TestEqual(TEXT("Follow replay selects latest"),M.DisplayFrame().Index,40);
    const FString Path=FPaths::ProjectDir()/TEXT("tmp/debug/source-export-test.csv");
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);
    TestTrue(TEXT("Export original field nodes"),M.ExportField(Path));
    FString CSV; FFileHelper::LoadFileToString(CSV,*Path);
    TArray<FString> Rows;CSV.ParseIntoArrayLines(Rows);
    TestEqual(TEXT("One row per original node plus header"),Rows.Num(),5234);
    TestTrue(TEXT("Export identifies actual source frame"),Rows.Num()>1&&Rows[1].StartsWith(TEXT("MeshGraphNets_SU2_test009,40,")));
    TestFalse(TEXT("Unavailable monitor values are absent"),CSV.Contains(TEXT("lift_coefficient")));
    M.Pause();M.Scrub(.5);TestEqual(TEXT("Can scrub entire recording"),M.DisplayFrame().Index,300);
    M.Step();TestEqual(TEXT("Step follows reviewed frame"),M.DisplayFrame().Index,301);
    const FString ViewPath=FPaths::ProjectDir()/TEXT("tmp/debug/playback-view-test.json");
    M.PlaybackRate=.5;M.bLoopPlayback=true;TestTrue(TEXT("Save playback preferences"),M.SaveProject(ViewPath));
    M.PlaybackRate=1.;M.bLoopPlayback=false;TestTrue(TEXT("Load playback preferences"),M.LoadProject(ViewPath));
    TestEqual(TEXT("Speed restored"),M.PlaybackRate,.5);TestTrue(TEXT("Loop restored"),M.bLoopPlayback);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioInvalid,"Studio.CFD.InvalidFixtureFailsClosed",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioInvalid::RunTest(const FString&)
{
    const FString Path=FPaths::ProjectDir()/TEXT("tmp/debug/invalid-flow.bin");
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);
    FFileHelper::SaveStringToFile(TEXT("invalid"),*Path);
    FRecordedSolver Bad(Path);
    TestEqual(TEXT("Corruption does not create frames"),Bad.FrameCount(),0);
    TestFalse(TEXT("Corruption is explained"),Bad.LoadError().IsEmpty());
    FStudioFieldValue V;TestFalse(TEXT("No fabricated fallback"),Bad.CaptureField(0)->Sample(FVector(-1,0,.2),V));
    return true;
}
#endif
