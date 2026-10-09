#include "SStudioHome4Monitors.h"
#include "StudioModel.h"
#include "StudioHome4Session.h"
#include "StudioHome4Reports.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
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
        if (!UI.Press(TEXT("Home4Section_Thresholds"))) return false;
        for (int32 I = 0; I < 9; ++I)
            Test.TestTrue(TEXT("No guessed threshold prefill"), UI.Text(FName(*FString::Printf(TEXT("Home4Policy%d"), I))).IsEmpty());
        const FString Values[] = {TEXT("0.01"), TEXT("0.03"), TEXT("0.001"), TEXT("0.1"), TEXT("0.02"), TEXT("0.001"), TEXT("0.1"), TEXT("0"), TEXT("0.25")};
        for (int32 I = 0; I < UE_ARRAY_COUNT(Values); ++I)
            if (!UI.Type(FName(*FString::Printf(TEXT("Home4Policy%d"), I)), Values[I])) return false;
        if (!UI.Press(TEXT("Home4ApplyPolicy"))) return false;
        Test.TestEqual(TEXT("Maximum-speed trouble trigger entered explicitly"),Panel->DiagnosticPolicy().MaximumSpeedTrigger.Get(-1),.25);
        Test.TestEqual(TEXT("Applied explicit reference retained"), Panel->DiagnosticPolicy().ForceReferenceMagnitude.Get(-1), .1);
        Test.TestTrue(TEXT("Independent Fx channel becomes healthy"), UI.Text(TEXT("Home4Health2")).Contains(TEXT("Healthy")));
        Test.TestTrue(TEXT("Explicit window limits become healthy"), UI.Text(TEXT("Home4Health3")).Contains(TEXT("Healthy")));
        Test.TestTrue(TEXT("Explicit rest tolerance becomes healthy"), UI.Text(TEXT("Home4Health4")).Contains(TEXT("Healthy")));
        if (!UI.Type(TEXT("Home4Policy3"), TEXT("NaN")) || !UI.Press(TEXT("Home4ApplyPolicy"))) return false;
        Test.TestEqual(TEXT("Invalid input preserves applied reference"), Panel->DiagnosticPolicy().ForceReferenceMagnitude.Get(-1), .1);
        Test.TestTrue(TEXT("Invalid threshold cause visible"), UI.Text(TEXT("Home4MonitorStatus")).Contains(TEXT("finite nonnegative")));
        if (!UI.Press(TEXT("Home4ForceComponent2"))) return false;
        Test.TestTrue(TEXT("Selected missing component cannot borrow Fx"), UI.Text(TEXT("Home4Health2")).Contains(TEXT("Unavailable")));
        if (!UI.Press(TEXT("Home4Section_Trouble")) || !UI.Press(TEXT("Home4LocateCell"))) return false;
        Test.TestEqual(TEXT("Routed locate invokes owner once"), Located, 1);
        Test.TestEqual(TEXT("Original cell reaches owner"), Facts.Cell->Y, 3);
        Test.TestEqual(TEXT("Original phi reaches owner"), Facts.Phi.Get(-1), .25);
        Test.TestFalse(TEXT("Missing tau reaches owner as unavailable"), Facts.Tau.IsSet());
        if(!UI.Press(TEXT("Home4Section_Outputs")))return false;
        Test.TestTrue(TEXT("Viz output labeled distinctly"), UI.Text(TEXT("Home4OutputKind2")).Contains(TEXT("Visualization snapshot")));
        Test.TestTrue(TEXT("Restart output labeled distinctly"), UI.Text(TEXT("Home4OutputKind3")).Contains(TEXT("Restart state")));
        if(!UI.Press(TEXT("Home4Section_Performance")))return false;
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
            Model = MakeShared<FStudioModel>(Root/TEXT("project"));
            Panel = SNew(SStudioHome4Monitors).Model(Model).Stream(UnitTestStream());
            UI = MakeUnique<FStudioHeadlessSlate>(Test, Panel.ToSharedRef(), FVector2D(720, 1000));
            Panel->BeginImportPath(Good); Started = FPlatformTime::Seconds();
        }
        ~FImportWorkflow()
        { UI.Reset(); Panel.Reset(); Model.Reset(); IFileManager::Get().DeleteDirectory(*Root, false, true); }
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
                Test.TestFalse(TEXT("Unidentified imported replay cannot bind to a grid"), Panel->OriginalRunIdentity().IsSet());
                Test.TestTrue(TEXT("Reports access the displayed replay"), Panel->DisplayedTelemetry() == Panel->ImportedReplay().Get());
                const auto Provenance=Panel->ReportProvenance();
                Test.TestTrue(TEXT("Replay report explicitly has no original run identity"),Provenance&&Provenance->bImportedReplay&&!Provenance->OriginalRunId);
                Test.TestTrue(TEXT("Replay report original source hash supplied"),Provenance&&Provenance->SourceSHA256.Len()==64);
                Preserved = Panel->ImportedReplay();
                Panel->BeginImportPath(Bad); Stage = 1; return false;
            }
            if (Stage == 1)
            {
                Test.TestTrue(TEXT("Invalid import preserves same prior replay"), Panel->ImportedReplay() == Preserved);
                Test.TestTrue(TEXT("Failure cause visible"), Panel->StatusText().Contains(TEXT("invalid or regressing")));
                Test.TestFalse(TEXT("Empty path treated as cancellation"), Panel->BeginImportPath(TEXT("")));
                Test.TestTrue(TEXT("Cancelled selection preserves replay"), Panel->ImportedReplay() == Preserved);
                Test.TestFalse(TEXT("Malformed explicit original run rejected"), Panel->BeginImportPath(Good,TEXT("not-a-guid")));
                Test.TestTrue(TEXT("Invalid identity retains previous original replay"), Panel->ImportedReplay() == Preserved);
                UI->Type(TEXT("Home4OriginalRunId"),TEXT("not-a-guid"));UI->Press(TEXT("Home4ImportTelemetry"));
                Test.TestTrue(TEXT("Native identity validation rejects before opening picker"), Panel->StatusText().Contains(TEXT("valid GUID")));
                UI->Type(TEXT("Home4OriginalRunId"),TEXT(""));
                Panel->BeginImportPath(Good); Panel->CancelImport(); Stage = 2; return false;
            }
            if(Stage==2)
            {
                Test.TestTrue(TEXT("Background cancellation preserves prior replay"), Panel->ImportedReplay() == Preserved);
                Test.TestTrue(TEXT("Background cancellation visibly recorded"), Panel->StatusText().Contains(TEXT("cancelled")));
                OriginalRun=FGuid::NewGuid();Panel->BeginImportPath(Good,OriginalRun.ToString());Stage=3;return false;
            }
            if(Stage==3)
            {
                if(!Test.TestTrue(TEXT("Identified replay has an original measurement"),Panel->LatestDisplayedMeasurement()!=nullptr))return true;
                Test.TestTrue(TEXT("Explicit original run supplied to science owner"),Panel->OriginalRunIdentity()&&*Panel->OriginalRunIdentity()==OriginalRun);
                Test.TestEqual(TEXT("Measurement keeps exact externally supplied run identity"),Panel->LatestDisplayedMeasurement()->Source.RunId,OriginalRun);
                Test.TestEqual(TEXT("Original file path is source identity"),Panel->LatestDisplayedMeasurement()->Source.SourceId,FPaths::ConvertRelativePathToFull(Good));
                Test.TestTrue(TEXT("Native source makes explicit original identity visible"),UI->Text(TEXT("Home4MonitorSource")).Contains(TEXT("Original run ID supplied by owner")));
                Test.TestTrue(TEXT("Identified replay report retains supplied original run"),Panel->ReportProvenance()&&Panel->ReportProvenance()->OriginalRunId==TOptional<FGuid>(OriginalRun));
                Preserved=Panel->ImportedReplay();PreservedHash=Panel->ImportedSourceSHA256();
                Test.TestEqual(TEXT("Original replay SHA256 recorded"),PreservedHash.Len(),64);
                const auto Stamp=IFileManager::Get().GetTimeStamp(*Good);
                RewriteSucceeded=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
                Panel->SetImportVerificationForAutomation([Path=Good,Stamp,Wrote=RewriteSucceeded]
                {
                    Wrote->store(FFileHelper::SaveStringToFile(TEXT("{\"step\":8,\"nonfinite\":true}\n"),*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM),std::memory_order_relaxed);
                    IFileManager::Get().SetTimeStamp(*Path,Stamp);
                });
                Panel->BeginImportPath(Good,OriginalRun.ToString());Stage=4;return false;
            }
            if(Stage==4)
            {
                Test.TestTrue(TEXT("Verification fixture actually rewrites the original source"),RewriteSucceeded&&RewriteSucceeded->load(std::memory_order_relaxed));
                FString Rewritten;Test.TestTrue(TEXT("Read rewritten source after verification"),FFileHelper::LoadFileToString(Rewritten,*Good));
                Test.TestEqual(TEXT("Verification fixture changed exact original bytes"),Rewritten,FString(TEXT("{\"step\":8,\"nonfinite\":true}\n")));
                Test.TestTrue(TEXT("Same-size rewrite with preserved timestamp rejects import"),Panel->StatusText().Contains(TEXT("changed during import")));
                Test.TestTrue(TEXT("Changed original bytes preserve prior replay"),Panel->ImportedReplay()==Preserved);
                Test.TestEqual(TEXT("Changed bytes preserve previous source hash"),Panel->ImportedSourceSHA256(),PreservedHash);
                Panel->SetImportVerificationForAutomation({});
                Test.TestTrue(TEXT("Restore original fixture"),FFileHelper::SaveStringToFile(TEXT("{\"step\":7,\"nonfinite\":true}\n"),*Good,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
                Panel->BeginImportPath(Good,OriginalRun.ToString());Model->Project.Id=FGuid::NewGuid();Stage=5;return false;
            }
            if(Stage==5)
            {
                Test.TestFalse(TEXT("Project change clears prior imported replay"),Panel->ImportedReplay().IsValid());
                Test.TestTrue(TEXT("Pending original replay cannot attach to another project"),Panel->StatusText().Contains(TEXT("not attached")));
                Panel->BeginImportPath(Good,OriginalRun.ToString());Stage=6;return false;
            }
            if(Stage==6)
            {
                Test.TestTrue(TEXT("New project explicitly imports its own report scope"),Panel->ReportProvenance().IsSet());
                Panel->BeginImportPath(Good,OriginalRun.ToString());Model->Project.Draft.Id=FGuid::NewGuid();
                Test.TestFalse(TEXT("Case switch immediately clears report getter"),Panel->ReportProvenance().IsSet());
                Test.TestTrue(TEXT("Case switch immediately clears telemetry getter"),Panel->DisplayedTelemetry()==nullptr);Stage=7;return false;
            }
            Test.TestFalse(TEXT("Case change clears prior imported replay"),Panel->ImportedReplay().IsValid());
            Test.TestTrue(TEXT("Project change clears prior session source"),Panel->DisplayedTelemetry()==nullptr);
            Test.TestTrue(TEXT("Pending original replay cannot attach to another case"),Panel->StatusText().Contains(TEXT("not attached")));
            return true;
        }
    private:
        FAutomationTestBase& Test;
        FString Root, Good, Bad, PreservedHash;
        double Started = 0;
        int32 Stage = 0;
        TSharedPtr<SStudioHome4Monitors> Panel;
        TSharedPtr<FStudioModel> Model;
        FGuid OriginalRun;
        TUniquePtr<FStudioHeadlessSlate> UI;
        TSharedPtr<FStudioHome4TelemetryStream> Preserved;
        TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe> RewriteSucceeded;
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4MonitorSciencePresentation,"Studio.Home4.Monitors.NativeSciencePlotsUnitsAndDisclosure",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4MonitorSciencePresentation::RunTest(const FString&)
{
    using namespace StudioHome4SciencePresentation;
    if(!TestTrue(TEXT("Slate initialized"),FSlateApplication::IsInitialized()))return false;
    auto Stream=MakeShared<FStudioHome4TelemetryStream>();const FGuid Run=FGuid::NewGuid();Stream->BeginRun({Run,TEXT("original-unit-test-science")});
    const FString Records=TEXT("{\"kind\":\"source_metadata\",\"source_metadata\":{\"force_units\":\"lattice\",\"unit_map\":{\"dx_m\":0.01,\"dt_s\":0.001,\"rho_kg_m3\":1000},\"normalization\":{\"force_divisor\":2,\"force_label\":\"F/mg\"},\"body_normalizations\":{\"body-1\":{\"force_divisor\":4,\"force_label\":\"F/mg body-1\"}}}}\n")
        TEXT("{\"step\":1,\"t_star\":0.1,\"forces\":{\"Fx\":2,\"mea_Fx\":1,\"Fx_p\":1.5,\"Fx_nu\":0.5},\"bodies\":[{\"id\":\"body-1\",\"forces\":{\"Fx\":4,\"mea_Fx\":3},\"state\":{\"position\":[1,2,3],\"attitude_deg\":[4,5,6]},\"fit\":{\"added_mass\":1}}],\"levels\":[{\"level\":2,\"mass_drift\":0.00001}],\"phase_energies\":{\"heavy\":{\"ke\":1}},\"interface\":{\"thickness_histogram\":{\"edges\":[3,4,5],\"counts\":[1,2]}}}\n")
        TEXT("{\"step\":2,\"t_star\":0.2,\"forces\":{\"Fx\":3,\"mea_Fx\":2},\"bodies\":[{\"id\":\"body-1\",\"forces\":{\"Fx\":8},\"state\":{\"position\":[1,2,4],\"attitude_deg\":[4,6,6]},\"fit\":{\"added_mass\":2}}],\"phase_energies\":{\"heavy\":{\"ke\":2}}}\n");
    const FTCHARToUTF8 Bytes(*Records);TestEqual(TEXT("Science fixture accepted"),Stream->AppendBytes(reinterpret_cast<const uint8*>(Bytes.Get()),Bytes.Length()).Accepted,3);
    EStudioHome4UnitDisplay Display=EStudioHome4UnitDisplay::Physical;
    const auto Panel=SNew(SStudioHome4Monitors).Stream(Stream).UnitDisplay_Lambda([&]{return Display;});
    FStudioHeadlessSlate UI(*this,Panel,FVector2D(620,1000));
    TestTrue(TEXT("Body details initially disclosed on request"),!UI.Exists(TEXT("Home4Detail_Bodies")));
    TestEqual(TEXT("SI force uses immutable original units"),Panel->PresentedForces().Series[0].Values.Last().Get(-1),30.);
    TestTrue(TEXT("Native force caption identifies original t*"),UI.Text(TEXT("Home4ForceCaption")).Contains(TEXT("original t*")));
    if(!UI.Press(TEXT("Home4NormalizeForces")))return false;
    TestEqual(TEXT("Native benchmark mode uses explicit divisor"),Panel->PresentedForces().Series[0].Values.Last().Get(-1),1.5);
    TestTrue(TEXT("Benchmark label visibly attributed"),UI.Text(TEXT("Home4ForceCaption")).Contains(TEXT("F/mg")));
    if(!UI.Press(TEXT("Home4SelectBody")))return false;
    TestEqual(TEXT("Select original body identity"),Panel->SelectedBodyIdentity(),FString(TEXT("body-1")));
    if(!UI.Press(TEXT("Home4NormalizeForces")))return false;
    TestEqual(TEXT("Per-body divisor independent of root force"),Panel->PresentedForces().Series[0].Values.Last().Get(-1),2.);
    TestFalse(TEXT("Missing body momentum channel stays a gap"),Panel->PresentedForces().Series[1].Values.Last().IsSet());
    if(!UI.Press(TEXT("Home4PlotComponent3")))return false;
    TestFalse(TEXT("Selecting missing My never borrows Fx"),Panel->PresentedForces().Series[0].Values.Last().IsSet());
    if(!UI.Press(TEXT("Home4PlotComponent0"))||!UI.Press(TEXT("Home4Section_Bodies")))return false;
    TestTrue(TEXT("Absent reference cannot pass a gate"),UI.Text(TEXT("Home4Detail_Bodies")).Contains(TEXT("Reference comparison not evaluated")));
    if(!UI.Press(TEXT("Home4SelectLevel"))||!UI.Press(TEXT("Home4SelectPhase")))return false;
    for(int32 I=0;I<18;++I)if(!UI.Press(TEXT("Home4NextMetric")))return false;
    TestEqual(TEXT("Source metric selector reaches explicit level ledger"),UI.Text(TEXT("Home4SelectedMetric")),FString(TEXT("Selected level mass drift")));
    TestEqual(TEXT("Chosen original level present at first sample"),Panel->PresentedHistory().Series[0].Values[0].Get(-1),.00001);
    TestFalse(TEXT("Chosen missing level sample cannot inherit"),Panel->PresentedHistory().Series[0].Values.Last().IsSet());
    Display=EStudioHome4UnitDisplay::Lattice;if(!UI.Press(TEXT("Home4NormalizeForces")))return false;
    TestEqual(TEXT("Parent unit attribute updates source force display"),Panel->PresentedForces().Series[0].Values.Last().Get(-1),8.);
    TestTrue(TEXT("Science owner identity remains unchanged"),Panel->OriginalRunIdentity()&&*Panel->OriginalRunIdentity()==Run);
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4TelemetryReportProvenance,"Studio.Home4.Reports.TelemetryReplayIdentityOriginalUnitsAndScope",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4TelemetryReportProvenance::RunTest(const FString&)
{
    using namespace StudioHome4MonitorTestFixtures;
    const FString Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-telemetry-report")/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Root,true);
    auto Model=MakeShared<FStudioModel>(Root/TEXT("model"));FStudioHome4Session Settings(Model);
    if(!TestTrue(TEXT("Apply artificial recipe for report scope"),Settings.ApplyRecipe(TEXT("th01-hull"))))return false;
    Model->Project.Draft.Home4->Units.DxMeters=.5;Model->Project.Draft.Home4->Units.DtSeconds=.2;
    auto Stream=MakeShared<FStudioHome4TelemetryStream>();const FGuid Replay=FGuid::NewGuid();Stream->BeginRun({Replay,TEXT("artificial-original-science")});
    const FString Source=TEXT("{\"kind\":\"source_metadata\",\"source_metadata\":{\"force_units\":\"lattice\",\"energy_units\":\"physical\",\"unit_map\":{\"dx_m\":0.01,\"dt_s\":0.001},\"normalization\":{\"force_divisor\":2,\"force_label\":\"F/mg\"}}}\n{\"step\":7,\"forces\":{\"Fx\":3}}\n");
    const FTCHARToUTF8 Bytes(*Source);TestEqual(TEXT("Artificial source metadata accepted"),Stream->AppendBytes(reinterpret_cast<const uint8*>(Bytes.Get()),Bytes.Length()).Accepted,2);
    FStudioHome4TelemetryProvenance P;P.StreamRunId=Replay;P.bImportedReplay=true;P.SourceId=TEXT("artificial-original-science");
    P.SourcePath=Root/TEXT("original.jsonl");P.SourceSHA256=FString::ChrN(64,TEXT('a'));P.AttachedProjectId=Model->Project.Id;P.AttachedCaseId=Model->Project.Draft.Id;
    FString Destination,Error;
    TestFalse(TEXT("Science export requires owner provenance envelope"),StudioHome4Reports::Export(Root,TEXT("no-envelope"),Model->SnapshotProject(),&Stream.Get(),Destination,Error));
    if(!TestTrue(TEXT("Independent replay report accepted with scoped envelope"),StudioHome4Reports::Export(Root,TEXT("replay"),Model->SnapshotProject(),&Stream.Get(),Destination,Error,nullptr,nullptr,&P)))return false;
    FString Text;FFileHelper::LoadFileToString(Text,*(Destination/TEXT("report.json")));TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);
    if(!TestTrue(TEXT("Telemetry report JSON readable"),O.IsValid()))return false;const auto V=O->GetObjectField(TEXT("telemetry_provenance"));
    TestEqual(TEXT("Replay identity explicitly separated from solver run"),V->GetStringField(TEXT("identity_kind")),FString(TEXT("independent_replay")));
    TestTrue(TEXT("Replay original solver run remains absent"),V->HasTypedField<EJson::Null>(TEXT("original_run_id")));
    TestEqual(TEXT("Replay ID retains independently supplied identity"),V->GetStringField(TEXT("replay_id")),Replay.ToString());
    TestEqual(TEXT("Original source hash retained"),V->GetStringField(TEXT("source_sha256")),P.SourceSHA256);
    const auto Metadata=V->GetObjectField(TEXT("source_metadata"));const auto Units=Metadata->GetObjectField(TEXT("unit_map"));
    TestEqual(TEXT("Immutable source dx independent of current draft"),Units->GetNumberField(TEXT("dx_m")),.01);
    TestEqual(TEXT("Immutable source dt independent of current draft"),Units->GetNumberField(TEXT("dt_s")),.001);
    TestTrue(TEXT("Missing source density never borrowed from recipe"),Units->HasTypedField<EJson::Null>(TEXT("rho_kg_m3")));
    TestTrue(TEXT("Missing source length convention remains absent"),Metadata->HasTypedField<EJson::Null>(TEXT("length_units")));
    TestEqual(TEXT("Explicit force convention retained"),Metadata->GetStringField(TEXT("force_units")),FString(TEXT("lattice")));
    FFileHelper::LoadFileToString(Text,*(Destination/TEXT("telemetry.csv")));TestTrue(TEXT("CSV retains raw force without draft conversion"),Text.Contains(TEXT("7,,,,,3,")));
    auto Wrong=P;Wrong.AttachedCaseId=FGuid::NewGuid();TestFalse(TEXT("Foreign case envelope rejected"),StudioHome4Reports::Export(Root,TEXT("case"),Model->SnapshotProject(),&Stream.Get(),Destination,Error,nullptr,nullptr,&Wrong));
    Wrong=P;Wrong.AttachedProjectId=FGuid::NewGuid();TestFalse(TEXT("Foreign project envelope rejected"),StudioHome4Reports::Export(Root,TEXT("project"),Model->SnapshotProject(),&Stream.Get(),Destination,Error,nullptr,nullptr,&Wrong));
    Wrong=P;Wrong.StreamRunId=FGuid::NewGuid();TestFalse(TEXT("Foreign science identity envelope rejected"),StudioHome4Reports::Export(Root,TEXT("run"),Model->SnapshotProject(),&Stream.Get(),Destination,Error,nullptr,nullptr,&Wrong));
    Wrong=P;Wrong.SourceSHA256=TEXT("bad");TestFalse(TEXT("Malformed source hash rejected"),StudioHome4Reports::Export(Root,TEXT("hash"),Model->SnapshotProject(),&Stream.Get(),Destination,Error,nullptr,nullptr,&Wrong));
    Wrong=P;Wrong.OriginalRunId=FGuid::NewGuid();TestFalse(TEXT("Original run cannot differ from measured owner identity"),StudioHome4Reports::Export(Root,TEXT("original-run"),Model->SnapshotProject(),&Stream.Get(),Destination,Error,nullptr,nullptr,&Wrong));
    P.OriginalRunId=Replay;TestTrue(TEXT("Explicit original run is retained separately"),StudioHome4Reports::Export(Root,TEXT("identified"),Model->SnapshotProject(),&Stream.Get(),Destination,Error,nullptr,nullptr,&P));
    FFileHelper::LoadFileToString(Text,*(Destination/TEXT("report.json")));FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);
    TestEqual(TEXT("Explicit original run appears only when supplied"),O->GetObjectField(TEXT("telemetry_provenance"))->GetStringField(TEXT("original_run_id")),Replay.ToString());
    const auto Owner=SNew(SStudioHome4Monitors).Model(Model).Stream(Stream);TestTrue(TEXT("Session stream owner supplies original report identity"),Owner->ReportProvenance()&&Owner->ReportProvenance()->OriginalRunId==TOptional<FGuid>(Replay));
    Model->Project.Draft.Id=FGuid::NewGuid();TestFalse(TEXT("Report getter clears immediately after case switch"),Owner->ReportProvenance().IsSet());TestTrue(TEXT("Telemetry getter clears before next frame"),Owner->DisplayedTelemetry()==nullptr);
    IFileManager::Get().DeleteDirectory(*Root,false,true);return !HasAnyErrors();
}
#endif
