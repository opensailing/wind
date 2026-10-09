#include "StudioHome4ReferenceSources.h"
#include "StudioHome4Couette.h"
#include "SStudioHome4Validation.h"
#include "StudioHome4Reports.h"
#include "StudioModel.h"
#include "StudioHeadlessSlate.h"
#include "Framework/Application/SlateApplication.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4ReferenceSourceFixtures
{
    FStudioHome4Spec Spec()
    {
        auto S=StudioHome4Recipes::Find(TEXT("couette-spin"))->Template;
        S.Lattice.Extents=FIntVector(32,32,8);S.Reference.LengthCells=8;
        S.Reference.TimeSteps=100;S.Fluids.Xi=5;S.Run.Steps=400;
        return S;
    }
    FString JSON(const TSharedRef<FJsonObject>& O)
    {FString S;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&S));return S;}
    FString Source(bool Reference,FGuid Run,const FStudioHome4Spec& S,const FString& Epoch=TEXT("identified-original-zero"),bool Analytic=false)
    {
        auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("schema"),TEXT("LBMStudio.Home4Series"));O->SetNumberField(TEXT("version"),1);
        O->SetStringField(TEXT("recipe_id"),S.RecipeId);O->SetStringField(TEXT("run_id"),Run.ToString());O->SetStringField(TEXT("source_id"),Reference?TEXT("Identified test reference, not literature"):TEXT("Identified original test result"));
        O->SetStringField(TEXT("kind"),Reference?TEXT("reference"):TEXT("actual"));O->SetStringField(TEXT("epoch"),Epoch);
        if(!Reference)O->SetObjectField(TEXT("original_run_spec"),StudioHome4Config::ToJSON(S));
        else
        {
            auto V=MakeShared<FJsonObject>();V->SetBoolField(TEXT("owner_verified"),true);V->SetStringField(TEXT("citation"),TEXT("Explicit automation fixture; no literature authenticity claim"));
            V->SetStringField(TEXT("sha256"),FString::ChrN(64,'b'));O->SetObjectField(TEXT("reference_verification"),V);
        }
        auto V=MakeShared<FJsonObject>();V->SetStringField(TEXT("id"),TEXT("torque"));V->SetStringField(TEXT("name"),TEXT("Signed original inner torque"));
        V->SetStringField(TEXT("x_name"),TEXT("t*"));V->SetStringField(TEXT("x_unit"),TEXT("dimensionless"));V->SetStringField(TEXT("unit"),TEXT("N m"));
        TArray<TSharedPtr<FJsonValue>> X,Y;for(double T:{0.,1.,2.}){X.Add(MakeShared<FJsonValueNumber>(T));Y.Add(MakeShared<FJsonValueNumber>(-4.*UE_DOUBLE_PI*(1./.75)));}
        V->SetArrayField(TEXT("x"),X);if(!Analytic)V->SetArrayField(TEXT("values"),Y);O->SetArrayField(TEXT("series"),{MakeShared<FJsonValueObject>(V)});
        if(Analytic)
        {
            auto A=MakeShared<FJsonObject>();A->SetStringField(TEXT("domain"),TEXT("concentric_cylinders"));A->SetStringField(TEXT("units"),TEXT("SI"));
            A->SetStringField(TEXT("torque_convention"),TEXT("fluid_on_inner_total"));A->SetNumberField(TEXT("dynamic_viscosity"),1);A->SetNumberField(TEXT("inner_radius"),1);A->SetNumberField(TEXT("outer_radius"),2);
            A->SetNumberField(TEXT("inner_omega"),1);A->SetNumberField(TEXT("outer_omega"),0);A->SetNumberField(TEXT("axial_span"),1);A->SetBoolField(TEXT("steady_newtonian_no_end_effects"),true);O->SetObjectField(TEXT("analytic_couette"),A);
        }
        return JSON(O);
    }
    class FNativeSourceWorkflow final:public IAutomationLatentCommand
    {
    public:
        explicit FNativeSourceWorkflow(FAutomationTestBase& T):Test(T)
        {
            Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-native-reference-sources")/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Root,true);
            ActualJSON=Source(false,FGuid::NewGuid(),Spec());ReferenceJSON=Source(true,FGuid::NewGuid(),Spec());
            ActualPath=Root/TEXT("actual.json");ReferencePath=Root/TEXT("reference.json");
            Test.TestTrue(TEXT("Write independent original actual"),FFileHelper::SaveStringToFile(ActualJSON,*ActualPath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
            Test.TestTrue(TEXT("Write independent original reference"),FFileHelper::SaveStringToFile(ReferenceJSON,*ReferencePath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
            Model=MakeShared<FStudioModel>(Root/TEXT("project"));Model->Project.Draft.Home4=Spec();OriginalRunCount=Model->Project.Runs.Num();State=MakeShared<FStudioHome4ValidationState>();
            Panel=SNew(SStudioHome4Validation).Model(Model).State(State);UI=MakeUnique<FStudioHeadlessSlate>(Test,Panel.ToSharedRef(),FVector2D(960,2400));
            Test.TestTrue(TEXT("Actual selector is native and reachable"),UI->Exists(TEXT("Home4SelectActualSource")));
            Test.TestTrue(TEXT("Reference selector is native and reachable"),UI->Exists(TEXT("Home4SelectReferenceSource")));
            Test.TestTrue(TEXT("Begin actual bounded background import"),Panel->ImportSourcePath(ActualPath,false));Started=FPlatformTime::Seconds();
        }
        ~FNativeSourceWorkflow(){UI.Reset();Panel.Reset();Model.Reset();IFileManager::Get().DeleteDirectory(*Root,false,true);}
        bool Update()override
        {
            Panel->PollImport();UI->Layout();
            if(FPlatformTime::Seconds()-Started>20){Test.AddError(TEXT("Independent source workflow exceeded its bounded deadline."));return true;}
            if(Panel->IsImporting())return false;
            if(Stage==0)
            {Test.TestTrue(TEXT("Begin independently selected reference import"),Panel->ImportSourcePath(ReferencePath,true));Stage=1;return false;}
            if(Stage==1)
            {
                UI->Type(TEXT("Home4AlignmentAbsolute"),TEXT("0"));UI->Type(TEXT("Home4AlignmentRelative"),TEXT("0"));
                Test.TestTrue(TEXT("Native exact alignment action is routed"),UI->Press(TEXT("Home4AlignSources")));
                if(!State->Evidence){Test.AddError(State->Status);return true;}
                Test.TestTrue(TEXT("Both independent exact original byte sets retained"),!State->Evidence->ActualOriginalBytes.IsEmpty()&&!State->Evidence->ReferenceOriginalBytes.IsEmpty());
                Test.TestEqual(TEXT("Original comparison passes explicit zero tolerance"),State->Evidence->ComparisonStatus(),FString(TEXT("passed")));
                Test.TestEqual(TEXT("Original comparison is scoped separately from edited draft"),State->Evidence->AttachedCaseId,Model->Project.Draft.Id);
                Test.TestEqual(TEXT("Import/alignment never changes original run history"),Model->Project.Runs.Num(),OriginalRunCount);
                FString Out,Error;
                if(!Test.TestTrue(TEXT("Report includes both independently selected original sources"),StudioHome4Reports::Export(Root,TEXT("report"),Model->SnapshotProject(),nullptr,Out,Error,State->Evidence.Get())))
                {Test.AddError(Error);return true;}
                FString Retained;
                Test.TestTrue(TEXT("Read exact actual source from report"),FFileHelper::LoadFileToString(Retained,*(Out/TEXT("original-actual-series.json"))));
                Test.TestEqual(TEXT("Report preserves original actual bytes/content"),Retained,ActualJSON);
                Test.TestTrue(TEXT("Read exact reference source from report"),FFileHelper::LoadFileToString(Retained,*(Out/TEXT("original-reference-series.json"))));
                Test.TestEqual(TEXT("Report preserves original reference bytes/content"),Retained,ReferenceJSON);
                Prior=State->Evidence;Test.TestFalse(TEXT("Cancelled selector preserves prior evidence"),Panel->ImportSourcePath(TEXT(""),true));
                Test.TestTrue(TEXT("Cancel retains exact evidence"),State->Evidence==Prior);
                Test.TestTrue(TEXT("Importing actual into reference selector is attempted safely"),Panel->ImportSourcePath(ActualPath,true));Stage=2;return false;
            }
            if(Stage==2)
            {
                Test.TestTrue(TEXT("Wrong source kind retains prior evidence"),State->Evidence==Prior);
                Test.TestTrue(TEXT("Wrong source kind has explicit failure"),State->Status.Contains(TEXT("kind")));
                Test.TestTrue(TEXT("Begin import before scope switch"),Panel->ImportSourcePath(ActualPath,false));
                Model->Project.Id=FGuid::NewGuid();Stage=3;return false;
            }
            Test.TestFalse(TEXT("Foreign project clears previous gate attachment"),State->Evidence.IsValid());
            Test.TestTrue(TEXT("Pending original source cannot transfer across scope"),State->Status.Contains(TEXT("scope changed")));return true;
        }
    private:
        FAutomationTestBase& Test;FString Root,ActualPath,ReferencePath,ActualJSON,ReferenceJSON;double Started=0;int32 Stage=0,OriginalRunCount=0;
        TSharedPtr<FStudioModel> Model;TSharedPtr<FStudioHome4ValidationState> State;TSharedPtr<SStudioHome4Validation> Panel;
        TSharedPtr<const FStudioHome4ReferenceEvidence> Prior;TUniquePtr<FStudioHeadlessSlate> UI;
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4IndependentOriginalAlignmentTest,"Studio.Home4.Validation.IndependentOriginalSourcesExactAlignmentAndIntegrity",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4IndependentOriginalAlignmentTest::RunTest(const FString&)
{
    using namespace StudioHome4ReferenceSourceFixtures;FString Error;FStudioHome4SeriesSource A,R;const auto S=Spec();
    TestTrue(TEXT("Parse independent original actual"),StudioHome4ReferenceSources::Parse(Source(false,FGuid::NewGuid(),S),S.RecipeId,A,Error));
    TestTrue(TEXT("Parse independent original reference"),StudioHome4ReferenceSources::Parse(Source(true,FGuid::NewGuid(),S),S.RecipeId,R,Error));
    FStudioHome4ReferenceEvidence E;
    TestTrue(TEXT("Exact matching samples compare"),StudioHome4ReferenceSources::AlignExact(A,R,0,0,0.,2.,E,Error));
    TestEqual(TEXT("All original aligned samples retained"),E.Series[0].Actual.Num(),3);
    TestEqual(TEXT("Declared original epoch retained"),E.Series[0].AbscissaEpoch,A.Epoch);
    TestTrue(TEXT("Composed source bytes reproduce exact comparison"),StudioHome4Validation::VerifyOriginalBytes(E,Error));
    FStudioHome4ScalarRun Scalar;FStudioHome4ExtractionPolicy P;P.Metric=TEXT("torque");P.Unit=TEXT("N m");P.AbscissaUnit=TEXT("dimensionless");P.Epoch=A.Epoch;P.Method=TEXT("last");P.WindowStart=0;P.WindowEnd=2;
    TestTrue(TEXT("Original declared epoch confirms scalar extraction"),StudioHome4Validation::ExtractScalar(E,1,P,Scalar,Error));
    TestTrue(TEXT("Scalar records exact original epoch match"),Scalar.Extraction.bEpochConfirmedFromOriginal);
    P.Epoch=TEXT("different epoch");TestFalse(TEXT("A different epoch cannot be silently accepted"),StudioHome4Validation::ExtractScalar(E,1,P,Scalar,Error));
    auto Bad=R;Bad.Epoch=TEXT("different epoch");const auto Prior=E.RunId;
    TestFalse(TEXT("Different original epochs cannot align"),StudioHome4ReferenceSources::AlignExact(A,Bad,0,0,{}, {},E,Error));
    TestEqual(TEXT("Rejected alignment retains prior evidence"),E.RunId,Prior);
    Bad=A;Bad.Data.OriginalBytes[0]^=1;
    TestFalse(TEXT("Mutated original bytes reject alignment"),StudioHome4ReferenceSources::AlignExact(Bad,R,0,0,{}, {},E,Error));
    auto Mutated=E;Mutated.Series[0].Actual[0]+=1;
    TestFalse(TEXT("Edited composed values fail exact-source verification"),StudioHome4Validation::VerifyOriginalBytes(Mutated,Error));
    Mutated=E;Mutated.ReferenceOriginalBytes.Reset();
    TestFalse(TEXT("Derived comparison document alone cannot imply retained originals"),StudioHome4Validation::VerifyOriginalBytes(Mutated,Error));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4CouetteOriginalAnalyticTest,"Studio.Home4.Validation.ExplicitOriginalCouetteAnalyticTorque",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4CouetteOriginalAnalyticTest::RunTest(const FString&)
{
    using namespace StudioHome4ReferenceSourceFixtures;FString Error,Unit;double Torque=0;
    FStudioHome4CouetteInputs C;C.Domain=TEXT("concentric_cylinders");C.Units=TEXT("SI");C.TorqueConvention=TEXT("fluid_on_inner_total");C.DynamicViscosity=1;C.InnerRadius=1;C.OuterRadius=2;C.InnerOmega=1;C.OuterOmega=0;C.AxialSpan=1;C.bSteadyNewtonianNoEndEffects=true;
    TestTrue(TEXT("Explicit circular Couette side torque evaluates"),StudioHome4Couette::Torque(C,Torque,Unit,Error));
    TestTrue(TEXT("Torque has correct fluid-on-body sign and circular-wall factor"),FMath::IsNearlyEqual(Torque,-16.*UE_DOUBLE_PI/3.,1.e-12));
    TestEqual(TEXT("Total SI torque unit"),Unit,FString(TEXT("N m")));
    C.OuterOmega=1;TestTrue(TEXT("Solid body rotation has zero viscous torque"),StudioHome4Couette::Torque(C,Torque,Unit,Error));TestEqual(TEXT("Equal omega torque is exactly zero"),Torque,0.);
    C.Domain=TEXT("rectangular_box");TestFalse(TEXT("Finite box cannot borrow a concentric analytic wall correction"),StudioHome4Couette::Torque(C,Torque,Unit,Error));
    C.Domain=TEXT("unbounded_stationary_far_field");C.OuterOmega=0;C.OuterRadius.Reset();C.TorqueConvention=TEXT("fluid_on_inner_per_length");C.AxialSpan.Reset();
    TestTrue(TEXT("Explicit unbounded per-length reference evaluates"),StudioHome4Couette::Torque(C,Torque,Unit,Error));TestTrue(TEXT("Unbounded fluid-on-inner torque"),FMath::IsNearlyEqual(Torque,-4.*UE_DOUBLE_PI,1.e-12));
    C.bSteadyNewtonianNoEndEffects=false;TestFalse(TEXT("Missing original analytic applicability condition remains unavailable"),StudioHome4Couette::Torque(C,Torque,Unit,Error));
    FStudioHome4SeriesSource A,R;const auto S=Spec();FStudioHome4ReferenceEvidence E;
    TestTrue(TEXT("Original numerical fixture parses independently"),StudioHome4ReferenceSources::Parse(Source(false,FGuid::NewGuid(),S),S.RecipeId,A,Error));
    TestTrue(TEXT("Original identified analytic source computes a reference without replacing measured values"),StudioHome4ReferenceSources::Parse(Source(true,FGuid::NewGuid(),S,TEXT("identified-original-zero"),true),S.RecipeId,R,Error));
    TestTrue(TEXT("Analytic method and source are explicit"),R.Data.ReferenceMethod.Contains(TEXT("No finite-box")));
    TestTrue(TEXT("Original numerical torque compares against independently declared analytic source"),StudioHome4ReferenceSources::AlignExact(A,R,0,0,{}, {},E,Error));
    TestEqual(TEXT("Explicit analytic comparison passes the fixture only"),E.ComparisonStatus(),FString(TEXT("passed")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4NativeIndependentSourceTest,"Studio.Home4.Validation.NativeIndependentSourceSelectorsAlignmentReportAndScope",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4NativeIndependentSourceTest::RunTest(const FString&)
{
    if(!FSlateApplication::IsInitialized()){AddError(TEXT("Native Slate must be initialized."));return false;}
    ADD_LATENT_AUTOMATION_COMMAND(StudioHome4ReferenceSourceFixtures::FNativeSourceWorkflow(*this));return true;
}
#endif
