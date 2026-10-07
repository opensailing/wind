#include "StudioJobs.h"
#include "StudioModel.h"
#include "SStudioHome4RunControls.h"
#include "StudioHeadlessSlate.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4RunControlTestFixtures
{
    // Deterministic control protocol fixtures. No CFD fields or science samples.
    FStudioCaseDraft Draft()
    {
        FStudioCaseDraft D; D.Setup.BackendId = TEXT("studio-control-harness"); D.Setup.MaxSteps = 1000;
        FStudioHome4Spec S; S.Reference.TimeSteps = 100.; S.Run.Steps = 1000;
        S.Run.MeasureEvery = 2; S.Run.SaveEvery = 3; S.Run.VizEvery = 4; S.Run.RestartEvery = 5;
        D.Home4 = S; return D;
    }
    void Pause(FStudioJobController& Job, const FStudioCaseDraft& D)
    { Job.Submit(TEXT("Unit test development controls"), D, 0); Job.Tick(.1); Job.Command(EStudioJobCommand::Pause, .1); Job.Tick(.2); }
    class FRangeAdapter final : public IStudioJobAdapter
    {
    public:
        FStudioJobRequest Request;
        TArray<FStudioJobEvent> Events;
        uint64 Sequence = 0;
        FStudioJobCapabilities Capabilities() const override
        { FStudioJobCapabilities C; C.BackendId = TEXT("studio-control-harness"); C.bControlHarness = C.bPause = C.bStep = C.bStepN = C.bRunToDimensionless = C.bReconnect = true; return C; }
        void Send(const FStudioJobRequest& R, double) override { Request = R; }
        void Poll(double, int32 Max, TArray<FStudioJobEvent>& Out) override
        { const int32 N = FMath::Min(Max, Events.Num()); Out.Append(Events.GetData(), N); Events.RemoveAt(0, N, EAllowShrinking::No); }
        void Reply(EStudioJobState State, EStudioJobEventKind Kind = EStudioJobEventKind::State, TOptional<int64> Steps = {})
        { FStudioJobEvent E; E.RunId = Request.RunId; E.CommandId = Request.CommandId; E.Sequence = ++Sequence; E.State = State; E.Kind = Kind; E.SimulatedSteps = Steps; Events.Add(E); }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ControlRanges, "Studio.Home4.Controls.ExactRangesFrozenReferencesAndCadences",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4ControlRanges::RunTest(const FString&)
{
    using namespace StudioHome4RunControlTestFixtures;
    FStudioJobController J(MakeUnique<FStudioControlHarness>()); auto D = Draft(); Pause(J, D);
    FStudioHome4Spec RoundTrip; FString ParseError;
    TestTrue(TEXT("Explicit restart cadence serializes through HOME4 request"), StudioHome4Config::Parse(StudioHome4Config::Serialize(*D.Home4), RoundTrip, ParseError));
    TestEqual(TEXT("Restart cadence round-trips independently"), RoundTrip.Run.RestartEvery.Get(-1), int64(5));
    D.Home4->Reference.TimeSteps = 999.; D.Home4->Run.MeasureEvery = 999;
    TestTrue(TEXT("Counted range accepted while paused"), J.StepN(10, .2));
    TestEqual(TEXT("No count inferred before acknowledgement"), J.SimulatedControlSteps(), int64(0));
    J.Tick(.3);
    TestEqual(TEXT("Exact acknowledged simulated step delta"), J.SimulatedControlSteps(), int64(10));
    TestEqual(TEXT("One range is one command acknowledgement"), J.CompletedStepCommands(), uint64(1));
    TestTrue(TEXT("Counted development range remains paused"), J.State() == EStudioJobState::Paused);
    TestEqual(TEXT("Four schedule kinds at crossings"), J.ScheduledOutputs().Num(), 4);
    TestEqual(TEXT("Trace crossings computed from captured cadence"), J.ScheduledOutputs()[0].Crossings, int64(5));
    TestEqual(TEXT("Slice crossings distinct"), J.ScheduledOutputs()[1].Crossings, int64(3));
    TestEqual(TEXT("Viz crossings distinct"), J.ScheduledOutputs()[2].Crossings, int64(2));
    TestEqual(TEXT("Restart crossings distinct"), J.ScheduledOutputs()[3].Crossings, int64(2));
    TestTrue(TEXT("Run to t* uses frozen time reference"), J.RunToDimensionless(.2, .3)); J.Tick(.4);
    TestEqual(TEXT("Frozen time maps requested target to integer control step"), J.SimulatedControlSteps(), int64(20));
    TestEqual(TEXT("Run to t* is another range acknowledgement"), J.CompletedStepCommands(), uint64(2));
    TestTrue(TEXT("Fractional time target reaches next integer step"), J.RunToDimensionless(.205, .4)); J.Tick(.5);
    TestEqual(TEXT("Integer ceiling target acknowledged"), J.SimulatedControlSteps(), int64(21));
    TestFalse(TEXT("No CFD telemetry exposed from control harness"), J.Telemetry().Sample.IsSet());
    TestTrue(TEXT("No recording claims attached"), J.Run()->GetDatasetId().IsEmpty());
    FStudioJobController Decimal(MakeUnique<FStudioControlHarness>()); Pause(Decimal, Draft());
    TestTrue(TEXT("Decimal integer target accepted"), Decimal.RunToDimensionless(.14, .2)); Decimal.Tick(.3);
    TestEqual(TEXT("Binary roundoff cannot add an extra lattice step"), Decimal.SimulatedControlSteps(), int64(14));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ControlValidation, "Studio.Home4.Controls.InvalidMissingAndHugeRanges",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4ControlValidation::RunTest(const FString&)
{
    using namespace StudioHome4RunControlTestFixtures;
    FStudioJobController J(MakeUnique<FStudioControlHarness>()); Pause(J, Draft());
    TestFalse(TEXT("Zero range rejected"), J.StepN(0, .2));
    TestFalse(TEXT("Negative range rejected"), J.StepN(-1, .2));
    TestFalse(TEXT("Frozen step limit enforced"), J.StepN(1001, .2));
    for (double Target : {0., -1., 11., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        TestFalse(TEXT("Invalid or out-of-bounds dimensionless target rejected"), J.RunToDimensionless(Target, .2));
    J.StepN(10, .2); J.Tick(.3);
    TestFalse(TEXT("Equal target cannot repeat a range"), J.RunToDimensionless(.1, .3));
    TestFalse(TEXT("Earlier target rejected"), J.RunToDimensionless(.05, .3));
    J.Command(EStudioJobCommand::Resume, .3); J.Tick(.4);
    TestFalse(TEXT("Step N unsupported while running"), J.StepN(1, .4));
    auto Missing = Draft(); Missing.Home4->Reference.TimeSteps.Reset();
    FStudioJobController K(MakeUnique<FStudioControlHarness>()); Pause(K, Missing);
    TestFalse(TEXT("Missing time map never guessed"), K.RunToDimensionless(1, .2));
    Missing.Home4->Reference.LengthCells = 2.; Missing.Home4->Reference.SpeedCellsPerStep = .1;
    FStudioJobController L(MakeUnique<FStudioControlHarness>()); Pause(L, Missing);
    TestTrue(TEXT("Explicit captured L/U reference supported"), L.RunToDimensionless(1, .2)); L.Tick(.3);
    TestEqual(TEXT("L/U control target exact"), L.SimulatedControlSteps(), int64(20));
    auto Huge = Draft(); Huge.Setup.MaxSteps = FStudioJobController::MaxSimulatedControlSteps;
    Huge.Home4->Run.Steps = FStudioJobController::MaxSimulatedControlSteps;
    FStudioJobController H(MakeUnique<FStudioControlHarness>()); Pause(H, Huge);
    TestTrue(TEXT("Huge bounded range dispatches once"), H.StepN(FStudioJobController::MaxSimulatedControlSteps, .2)); H.Tick(.3);
    TestEqual(TEXT("Huge range exact without per-step processing"), H.SimulatedControlSteps(), FStudioJobController::MaxSimulatedControlSteps);
    TestEqual(TEXT("Huge range stores four arithmetic summaries"), H.ScheduledOutputs().Num(), 4);
    TestEqual(TEXT("Huge trace summary exact"), H.ScheduledOutputs()[0].Crossings, int64(500000000000));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ControlAuthority, "Studio.Home4.Controls.CorrelatedRepliesStopReconnectAndBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4ControlAuthority::RunTest(const FString&)
{
    using namespace StudioHome4RunControlTestFixtures;
    auto Adapter = MakeUnique<FRangeAdapter>(); auto* A = Adapter.Get(); FStudioJobController J(MoveTemp(Adapter));
    J.Submit(TEXT("Control authority unit fixture"), Draft(), 0);
    A->Reply(EStudioJobState::Preparing); A->Reply(EStudioJobState::Queued); A->Reply(EStudioJobState::Running); J.Tick(.1);
    J.Command(EStudioJobCommand::Pause, .1); A->Reply(EStudioJobState::Paused); J.Tick(.2);
    J.StepN(4, .2);
    A->Reply(EStudioJobState::Paused, EStudioJobEventKind::StepCompleted, 3); J.Tick(.3);
    TestTrue(TEXT("Wrong range count cannot complete request"), J.IsPending());
    TestEqual(TEXT("Wrong result cannot mutate count"), J.SimulatedControlSteps(), int64(0));
    A->Reply(EStudioJobState::Paused, EStudioJobEventKind::StepCompleted, 4); A->Events.Last().RunId = FGuid::NewGuid(); J.Tick(.4);
    TestTrue(TEXT("Foreign reply cannot complete request"), J.IsPending());
    A->Reply(EStudioJobState::Paused, EStudioJobEventKind::StepCompleted, 4); J.Tick(.5);
    TestEqual(TEXT("Matching result alone mutates count"), J.SimulatedControlSteps(), int64(4));
    auto Harness = MakeUnique<FStudioControlHarness>(); auto* H = Harness.Get(); FStudioJobController K(MoveTemp(Harness)); Pause(K, Draft());
    K.StepN(10, .2); K.Command(EStudioJobCommand::Stop, .201); K.Tick(.3);
    TestEqual(TEXT("Authoritative stop cancels scheduled range result"), K.SimulatedControlSteps(), int64(0));
    TestTrue(TEXT("Cancelled range emits no scheduled outputs"), K.ScheduledOutputs().IsEmpty());
    K.Submit(TEXT("Reconnect unit fixture"), Draft(), .4); K.Tick(.5); K.Command(EStudioJobCommand::Pause, .5); K.Tick(.6);
    K.StepN(10, .6); H->InjectDisconnect(.615); K.Tick(.616); K.Tick(.63);
    TestEqual(TEXT("Stale in-flight result cannot update disconnected controller"), K.SimulatedControlSteps(), int64(0));
    K.Command(EStudioJobCommand::Reconnect, .64); K.Tick(.7);
    TestEqual(TEXT("Reconnect query recovers authoritative harness counter"), K.SimulatedControlSteps(), int64(10));
    TestTrue(TEXT("Reconnect returns actual paused state"), K.State() == EStudioJobState::Paused);
    for (int32 I = 0; I < 30; ++I) { K.StepN(10, .8 + I * .1); K.Tick(.85 + I * .1); }
    TestEqual(TEXT("Retained schedule notice budget bounded"), K.ScheduledOutputs().Num(), 64);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4RunControlNative, "Studio.Home4.Controls.NativeWidgetKeepsRecordingAndCamera",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4RunControlNative::RunTest(const FString&)
{
    using namespace StudioHome4RunControlTestFixtures;
    if (!TestTrue(TEXT("Slate initialized"), FSlateApplication::IsInitialized())) return false;
    auto M = MakeShared<FStudioModel>(FPaths::ProjectDir() / TEXT("tmp/debug/home4-run-controls") / FGuid::NewGuid().ToString());
    M->EditCase(TEXT("Unit test HOME4 control inputs"), [](auto& D) { D.Home4 = Draft().Home4; D.Setup.MaxSteps = 1000; });
    M->Scrub(.5); const int32 Frame = M->SelectedFrame; const auto Camera = M->Project.Camera; const auto Source = M->Solver;
    auto Panel = SNew(SStudioHome4RunControls).Model(M);
    FStudioHeadlessSlate UI(*this, Panel, FVector2D(600, 1000));
    if (!UI.Inspect(TEXT("home4-run-controls-initial"), {TEXT("Home4RunHarness"), TEXT("Home4RunReplay"), TEXT("Home4RunCapabilities"), TEXT("Home4RunStepN"), TEXT("Home4RunToTime")})) return false;
    TestTrue(TEXT("Replay mode explicitly displayed"), UI.Text(TEXT("Home4RunCapabilities")).Contains(TEXT("Recorded replay")));
    TestFalse(TEXT("Range controls disabled for replay"), UI.Find(TEXT("Home4RunStepN"))->IsEnabled());
    if (!UI.Press(TEXT("Home4RunHarness")) || !UI.Press(TEXT("Home4RunSubmit"))) return false;
    M->Tick(.1);
    TestTrue(TEXT("Harness source explicit"), UI.Text(TEXT("Home4RunCapabilities")).Contains(TEXT("Development harness")));
    TestTrue(TEXT("No solver claims explicit"), UI.Text(TEXT("Home4RunCapabilities")).Contains(TEXT("No CFD measurements")));
    TestFalse(TEXT("Mode switch disabled for active job"), UI.Find(TEXT("Home4RunReplay"))->IsEnabled());
    if (!UI.Press(TEXT("Home4RunPause"))) return false; M->Tick(.1);
    if (!UI.Type(TEXT("Home4RunStepCount"), TEXT("10")) || !UI.Press(TEXT("Home4RunStepN"))) return false; M->Tick(.1); Panel->Tick(FGeometry(), 0, 0);
    TestEqual(TEXT("Native counted range acknowledged"), M->Job().SimulatedControlSteps(), int64(10));
    if (!UI.Type(TEXT("Home4RunTargetTime"), TEXT("0.2")) || !UI.Press(TEXT("Home4RunToTime"))) return false; M->Tick(.1); Panel->Tick(FGeometry(), 0, 0);
    TestEqual(TEXT("Native run-to uses frozen reference"), M->Job().SimulatedControlSteps(), int64(20));
    TestTrue(TEXT("Distinct restart schedule has no file claim"), UI.Text(TEXT("Home4RunOutput3")).Contains(TEXT("Restart")));
    TestTrue(TEXT("Storage never guessed"), UI.Text(TEXT("Home4RunOutput3")).Contains(TEXT("storage size unavailable")));
    if (!UI.Type(TEXT("Home4OutputSize3"), TEXT("100"))) return false;
    TestTrue(TEXT("User supplied estimate labeled with source"), UI.Text(TEXT("Home4RunOutput3")).Contains(TEXT("user range estimate 200 bytes")));
    if (!UI.Type(TEXT("Home4RunStepCount"), TEXT("1.5")) || !UI.Press(TEXT("Home4RunStepN"))) return false;
    TestEqual(TEXT("Invalid count cannot mutate acknowledged steps"), M->Job().SimulatedControlSteps(), int64(20));
    TestTrue(TEXT("Invalid input cause visible"), UI.Text(TEXT("Home4RunControlStatus")).Contains(TEXT("positive bounded integer")));
    TestEqual(TEXT("Control commands preserve original frame"), M->SelectedFrame, Frame);
    TestTrue(TEXT("Control commands preserve camera"), StudioView::CameraEquals(M->Project.Camera, Camera));
    TestTrue(TEXT("Control commands preserve recording identity"), M->Solver == Source);
    if (!UI.Press(TEXT("Home4RunStop"))) return false; M->Tick(.1);
    if (!UI.Press(TEXT("Home4RunReplay"))) return false;
    TestFalse(TEXT("Replay restored after confirmed stop"), M->Project.bControlHarness);
    return true;
}
#endif
