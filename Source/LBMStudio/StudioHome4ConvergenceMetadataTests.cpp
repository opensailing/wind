#include "StudioHome4Validation.h"
#include "StudioHome4ReportPlots.h"
#include "StudioHome4Reports.h"
#include "StudioHome4Session.h"
#include "SStudioHome4Validation.h"
#include "StudioModel.h"
#include "StudioHeadlessSlate.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4ConvergenceMetadataTestPrivate
{
    FStudioHome4ReferenceEvidence Fixture()
    {
        // Artificial evidence confined to tests, never installed scientific data.
        FStudioHome4ReferenceEvidence E;E.RecipeId=TEXT("th01-hull");E.RunId=FGuid::NewGuid();E.ActualSource=TEXT("artificial-measurements");E.ReferenceSource=TEXT("artificial-reference");
        FStudioHome4ReferenceSeries S;S.Id=TEXT("Fx");S.Name=TEXT("Original force");S.AbscissaName=TEXT("t*");S.AbscissaUnit=TEXT("1");S.Unit=TEXT("N");S.Abscissae={1,2,3};S.Actual={1,1.1,1.2};S.Reference=S.Actual;E.Series={S};
        E.OrderMetric=S.Id;E.OrderUnit=S.Unit;E.OrderRuns={{FGuid::NewGuid(),1,1.16},{FGuid::NewGuid(),2,1.04},{FGuid::NewGuid(),4,1.01}};
        auto& X=E.OrderRuns[0].Extraction;X.WindowStart=10;X.WindowEnd=20;X.AbscissaUnit=TEXT("s");X.Epoch=TEXT("original solver time zero");X.Method=TEXT("trapezoid mean");X.Source=TEXT("original forces.csv");X.SourceSHA256=FString::ChrN(64,TEXT('b'));
        return E;
    }
    TSharedRef<FJsonObject> Object(const FString& Text)
    {TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O.ToSharedRef();}
    FString JSON(const TSharedRef<FJsonObject>& O){FString S;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&S));return S;}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ConvergenceMetadata,"Studio.Home4.Validation.OptionalOriginalScalarExtractionMetadata",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ConvergenceMetadata::RunTest(const FString&)
{
    using namespace StudioHome4ConvergenceMetadataTestPrivate;const auto Input=Fixture();FStudioHome4ReferenceEvidence E;FString Error;FStudioHome4ReferenceExpectation Expected;Expected.RecipeId=Input.RecipeId;
    const FString Source=StudioHome4Validation::SerializeEvidence(Input);
    if(!TestTrue(TEXT("Optional original extraction contract parsed"),StudioHome4Validation::Parse(Source,Expected,E,Error)))return false;
    TestEqual(TEXT("Original window start preserved"),E.OrderRuns[0].Extraction.WindowStart.Get(-1),10.);TestEqual(TEXT("Original method retained"),E.OrderRuns[0].Extraction.Method,FString(TEXT("trapezoid mean")));
    TestFalse(TEXT("Missing second-rung window stays unknown"),E.OrderRuns[1].Extraction.WindowStart.IsSet());TestTrue(TEXT("Missing third-rung source stays unknown"),E.OrderRuns[2].Extraction.Source.IsEmpty());
    TestTrue(TEXT("Original scalar order still evaluated independently"),FMath::IsNearlyEqual(E.ObservedOrder.Get(-1),2.,1e-12));
    FStudioHome4ReferenceEvidence Again;TestTrue(TEXT("Metadata round trip valid"),StudioHome4Validation::Parse(StudioHome4Validation::SerializeEvidence(E),Expected,Again,Error));
    TestEqual(TEXT("Original per-rung epoch round trips"),Again.OrderRuns[0].Extraction.Epoch,E.OrderRuns[0].Extraction.Epoch);
    const auto SHA=E.SourceSHA256;
    for(int32 Kind=0;Kind<8;++Kind)
    {
        auto O=Object(Source);const auto X=O->GetObjectField(TEXT("order"))->GetArrayField(TEXT("runs"))[0]->AsObject()->GetObjectField(TEXT("extraction"));
        if(Kind==0)X->RemoveField(TEXT("window_end"));if(Kind==1)X->SetNumberField(TEXT("window_end"),10);if(Kind==2)X->RemoveField(TEXT("abscissa_unit"));
        if(Kind==3)X->SetStringField(TEXT("source_sha256"),TEXT("bad"));if(Kind==4)X->RemoveField(TEXT("source"));if(Kind==5)X->SetStringField(TEXT("window_start"),TEXT("10"));
        if(Kind==6)X->SetStringField(TEXT("method"),TEXT("bad\nmethod"));if(Kind==7)X->SetBoolField(TEXT("window_equivalent"),true);
        TestFalse(TEXT("Invalid extraction rejects transactionally"),StudioHome4Validation::Parse(JSON(O),Expected,E,Error));TestEqual(TEXT("Invalid extraction retains prior source identity"),E.SourceSHA256,SHA);
    }
    TestTrue(TEXT("Missing window displayed as unknown"),StudioHome4Validation::ScalarRunDescription(E.OrderRuns[1]).Contains(TEXT("Extraction window unknown")));
    if(!TestTrue(TEXT("Native Slate available"),FSlateApplication::IsInitialized()))return false;
    auto State=MakeShared<FStudioHome4ValidationState>();State->Evidence=MakeShared<FStudioHome4ReferenceEvidence>(E);
    const auto Widget=SNew(SStudioHome4Validation).State(State);FStudioHeadlessSlate UI(*this,Widget,FVector2D(760,1100));UI.Layout();
    TestTrue(TEXT("Native original method visible"),UI.Text(TEXT("Home4ConvergenceIdentity")).Contains(TEXT("trapezoid mean")));TestTrue(TEXT("Native absence remains unknown"),UI.Text(TEXT("Home4ConvergenceIdentity")).Contains(TEXT("Extraction window unknown")));
    const FString Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-convergence-metadata")/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Root,true);
    auto Model=MakeShared<FStudioModel>(Root/TEXT("model"));FStudioHome4Session Session(Model);Session.ApplyRecipe(E.RecipeId);E.AttachedProjectId=Model->Project.Id;E.AttachedCaseId=Model->Project.Draft.Id;
    FString Destination;TestTrue(TEXT("Original extraction metadata exported with report"),StudioHome4Reports::Export(Root,TEXT("report"),Model->SnapshotProject(),nullptr,Destination,Error,&E));
    FString Metadata;FFileHelper::LoadFileToString(Metadata,*(Destination/TEXT("report.json")));const auto Report=Object(Metadata);const auto Figures=Report->GetArrayField(TEXT("figures"));
    if(TestEqual(TEXT("Reference and convergence figures retained"),Figures.Num(),2))
    {
        const auto Runs=Figures[1]->AsObject()->GetArrayField(TEXT("runs"));TestEqual(TEXT("Report original source hash per rung"),Runs[0]->AsObject()->GetObjectField(TEXT("extraction"))->GetStringField(TEXT("source_sha256")),Input.OrderRuns[0].Extraction.SourceSHA256);
        TestTrue(TEXT("Report missing window is null"),Runs[1]->AsObject()->GetObjectField(TEXT("extraction"))->HasTypedField<EJson::Null>(TEXT("window_start")));
    }
    FString CSV;FFileHelper::LoadFileToString(CSV,*(Destination/TEXT("convergence.csv")));
    TestTrue(TEXT("Convergence CSV preserves original extraction fields"),CSV.Contains(TEXT("window_start,window_end,abscissa_unit,epoch,extraction_method,original_source,original_source_sha256"))&&CSV.Contains(TEXT(",10,20,\"s\",\"original solver time zero\",\"trapezoid mean\"")));
    IFileManager::Get().DeleteDirectory(*Root,false,true);return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ReferencePlotCaching,"Studio.Home4.Validation.PreparedOverlayCachesImmutableOriginalArrays",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ReferencePlotCaching::RunTest(const FString&)
{
    auto Evidence=MakeShared<FStudioHome4ReferenceEvidence>();FStudioHome4ReferenceSeries S;S.Name=TEXT("Large artificial test series");S.AbscissaName=TEXT("original t");S.AbscissaUnit=TEXT("s");S.Unit=TEXT("N");
    for(int32 I=0;I<100000;++I){S.Abscissae.Add(double(I));S.Actual.Add(double(I%100));S.Reference.Add(double(I%100)+.1);}Evidence->Series.Add(MoveTemp(S));
    FStudioHome4ReferencePlotCache Cache;const TSharedPtr<const FStudioHome4ReferenceEvidence> Immutable=Evidence;const auto* Prepared=Cache.Get(Immutable,0,false);
    if(!TestTrue(TEXT("Large original overlay prepared once"),Prepared!=nullptr))return false;
    TestEqual(TEXT("All originals remain retained"),Prepared->X.Num(),100000);TestEqual(TEXT("Paint selection remains bounded"),Prepared->PreviewIndices.Num(),2000);const auto* Data=Prepared->X.GetData();
    for(int32 I=0;I<1000;++I)Cache.Get(Immutable,0,false);
    TestEqual(TEXT("Repeated paints perform no full-array preparation"),Cache.PreparationCount(),uint64(1));TestTrue(TEXT("Repeated paint keeps prepared original arrays"),Cache.Get(Immutable,0,false)->X.GetData()==Data);
    Cache.Get(Immutable,1,false);TestEqual(TEXT("Invalid selection changes cache once"),Cache.PreparationCount(),uint64(2));TestTrue(TEXT("Invalid selection cannot borrow previous plot"),Cache.Get(Immutable,1,false)==nullptr);
    auto Replacement=MakeShared<FStudioHome4ReferenceEvidence>(*Evidence);Cache.Get(Replacement,0,false);TestEqual(TEXT("New immutable evidence prepares new source"),Cache.PreparationCount(),uint64(3));
    Cache.Get({},0,false);TestTrue(TEXT("Cleared evidence clears prepared overlay"),Cache.Get({},0,false)==nullptr);TestEqual(TEXT("Cleared evidence does not repeat work"),Cache.PreparationCount(),uint64(4));return !HasAnyErrors();
}
#endif
