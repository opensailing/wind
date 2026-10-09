#include "StudioHome4Runtime.h"
#include "StudioModel.h"
#include "SStudioHome4Runtime.h"
#include "StudioHeadlessSlate.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4RuntimeTestFixtures
{
    FStudioHome4Spec Spec()
    {
        FStudioHome4Spec S;
        S.RecipeId = TEXT("fixture-recipe");
        S.Run.Steps = 1000;
        S.Reference.TimeSteps = 100;
        S.Run.MeasureEvery = 2;
        S.Run.SaveEvery = 3;
        S.Run.VizEvery = 4;
        S.Run.RestartEvery = 5;
        return S;
    }
    TSharedRef<FStudioModel> Model()
    {
        auto M = MakeShared<FStudioModel>(FPaths::ProjectDir() / TEXT("tmp/debug/home4-runtime") / FGuid::NewGuid().ToString());
        M->Project.Draft.Home4 = Spec();
        return M;
    }
    FStudioHome4BackendVerification Verification(EStudioHome4Backend Backend = EStudioHome4Backend::Metal)
    {
        FStudioHome4BackendVerification V;
        V.Id = FGuid::NewGuid(); V.Target = TEXT("fixture-local"); V.Host = TEXT("fixture-host"); V.Device = TEXT("fixture-device");
        V.Source = TEXT("Unit test development response, no measured device values"); V.EffectiveBackend = Backend;
        V.ExtensionImported = Backend != EStudioHome4Backend::PyTorch; V.bDevelopmentResponse = true;
        return V;
    }
    void Append(FStudioHome4TelemetryStream& S, const FString& JSON)
    {
        const FTCHARToUTF8 Bytes(*JSON);
        S.AppendBytes(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
    }

    class FLiveTailWorkflow final : public IAutomationLatentCommand
    {
    public:
        explicit FLiveTailWorkflow(FAutomationTestBase& InTest) : Test(InTest)
        {
            Root = FPaths::ProjectDir() / TEXT("tmp/debug/home4-live-tail") / FGuid::NewGuid().ToString();
            IFileManager::Get().MakeDirectory(*Root, true); Path = Root / TEXT("original-fixture.jsonl");
            Test.TestTrue(TEXT("Write original historical prefix"), FFileHelper::SaveStringToFile(TEXT("{\"step\":0,\"umax\":0.01}\n{\"step\":1,\"nonfinite\":true}\n"), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
            M = Model(); R = MakeShared<FStudioHome4RuntimeSession>(M); OriginalRun = FGuid::NewGuid();
            FString Error; R->SetBackendVerification(Verification(), Error); R->Submit(Spec(), OriginalRun, TEXT("me"), 0, Error);
            R->Tick(.1); R->Tick(.2); Clock = .2;
            FStudioHome4DiagnosticPolicy P; P.MaximumSpeedTrigger = .1; R->SetDiagnosticPolicy(P);
            R->SetLocate([this](const FStudioHome4ActionRequest& A) { ++LocateCount; LocatedRun = A.Source.RunId; LocatedCell = A.Facts.Cell; });
            Test.TestTrue(TEXT("Attach original live source"), R->AttachLiveLog(Path, OriginalRun, Error));
            Started = FPlatformTime::Seconds();
        }
        ~FLiveTailWorkflow()
        { R.Reset(); M.Reset(); IFileManager::Get().DeleteDirectory(*Root, false, true); }
        bool Update() override
        {
            Clock += .05; R->Tick(Clock);
            if (FPlatformTime::Seconds() - Started > 20)
            { Test.AddError(TEXT("Original live tail did not satisfy its bounded test deadline.")); return true; }
            const auto Stream = R->ScienceStream();
            if (Stage == 0)
            {
                if (!Stream || !Stream->Latest() || Stream->Latest()->Step.Get(-1) != 1) return false;
                Test.TestTrue(TEXT("Existing historical bad record cannot command current job"), R->GuardOutcomes().IsEmpty());
                Test.TestTrue(TEXT("Historical bad record leaves owned protocol running"), R->QueueJobs()[0].State == EStudioHome4QueueState::Running);
                Test.TestTrue(TEXT("Append complete safe record and fragmented hotspot"), FFileHelper::SaveStringToFile(TEXT("{\"step\":2,\"umax\":0.04}\n{\"step\":3,\"umax\":0.2,"), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append));
                Stage = 1; return false;
            }
            if (Stage == 1)
            {
                if (!Stream || !Stream->Latest() || Stream->Latest()->Step.Get(-1) != 2 || Stream->BufferedBytes() == 0) return false;
                Test.TestTrue(TEXT("Fragment cannot produce a scientific measurement or action"), R->GuardOutcomes().IsEmpty());
                Test.TestTrue(TEXT("Complete original hotspot fragment"), FFileHelper::SaveStringToFile(TEXT("\"umax_loc\":[2,3,4]}\n"), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append));
                Stage = 2; return false;
            }
            if (Stage == 2)
            {
                if (R->GuardOutcomes().IsEmpty() || !R->GuardOutcomes().Last().bStopAcknowledged) return false;
                const auto& G = R->GuardOutcomes().Last();
                Test.TestEqual(TEXT("One complete live hotspot dispatches once"), R->GuardOutcomes().Num(), 1);
                Test.TestEqual(TEXT("Live guard recovery refers to last safe original step"), G.Request.LastGoodStep.Get(-1), int64(2));
                Test.TestTrue(TEXT("Recovery protocol states that no restart file exists"), G.Recovery.Contains(TEXT("No restart file generated")));
                Test.TestEqual(TEXT("Source-bound camera callback dispatched once"), LocateCount, 1);
                Test.TestEqual(TEXT("Original run reaches owner callback"), LocatedRun, OriginalRun);
                Test.TestEqual(TEXT("Original cell reaches owner callback"), LocatedCell.Get(FIntVector()).Y, 3);
                Test.TestTrue(TEXT("Stop acknowledgment is independent of science"), R->QueueJobs()[0].State == EStudioHome4QueueState::Stopped);
                Test.TestFalse(TEXT("Live log never supplies queue scientific completion implicitly"), R->QueueJobs()[0].bScientificResultsAttached);
                Test.TestTrue(TEXT("Replace original source with shorter fixture"), FFileHelper::SaveStringToFile(TEXT("{\"step\":0}\n"), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
                Stage = 3; return false;
            }
            if (Stage == 3)
            {
                if (!R->LiveStatus().Contains(TEXT("truncated/replaced"))) return false;
                Test.TestEqual(TEXT("Replacement cannot replace retained science"), Stream->Latest()->Step.Get(-1), int64(3));
                Test.TestEqual(TEXT("Replacement cannot duplicate live guard"), R->GuardOutcomes().Num(), 1);
                M->Project.Id = FGuid::NewGuid();
                Test.TestFalse(TEXT("Project change immediately unbinds science getter"), R->ScienceStream().IsValid());
                Test.TestTrue(TEXT("Project change immediately unbinds guard outcomes"), R->GuardOutcomes().IsEmpty());
                Test.TestTrue(TEXT("Project change immediately unbinds logs"), R->LogLines().IsEmpty());
                return true;
            }
            return false;
        }
    private:
        FAutomationTestBase& Test;
        FString Root, Path;
        TSharedPtr<FStudioModel> M;
        TSharedPtr<FStudioHome4RuntimeSession> R;
        FGuid OriginalRun, LocatedRun;
        TOptional<FIntVector> LocatedCell;
        double Started = 0, Clock = 0;
        int32 Stage = 0, LocateCount = 0;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4RuntimeLiveTailTest, "Studio.Home4.Runtime.BoundedLiveTailHistoricalPrefixGuardsAndScope",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4RuntimeLiveTailTest::RunTest(const FString&)
{ ADD_LATENT_AUTOMATION_COMMAND(StudioHome4RuntimeTestFixtures::FLiveTailWorkflow(*this)); return true; }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4RuntimeOwnedQueueTest, "Studio.Home4.Runtime.BackendVerificationFallbackAndOwnedQueue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4RuntimeOwnedQueueTest::RunTest(const FString&)
{
    using namespace StudioHome4RuntimeTestFixtures;
    auto M = Model(); FStudioHome4RuntimeSession R(M); FString Error; auto S = Spec();
    const auto A = FGuid::NewGuid(), B = FGuid::NewGuid();
    TestFalse(TEXT("Submission has no implicit target verification"), R.Submit(S, A, TEXT("me"), 0, Error));
    auto V = Verification(EStudioHome4Backend::PyTorch);
    TestTrue(TEXT("Explicit fallback response accepted"), R.SetBackendVerification(V, Error));
    TestFalse(TEXT("Fallback cannot launch before confirmation"), R.Submit(S, A, TEXT("me"), 0, Error));
    TestFalse(TEXT("Confirmation is bound to this exact response"), R.ConfirmFallback(FGuid::NewGuid()));
    TestTrue(TEXT("Exact fallback accepted explicitly"), R.ConfirmFallback(V.Id));
    TestTrue(TEXT("First immutable request queued"), R.Submit(S, A, TEXT("me"), 0, Error));
    TestTrue(TEXT("Second request on same device queued"), R.Submit(S, B, TEXT("me"), 0, Error));
    S.Run.Steps = 1; S.Reference.TimeSteps = 1;
    R.Tick(.1); R.Tick(.2);
    TestTrue(TEXT("First protocol runs"), R.QueueJobs()[0].State == EStudioHome4QueueState::Running);
    TestTrue(TEXT("Same device second request waits"), R.QueueJobs()[1].State == EStudioHome4QueueState::Queued);
    TestEqual(TEXT("Immutable original request remains captured"), R.QueueJobs()[0].FrozenSpec.Run.Steps.Get(-1), int64(1000));
    TestTrue(TEXT("Pause acknowledged by protocol"), R.Command(A, EStudioJobCommand::Pause, .2, Error));
    TestTrue(TEXT("Step N controls supported"), R.Command(A, EStudioJobCommand::Step, .2, Error, 10));
    TestTrue(TEXT("Frozen reference maps later t*"), R.Command(A, EStudioJobCommand::RunToDimensionless, .2, Error, 1, .14));
    TestEqual(TEXT("Decimal target has no extra roundoff step"), R.QueueJobs()[0].AcknowledgedControlSteps, int64(14));
    TestFalse(TEXT("Foreign run cannot receive command"), R.Command(FGuid::NewGuid(), EStudioJobCommand::Stop, .2, Error));
    TestTrue(TEXT("Development completion available"), R.CompleteDevelopment(A, .2, Error));
    R.Tick(.3); R.Tick(.4);
    TestTrue(TEXT("Freed device starts next protocol"), R.QueueJobs()[1].State == EStudioHome4QueueState::Running);
    TestFalse(TEXT("Control completion never attaches science"), R.QueueJobs()[0].bScientificResultsAttached);
    TestFalse(TEXT("Control queue never fabricates a science stream"), R.ScienceStream().IsValid());
    TestTrue(TEXT("No throughput fabricated by completion"), R.PerformanceRecords().IsEmpty());
    const auto* Captured = M->Project.Runs.FindByPredicate([&](const auto& Run) { return Run.GetId() == A; });
    TestTrue(TEXT("Planned identity appears in immutable project history"), Captured && Captured->GetConfiguration() && Captured->GetConfiguration()->Home4.IsSet());
    if (Captured && Captured->GetConfiguration())
        TestEqual(TEXT("Project history retains original HOME4 step budget"), Captured->GetConfiguration()->Home4->Run.Steps.Get(-1), int64(1000));
    TestTrue(TEXT("Explicit transport disconnect reachable"), R.DisconnectDevelopment(B, .4, Error));
    TestTrue(TEXT("Disconnected owned protocol enables reconnect"), R.CanCommand(B, EStudioJobCommand::Reconnect));
    TestTrue(TEXT("Reconnect acknowledged paused without scientific progress"), R.Command(B, EStudioJobCommand::Reconnect, .4, Error));
    TestTrue(TEXT("Reconnect exposes paused state"), R.QueueJobs()[1].State == EStudioHome4QueueState::Paused);
    V.Id = FGuid::NewGuid(); R.SetBackendVerification(V, Error);
    TestFalse(TEXT("Changed fallback requires new confirmation"), R.Submit(Spec(), FGuid::NewGuid(), TEXT("me"), .4, Error));
    M->Project.Draft.Id = FGuid::NewGuid();
    TestTrue(TEXT("Case change immediately unbinds queued jobs"), R.QueueJobs().IsEmpty());
    TestFalse(TEXT("Case change immediately unbinds backend response"), R.BackendVerification().IsSet());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4RuntimeMeasurementsTest, "Studio.Home4.Runtime.OriginalMeasuredHistoryAndPlannedDiskForecast",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4RuntimeMeasurementsTest::RunTest(const FString&)
{
    using namespace StudioHome4RuntimeTestFixtures;
    auto M = Model(); FStudioHome4RuntimeSession R(M); FString Error;
    const auto V = Verification(); R.SetBackendVerification(V, Error);
    FStudioHome4MeasuredRun H; H.RunId = FGuid::NewGuid(); H.Host = V.Host; H.Device = V.Device; H.RecipeId = Spec().RecipeId;
    H.Backend = V.EffectiveBackend; H.Source = TEXT("Original completed-run work/time fixture"); H.SourceSHA256 = FString::ChrN(64, 'a');
    H.NodeUpdates = 100000000; H.ElapsedSeconds = 2;
    TestFalse(TEXT("Uncompleted measurements cannot enter completed history"), R.RecordMeasuredRun(H, Error));
    H.bCompletedOriginalRun = true;
    TestTrue(TEXT("Identified original actual work/time accepted"), R.RecordMeasuredRun(H, Error));
    TestEqual(TEXT("Measured MLUPS computed from actual node updates and time"), R.PerformanceRecords()[0].MLUPS(), 50.);
    TestFalse(TEXT("Duplicate original measurement cannot multiply history"), R.RecordMeasuredRun(H, Error));
    const FString Serialized = R.SerializePerformanceHistory();
    TestTrue(TEXT("Identified history round-trips"), R.ParsePerformanceHistory(Serialized, Error));
    TestFalse(TEXT("Malformed history rejected"), R.ParsePerformanceHistory(TEXT("{\"schema\":\"LBMStudio.Home4MeasuredHistory\",\"records\":[{}]}"), Error));
    TestEqual(TEXT("Failed history import preserves previous record"), R.PerformanceRecords().Num(), 1);
    auto S = Spec(); S.Run.Steps = 12;
    const uint64 Sizes[] = {10, 100, 1000, 10000};
    TArray<FStudioHome4OutputLayout> Layouts;
    for (int32 I = 0; I < 4; ++I)
    { FStudioHome4OutputLayout L; L.Kind = EStudioHome4OutputKind(I); L.BytesPerOutput = Sizes[I]; L.Source = TEXT("Explicit fixture output layout"); Layouts.Add(L); }
    const auto Forecast = FStudioHome4RuntimeSession::Forecast(S, Layouts, uint64(1000));
    TestEqual(TEXT("Independent trace cadence forecasts whole planned run"), Forecast.Counts[0].Get(0), uint64(6));
    TestEqual(TEXT("Independent slice cadence"), Forecast.Counts[1].Get(0), uint64(4));
    TestEqual(TEXT("Independent visualization cadence"), Forecast.Counts[2].Get(0), uint64(3));
    TestEqual(TEXT("Independent restart cadence"), Forecast.Counts[3].Get(0), uint64(2));
    TestEqual(TEXT("Attributed output layouts total"), Forecast.TotalBytes.Get(0), uint64(23460));
    TestTrue(TEXT("Supplied destination free space is checked before launch"), Forecast.bExceedsSuppliedFreeSpace);
    Layouts[2].BytesPerOutput.Reset();
    const auto Unknown = FStudioHome4RuntimeSession::Forecast(S, Layouts);
    TestFalse(TEXT("Unknown output size keeps total unavailable"), Unknown.TotalBytes.IsSet());
    TestFalse(TEXT("Missing visualization layout remains unknown"), Unknown.Bytes[2].IsSet());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4RuntimeLastGoodTest, "Studio.Home4.Runtime.ConsecutiveHotspotsKeepLastSafeStateAndBodyGates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4RuntimeLastGoodTest::RunTest(const FString&)
{
    using namespace StudioHome4RuntimeTestFixtures;
    FStudioHome4TelemetryStream S; S.BeginRun({FGuid::NewGuid(), TEXT("original-hotspot-fixture")});
    FStudioHome4DiagnosticPolicy P; P.MaximumSpeedTrigger = .1; P.ForceAbsoluteTolerance = .01; P.ForceRelativeTolerance = .01; P.ForceReferenceMagnitude = 1.;
    S.SetDiagnosticPolicy(P);
    Append(S, TEXT("{\"step\":1,\"umax\":0.01}\n{\"step\":2,\"umax\":0.2}\n{\"step\":3,\"umax\":0.3}\n"));
    TestEqual(TEXT("Consecutive hotspots request last safe original step"), S.ActionRequests().Last().LastGoodStep.Get(-1), int64(1));
    Append(S, TEXT("{\"step\":4,\"umax\":0.04}\n{\"step\":5,\"nonfinite\":true}\n"));
    TestEqual(TEXT("New finite safe original step replaces prior safe state"), S.ActionRequests().Last().LastGoodStep.Get(-1), int64(4));
    Append(S, TEXT("{\"step\":6,\"forces\":{\"Fx\":1,\"mea_Fx\":1},\"bodies\":[{\"id\":\"body-A\",\"forces\":{\"Fx\":2,\"mea_Fx\":4}}]}\n"));
    TestTrue(TEXT("Original root force independently passes supplied tolerance"), FStudioHome4Diagnostics::Evaluate(*S.Latest(), P)[2].Status == EStudioHome4Health::Healthy);
    P.BodyId = TEXT("body-A");
    TestTrue(TEXT("Selected body's discrepant channels warn"), FStudioHome4Diagnostics::Evaluate(*S.Latest(), P)[2].Status == EStudioHome4Health::Warning);
    P.BodyId = TEXT("missing-body");
    TestTrue(TEXT("Missing selected body cannot borrow root channels"), FStudioHome4Diagnostics::Evaluate(*S.Latest(), P)[2].Status == EStudioHome4Health::Unavailable);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4RuntimeNativeTest, "Studio.Home4.Runtime.NativeTargetsQueueForecastAndNoScienceFabrication",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4RuntimeNativeTest::RunTest(const FString&)
{
    using namespace StudioHome4RuntimeTestFixtures;
    if (!TestTrue(TEXT("Slate initialized"), FSlateApplication::IsInitialized())) return false;
    auto M = Model(); auto R = MakeShared<FStudioHome4RuntimeSession>(M);
    const auto Camera = M->Project.Camera; const int32 Frame = M->SelectedFrame;
    const auto Panel = SNew(SStudioHome4Runtime).Model(M).Runtime(R);
    FStudioHeadlessSlate UI(*this, Panel, FVector2D(900, 1600));
    if (!UI.Press(TEXT("Home4RuntimeFallback")) || !UI.Press(TEXT("Home4RuntimeSubmit"))) return false;
    TestTrue(TEXT("Fallback warning visible before launch"), UI.Text(TEXT("Home4RuntimeNotice")).Contains(TEXT("explicit confirmation")));
    TestTrue(TEXT("Unconfirmed native queue remains empty"), R->QueueJobs().IsEmpty());
    if (!UI.Press(TEXT("Home4RuntimeConfirmFallback")) || !UI.Press(TEXT("Home4RuntimeSubmit"))) return false;
    TestEqual(TEXT("Reviewed native request queued"), R->QueueJobs().Num(), 1);
    TestTrue(TEXT("Unknown measurements remain visible"), UI.Text(TEXT("Home4RuntimeSummary")).Contains(TEXT("DEVELOPMENT")));
    TestTrue(TEXT("Storage remains unknown before supplied layout"), UI.Text(TEXT("Home4RuntimeDiskForecast")).Contains(TEXT("Full planned total: unknown")));
    for (int32 I = 0; I < 4; ++I)
        if (!UI.Type(FName(*FString::Printf(TEXT("Home4RuntimeOutputSize%d"), I)), TEXT("100"))) return false;
    UI.Type(TEXT("Home4RuntimeEstimateSource"),TEXT("Explicit user estimate"));UI.Type(TEXT("Home4RuntimeEstimateAssumption"),TEXT("User supplied uncompressed bytes including all overhead"));
    UI.Press(TEXT("Home4RuntimeApplyEstimates"));
    TestTrue(TEXT("Native full planned output estimate is attributed"), UI.Text(TEXT("Home4RuntimeDiskForecast")).Contains(TEXT("Explicit user estimate")));
    TestFalse(TEXT("No CFD fields fabricated by native queue"), R->ScienceStream().IsValid());
    TestEqual(TEXT("Native protocol leaves original playback unchanged"), M->SelectedFrame, Frame);
    TestTrue(TEXT("Native protocol leaves camera unchanged"), StudioView::CameraEquals(M->Project.Camera, Camera));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4RuntimeWarmStartPreflightTest, "Studio.Home4.Runtime.WarmStartRequiresScopedPreparedCheckpoint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4RuntimeWarmStartPreflightTest::RunTest(const FString&)
{
    using namespace StudioHome4RuntimeTestFixtures;
    auto M=Model();FStudioHome4RuntimeSession R(M);FString Error;auto S=Spec();
    TestTrue(TEXT("Review original backend response"),R.SetBackendVerification(Verification(),Error));
    const int32 OriginalRunCount=M->Project.Runs.Num(),OriginalJobCount=M->Project.JobHistory.Num();
    S.Run.InitState=TEXT("uninspected-original-restart.npz");
    TestFalse(TEXT("Nonempty warm-start cannot enqueue without original checkpoint preflight"),R.Submit(S,FGuid::NewGuid(),TEXT("me"),0,Error));
    TestTrue(TEXT("Rejected warm-start leaves queue empty"),R.QueueJobs().IsEmpty());
    TestEqual(TEXT("Rejected warm-start preserves immutable run history"),M->Project.Runs.Num(),OriginalRunCount);
    TestEqual(TEXT("Rejected warm-start preserves job history"),M->Project.JobHistory.Num(),OriginalJobCount);
    TestTrue(TEXT("Missing preflight has actionable reason"),Error.Contains(TEXT("checkpoint")));
    S.Run.InitState.Empty();
    TestTrue(TEXT("Ordinary explicit development request remains reachable"),R.Submit(S,FGuid::NewGuid(),TEXT("me"),0,Error));
    TestFalse(TEXT("Ordinary request never fabricates prepared checkpoint"),R.QueueJobs()[0].PreparedCheckpoint.IsValid());
    return true;
}
#endif
