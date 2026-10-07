#include "StudioHome4Runtime.h"
#include "SStudioHome4Runtime.h"
#include "StudioModel.h"
#include "StudioHeadlessSlate.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4PersistedCostOutputTest,"Studio.Home4.Runtime.PersistedAttributedOutputSizesAndCompatibleMeasuredWorkloads",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4PersistedCostOutputTest::RunTest(const FString&)
{
    FStudioHome4Spec S;S.RecipeId=TEXT("explicit-cost-fixture");S.Lattice.Extents=FIntVector(10,10,10);S.Run.Steps=100;
    S.Run.MeasureEvery=2;S.Run.SaveEvery=5;S.Run.VizEvery=10;S.Run.RestartEvery=20;S.Geometry.BodyMotion=TEXT("fixed");S.Geometry.RetabulationPolicy=TEXT("static");
    S.Performance.OutputByteEstimates={10.1,100,1000,10000};S.Performance.OutputEstimateSource=TEXT("Identified source allocation worksheet");S.Performance.OutputEstimateAssumption=TEXT("Uncompressed output, all headers included");
    const auto Layout=FStudioHome4RuntimeSession::ConfiguredOutputLayouts(S);TestEqual(TEXT("Fractional byte estimate conservatively rounded up"),Layout[0].BytesPerOutput.Get(0),uint64(11));
    const auto F=FStudioHome4RuntimeSession::Forecast(S,Layout);TestEqual(TEXT("Whole-plan persisted output forecast"),F.TotalBytes.Get(0),uint64(62550));
    auto Missing=S;Missing.Performance.OutputEstimateAssumption.Empty();TestFalse(TEXT("Missing storage assumption cannot become an attributed nominal size"),FStudioHome4RuntimeSession::Forecast(Missing,FStudioHome4RuntimeSession::ConfiguredOutputLayouts(Missing)).TotalBytes.IsSet());
    auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-persisted-cost")/FGuid::NewGuid().ToString());M->Project.Draft.Home4=S;FStudioHome4RuntimeSession R(M);FString Error;
    FStudioHome4BackendVerification V;V.Id=FGuid::NewGuid();V.Target=TEXT("original fixture target");V.Host=TEXT("original fixture host");V.Device=TEXT("original fixture device");V.Source=TEXT("Original fixture response");V.EffectiveBackend=EStudioHome4Backend::Metal;V.ExtensionImported=true;V.bDevelopmentResponse=true;R.SetBackendVerification(V,Error);
    FStudioHome4MeasuredRun H;H.RunId=FGuid::NewGuid();H.Host=V.Host;H.Device=V.Device;H.RecipeId=S.RecipeId;H.Backend=V.EffectiveBackend;H.Source=TEXT("Original completed fixture work/time");H.SourceSHA256=FString::ChrN(64,'a');H.RetabulationPolicy=S.Geometry.RetabulationPolicy;H.NodeUpdates=1000000;H.ElapsedSeconds=1;H.bCompletedOriginalRun=true;
    TestFalse(TEXT("Absent immutable workload cannot predict current run"),R.EstimatedSeconds(S,H).IsSet());H.OriginalRunSpec=S;
    TestTrue(TEXT("Matching declared machine/workload supports attributable estimate"),R.EstimatedSeconds(S,H).IsSet());
    TestEqual(TEXT("Current work divided by original measured update rate"),R.EstimatedSeconds(S,H).Get(-1),.1);
    Missing=S;Missing.Geometry.BodyMotion=TEXT("free");TestFalse(TEXT("Different motion cannot reuse static-body throughput"),R.EstimatedSeconds(Missing,H).IsSet());
    Missing=S;Missing.Geometry.RetabulateEvery=10;TestFalse(TEXT("Changed retabulation cadence cannot reuse old workload rate"),R.EstimatedSeconds(Missing,H).IsSet());
    if(!TestTrue(TEXT("Identified original workload can persist in measured history: ")+Error,R.RecordMeasuredRun(H,Error))){AddError(Error);return false;}
    const auto JSON=R.SerializePerformanceHistory();FStudioHome4RuntimeSession Reloaded(M);
    if(!TestTrue(TEXT("Original workload history round-trips"),Reloaded.ParsePerformanceHistory(JSON,Error))){AddError(Error);return false;}
    if(!TestEqual(TEXT("Exactly one original workload record restored"),Reloaded.PerformanceRecords().Num(),1))return false;
    TestTrue(TEXT("Immutable original workload spec retained"),Reloaded.PerformanceRecords()[0].OriginalRunSpec.IsSet());return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4NativePersistedOutputTest,"Studio.Home4.Runtime.NativeOutputAssumptionsPersistAndScope",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4NativePersistedOutputTest::RunTest(const FString&)
{
    if(!FSlateApplication::IsInitialized()){AddError(TEXT("Native Slate unavailable."));return false;}
    auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-native-output")/FGuid::NewGuid().ToString());FStudioHome4Spec S;S.Run.Steps=100;S.Run.MeasureEvery=2;S.Run.SaveEvery=5;S.Run.VizEvery=10;S.Run.RestartEvery=20;M->Project.Draft.Home4=S;
    auto R=MakeShared<FStudioHome4RuntimeSession>(M);const auto Panel=SNew(SStudioHome4Runtime).Model(M).Runtime(R);FStudioHeadlessSlate UI(*this,Panel,FVector2D(1200,2400));
    for(int32 I=0;I<4;++I)UI.Type(FName(*FString::Printf(TEXT("Home4RuntimeOutputSize%d"),I)),TEXT("100"));
    UI.Type(TEXT("Home4RuntimeEstimateSource"),TEXT("identified explicit fixture source"));UI.Type(TEXT("Home4RuntimeEstimateAssumption"),TEXT("explicit uncompressed bytes including headers"));UI.Press(TEXT("Home4RuntimeApplyEstimates"));
    TestEqual(TEXT("Native four-channel estimates stored in request"),M->Project.Draft.Home4->Performance.OutputByteEstimates.Num(),4);
    TestEqual(TEXT("Original source attribution survives in config"),M->Project.Draft.Home4->Performance.OutputEstimateSource,FString(TEXT("identified explicit fixture source")));
    TestTrue(TEXT("Native whole-plan estimate uses retained source"),UI.Text(TEXT("Home4RuntimeDiskForecast")).Contains(TEXT("identified explicit fixture source")));
    FStudioHome4Spec Parsed;FString Error;TestTrue(TEXT("Stored output estimates survive project spec round-trip"),StudioHome4Config::Parse(StudioHome4Config::Serialize(*M->Project.Draft.Home4),Parsed,Error));
    M->Project.Id=FGuid::NewGuid();M->Project.Draft.Id=FGuid::NewGuid();M->Project.Draft.Home4=S;Panel->Tick(FGeometry(),0,0);UI.Layout();
    TestTrue(TEXT("New scope cannot inherit prior output attribution"),UI.Text(TEXT("Home4RuntimeDiskForecast")).Contains(TEXT("Full planned total: unknown")));return true;
}
#endif
