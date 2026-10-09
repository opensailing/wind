#include "SStudioHome4Monitors.h"
#include "SStudioHome4Reports.h"
#include "SStudioHome4Runtime.h"
#include "StudioHome4Runtime.h"
#include "StudioHome4Validation.h"
#include "StudioHome4RecipeGates.h"
#include "StudioModel.h"
#include "StudioHeadlessSlate.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Event.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4ClosureFixtures
{
    struct FOriginalReadGate
    {
        FEvent* Release=FPlatformProcess::GetSynchEventFromPool(true);
        ~FOriginalReadGate(){FPlatformProcess::ReturnSynchEventToPool(Release);}
    };
    void Append(FStudioHome4TelemetryStream& S,const FString& JSON)
    {const FTCHARToUTF8 B(*JSON);int32 Offset=0;while(Offset<B.Length()){const auto R=S.AppendBytes(reinterpret_cast<const uint8*>(B.Get())+Offset,B.Length()-Offset);if(!R.ConsumedBytes)break;Offset+=R.ConsumedBytes;}}
    class FReplayJourney final:public IAutomationLatentCommand
    {
    public:
        explicit FReplayJourney(FAutomationTestBase& T):Test(T)
        {
            Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-replay-window")/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Root,true);Path=Root/TEXT("original.jsonl");
            Original=TEXT("{\"umax\":0.001}\n");for(int32 I=0;I<5000;++I)Original+=FString::Printf(TEXT("{\"step\":%d,\"mass_ledger\":{\"phi\":0.00001,\"injected\":%d}}\n"),I,I);
            FFileHelper::SaveStringToFile(Original,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            M=MakeShared<FStudioModel>(Root/TEXT("project"));FStudioHome4Spec S;S.RecipeId=TEXT("explicit-replay-fixture");M->Project.Draft.Home4=S;
            Panel=SNew(SStudioHome4Monitors).Model(M);UI=MakeUnique<FStudioHeadlessSlate>(Test,Panel.ToSharedRef(),FVector2D(1200,2800));Started=FPlatformTime::Seconds();Panel->BeginImportPath(Path,Run.ToString());
        }
        ~FReplayJourney(){UI.Reset();Panel.Reset();M.Reset();IFileManager::Get().DeleteDirectory(*Root,false,true);}
        bool Update()override
        {
            Panel->PollImport();UI->Layout();if(FPlatformTime::Seconds()-Started>20){Test.AddError(TEXT("Replay interval journey exceeded bounded deadline."));return true;}if(Panel->IsImporting())return false;
            if(Stage==0)
            {
                if(!Test.TestTrue(TEXT("Complete original log imported"),Panel->ImportedReplay().IsValid())){Test.AddError(Panel->StatusText());return true;}
                Test.TestEqual(TEXT("Default chart preview remains bounded"),Panel->ImportedReplay()->History().Num(),240);SHA=Panel->ImportedSourceSHA256();
                UI->Type(TEXT("Home4ReplayStepStart"),TEXT("40"));UI->Type(TEXT("Home4ReplayStepEnd"),TEXT("50"));UI->Press(TEXT("Home4LoadReplayInterval"));++Stage;return false;
            }
            if(Stage==1)
            {
                const auto S=Panel->ImportedReplay();if(!Test.TestEqual(TEXT("All exact selected originals retained"),S->History().Num(),11))return true;
                Test.TestEqual(TEXT("Original record index includes earlier omitted line"),S->History()[0].RecordIndex,uint64(42));Test.TestEqual(TEXT("Selected latest stops at exact original endpoint"),S->Latest()->Step.Get(-1),int64(50));
                Test.TestEqual(TEXT("Selection keeps full original source SHA"),Panel->ImportedSourceSHA256(),SHA);Test.TestEqual(TEXT("Selection keeps immutable original run identity"),Panel->OriginalRunIdentity().Get(FGuid()),Run);
                auto Report=SNew(SStudioHome4Reports).Model(M).Monitors(Panel);if(!Test.TestTrue(TEXT("Native report exports selected interval and full original bytes"),Report->ExportTo(Root,TEXT("selected-report")))){Test.AddError(Report->StatusText());return true;}
                FString Exported,Manifest;FFileHelper::LoadFileToString(Exported,*(Root/TEXT("selected-report/original-telemetry.jsonl")));FFileHelper::LoadFileToString(Manifest,*(Root/TEXT("selected-report/report.json")));
                Test.TestEqual(TEXT("Full original source survives narrow selection"),Exported,Original);Test.TestTrue(TEXT("Report records exact original step selection"),Manifest.Contains(TEXT("selected_original_step_start")));
                const auto Provenance=Panel->ReportProvenance();if(!Test.TestTrue(TEXT("Selected replay provides its scoped report envelope"),Provenance.IsSet()))return true;
                FString RejectedPath,Error;auto Wrong=*Provenance;Wrong.SelectedStepEnd.Reset();
                Test.TestFalse(TEXT("Raw report rejects one-sided original replay bounds safely"),StudioHome4Reports::Export(Root,TEXT("unpaired"),M->SnapshotProject(),S.Get(),RejectedPath,Error,nullptr,nullptr,&Wrong));
                Test.TestFalse(TEXT("Malformed replay envelope publishes no directory"),IFileManager::Get().DirectoryExists(*(Root/TEXT("unpaired"))));
                Wrong=*Provenance;Wrong.SelectedStepStart=41;
                Test.TestFalse(TEXT("Declared replay interval cannot exclude retained original steps"),StudioHome4Reports::Export(Root,TEXT("wrong-selection"),M->SnapshotProject(),S.Get(),RejectedPath,Error,nullptr,nullptr,&Wrong));
                UI->Type(TEXT("Home4ReplayStepStart"),TEXT("0"));UI->Type(TEXT("Home4ReplayStepEnd"),TEXT("4999"));UI->Press(TEXT("Home4LoadReplayInterval"));++Stage;return false;
            }
            if(Stage==2)
            {
                Test.TestTrue(TEXT("Oversized exact interval is rejected explicitly"),Panel->StatusText().Contains(TEXT("4096")));Test.TestEqual(TEXT("Oversized selection preserves prior interval"),Panel->ImportedReplay()->History().Num(),11);
                auto Changed=Original.Replace(TEXT("0.00001"),TEXT("0.00002"));FFileHelper::SaveStringToFile(Changed,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
                UI->Type(TEXT("Home4ReplayStepStart"),TEXT("30"));UI->Type(TEXT("Home4ReplayStepEnd"),TEXT("35"));UI->Press(TEXT("Home4LoadReplayInterval"));++Stage;return false;
            }
            Test.TestTrue(TEXT("Changed pinned original source cannot silently replace replay"),Panel->StatusText().Contains(TEXT("differ from the imported source")));Test.TestEqual(TEXT("Changed source preserves prior immutable source hash"),Panel->ImportedSourceSHA256(),SHA);return true;
        }
    private:
        FAutomationTestBase& Test;FString Root,Path,Original,SHA;FGuid Run=FGuid::NewGuid();double Started=0;int32 Stage=0;
        TSharedPtr<FStudioModel> M;TSharedPtr<SStudioHome4Monitors> Panel;TUniquePtr<FStudioHeadlessSlate> UI;
    };
    class FOriginalImportJourney final:public IAutomationLatentCommand
    {
    public:
        explicit FOriginalImportJourney(FAutomationTestBase& T):Test(T)
        {
            Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-native-status-history")/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Root,true);
            M=MakeShared<FStudioModel>(Root/TEXT("project"));Spec.RecipeId=TEXT("couette-spin");Spec.Geometry.BodyMotion=TEXT("fixed");Spec.Geometry.RetabulationPolicy=TEXT("static");M->Project.Draft.Home4=Spec;
            Runtime=MakeShared<FStudioHome4RuntimeSession>(M);Panel=SNew(SStudioHome4Runtime).Model(M).Runtime(Runtime);UI=MakeUnique<FStudioHeadlessSlate>(Test,Panel.ToSharedRef(),FVector2D(1400,2800));
            const FString Status=FString::Printf(TEXT("{\"schema\":\"LBMStudio.Home4TargetStatus\",\"verification_id\":\"%s\",\"target\":\"original-fixture-target\",\"host\":\"original-fixture-host\",\"device\":\"original-fixture-device\",\"source\":\"explicit original response automation fixture\",\"effective_backend\":\"metal\",\"extension_imported\":true,\"development_response\":false,\"utilization_percent\":5,\"owner\":\"fixture-owner\",\"power_watts\":12}"),*Verification.ToString());
            StatusPath=Root/TEXT("original-status.json");HistoryPath=Root/TEXT("original-history.json");PublishedPath=Root/TEXT("original-published.json");BadPath=Root/TEXT("bad.json");
            FFileHelper::SaveStringToFile(Status,*StatusPath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);FFileHelper::SaveStringToFile(TEXT("{}"),*BadPath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            Started=FPlatformTime::Seconds();Panel->ImportPath(StatusPath,false);
        }
        ~FOriginalImportJourney(){if(ReadGate)ReadGate->Release->Trigger();UI.Reset();Panel.Reset();Reports.Reset();Runtime.Reset();M.Reset();IFileManager::Get().DeleteDirectory(*Root,false,true);}
        bool Update()override
        {
            Panel->Tick(FGeometry(),FPlatformTime::Seconds(),0);if(Reports)Reports->PollImport();UI->Layout();if(FPlatformTime::Seconds()-Started>20){Test.AddError(TEXT("Original native import journey exceeded bounded deadline."));return true;}if(Panel->IsImporting()||(Reports&&Reports->IsImporting()))return false;
            if(Stage==0)
            {
                const auto B=Runtime->BackendVerification();if(!Test.TestTrue(TEXT("Native asynchronous original target import attached"),B&&B->Id==Verification)){Test.AddError(Panel->ImportStatus());return true;}
                Test.TestFalse(TEXT("Original response is distinct from development response"),B->bDevelopmentResponse);Test.TestTrue(TEXT("Original resource measurements shown by native widget"),UI->Text(TEXT("Home4RuntimeSummary")).Contains(TEXT("12 W")));
                FString Error;if(!Test.TestTrue(TEXT("Capture immutable request for matching provenance fixture"),Runtime->Submit(Spec,Run,TEXT("fixture-owner"),FPlatformTime::Seconds(),Error))){Test.AddError(Error);return true;}
                FStudioHome4MeasuredRun H;H.RunId=Run;H.Host=B->Host;H.Device=B->Device;H.RecipeId=Spec.RecipeId;H.Backend=B->EffectiveBackend;H.Source=TEXT("identified completed original work/time fixture");H.SourceSHA256=FString::ChrN(64,'a');H.RetabulationPolicy=Spec.Geometry.RetabulationPolicy;H.NodeUpdates=2000000;H.ElapsedSeconds=1;H.bCompletedOriginalRun=true;H.OriginalRunSpec=Spec;
                FStudioHome4RuntimeSession Fixture(nullptr);if(!Fixture.RecordMeasuredRun(H,Error)){Test.AddError(Error);return true;}
                FFileHelper::SaveStringToFile(Fixture.SerializePerformanceHistory(),*HistoryPath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);Panel->ImportPath(HistoryPath,true);++Stage;return false;
            }
            if(Stage==1)
            {
                if(!Test.TestEqual(TEXT("Native asynchronous history retains actual source rate"),Runtime->PerformanceRecords().Num(),1))return true;
                const auto* Record=M->Project.Runs.FindByPredicate([this](const auto& R){return R.GetId()==Run;});if(!Test.TestTrue(TEXT("Exact original measured run updates its matching history card"),Record&&Record->GetProvenance()&&Record->GetProvenance()->MeasuredMLUPS.Get(-1)==2))return true;
                FStudioProject Reload;FString Error;Test.TestTrue(TEXT("Original measured provenance survives project persistence"),StudioProjectIO::Parse(StudioProjectIO::Serialize(M->SnapshotProject()),Reload,Error));
                const auto OriginalProject=M->Project;const FString PriorRecord=StudioCaseIO::Serialize(*Record->GetConfiguration());
                const int32 MatchedIndex=M->Project.Runs.IndexOfByPredicate([this](const auto& R){return R.GetId()==Run;});
                auto PriorProvenance=Record->GetProvenance().GetValue();PriorProvenance.MeasuredMLUPS.Reset();PriorProvenance.MeasurementSource.Empty();PriorProvenance.MeasurementSHA256.Empty();PriorProvenance.OriginalRunSpec.Reset();
                M->Project.Runs[MatchedIndex]=Record->WithProvenance(PriorProvenance);
                const int32 BeforePadding=FTCHARToUTF8(*StudioProjectIO::Serialize(M->SnapshotProject())).Length();
                M->Project.RecoverySource=FString::ChrN((4*1024*1024-8192-512-BeforePadding)/3,TCHAR(0x754c));
                const FString Padded=StudioProjectIO::Serialize(M->SnapshotProject());const FTCHARToUTF8 PaddedBytes(*Padded);
                if(!Test.TestTrue(TEXT("Unicode persistence boundary is below file byte limit but well below character limit"),PaddedBytes.Length()<4*1024*1024&&PaddedBytes.Length()>4*1024*1024-16384&&Padded.Len()<2*1024*1024))return true;
                Test.TestTrue(TEXT("Unicode boundary fixture remains a structurally valid project"),StudioProjectIO::Parse(Padded,Reload,Error));
                FStudioHome4RuntimeSession BudgetRuntime(M);Test.TestTrue(TEXT("Original rates remain independently recordable when card enrichment exceeds file budget"),BudgetRuntime.ParsePerformanceHistory(Runtime->SerializePerformanceHistory(),Error));
                Test.TestFalse(TEXT("UTF-8 byte limit prevents an unloadable enriched run card"),M->Project.Runs[MatchedIndex].GetProvenance()->MeasuredMLUPS.IsSet());
                Test.TestEqual(TEXT("Rejected enrichment leaves frozen original configuration intact"),StudioCaseIO::Serialize(*M->Project.Runs[MatchedIndex].GetConfiguration()),PriorRecord);
                FStudioHome4ReferenceEvidence Evidence;Evidence.RecipeId=Spec.RecipeId;Evidence.RunId=Run;Evidence.ActualSource=TEXT("explicit original fixture");Evidence.ReferenceSource=TEXT("explicit identified reference fixture");Evidence.OriginalRunSpec=Spec;
                FStudioHome4ReferenceSeries Series;Series.Id=TEXT("torque");Series.Name=TEXT("torque");Series.Unit=TEXT("N m");Series.AbscissaName=TEXT("t*");Series.AbscissaUnit=TEXT("1");Series.Abscissae={0,1};Series.Actual={1,1};Series.Reference=Series.Actual;Evidence.Series.Add(Series);
                FStudioHome4ReferenceExpectation Expected;Expected.RecipeId=Spec.RecipeId;
                if(!Test.TestTrue(TEXT("Original evidence boundary fixture parses strictly"),StudioHome4Validation::Parse(StudioHome4Validation::SerializeEvidence(Evidence),Expected,Evidence,Error)))return true;
                Evidence.AttachedProjectId=M->Project.Id;Evidence.AttachedCaseId=M->Project.Draft.Id;
                Test.TestFalse(TEXT("Original gate attachment also respects UTF-8 project file budget"),Runtime->AttachOriginalEvidence(Evidence,Error));
                Test.TestTrue(TEXT("Evidence byte-budget rejection is explicit"),Error.Contains(TEXT("document budget")));
                M->Project=OriginalProject;
                Panel->ImportPath(BadPath,true);++Stage;return false;
            }
            if(Stage==2)
            {
                Test.TestEqual(TEXT("Malformed original history preserves prior records"),Runtime->PerformanceRecords().Num(),1);
                ReadGate=MakeShared<FOriginalReadGate,ESPMode::ThreadSafe>();Panel->SetImportVerificationForAutomation([Gate=ReadGate]{Gate->Release->Wait(5000);});
                Test.TestTrue(TEXT("Start actual original read before native cancellation"),Panel->ImportPath(StatusPath,false));
                Test.TestTrue(TEXT("Enabled native cancel action receives input while worker is held"),UI->Press(TEXT("Home4RuntimeCancelImport")));
                ReadGate->Release->Trigger();Panel->SetImportVerificationForAutomation({});++Stage;return false;
            }
            if(Stage==3)
            {
                Test.TestTrue(TEXT("Native cancel preserves original reviewed target"),Runtime->BackendVerification()&&Runtime->BackendVerification()->Id==Verification);Test.TestTrue(TEXT("Cancellation outcome explicit"),Panel->ImportStatus().Contains(TEXT("cancelled")));
                FStudioHome4ReferenceEvidence E;E.RecipeId=Spec.RecipeId;E.RunId=FGuid::NewGuid();E.ActualSource=TEXT("original coefficient fixture");E.ReferenceSource=TEXT("identified published fixture");FStudioHome4ReferenceSeries S;S.Id=TEXT("torque");S.Name=TEXT("torque");S.Unit=TEXT("N m");S.AbscissaName=TEXT("t*");S.AbscissaUnit=TEXT("1");S.Abscissae={0,1};S.Actual={1,1};S.Reference={1,1};E.Series.Add(S);
                FFileHelper::SaveStringToFile(StudioHome4Validation::SerializeEvidence(E),*PublishedPath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
                Reports=SNew(SStudioHome4Reports).Model(M).Runtime(Runtime);Reports->ImportPublishedPath(PublishedPath);++Stage;return false;
            }
            if(Stage==4)
            {Test.TestTrue(TEXT("Published evidence uses bounded background original import"),Reports->StatusText().Contains(TEXT("selected")));Panel->ImportPath(StatusPath,false);M->Project.Id=FGuid::NewGuid();M->Project.Draft.Id=FGuid::NewGuid();++Stage;return false;}
            Test.TestTrue(TEXT("Pending original status cannot attach after scope switch"),Panel->ImportStatus().Contains(TEXT("changed while importing")));Test.TestFalse(TEXT("Foreign scope cannot retain reviewed target"),Runtime->BackendVerification().IsSet());return true;
        }
    private:
        FAutomationTestBase& Test;FString Root,StatusPath,HistoryPath,PublishedPath,BadPath;double Started=0;int32 Stage=0;FGuid Verification=FGuid::NewGuid(),Run=FGuid::NewGuid();FStudioHome4Spec Spec;
        TSharedPtr<FStudioModel> M;TSharedPtr<FStudioHome4RuntimeSession> Runtime;TSharedPtr<SStudioHome4Runtime> Panel;TSharedPtr<SStudioHome4Reports> Reports;TUniquePtr<FStudioHeadlessSlate> UI;
        TSharedPtr<FOriginalReadGate,ESPMode::ThreadSafe> ReadGate;
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ExactReplayWindowJourney,"Studio.Home4.Monitors.NativeExactOriginalReplayWindowAndFullSourceReport",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ExactReplayWindowJourney::RunTest(const FString&)
{if(!FSlateApplication::IsInitialized()){AddError(TEXT("Native Slate unavailable."));return false;}ADD_LATENT_AUTOMATION_COMMAND(StudioHome4ClosureFixtures::FReplayJourney(*this));return true;}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4NativeOriginalTargetHistoryJourney,"Studio.Home4.Runtime.NativeOriginalStatusHistoryProvenanceCancellationAndScope",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4NativeOriginalTargetHistoryJourney::RunTest(const FString&)
{if(!FSlateApplication::IsInitialized()){AddError(TEXT("Native Slate unavailable."));return false;}ADD_LATENT_AUTOMATION_COMMAND(StudioHome4ClosureFixtures::FOriginalImportJourney(*this));return true;}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4OriginalBodyComparisonChart,"Studio.Home4.Telemetry.OriginalBodyComparisonCurvesAndConventionGaps",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4OriginalBodyComparisonChart::RunTest(const FString&)
{
    using namespace StudioHome4SciencePresentation;FStudioHome4TelemetryStream S;S.BeginRun({FGuid::NewGuid(),TEXT("explicit original body fixture")});
    StudioHome4ClosureFixtures::Append(S,TEXT("{\"kind\":\"source_metadata\",\"source_metadata\":{\"force_units\":\"physical\",\"length_units\":\"physical\"}}\n"));
    StudioHome4ClosureFixtures::Append(S,TEXT("{\"step\":1,\"bodies\":[{\"id\":\"hull\",\"forces\":{\"Fz\":14,\"My\":8},\"attitude\":{\"equilibrium_heave\":1,\"running_heave\":2,\"reference_heave\":3,\"reference_source\":\"identified original tank\",\"k33\":10,\"k35\":2,\"k55\":4,\"k33_unit\":\"N/m\",\"k35_unit\":\"N\",\"k55_unit\":\"N m\",\"stiffness_convention\":\"symmetric_heave_m_pitch_rad_load_Fz_N_My_Nm\"},\"fit\":{\"added_mass\":2,\"reference_added_mass\":2.1,\"added_mass_unit\":\"kg\",\"reference_source\":\"identified original BEM\"}}]}\n"));
    auto H=History(&S,EMetric::BodyHeaveComparison,EStudioHome4UnitDisplay::Physical,0,false,TEXT("hull"));if(!TestEqual(TEXT("Separate original attitude comparison channels"),H.Series.Num(),5))return false;
    TestEqual(TEXT("Held original heave"),H.Series[0].Values[0].Get(-1),1.);TestEqual(TEXT("Running original heave"),H.Series[1].Values[0].Get(-1),2.);TestEqual(TEXT("Identified original reference heave"),H.Series[3].Values[0].Get(-1),3.);
    TestTrue(TEXT("Declared K inverse appears as independent calculated channel"),FMath::IsNearlyEqual(H.Series[4].Values[0].Get(-1),10./9.,1.e-12));
    H=History(&S,EMetric::AddedMass,EStudioHome4UnitDisplay::Physical,0,false,TEXT("hull"));TestEqual(TEXT("Fitted/reference original coefficients overlay"),H.Series.Num(),2);TestEqual(TEXT("Original reference coefficient retained"),H.Series[1].Values[0].Get(-1),2.1);
    H=History(&S,EMetric::BodyHeaveComparison,EStudioHome4UnitDisplay::Lattice,0,false,TEXT("hull"));TestFalse(TEXT("Calculated SI attitude cannot borrow a missing lattice map"),H.Series[4].Values[0].IsSet());return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4OriginalRecipeMetricContext,"Studio.Home4.Validation.OriginalTerminalAndFitMetricQualification",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4OriginalRecipeMetricContext::RunTest(const FString&)
{
    for(const auto* Id:{TEXT("sedimentation"),TEXT("vugts-barge")})
    {
        FStudioHome4ReferenceEvidence E;E.RecipeId=Id;E.RunId=FGuid::NewGuid();E.ActualSource=TEXT("identified original fixture");E.ReferenceSource=TEXT("identified reference fixture");E.OriginalRunSpec=StudioHome4Recipes::Find(Id)->Template;E.bReferenceOwnerVerified=true;E.ReferenceCitation=TEXT("explicit reference fixture");E.ReferenceSHA256=FString::ChrN(64,'b');
        for(const auto& Metric:StudioHome4RecipeGates::RequiredMetrics(Id))
        {
            FStudioHome4ReferenceSeries S;S.Id=Metric;S.Name=Metric;S.AbscissaName=TEXT("original x");S.AbscissaUnit=TEXT("s");S.AbscissaEpoch=TEXT("explicit original zero");S.Unit=TEXT("original fixture unit");S.Abscissae={0,1,2};S.Actual={1,1,1};S.Reference=S.Actual;S.Gate=StudioHome4Recipes::Compare(S.Actual,S.Reference,0,0,E.ReferenceSource);E.Series.Add(S);
        }
        TestEqual(TEXT("Named transient/unqualified fit cannot establish recipe gate"),E.RecipeGateStatus(),FString(TEXT("not_evaluated")));
        for(auto& S:E.Series)
        {S.ExtractionMethod=TEXT("identified original method");S.ExtractionWindowStart=0;S.ExtractionWindowEnd=2;S.ExtractionWindowUnit=TEXT("s");S.ExtractionEpoch=TEXT("explicit original zero");S.SamplingConvention=E.RecipeId==TEXT("sedimentation")?TEXT("steady-terminal"):TEXT("original-frequency-curve");if(E.RecipeId==TEXT("vugts-barge")){S.MotionMode=TEXT("heave");S.Normalization=TEXT("explicit original mass and damping convention");S.AbscissaName=TEXT("frequency");S.AbscissaUnit=TEXT("Hz");}}
        FStudioHome4ReferenceEvidence Parsed;FStudioHome4ReferenceExpectation X;X.RecipeId=Id;FString Error;
        if(!TestTrue(TEXT("Explicit original qualification parses and preserves fields"),StudioHome4Validation::Parse(StudioHome4Validation::SerializeEvidence(E),X,Parsed,Error))){AddError(Error);return false;}
        TestEqual(TEXT("Qualified original reference fixture can establish chosen gate"),Parsed.RecipeGateStatus(),FString(TEXT("passed")));TestEqual(TEXT("Original extraction method retained"),Parsed.Series[0].ExtractionMethod,FString(TEXT("identified original method")));
        if(E.RecipeId==TEXT("vugts-barge"))
        {
            auto WrongMotion=Parsed;for(auto& Series:WrongMotion.Series)Series.MotionMode=TEXT("roll");
            TestEqual(TEXT("Roll coefficients cannot qualify an original forced-heave run"),WrongMotion.RecipeGateStatus(),FString(TEXT("not_evaluated")));
        }
        Parsed.Series[0].ExtractionWindowEnd.Reset();TestFalse(TEXT("One-sided original extraction metadata cannot parse"),StudioHome4Validation::Parse(StudioHome4Validation::SerializeEvidence(Parsed),X,E,Error));
        TestEqual(TEXT("Public gate safely rejects one-sided original extraction metadata"),Parsed.RecipeGateStatus(),FString(TEXT("not_evaluated")));
        Parsed.Series[0].ExtractionWindowEnd=Parsed.Series[0].ExtractionWindowStart;
        TestEqual(TEXT("Public gate rejects a non-increasing original extraction window"),Parsed.RecipeGateStatus(),FString(TEXT("not_evaluated")));
    }
    return true;
}
#endif
