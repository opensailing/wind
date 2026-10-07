#include "StudioHome4Reports.h"
#include "StudioHome4ReportPlots.h"
#include "StudioHome4Validation.h"
#include "StudioHome4Recipes.h"
#include "StudioProject.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#define UI UI_HOME4_REPORT_COMPLETION_TEST
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ReportOriginalScienceTest,"Studio.Home4.Reports.OriginalScienceGapsSidecarsAndCapturedBytes",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ReportOriginalScienceTest::RunTest(const FString&)
{
    FStudioProject P;P.Draft.Home4=FStudioHome4Spec();const FGuid Run=FGuid::NewGuid();
    const FString JSON=TEXT("{\"step\":1,\"t_star\":0,\"mass_ledger\":{\"phi\":0.00001},\"forces\":{\"Fx\":1,\"mea_Fx\":1},\"budget\":{\"W\":1,\"res\":0.1}}\n")
        TEXT("{\"step\":2,\"t_star\":1}\n{\"step\":3,\"t_star\":2,\"forces\":{\"Fx\":3},\"budget\":{\"W\":2,\"res\":0.2}}\n");
    const FTCHARToUTF8 UTF8(*JSON);TArray<uint8> Bytes;Bytes.Append(reinterpret_cast<const uint8*>(UTF8.Get()),UTF8.Length());
    FStudioHome4TelemetryStream S;S.BeginRun({Run,TEXT("original-science-unit-fixture")});S.AppendBytes(Bytes.GetData(),Bytes.Num());
    const auto H=StudioHome4SciencePresentation::History(&S,StudioHome4SciencePresentation::EMetric::Forces,EStudioHome4UnitDisplay::Lattice);
    FStudioHome4ReportPlot Plot;FString Error;TestTrue(TEXT("Gapped original force figure prepares"),StudioHome4ReportPlots::Science(H,TEXT("Original force"),Plot,Error));
    TestFalse(TEXT("Missing middle sample remains a true gap"),Plot.Present(0,1));
    TestTrue(TEXT("Round-trip table retains blank original gap"),StudioHome4ReportPlots::CSV(Plot).Contains(TEXT("1,1,,,,,")));
    FStudioHome4TelemetryProvenance Provenance;Provenance.StreamRunId=Run;Provenance.OriginalRunId=Run;Provenance.SourceId=TEXT("original-science-unit-fixture");
    Provenance.SourcePath=TEXT("original-fixture.jsonl");Provenance.AttachedProjectId=P.Id;Provenance.AttachedCaseId=P.Draft.Id;Provenance.bCapturedPrefix=true;Provenance.CapturedByteCount=Bytes.Num();
    uint8 Hash[32];unsigned int Count=0;EVP_Digest(Bytes.GetData(),Bytes.Num(),Hash,&Count,EVP_sha256(),nullptr);Provenance.SourceSHA256=BytesToHex(Hash,Count).ToLower();
    FStudioHome4ReportInputs Inputs;Inputs.OriginalTelemetryBytes=&Bytes;
    const FString Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-report-completion")/FGuid::NewGuid().ToString();FString Out;
    IFileManager::Get().MakeDirectory(*Root,true);
    const bool Exported=StudioHome4Reports::Export(Root,TEXT("report"),P,&S,Out,Error,nullptr,nullptr,&Provenance,&Inputs);
    if(!TestTrue(TEXT("Original science report publishes atomically: ")+Error,Exported))return false;
    TestTrue(TEXT("Independent original force SVG created"),IFileManager::Get().FileExists(*(Out/TEXT("science-force-0.svg"))));
    TestTrue(TEXT("Adjacent original force JSON sidecar created"),IFileManager::Get().FileExists(*(Out/TEXT("science-force-0.json"))));
    TArray<uint8> Retained;TestTrue(TEXT("Read exact retained source"),FFileHelper::LoadFileToArray(Retained,*(Out/TEXT("original-live-prefix.jsonl"))));
    TestTrue(TEXT("Exact original capture bytes retained without serialization"),Retained==Bytes);
    FString Metadata;FFileHelper::LoadFileToString(Metadata,*(Out/TEXT("report.json")));
    TestTrue(TEXT("Capture remains explicitly a prefix"),Metadata.Contains(TEXT("\"captured_prefix\": true")));
    TestFalse(TEXT("Published report cannot be overwritten"),StudioHome4Reports::Export(Root,TEXT("report"),P,&S,Out,Error,nullptr,nullptr,&Provenance,&Inputs));
    Bytes[0]=' ';TestFalse(TEXT("Mutated original capture cannot publish under previous hash"),StudioHome4Reports::Export(Root,TEXT("changed"),P,&S,Out,Error,nullptr,nullptr,&Provenance,&Inputs));
    IFileManager::Get().DeleteDirectory(*Root,false,true);return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4FigurePublicationContractTest,"Studio.Home4.Reports.FigureRunAndOriginalPublishedCoefficientRule",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4FigurePublicationContractTest::RunTest(const FString&)
{
    FStudioHome4Spec S=StudioHome4Recipes::Find(TEXT("couette-spin"))->Template;S.Run.OutDirectory=TEXT("run-original");FStudioHome4Spec Figure;FString Error;
    TestTrue(TEXT("Figure request derives separate _viz destination"),StudioHome4Reports::FigureRun(S,Figure,Error));
    TestEqual(TEXT("Original request remains unchanged"),S.Run.OutDirectory,FString(TEXT("run-original")));
    TestEqual(TEXT("Explicit figure destination"),Figure.Run.OutDirectory,FString(TEXT("run-original_viz")));
    FStudioHome4ReferenceEvidence E;E.RecipeId=S.RecipeId;E.RunId=FGuid::NewGuid();E.OriginalRunSpec=S;E.ActualSource=TEXT("Original published fixture");E.ReferenceSource=TEXT("Explicit analytic fixture");
    E.bReferenceOwnerVerified=true;E.ReferenceCitation=TEXT("Unit test analytic source");E.ReferenceSHA256=FString::ChrN(64,'a');
    FStudioHome4ReferenceSeries Series;Series.Id=TEXT("torque");Series.Name=TEXT("Torque");Series.Unit=TEXT("N m");Series.AbscissaName=TEXT("t*");Series.AbscissaUnit=TEXT("dimensionless");
    Series.Abscissae={0,1};Series.Actual={1,1};Series.Reference={1,1};Series.AbsoluteTolerance=.01;E.Series.Add(Series);
    FStudioHome4ReferenceExpectation Expected;Expected.RecipeId=S.RecipeId;FStudioHome4ReferenceEvidence Published,Current;
    TestTrue(TEXT("Original published fixture parses"),StudioHome4Validation::Parse(StudioHome4Validation::SerializeEvidence(E),Expected,Published,Error));
    E.RunId=FGuid::NewGuid();E.OriginalRunSpec=Figure;E.ActualSource=TEXT("Independent original figure fixture");
    TestTrue(TEXT("Original figure fixture parses"),StudioHome4Validation::Parse(StudioHome4Validation::SerializeEvidence(E),Expected,Current,Error));
    TestTrue(TEXT("Original figure agrees with published original coefficients"),StudioHome4Reports::PublicationCheck(Current,Published,.001,0,Error));
    E.Series[0].Actual={2,2};StudioHome4Validation::Parse(StudioHome4Validation::SerializeEvidence(E),Expected,Current,Error);
    TestFalse(TEXT("Mismatched original figure coefficients cannot publish"),StudioHome4Reports::PublicationCheck(Current,Published,.001,0,Error));
    TestTrue(TEXT("Failure identifies original coefficient difference"),Error.Contains(TEXT("differs from published")));
    return true;
}
#endif
