#include "StudioHome4ReportPlots.h"
#include "StudioHome4Reports.h"
#include "StudioHome4Session.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4ReportPlotFixtures
{
    FStudioHome4ReferenceEvidence Evidence(const FStudioProject& P)
    {
        FStudioHome4ReferenceEvidence E;E.RecipeId=P.Draft.Home4->RecipeId;E.RunId=FGuid(1,2,3,4);
        E.AttachedProjectId=P.Id;E.AttachedCaseId=P.Draft.Id;
        E.ActualSource=TEXT("unit-test actual <original> & retained");E.ReferenceSource=TEXT("unit-test reference oracle");
        E.SourcePath=TEXT("unit-test/path_with%chars.json");E.SourceSHA256=FString::ChrN(64,TEXT('a'));
        FStudioHome4ReferenceSeries S;S.Id=TEXT("force");S.Name=TEXT("Unit-test force_1 & 50% {oracle}");S.Unit=TEXT("kg m/s²");S.AbscissaName=TEXT("t*");S.AbscissaUnit=TEXT("1");
        S.Abscissae={0,.5,1};S.Actual={1.16,1.04,1.01};S.Reference=S.Actual;S.AbsoluteTolerance=.001;S.RelativeTolerance=.02;E.Series.Add(S);
        E.OrderMetric=S.Id;E.OrderUnit=S.Unit;E.OrderRuns={{FGuid(1,0,0,0),1,1.16},{FGuid(2,0,0,0),2,1.04},{FGuid(3,0,0,0),4,1.01}};return E;
    }
    FString Read(const FString& Path){FString S;FFileHelper::LoadFileToString(S,*Path);return S;}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ReportOriginalPlot,"Studio.Home4.Reports.OriginalPlotSelectionAndNumericTables",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ReportOriginalPlot::RunTest(const FString&)
{
    FStudioHome4ReferenceSeries S;S.Name=TEXT("unit-test sampled value");S.Unit=TEXT("1");S.AbscissaName=TEXT("sample");S.AbscissaUnit=TEXT("1");
    for(int32 I=0;I<10001;++I){S.Abscissae.Add(I*.125);S.Actual.Add(I==10000?99.:I*.01);S.Reference.Add(I*.02);}
    FStudioHome4ReportPlot P;FString Error;TestTrue(TEXT("Original arrays prepare without altering measurements"),StudioHome4ReportPlots::Reference(S,P,Error));
    TestEqual(TEXT("Display preview bounded"),P.PreviewIndices.Num(),2000);TestEqual(TEXT("Last original retained"),P.PreviewIndices.Last(),10000);
    TestEqual(TEXT("Full arrays preserved"),P.Channels[0].Num(),10001);
    TestEqual(TEXT("Last actual original retained exactly"),P.Channels[0].Last(),99.);
    const auto Last=P.Normalized(0,P.PreviewIndices.Last());TestEqual(TEXT("Final original abscissa reaches right boundary"),Last.X,1.);
    for(int32 K=0;K<P.PreviewIndices.Num();++K)TestEqual(TEXT("Documented integer source selection"),P.PreviewIndices[K],int32(int64(K)*10000/1999));
    const FString CSV=StudioHome4ReportPlots::CSV(P);TArray<FString> Lines;CSV.ParseIntoArrayLines(Lines);
    TestEqual(TEXT("Table contains every original row plus header"),Lines.Num(),10002);TestTrue(TEXT("Final row retains complete values"),Lines.Last().StartsWith(TEXT("10000,1250,99,200")));
    TestTrue(TEXT("SVG joins actual selected original coordinates"),StudioHome4ReportPlots::SVG(P,TEXT("unit-test <source> & identity")).Contains(TEXT("870.000000,")));
    TestTrue(TEXT("SVG identity XML escaped"),StudioHome4ReportPlots::SVG(P,TEXT("unit-test <source> & identity")).Contains(TEXT("&lt;source&gt; &amp; identity")));
    TestTrue(TEXT("TikZ has finite final original position"),StudioHome4ReportPlots::TikZ(P).Contains(TEXT("(1.00000000,")));
    S.Actual.Reset();TestFalse(TEXT("Misaligned chart rejected safely"),StudioHome4ReportPlots::Reference(S,P,Error));
    S.Abscissae={-TNumericLimits<double>::Max(),TNumericLimits<double>::Max()};S.Actual={-TNumericLimits<double>::Max(),TNumericLimits<double>::Max()};S.Reference=S.Actual;
    TestTrue(TEXT("Extreme finite originals remain chartable"),StudioHome4ReportPlots::Reference(S,P,Error));
    TestEqual(TEXT("Safe extreme origin"),P.Normalized(0,0),FVector2D(0,0));TestEqual(TEXT("Safe extreme endpoint"),P.Normalized(0,1),FVector2D(1,1));
    S.Abscissae={1};S.Actual={5};S.Reference={5};TestTrue(TEXT("Single original remains visible"),StudioHome4ReportPlots::Reference(S,P,Error));TestEqual(TEXT("Constant original centered"),P.Normalized(0,0),FVector2D(.5,.5));
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ReportFigures,"Studio.Home4.Reports.FiguresScopeProvenanceAndFailure",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ReportFigures::RunTest(const FString&)
{
    using namespace StudioHome4ReportPlotFixtures;
    const FString Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-report-figures")/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Root,true);
    auto M=MakeShared<FStudioModel>(Root/TEXT("model"));FStudioHome4Session Session(M);TestTrue(TEXT("Apply real recipe settings"),Session.ApplyRecipe(TEXT("th01-hull")));
    M->Project.Name=TEXT("Unit-test Δx νH ρ σ τ · force_1 & 50% {report}");auto E=Evidence(M->Project);FString Path,Error;
    TestTrue(TEXT("Attached original evidence generates figures"),StudioHome4Reports::Export(Root,TEXT("figures"),M->SnapshotProject(),nullptr,Path,Error,&E));
    if(Path.IsEmpty())return false;
    AddInfo(TEXT("Independent TeX compile audit artifact: ")+Path);
    const FString TeX=Read(Path/TEXT("report.tex"));
    TestFalse(TEXT("Supplied evidence never claims absent"),TeX.Contains(TEXT("Reference evidence is not supplied")));
    TestTrue(TEXT("Report directly includes plotted original figure"),TeX.Contains(TEXT("\\input{reference-01.tikz}")) && TeX.Contains(TEXT("\\input{convergence.tikz}")));
    TestTrue(TEXT("Explicit UTF-8 TeX engine"),TeX.Contains(TEXT("fontspec")));
    TestTrue(TEXT("Unicode units escaped as valid math"),TeX.Contains(TEXT("\\ensuremath{{}^{2}}")) && TeX.Contains(TEXT("\\ensuremath{\\nu}")));
    TestTrue(TEXT("User metacharacters escaped"),TeX.Contains(TEXT("force\\_1 \\& 50\\% \\{report\\}")));
    TestTrue(TEXT("Coverage remains unknown"),TeX.Contains(TEXT("coverage is unknown")));
    TSharedPtr<FJsonObject> O;TestTrue(TEXT("Figure manifest parseable"),FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Read(Path/TEXT("report.json"))),O));
    if(!O)return false;const auto Figures=O->GetArrayField(TEXT("figures"));TestEqual(TEXT("Reference plus measured convergence figures"),Figures.Num(),2);
    if(Figures.Num()!=2)return false;const auto Ref=Figures[0]->AsObject();
    TestEqual(TEXT("Exact original series ID"),Ref->GetStringField(TEXT("metric_id")),E.Series[0].Id);
    TestEqual(TEXT("Exact original source hash"),Ref->GetStringField(TEXT("source_sha256")),E.SourceSHA256);
    TestEqual(TEXT("Original window endpoint"),Ref->GetNumberField(TEXT("window_end")),1.);
    TestEqual(TEXT("Original absolute tolerance"),Ref->GetNumberField(TEXT("absolute_tolerance")),.001);
    const auto Order=Figures[1]->AsObject();TestEqual(TEXT("Three original run IDs retained"),Order->GetArrayField(TEXT("runs")).Num(),3);
    TestTrue(TEXT("Missing averaging window disclosed"),Order->GetStringField(TEXT("scalar_window_status")).Contains(TEXT("not_supplied")));
    const FString Numeric=Read(Path/TEXT("convergence.csv"));for(const auto& R:E.OrderRuns)TestTrue(TEXT("Numeric table retains original run identity"),Numeric.Contains(R.RunId.ToString()));
    const FString Preserved=Read(Path/TEXT("report.json"));
    TestFalse(TEXT("Failed publish preserves existing bundle"),StudioHome4Reports::Export(Root,TEXT("figures"),M->SnapshotProject(),nullptr,Path,Error,&E));TestEqual(TEXT("Existing manifest unchanged"),Read(Path/TEXT("report.json")),Preserved);
    for(int32 I=0;I<4;++I){auto Wrong=E;if(I==0)Wrong.AttachedProjectId=FGuid::NewGuid();if(I==1)Wrong.AttachedCaseId=FGuid::NewGuid();if(I==2)Wrong.RecipeId=TEXT("foreign");if(I==3)Wrong.AttachedProjectId.Invalidate();TestFalse(TEXT("Raw export API rejects foreign or unattached evidence"),StudioHome4Reports::Export(Root,TEXT("wrong"),M->SnapshotProject(),nullptr,Path,Error,&Wrong));}
    auto Empty=E;Empty.Series.Reset();TestFalse(TEXT("Empty attached evidence cannot produce invented figure"),StudioHome4Reports::Export(Root,TEXT("empty"),M->SnapshotProject(),nullptr,Path,Error,&Empty));TestFalse(TEXT("Failed evidence publishes nothing"),IFileManager::Get().DirectoryExists(*(Root/TEXT("empty"))));
    TestTrue(TEXT("Report without evidence remains valid"),StudioHome4Reports::Export(Root,TEXT("no-evidence"),M->SnapshotProject(),nullptr,Path,Error));TestFalse(TEXT("No evidence means no fake plot"),IFileManager::Get().FileExists(*(Path/TEXT("reference-01.svg"))));
    TestTrue(TEXT("Missing evidence wording only used when absent"),Read(Path/TEXT("report.tex")).Contains(TEXT("Reference evidence is not supplied")));
    return !HasAnyErrors();
}
#endif
