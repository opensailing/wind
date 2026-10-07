#include "StudioHome4Telemetry.h"
#include "Misc/AutomationTest.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4TelemetryTestFixtures
{
    // All numerical records below are unit test fixtures. They are not CFD data,
    // never loaded by application code and never represent driver instrumentation.
    FStudioHome4Source UnitTestSource()
    { return {FGuid::NewGuid(), TEXT("unit-test-science-jsonl")}; }
    FStudioHome4TailResult Feed(FStudioHome4TelemetryStream& Stream, const FString& Text)
    {
        const FTCHARToUTF8 UTF8(*Text);
        return Stream.AppendBytes(reinterpret_cast<const uint8*>(UTF8.Get()), UTF8.Length());
    }
    FStudioHome4DiagnosticPolicy UnitTestPolicy()
    {
        FStudioHome4DiagnosticPolicy P;
        P.BudgetAbsoluteTolerance = .01;
        P.ForceRelativeTolerance = .03; P.ForceAbsoluteTolerance = .001; P.ForceReferenceMagnitude = 1.;
        P.WindowRelativeTolerance = .02; P.WindowAbsoluteTolerance = .001; P.WindowReferenceMagnitude = 1.;
        P.RestPressureTolerance = 0.;
        return P;
    }
    FString UnitTestRecord(int32 Step)
    { return FString::Printf(TEXT("{\"step\":%d,\"t_lat\":%d,\"t_phys\":%g,\"t_star\":%g}\n"), Step, Step, Step / 10., Step / 20.); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4SchemaTest, "Studio.Home4.Telemetry.ProposedSchemaAndOptionalExtensions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4SchemaTest::RunTest(const FString&)
{
    using namespace StudioHome4TelemetryTestFixtures;
    FStudioHome4TelemetryStream Stream;
    const auto Source = UnitTestSource();
    TestTrue(TEXT("Owner supplies explicit run/source identity"), Stream.BeginRun(Source));
    const FString Fixture = TEXT("{\"step\":12,\"t_lat\":12,\"t_phys\":0.84,\"t_star\":2.31,")
        TEXT("\"backend\":\"metal\",\"mlups_inst\":61.2,\"mlups_cum\":63,\"ma_inst\":0.071,")
        TEXT("\"tau_min\":0.5083,\"umax\":0.0412,\"umax_loc\":[2,4,6],")
        TEXT("\"mass_ledger\":{\"phi\":-0.000013,\"levels\":[-0.000013,0.000002],\"injected\":0,\"level_injected\":[0,0.000001]},")
        TEXT("\"ke\":{\"water\":0.0031,\"air\":0.000008},\"pe_surface\":0.0012,\"div_u_norm\":0.00024,")
        TEXT("\"forces\":{\"Fx\":-0.011,\"Fy\":0,\"Fz\":0.42,\"My\":0.0003,\"Fx_p\":-0.008,\"Fz_p\":0.41,\"Fx_nu\":-0.003,\"mea_Fx\":-0.0108},")
        TEXT("\"window\":{\"Fx\":-0.011,\"Fx_prev\":-0.0112,\"start\":2,\"end\":3,\"prev_start\":1,\"prev_end\":2},")
        TEXT("\"safeguards\":{\"limiter_cells\":0,\"threshold_cells\":2},")
        TEXT("\"budget\":{\"W\":1,\"D_near\":0.5,\"D_far\":0.1,\"D_air\":0.1,\"Z_beach\":0.1,\"Z_floor\":0.1,\"dKE\":0.1,\"dPE\":0,\"res\":0,")
        TEXT("\"phases\":{\"water\":{\"W\":0.5,\"D_near\":0.4}}},")
        TEXT("\"performance\":{\"elapsed_seconds\":2,\"node_updates\":4000000,\"transferred_bytes\":1000000000,\"cumulative_elapsed_seconds\":4,\"cumulative_node_updates\":12000000},")
        TEXT("\"wb_rest\":{\"at_rest\":true,\"pd_max\":0},\"trace\":\"trace.csv\",\"slice\":\"slice.npz\",\"snapshot\":null,\"checkpoint\":null,\"nonfinite\":false}\n");
    const auto R = Feed(Stream, Fixture);
    TestEqual(TEXT("Proposed raw science schema accepted"), R.Accepted, 1);
    if (!Stream.Latest()) return false;
    const auto& S = *Stream.Latest();
    TestTrue(TEXT("Session run identity retained"), S.Source.RunId == Source.RunId);
    TestEqual(TEXT("Source identity retained"), S.Source.SourceId, Source.SourceId);
    TestEqual(TEXT("Step integer exact"), S.Step.Get(-1), int64(12));
    TestEqual(TEXT("Original backend retained"), S.Backend, FString(TEXT("metal")));
    TestEqual(TEXT("Per-level ledger retained"), S.Mass.LevelDrifts.Num(), 2);
    TestEqual(TEXT("Per-level correction injection retained"), S.Mass.LevelInjections[1].Get(-1), .000001);
    TestEqual(TEXT("Measured zero remains a zero"), S.LimiterCells.Get(-1), int64(0));
    TestEqual(TEXT("Pressure force retained"), S.Forces.PressureFz.Get(-1), .41);
    TestEqual(TEXT("Viscous force retained"), S.Forces.ViscousFx.Get(0), -.003);
    TestFalse(TEXT("Unreported second-channel component unavailable"), S.Forces.MomentumFz.IsSet());
    TestEqual(TEXT("Previous averaging window retained"), S.Window.PreviousFx.Get(0), -.0112);
    TestTrue(TEXT("Per-phase budget retained with missing terms"), S.PhaseBudgets.Contains(TEXT("water")) && !S.PhaseBudgets[TEXT("water")].IsComplete());
    const auto P = FStudioHome4Diagnostics::Performance(S);
    TestEqual(TEXT("Instant MLUPS uses measured window"), P.MLUPSInstant.Get(-1), 2.);
    TestEqual(TEXT("Cumulative MLUPS uses cumulative work"), P.MLUPSCumulative.Get(-1), 3.);
    TestEqual(TEXT("Bandwidth uses supplied transfer bytes and measured elapsed"), P.GigabytesPerSecond.Get(-1), .5);
    TestEqual(TEXT("Reported rate remains available separately"), S.ReportedMLUPSInstant.Get(-1), 61.2);
    TestEqual(TEXT("Trace and slice events are independent"), Stream.OutputEvents().Num(), 2);
    TestTrue(TEXT("Trace kind retained"), Stream.OutputEvents()[0].Kind == EStudioHome4OutputKind::Trace);
    TestTrue(TEXT("Slice kind retained"), Stream.OutputEvents()[1].Kind == EStudioHome4OutputKind::Slice);
    TestFalse(TEXT("Null restart never fabricates a restart"), Stream.LastRestart().IsSet());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ChunksTest, "Studio.Home4.Telemetry.PartialChunksAndBoundedWork",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4ChunksTest::RunTest(const FString&)
{
    using namespace StudioHome4TelemetryTestFixtures;
    FStudioHome4TailLimits Limits;
    Limits.MaxBytesPerAppend = 48; Limits.MaxLinesPerAppend = 2; Limits.MaxLineBytes = 32;
    Limits.MaxHistory = 3; Limits.MaxOutputEvents = 2; Limits.MaxActionRequests = 2;
    FStudioHome4TelemetryStream Stream(Limits);
    const FString Records = TEXT("{\"step\":1}\n{\"step\":2}\n{\"step\":3}\n{\"step\":4}\n");
    TestEqual(TEXT("No ingestion without owner identity"), Feed(Stream, Records).ConsumedBytes, 0);
    TestFalse(TEXT("Invalid run rejected"), Stream.BeginRun({FGuid(), TEXT("test")}));
    TestFalse(TEXT("Empty source rejected"), Stream.BeginRun({FGuid::NewGuid(), TEXT(" ")}));
    Stream.BeginRun(UnitTestSource());
    const auto First = Feed(Stream, Records);
    TestEqual(TEXT("Two complete lines per call"), First.CompleteLines, 2);
    TestTrue(TEXT("Caller retains bounded suffix"), First.ConsumedBytes < Records.Len());
    const auto Next = Feed(Stream, Records.Mid(First.ConsumedBytes));
    TestEqual(TEXT("Suffix can be retried"), Next.Accepted, 2);
    TestEqual(TEXT("History retention bounded"), Stream.History().Num(), 3);
    TestEqual(TEXT("Oldest bounded sample exact"), Stream.History()[0].Step.Get(-1), int64(2));
    Feed(Stream, TEXT("{\"step\":"));
    TestEqual(TEXT("Partial line retained"), Stream.BufferedBytes(), 8);
    TestEqual(TEXT("Partial line not published"), Stream.Latest()->Step.Get(-1), int64(4));
    TestEqual(TEXT("Partial line completes"), Feed(Stream, TEXT("5}\r\n")).Accepted, 1);
    TestEqual(TEXT("CRLF record exact"), Stream.Latest()->Step.Get(-1), int64(5));
    const auto Big = Feed(Stream, FString::ChrN(100, TEXT('x')));
    TestEqual(TEXT("Byte work bound honored"), Big.ConsumedBytes, 48);
    TestTrue(TEXT("Oversized line has no retained unbounded data"), Stream.BufferedBytes() <= Limits.MaxLineBytes);
    TestEqual(TEXT("Discard continues through newline"), Feed(Stream, TEXT("\n{\"step\":6}\n")).Oversized, 1);
    TestEqual(TEXT("Parser recovers after oversized line"), Stream.Latest()->Step.Get(-1), int64(6));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4TelemetryValidationTest, "Studio.Home4.Telemetry.StrictTypesMalformedUnknownAndUTF8",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4TelemetryValidationTest::RunTest(const FString&)
{
    using namespace StudioHome4TelemetryTestFixtures;
    FStudioHome4TelemetryStream Stream; Stream.BeginRun(UnitTestSource());
    const TCHAR* Bad[] = {
        TEXT("{\"step\":\"1\"}"), TEXT("{\"step\":true}"), TEXT("{\"step\":1.5}"),
        TEXT("{\"step\":-1}"), TEXT("{\"step\":9007199254740992}"), TEXT("{\"t_phys\":-1}"),
        TEXT("{\"tau_min\":\"NaN\"}"), TEXT("{\"umax\":NaN}"), TEXT("{\"umax\":1e999}"),
        TEXT("{\"nonfinite\":1}"), TEXT("{\"umax_loc\":[1,2.5,3]}"), TEXT("{\"umax_loc\":[1,2]}"),
        TEXT("{\"ke\":{\"air\":-1}}"), TEXT("{\"mass_ledger\":{\"levels\":[true]}}"),
        TEXT("{\"safeguards\":{\"threshold_cells\":-1}}"), TEXT("{\"forces\":[]}"),
        TEXT("{\"window\":{\"start\":2,\"end\":1}}"), TEXT("{\"snapshot\":true}"),
        TEXT("{\"kind\":\"restart\"}"), TEXT("{\"snapshot\":\" \"}"),
        TEXT("{\"step\":1,\"step\":2}"), TEXT("{\"step\":1,\"st\\u0065p\":2}"),
        TEXT("{\"backend\":\"bad\\u0000source\"}"), TEXT("{\"step\":1"), TEXT("[]")
    };
    for (const auto* Record : Bad)
        TestEqual(FString::Printf(TEXT("Reject malformed unit fixture %s"), Record), Feed(Stream, FString(Record) + TEXT("\n")).Malformed, 1);
    TestTrue(TEXT("Malformed records never enter history"), Stream.History().IsEmpty());
    TestEqual(TEXT("Unknown fields-only object skipped"), Feed(Stream, TEXT("{\"future_metric\":3}\n")).Unknown, 1);
    TestEqual(TEXT("Unknown event skipped"), Feed(Stream, TEXT("{\"kind\":\"future_event\",\"step\":2}\n")).Unknown, 1);
    TestEqual(TEXT("Empty tagged measurement skipped"), Feed(Stream, TEXT("{\"kind\":\"measurement\",\"future_metric\":3}\n")).Unknown, 1);
    TestEqual(TEXT("Non-JSON stdout treated as malformed"), Feed(Stream, TEXT("driver starting\n")).Malformed, 1);
    const uint8 InvalidUTF8[] = {'{', '"', 'x', '"', ':', '"', 0xc0, 0xaf, '"', '}', '\n'};
    TestEqual(TEXT("Invalid UTF8 rejected"), Stream.AppendBytes(InvalidUTF8, UE_ARRAY_COUNT(InvalidUTF8)).Malformed, 1);
    FString Deep = TEXT("{\"future\":");
    Deep += FString::ChrN(80, TEXT('[')); Deep += TEXT("0"); Deep += FString::ChrN(80, TEXT(']')); Deep += TEXT("}\n");
    TestEqual(TEXT("Excessive nesting rejected before deserialize"), Feed(Stream, Deep).Malformed, 1);
    TestEqual(TEXT("Valid recognized fields survive unknown extension"), Feed(Stream, TEXT("{\"step\":1,\"future_metric\":3}\n")).Accepted, 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ResetTest, "Studio.Home4.Telemetry.FileResetAndRunMonotonicity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4ResetTest::RunTest(const FString&)
{
    using namespace StudioHome4TelemetryTestFixtures;
    FStudioHome4TelemetryStream Stream; Stream.BeginRun(UnitTestSource());
    Feed(Stream, UnitTestRecord(10)); Feed(Stream, TEXT("{\"step\":99"));
    Stream.ResetTail();
    TestEqual(TEXT("File reset clears partial bytes"), Stream.BufferedBytes(), 0);
    TestEqual(TEXT("File reset retains run history"), Stream.History().Num(), 1);
    TestEqual(TEXT("Truncated source cannot regress this run"), Feed(Stream, UnitTestRecord(1)).Regressing, 1);
    Feed(Stream, TEXT("{\"step\":null,\"t_phys\":null,\"nonfinite\":false}\n"));
    TestFalse(TEXT("Missing step stays unavailable"), Stream.Latest()->Step.IsSet());
    TestEqual(TEXT("Regression rejected across missing step"), Feed(Stream, TEXT("{\"step\":9}\n")).Regressing, 1);
    TestEqual(TEXT("Physical time regression independently rejected"), Feed(Stream, TEXT("{\"step\":11,\"t_phys\":0.1}\n")).Regressing, 1);
    TestEqual(TEXT("Lattice time regression independently rejected"), Feed(Stream, TEXT("{\"t_lat\":1}\n")).Regressing, 1);
    TestEqual(TEXT("Dimensionless time regression independently rejected"), Feed(Stream, TEXT("{\"t_star\":0.1}\n")).Regressing, 1);
    TestEqual(TEXT("Rejected samples cannot poison baseline"), Feed(Stream, UnitTestRecord(11)).Accepted, 1);
    const auto NextSource = UnitTestSource(); Stream.BeginRun(NextSource);
    TestTrue(TEXT("Explicit run reset clears history"), Stream.History().IsEmpty());
    TestFalse(TEXT("Explicit run reset clears recovery sample"), Stream.LastGoodSample().IsSet());
    TestEqual(TEXT("New run begins with independent step/time"), Feed(Stream, UnitTestRecord(1)).Accepted, 1);
    TestTrue(TEXT("New run identity is explicit"), Stream.Latest()->Source.RunId == NextSource.RunId);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4GuardTest, "Studio.Home4.Telemetry.NonfiniteLocatorAndDistinctOutputs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4GuardTest::RunTest(const FString&)
{
    using namespace StudioHome4TelemetryTestFixtures;
    FStudioHome4TailLimits Limits; Limits.MaxOutputEvents = 2; Limits.MaxActionRequests = 2;
    FStudioHome4TelemetryStream Stream(Limits); Stream.BeginRun(UnitTestSource());
    Feed(Stream, TEXT("{\"step\":1,\"checkpoint\":\"full-state.npz\",\"snapshot\":\"viewer.npz\"}\n"));
    TestEqual(TEXT("Only full state is restart"), Stream.LastRestart()->Path, FString(TEXT("full-state.npz")));
    TestTrue(TEXT("Viz stays distinct from restart"), Stream.OutputEvents()[1].Kind == EStudioHome4OutputKind::Restart);
    const FString Guard = TEXT("{\"step\":2,\"nonfinite\":true,\"umax\":null,\"locator\":{\"cell\":[2,3,4],\"phi\":1.1,\"tau\":0.501,\"limiter\":true,\"threshold\":false,\"band\":true,\"sponge\":false,\"cut_link_shell\":true,\"zone\":\"body band\"}}\n");
    TestEqual(TEXT("Nonfinite guard requires no nonfinite JSON number"), Feed(Stream, Guard).Accepted, 1);
    TestTrue(TEXT("Guard event preserved"), Stream.Latest()->bNonfinite);
    TestEqual(TEXT("Last good numerical sample retained"), Stream.LastGoodSample()->Step.Get(-1), int64(1));
    TestEqual(TEXT("Last reported restart retained"), Stream.LastRestart()->Path, FString(TEXT("full-state.npz")));
    const auto& A = Stream.ActionRequests().Last();
    TestTrue(TEXT("Guard requests stop/checkpoint/location without certifying completion"), A.bStop && A.bCheckpointLastGoodState && A.bLocateCell);
    TestEqual(TEXT("Recovery step refers to measured last good sample"), A.LastGoodStep.Get(-1), int64(1));
    TestEqual(TEXT("Original local phi preserved including overshoot"), A.Facts.Phi.Get(0), 1.1);
    TestEqual(TEXT("Original local tau preserved"), A.Facts.Tau.Get(0), .501);
    TestFalse(TEXT("Unreported beach flag stays unavailable"), A.Facts.InBeach.IsSet());
    TestEqual(TEXT("Cell marker uses reported lattice coordinate"), A.Facts.Cell->X, 2);
    TestEqual(TEXT("No guard checkpoint completion fabricated"), Stream.OutputEvents().Num(), 2);
    Feed(Stream, TEXT("{\"kind\":\"visualization\",\"step\":2,\"path\":\"new-viewer.npz\"}\n"));
    TestEqual(TEXT("Standalone viz cannot replace restart"), Stream.LastRestart()->Path, FString(TEXT("full-state.npz")));
    TestEqual(TEXT("Output retention bounded"), Stream.OutputEvents().Num(), 2);
    TestEqual(TEXT("Output event does not replace numerical sample"), Stream.Latest()->RecordIndex, uint64(2));
    Feed(Stream, TEXT("{\"kind\":\"restart\",\"step\":2,\"path\":\"second-full-state.npz\"}\n"));
    TestEqual(TEXT("Explicit restart event may replace restart"), Stream.LastRestart()->Path, FString(TEXT("second-full-state.npz")));
    Feed(Stream, TEXT("{\"step\":3,\"nonfinite\":true}\n{\"step\":4,\"nonfinite\":true}\n"));
    TestEqual(TEXT("Guard action retention bounded"), Stream.ActionRequests().Num(), 2);
    Stream.BeginRun(UnitTestSource()); Feed(Stream, TEXT("{\"nonfinite\":true}\n"));
    TestFalse(TEXT("No last good state means no promised recovery checkpoint"), Stream.ActionRequests()[0].bCheckpointLastGoodState);
    TestFalse(TEXT("No location means no invented marker"), Stream.ActionRequests()[0].bLocateCell);
    TestFalse(TEXT("Run reset clears old restart"), Stream.LastRestart().IsSet());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4HealthTest, "Studio.Home4.Telemetry.FiveHealthSignalsRequireEvidence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4HealthTest::RunTest(const FString&)
{
    using namespace StudioHome4TelemetryTestFixtures;
    FStudioHome4Sample S; auto P = UnitTestPolicy();
    auto Signals = FStudioHome4Diagnostics::Evaluate(S, P);
    TestEqual(TEXT("Exactly five primary health signals"), Signals.Num(), 5);
    for (const auto& Signal : Signals)
    {
        TestTrue(TEXT("Missing measurements unavailable"), Signal.Status == EStudioHome4Health::Unavailable);
        TestFalse(TEXT("Missing measurements cannot fabricate zero"), Signal.Value.IsSet());
        TestFalse(TEXT("Signal supplies an action"), Signal.Remedy.IsEmpty());
    }
    S.Mass.PhiDrift = .000099; S.Mass.LevelDrifts.Add(.00002);
    Signals = FStudioHome4Diagnostics::Evaluate(S, P);
    TestTrue(TEXT("Mass strictly below threshold healthy"), Signals[0].Status == EStudioHome4Health::Healthy);
    S.Mass.LevelDrifts[0] = .0001;
    TestTrue(TEXT("Per-level mass at threshold warns"), FStudioHome4Diagnostics::Evaluate(S, P)[0].Status == EStudioHome4Health::Warning);
    S.Mass.LevelDrifts[0].Reset();
    TestTrue(TEXT("Missing per-level ledger cannot pass"), FStudioHome4Diagnostics::Evaluate(S, P)[0].Status == EStudioHome4Health::Unavailable);
    S.Budget.Work = 1.; S.Budget.DissipationNear = .5; S.Budget.DissipationFar = .1; S.Budget.DissipationAir = .1;
    S.Budget.BeachLoss = .1; S.Budget.FloorLoss = .1; S.Budget.DeltaKE = .1; S.Budget.DeltaPE = 0.;
    TestTrue(TEXT("Incomplete budget cannot pass"), FStudioHome4Diagnostics::Evaluate(S, P)[1].Status == EStudioHome4Health::Unavailable);
    S.Budget.Residual = 0.;
    TestTrue(TEXT("Complete balanced budget passes"), FStudioHome4Diagnostics::Evaluate(S, P)[1].Status == EStudioHome4Health::Healthy);
    S.Budget.Work = 2.;
    TestTrue(TEXT("Fabricated zero residual cannot mask imbalance"), FStudioHome4Diagnostics::Evaluate(S, P)[1].Status == EStudioHome4Health::Warning);
    S.Forces.Fx = 0.; S.Forces.MomentumFx = .0005;
    P.ForceReferenceMagnitude = 0.;
    TestTrue(TEXT("Near-zero force uses absolute tolerance"), FStudioHome4Diagnostics::Evaluate(S, P)[2].Status == EStudioHome4Health::Healthy);
    S.Forces.MomentumFx = .01;
    TestTrue(TEXT("Near-zero force discrepancy warns"), FStudioHome4Diagnostics::Evaluate(S, P)[2].Status == EStudioHome4Health::Warning);
    P.ForceReferenceMagnitude.Reset();
    TestTrue(TEXT("No guessed force reference"), FStudioHome4Diagnostics::Evaluate(S, P)[2].Status == EStudioHome4Health::Unavailable);
    P.ForceReferenceMagnitude = 1.; P.ForceComponent = FStudioHome4DiagnosticPolicy::EComponent::Fz;
    TestTrue(TEXT("Component selection never substitutes Fx for missing Fz"), FStudioHome4Diagnostics::Evaluate(S, P)[2].Status == EStudioHome4Health::Unavailable);
    S.Window.Fx = 1.; S.Window.PreviousFx = 1.01;
    TestTrue(TEXT("Previous/current bracket within tolerance passes"), FStudioHome4Diagnostics::Evaluate(S, P)[3].Status == EStudioHome4Health::Healthy);
    S.Window.PreviousFx.Reset();
    TestTrue(TEXT("One window cannot establish steadiness"), FStudioHome4Diagnostics::Evaluate(S, P)[3].Status == EStudioHome4Health::Unavailable);
    S.RestMaxDynamicPressure = 0.;
    TestTrue(TEXT("Zero pressure alone never implies rest"), FStudioHome4Diagnostics::Evaluate(S, P)[4].Status == EStudioHome4Health::Unavailable);
    S.RestCondition = false;
    TestTrue(TEXT("Explicitly moving run cannot pass rest test"), FStudioHome4Diagnostics::Evaluate(S, P)[4].Status == EStudioHome4Health::Unavailable);
    S.RestCondition = true;
    TestTrue(TEXT("Explicit rest plus exact zero passes"), FStudioHome4Diagnostics::Evaluate(S, P)[4].Status == EStudioHome4Health::Healthy);
    S.RestMaxDynamicPressure = 1e-12;
    TestTrue(TEXT("Exact-zero rest gate detects any pressure growth"), FStudioHome4Diagnostics::Evaluate(S, P)[4].Status == EStudioHome4Health::Warning);
    S.bNonfinite = true;
    for (const auto& Signal : FStudioHome4Diagnostics::Evaluate(S, P))
        TestTrue(TEXT("Guard event cannot show healthy science"), Signal.Status == EStudioHome4Health::Warning);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4PerformanceTest, "Studio.Home4.Telemetry.MeasuredPerformanceAndHotspotRequests",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4PerformanceTest::RunTest(const FString&)
{
    using namespace StudioHome4TelemetryTestFixtures;
    FStudioHome4Sample S; S.Step = 100; S.ReportedMLUPSInstant = 60.;
    auto Perf = FStudioHome4Diagnostics::Performance(S);
    TestFalse(TEXT("Step count and reported rate cannot invent elapsed work"), Perf.MLUPSInstant.IsSet() || Perf.MLUPSCumulative.IsSet() || Perf.GigabytesPerSecond.IsSet());
    S.Work.ElapsedSeconds = 0.; S.Work.NodeUpdates = 1000.;
    TestFalse(TEXT("Zero elapsed interval gives no rate"), FStudioHome4Diagnostics::Performance(S).MLUPSInstant.IsSet());
    S.Work.ElapsedSeconds = 2.; S.Work.NodeUpdates = 0.;
    TestEqual(TEXT("Measured stationary work is honest zero"), FStudioHome4Diagnostics::Performance(S).MLUPSInstant.Get(-1), 0.);
    TestFalse(TEXT("Missing transfer count gives no guessed bandwidth"), FStudioHome4Diagnostics::Performance(S).GigabytesPerSecond.IsSet());
    S.Work.NodeUpdates = std::numeric_limits<double>::infinity();
    TestFalse(TEXT("Nonfinite work cannot escape via public diagnostics"), FStudioHome4Diagnostics::Performance(S).MLUPSInstant.IsSet());
    FStudioHome4TelemetryStream Stream; Stream.BeginRun(UnitTestSource());
    Feed(Stream, TEXT("{\"step\":1,\"performance\":{\"cumulative_elapsed_seconds\":2,\"cumulative_node_updates\":200}}\n"));
    TestEqual(TEXT("Cumulative elapsed regression rejected"), Feed(Stream, TEXT("{\"step\":2,\"performance\":{\"cumulative_elapsed_seconds\":1}}\n")).Regressing, 1);
    TestEqual(TEXT("Cumulative work regression rejected"), Feed(Stream, TEXT("{\"step\":2,\"performance\":{\"cumulative_node_updates\":100}}\n")).Regressing, 1);
    auto Policy = UnitTestPolicy(); Policy.MaximumSpeedTrigger = .1; Stream.SetDiagnosticPolicy(Policy);
    Feed(Stream, TEXT("{\"step\":2,\"umax\":0.2,\"umax_loc\":[4,5,6]}\n"));
    TestEqual(TEXT("Measured hotspot creates action request"), Stream.ActionRequests().Num(), 1);
    const auto& Action = Stream.ActionRequests()[0];
    TestEqual(TEXT("Hotspot coordinate preserved"), Action.Facts.Cell->Z, 6);
    TestFalse(TEXT("Missing local phi remains unavailable"), Action.Facts.Phi.IsSet());
    TestFalse(TEXT("No known restart means no invented restart path"), Action.RestartPath.IsSet());
    TestEqual(TEXT("Hotspot recovery uses prior measured good state"), Action.LastGoodStep.Get(-1), int64(1));
    return true;
}
#endif
