#include "SStudioHome4Monitors.h"
#include "StudioHeadlessSlate.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4MonitorTestFixtures
{
    // Original source bytes here are unit test fixtures, never shipped CFD data.
    TSharedPtr<FStudioHome4TelemetryStream> UnitTestStream()
    {
        auto Stream = MakeShared<FStudioHome4TelemetryStream>();
        Stream->BeginRun({FGuid::NewGuid(), TEXT("unit-test-monitor-source")});
        const FString Record = TEXT("{\"step\":4,\"backend\":\"metal\",\"mass_ledger\":{\"phi\":0.000001},\"forces\":{\"Fx\":0.1,\"mea_Fx\":0.1001},")
            TEXT("\"window\":{\"Fx\":0.1,\"Fx_prev\":0.1001},\"wb_rest\":{\"at_rest\":true,\"pd_max\":0},\"locator\":{\"cell\":[2,3,4],\"phi\":0.25},")
            TEXT("\"trace\":\"test-trace.csv\",\"slice\":\"test-slice.npz\",\"snapshot\":\"test-viz.npz\",\"checkpoint\":\"test-restart.npz\"}\n");
        const FTCHARToUTF8 Bytes(*Record);
        Stream->AppendBytes(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
        return Stream;
    }
    bool NativeWorkflow(FAutomationTestBase& Test, double Width)
    {
        if (!Test.TestTrue(TEXT("Slate initialized"), FSlateApplication::IsInitialized())) return false;
        const auto Stream = UnitTestStream();
        int32 Located = 0;
        FStudioHome4CellFacts Facts;
        const auto Panel = SNew(SStudioHome4Monitors).Stream(Stream).OnLocateCell_Lambda([&](const FStudioHome4CellFacts& F) { ++Located; Facts = F; });
        FStudioHeadlessSlate UI(Test, Panel, FVector2D(Width, 1000));
        if (!UI.Inspect(FString::Printf(TEXT("home4-monitors-%d-initial"), int32(Width)),
            {TEXT("Home4MonitorSource"), TEXT("Home4Health0"), TEXT("Home4Health1"), TEXT("Home4Health2"), TEXT("Home4Health3"), TEXT("Home4Health4"), TEXT("Home4ImportTelemetry")})) return false;
        Test.TestTrue(TEXT("Session source attributed explicitly"), UI.Text(TEXT("Home4MonitorSource")).Contains(TEXT("unit-test-monitor-source")));
        Test.TestTrue(TEXT("Mass has fixed source threshold"), UI.Text(TEXT("Home4Health0")).Contains(TEXT("Healthy")));
        Test.TestTrue(TEXT("Force gate initially unavailable without user limits"), UI.Text(TEXT("Home4Health2")).Contains(TEXT("Unavailable")));
        for (int32 I = 0; I < 8; ++I)
            Test.TestTrue(TEXT("No guessed threshold prefill"), UI.Text(FName(*FString::Printf(TEXT("Home4Policy%d"), I))).IsEmpty());
        const FString Values[] = {TEXT("0.01"), TEXT("0.03"), TEXT("0.001"), TEXT("0.1"), TEXT("0.02"), TEXT("0.001"), TEXT("0.1"), TEXT("0")};
        for (int32 I = 0; I < UE_ARRAY_COUNT(Values); ++I)
            if (!UI.Type(FName(*FString::Printf(TEXT("Home4Policy%d"), I)), Values[I])) return false;
        if (!UI.Press(TEXT("Home4ApplyPolicy"))) return false;
        Test.TestEqual(TEXT("Applied explicit reference retained"), Panel->DiagnosticPolicy().ForceReferenceMagnitude.Get(-1), .1);
        Test.TestTrue(TEXT("Independent Fx channel becomes healthy"), UI.Text(TEXT("Home4Health2")).Contains(TEXT("Healthy")));
        Test.TestTrue(TEXT("Explicit window limits become healthy"), UI.Text(TEXT("Home4Health3")).Contains(TEXT("Healthy")));
        Test.TestTrue(TEXT("Explicit rest tolerance becomes healthy"), UI.Text(TEXT("Home4Health4")).Contains(TEXT("Healthy")));
        if (!UI.Type(TEXT("Home4Policy3"), TEXT("NaN")) || !UI.Press(TEXT("Home4ApplyPolicy"))) return false;
        Test.TestEqual(TEXT("Invalid input preserves applied reference"), Panel->DiagnosticPolicy().ForceReferenceMagnitude.Get(-1), .1);
        Test.TestTrue(TEXT("Invalid threshold cause visible"), UI.Text(TEXT("Home4MonitorStatus")).Contains(TEXT("finite nonnegative")));
        if (!UI.Press(TEXT("Home4ForceComponent2"))) return false;
        Test.TestTrue(TEXT("Selected missing component cannot borrow Fx"), UI.Text(TEXT("Home4Health2")).Contains(TEXT("Unavailable")));
        if (!UI.Press(TEXT("Home4LocateCell"))) return false;
        Test.TestEqual(TEXT("Routed locate invokes owner once"), Located, 1);
        Test.TestEqual(TEXT("Original cell reaches owner"), Facts.Cell->Y, 3);
        Test.TestEqual(TEXT("Original phi reaches owner"), Facts.Phi.Get(-1), .25);
        Test.TestFalse(TEXT("Missing tau reaches owner as unavailable"), Facts.Tau.IsSet());
        Test.TestTrue(TEXT("Viz output labeled distinctly"), UI.Text(TEXT("Home4OutputKind2")).Contains(TEXT("Visualization snapshot")));
        Test.TestTrue(TEXT("Restart output labeled distinctly"), UI.Text(TEXT("Home4OutputKind3")).Contains(TEXT("Restart state")));
        Test.TestTrue(TEXT("Unreported measured throughput unavailable"), UI.Text(TEXT("Home4Detail_Performance")).Contains(TEXT("Unavailable")));
        return !Test.HasAnyErrors();
    }

    class FImportWorkflow final : public IAutomationLatentCommand
    {
    public:
        explicit FImportWorkflow(FAutomationTestBase& InTest) : Test(InTest)
        {
            Root = FPaths::ProjectDir() / TEXT("tmp/debug/home4-monitor-import") / FGuid::NewGuid().ToString();
            IFileManager::Get().MakeDirectory(*Root, true);
            Good = Root / TEXT("unit-test-fixture.jsonl"); Bad = Root / TEXT("malformed-unit-test-fixture.jsonl");
            Test.TestTrue(TEXT("Write original unit fixture"), FFileHelper::SaveStringToFile(TEXT("{\"step\":7,\"nonfinite\":true}\n"), *Good, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
            Test.TestTrue(TEXT("Write malformed unit fixture"), FFileHelper::SaveStringToFile(TEXT("{\"step\":\"bad\"}\n"), *Bad, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
            Panel = SNew(SStudioHome4Monitors).Stream(UnitTestStream());
            UI = MakeUnique<FStudioHeadlessSlate>(Test, Panel.ToSharedRef(), FVector2D(720, 1000));
            Panel->BeginImportPath(Good); Started = FPlatformTime::Seconds();
        }
        ~FImportWorkflow()
        { UI.Reset(); Panel.Reset(); IFileManager::Get().DeleteDirectory(*Root, false, true); }
        bool Update() override
        {
            Panel->PollImport();
            if (FPlatformTime::Seconds() - Started > 20) { Test.AddError(TEXT("Science import did not complete within test deadline.")); return true; }
            if (Panel->IsImporting()) return false;
            if (Stage == 0)
            {
                Test.TestTrue(TEXT("Valid file installed as imported replay"), Panel->IsImportedReplay());
                Test.TestTrue(TEXT("Original bytes attributed as replay"), UI->Text(TEXT("Home4MonitorSource")).Contains(TEXT("Imported replay")));
                Test.TestTrue(TEXT("Guard remains historical"), UI->Text(TEXT("Home4MonitorSource")).Contains(TEXT("never stops or checkpoints")));
                Test.TestEqual(TEXT("Original science step retained"), Panel->ImportedReplay()->Latest()->Step.Get(-1), int64(7));
                Preserved = Panel->ImportedReplay();
                Panel->BeginImportPath(Bad); Stage = 1; return false;
            }
            if (Stage == 1)
            {
                Test.TestTrue(TEXT("Invalid import preserves same prior replay"), Panel->ImportedReplay() == Preserved);
                Test.TestTrue(TEXT("Failure cause visible"), Panel->StatusText().Contains(TEXT("invalid or regressing")));
                Test.TestFalse(TEXT("Empty path treated as cancellation"), Panel->BeginImportPath(TEXT("")));
                Test.TestTrue(TEXT("Cancelled selection preserves replay"), Panel->ImportedReplay() == Preserved);
                Panel->BeginImportPath(Good); Panel->CancelImport(); Stage = 2; return false;
            }
            Test.TestTrue(TEXT("Background cancellation preserves prior replay"), Panel->ImportedReplay() == Preserved);
            Test.TestTrue(TEXT("Background cancellation visibly recorded"), Panel->StatusText().Contains(TEXT("cancelled")));
            return true;
        }
    private:
        FAutomationTestBase& Test;
        FString Root, Good, Bad;
        double Started = 0;
        int32 Stage = 0;
        TSharedPtr<SStudioHome4Monitors> Panel;
        TUniquePtr<FStudioHeadlessSlate> UI;
        TSharedPtr<FStudioHome4TelemetryStream> Preserved;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4MonitorCompact, "Studio.Home4.Monitors.NativeCompact",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4MonitorCompact::RunTest(const FString&)
{ return StudioHome4MonitorTestFixtures::NativeWorkflow(*this, 560); }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4MonitorWide, "Studio.Home4.Monitors.NativeWide",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4MonitorWide::RunTest(const FString&)
{ return StudioHome4MonitorTestFixtures::NativeWorkflow(*this, 960); }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4MonitorImport, "Studio.Home4.Monitors.TransactionalBackgroundReplayImport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FStudioHome4MonitorImport::RunTest(const FString&)
{
    if (!TestTrue(TEXT("Slate initialized"), FSlateApplication::IsInitialized())) return false;
    ADD_LATENT_AUTOMATION_COMMAND(StudioHome4MonitorTestFixtures::FImportWorkflow(*this)); return true;
}
#endif
