#include "StudioHome4Validation.h"
#include "SStudioHome4Validation.h"
#include "StudioModel.h"
#include "StudioHeadlessSlate.h"
#include "Misc/AutomationTest.h"
#include "Framework/Application/SlateApplication.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4ValidationTestFixtures
{
    // All numbers and source names below are explicitly unit test fixtures.
    // They are not published reference measurements and never ship as CFD data.
    FString FixtureJSON()
    {
        FStudioHome4ReferenceEvidence E; E.RecipeId=TEXT("rti-fakhari"); E.RunId=FGuid(1,2,3,4);
        E.ActualSource=TEXT("unit-test actual series");E.ReferenceSource=TEXT("unit-test reference oracle");
        FStudioHome4ReferenceSeries S;S.Id=TEXT("front");S.Name=TEXT("Unit test front");S.Unit=TEXT("L");S.AbscissaName=TEXT("t*");S.AbscissaUnit=TEXT("dimensionless");
        S.Abscissae={0,1,2};S.Actual={1,2,3};S.Reference={1,2,3};S.AbsoluteTolerance=.01;S.RelativeTolerance=.01;E.Series.Add(S);
        return StudioHome4Validation::SerializeEvidence(E);
    }
    FStudioHome4ReferenceExpectation Expect()
    {FStudioHome4ReferenceExpectation E;E.RecipeId=TEXT("rti-fakhari");E.RunId=FGuid(1,2,3,4);return E;}
    FString JSON(const TSharedRef<FJsonObject>& O)
    {FString S;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&S));return S;}
    TSharedPtr<FJsonObject> Object()
    {TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FixtureJSON()),O);return O;}
    FStudioHome4Spec Spec()
    {
        FStudioHome4Spec S;S.RecipeId=TEXT("rti-fakhari");S.LineageId=TEXT("unit-test lineage");
        S.Reference.LengthCells=100;S.Reference.SpeedCellsPerStep=.02;S.Reference.TimeSteps=1000;
        S.Fluids.Xi=4;S.Fluids.NuHeavy=.01;S.Fluids.NuLight=.002;S.Fluids.Mobility=.03;S.Fluids.Sigma=.0001;
        S.Fluids.Gravity=.00001;S.Fluids.RhoHeavy=3;S.Fluids.RhoLight=1;
        S.Lattice.Extents=FIntVector(100,50,25);S.Lattice.PadUp=2;
        S.Units.DxMeters=.001;S.Units.DtSeconds=.0001;S.Units.DensityReferenceKgM3=1000;
        S.Geometry.SinkCells=2;S.Geometry.BandCells=5;S.Geometry.CenterOfGravity=FVector(30,20,10);
        S.Multidomain.Levels=2;S.Multidomain.LevelCells={125000,10000};S.Multidomain.FinestMobility=.06;S.Multidomain.Margin=4;S.Multidomain.TauFloor=.51;
        S.Run.Steps=1000;S.Run.MeasureEvery=10;S.Run.SaveEvery=20;S.Run.VizEvery=50;S.Run.RestartEvery=100;S.Run.Tag=TEXT("unit_fixture");
        FStudioHome4Allocation A;A.Name=TEXT("unit-test allocation");A.Nodes=125000;A.Components=27;A.Buffers=2;S.Performance.Allocations.Add(A);
        S.Performance.MeasuredMLUPS=10;S.Performance.MeasurementSource=TEXT("unit-test measured rate oracle");return S;
    }
    class FScopedImport final : public IAutomationLatentCommand
    {
    public:
        explicit FScopedImport(FAutomationTestBase& InTest):Test(InTest)
        {
            Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-reference-import")/FGuid::NewGuid().ToString();
            IFileManager::Get().MakeDirectory(*Root,true);Path=Root/TEXT("unit-test-reference.json");
            Test.TestTrue(TEXT("Write identified unit test reference"),FFileHelper::SaveStringToFile(FixtureJSON(),*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
            Model=MakeShared<FStudioModel>(Root/TEXT("project"));Model->EditCase(TEXT("Unit test recipe"),[](auto& D){D.Home4=Spec();});
            State=MakeShared<FStudioHome4ValidationState>();Panel=SNew(SStudioHome4Validation).Model(Model).State(State);
            Test.TestTrue(TEXT("Begin bounded original reference import"),Panel->ImportPath(Path,Expect()));Started=FPlatformTime::Seconds();
        }
        ~FScopedImport(){Panel.Reset();Model.Reset();IFileManager::Get().DeleteDirectory(*Root,false,true);}
        bool Update() override
        {
            Panel->PollImport();
            if(FPlatformTime::Seconds()-Started>20){Test.AddError(TEXT("Reference import exceeded test deadline."));return true;}
            if(Panel->IsImporting())return false;
            if(Stage==0)
            {
                if(!Test.TestTrue(TEXT("Valid imported reference installed"),State->Evidence.IsValid()))return true;
                Test.TestEqual(TEXT("Original path retained"),State->Evidence->SourcePath,FPaths::ConvertRelativePathToFull(Path));
                Test.TestEqual(TEXT("Original byte hash retained"),State->Evidence->SourceSHA256.Len(),64);
                Preserved=State->Evidence;Panel->ImportPath(Root/TEXT("missing.json"),Expect());Stage=1;return false;
            }
            if(Stage==1)
            {
                Test.TestTrue(TEXT("Failure preserves prior identified evidence"),State->Evidence==Preserved);
                Test.TestFalse(TEXT("Selection cancel does not replace evidence"),Panel->ImportPath(TEXT(""),Expect()));
                Test.TestTrue(TEXT("Cancel preserves same evidence"),State->Evidence==Preserved);
                Panel->ImportPath(Path,Expect());Model->Project.Id=FGuid::NewGuid();Stage=2;return false;
            }
            Test.TestFalse(TEXT("Project switch rejects pending gate attachment"),State->Evidence.IsValid());
            Test.TestTrue(TEXT("Rejected completion names scope change"),State->Status.Contains(TEXT("changed while importing")));
            Test.TestEqual(TEXT("Shared evidence state belongs to new project"),State->ProjectId,Model->Project.Id);
            return true;
        }
    private:
        FAutomationTestBase& Test;FString Root,Path;int32 Stage=0;double Started=0;
        TSharedPtr<FStudioModel> Model;TSharedPtr<SStudioHome4Validation> Panel;
        TSharedPtr<FStudioHome4ValidationState> State;TSharedPtr<const FStudioHome4ReferenceEvidence> Preserved;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ReferenceParser, "Studio.Home4.Validation.StrictAlignedImportAndIdentity",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ReferenceParser::RunTest(const FString&)
{
    using namespace StudioHome4ValidationTestFixtures;
    FStudioHome4ReferenceEvidence E;FString Error;
    TestTrue(TEXT("Explicit aligned evidence accepted"),StudioHome4Validation::Parse(FixtureJSON(),Expect(),E,Error));
    TestEqual(TEXT("Evidence pass refers only to actual supplied data"),E.GateStatus(),FString(TEXT("passed")));
    TestEqual(TEXT("Original import hash retained"),E.SourceSHA256.Len(),64);
    const FString Hash=E.SourceSHA256;FStudioHome4ReferenceEvidence Same;
    StudioHome4Validation::Parse(FixtureJSON(),Expect(),Same,Error);TestEqual(TEXT("Identity hashes exact source bytes"),Same.SourceSHA256,Hash);
    auto Wrong=Expect();Wrong.RecipeId=TEXT("th01-hull");
    TestFalse(TEXT("Wrong recipe rejected transactionally"),StudioHome4Validation::Parse(FixtureJSON(),Wrong,E,Error));
    TestEqual(TEXT("Failure retains original evidence"),E.SourceSHA256,Hash);
    Wrong=Expect();Wrong.RunId=FGuid::NewGuid();TestFalse(TEXT("Wrong run rejected"),StudioHome4Validation::Parse(FixtureJSON(),Wrong,E,Error));
    Wrong=Expect();Wrong.ReferenceSource=TEXT("unmatched reference");TestFalse(TEXT("Explicit reference identity mismatch rejected"),StudioHome4Validation::Parse(FixtureJSON(),Wrong,E,Error));
    for(int32 Kind=0;Kind<9;++Kind)
    {
        auto O=Object();auto Series=O->GetArrayField(TEXT("series"))[0]->AsObject();
        if(Kind==0)Series->SetArrayField(TEXT("x"),{MakeShared<FJsonValueNumber>(1),MakeShared<FJsonValueNumber>(1),MakeShared<FJsonValueNumber>(2)});
        if(Kind==1)Series->SetArrayField(TEXT("actual"),{MakeShared<FJsonValueNumber>(1)});
        if(Kind==2)Series->RemoveField(TEXT("unit"));
        if(Kind==3)Series->RemoveField(TEXT("absolute_tolerance"));
        if(Kind==4)Series->SetNumberField(TEXT("relative_tolerance"),-1);
        if(Kind==5)Series->SetStringField(TEXT("absolute_tolerance"),TEXT("0.01"));
        if(Kind==6)O->SetArrayField(TEXT("series"),{MakeShared<FJsonValueObject>(Series),MakeShared<FJsonValueObject>(Series)});
        if(Kind==7)O->SetStringField(TEXT("run_id"),TEXT("invalid"));
        if(Kind==8)O->SetNumberField(TEXT("version"),2);
        TestFalse(TEXT("Malformed/mismatched original evidence rejected"),StudioHome4Validation::Parse(JSON(O.ToSharedRef()),Expect(),E,Error));
    }
    FString Duplicated=FixtureJSON();Duplicated=Duplicated.Replace(TEXT("\"version\""),TEXT("\"version\":1,\"version\""));
    TestFalse(TEXT("Duplicate decoded key rejected"),StudioHome4Validation::Parse(Duplicated,Expect(),E,Error));
    const FString Nonfinite=FixtureJSON().Replace(TEXT("\"version\""),TEXT("\"nonfinite\":1e999,\"version\""));
    TestFalse(TEXT("Nonfinite numeric token rejected before deserialization"),StudioHome4Validation::Parse(Nonfinite,Expect(),E,Error));
    auto Failing=Object();Failing->GetArrayField(TEXT("series"))[0]->AsObject()->SetArrayField(TEXT("actual"),
        {MakeShared<FJsonValueNumber>(1),MakeShared<FJsonValueNumber>(2),MakeShared<FJsonValueNumber>(4)});
    TestTrue(TEXT("Valid measured mismatch is imported"),StudioHome4Validation::Parse(JSON(Failing.ToSharedRef()),Expect(),E,Error));
    TestEqual(TEXT("Actual mismatch produces failed gate"),E.GateStatus(),FString(TEXT("failed")));
    TestEqual(TEXT("Maximum absolute error exact"),E.Series[0].Gate.MaximumAbsoluteError.Get(-1),1.);
    const auto Metadata=StudioHome4Validation::EvidenceMetadata(E);
    TestEqual(TEXT("Report metadata retains original source hash"),Metadata->GetStringField(TEXT("original_sha256")),E.SourceSHA256);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ReferenceOrder, "Studio.Home4.Validation.IdentifiedThreeRunObservedOrder",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ReferenceOrder::RunTest(const FString&)
{
    using namespace StudioHome4ValidationTestFixtures;
    FStudioHome4ReferenceEvidence E;FString Error;StudioHome4Validation::Parse(FixtureJSON(),Expect(),E,Error);
    E.OrderMetric=TEXT("front");E.OrderUnit=TEXT("L");E.OrderRuns={{FGuid(1,0,0,0),1,1.16},{FGuid(2,0,0,0),2,1.04},{FGuid(3,0,0,0),4,1.01}};
    FStudioHome4ReferenceEvidence Imported;
    TestTrue(TEXT("Three explicit scalar runs accepted"),StudioHome4Validation::Parse(StudioHome4Validation::SerializeEvidence(E),Expect(),Imported,Error));
    TestTrue(TEXT("Second-order scalar convergence measured"),FMath::IsNearlyEqual(Imported.ObservedOrder.Get(-1),2.,1e-12));
    E.OrderRuns[2].Refinement=5;
    TestFalse(TEXT("Unequal refinement ratio rejected"),StudioHome4Validation::Parse(StudioHome4Validation::SerializeEvidence(E),Expect(),Imported,Error));
    E.OrderRuns[2].Refinement=4;E.OrderRuns[2].Value=1.1;
    TestTrue(TEXT("Oscillatory measured sequence retained"),StudioHome4Validation::Parse(StudioHome4Validation::SerializeEvidence(E),Expect(),Imported,Error));
    TestFalse(TEXT("Oscillatory sequence cannot claim order"),Imported.ObservedOrder.IsSet());
    E.OrderUnit=TEXT("m");TestFalse(TEXT("Mismatched scalar unit rejected"),StudioHome4Validation::Parse(StudioHome4Validation::SerializeEvidence(E),Expect(),Imported,Error));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4RefinementPhysics, "Studio.Home4.Validation.RefinementPhysicsCountsCadencesAndCost",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4RefinementPhysics::RunTest(const FString&)
{
    using namespace StudioHome4ValidationTestFixtures;
    const auto Base=Spec();const auto D=StudioHome4Config::Derive(Base);TArray<FStudioHome4LadderRung> R;FString Error;
    TestTrue(TEXT("Sound acoustic refinement generated"),StudioHome4Recipes::Ladder(Base,{1,2,4},R,Error));if(R.Num()!=3)return false;
    const auto& Fine=R[2];const auto F=StudioHome4Config::Derive(Fine.Spec);
    for(const auto& Pair:TArray<TPair<TOptional<double>,TOptional<double>>>{{D.Reynolds,F.Reynolds},{D.Froude,F.Froude},{D.Bond,F.Bond},
        {D.Weber,F.Weber},{D.Capillary,F.Capillary},{D.Peclet,F.Peclet},{D.Cahn,F.Cahn},{D.Atwood,F.Atwood},{D.Mach,F.Mach}})
        TestTrue(TEXT("Supplied dimensionless physics conserved"),Pair.Key&&Pair.Value&&FMath::IsNearlyEqual(*Pair.Key,*Pair.Value,1e-9*FMath::Max(1.,FMath::Abs(*Pair.Key))));
    TestEqual(TEXT("Finest mobility scales with lattice length"),Fine.Spec.Multidomain.FinestMobility.Get(-1),.24);
    TestEqual(TEXT("Spatial MD margin scales in cells"),Fine.Spec.Multidomain.Margin.Get(-1),16.);
    TestEqual(TEXT("Body-length pad ratio retained"),Fine.Spec.Lattice.PadUp.Get(-1),2.);
    TestEqual(TEXT("Root-cell geometry band scales"),Fine.Spec.Geometry.BandCells.Get(-1),20.);
    TestEqual(TEXT("CoG root coordinate scales"),Fine.Spec.Geometry.CenterOfGravity->X,120.);
    TestEqual(TEXT("Per-level cell volume scales"),Fine.Spec.Multidomain.LevelCells[1],int64(640000));
    TestEqual(TEXT("Allocation nodes scale with volume"),Fine.Spec.Performance.Allocations[0].Nodes.Get(-1),int64(8000000));
    TestEqual(TEXT("Trace interval scales coherently with steps"),Fine.Spec.Run.MeasureEvery.Get(-1),int64(40));
    TestEqual(TEXT("Restart interval independent but coherent"),Fine.Spec.Run.RestartEvery.Get(-1),int64(400));
    TestTrue(TEXT("Known rate cost accounts volume and time"),R[0].EstimatedSeconds&&Fine.EstimatedSeconds&&FMath::IsNearlyEqual(*Fine.EstimatedSeconds/ *R[0].EstimatedSeconds,256.,1e-10));
    TestTrue(TEXT("Lineage retained"),Fine.Spec.LineageId==Base.LineageId);
    TestTrue(TEXT("Planned run identities distinct"),R[0].PlannedRunId!=Fine.PlannedRunId);
    auto Unmeasured=Base;Unmeasured.Performance.MeasurementSource.Empty();StudioHome4Recipes::Ladder(Unmeasured,{1,2},R,Error);
    TestFalse(TEXT("Unattributed hardware speed cannot provide cost"),R[1].EstimatedSeconds.IsSet());
    const int32 Preserved=R.Num();auto Unsupported=Base;Unsupported.Zones.Sponge=8;
    TestFalse(TEXT("Unknown driver zone units block unsafe scaling"),StudioHome4Recipes::Ladder(Unsupported,{1,2},R,Error));
    TestEqual(TEXT("Failure preserves prior ladder"),R.Num(),Preserved);
    Unsupported=Base;Unsupported.Reference.LengthCells=0;
    TestFalse(TEXT("Zero body length cannot generate ladder"),StudioHome4Recipes::Ladder(Unsupported,{1,2},R,Error));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4EvidenceBundles, "Studio.Home4.Validation.AtomicEvidenceAndDevelopmentQueueBundles",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4EvidenceBundles::RunTest(const FString&)
{
    using namespace StudioHome4ValidationTestFixtures;
    const FString Parent=FPaths::ProjectDir()/TEXT("tmp/debug/home4-validation-bundles")/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Parent,true);
    FStudioHome4ReferenceEvidence E;FString Error,Path;StudioHome4Validation::Parse(FixtureJSON(),Expect(),E,Error);
    TestTrue(TEXT("Publish identified evidence bundle"),StudioHome4Validation::ExportEvidence(Parent,TEXT("evidence"),E,Path,Error));
    TestTrue(TEXT("Aligned measurements exported"),IFileManager::Get().FileExists(*(Path/TEXT("aligned_reference.json"))));
    TestFalse(TEXT("Evidence cannot overwrite original bundle"),StudioHome4Validation::ExportEvidence(Parent,TEXT("evidence"),E,Path,Error));
    TArray<FStudioHome4LadderRung> R;StudioHome4Recipes::Ladder(Spec(),{1,2,4},R,Error);
    TestTrue(TEXT("Publish development queue specifications"),StudioHome4Validation::ExportLadder(Parent,TEXT("ladder"),R,Path,Error));
    FString Text;FFileHelper::LoadFileToString(Text,*(Path/TEXT("ladder.json")));TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);
    TestFalse(TEXT("Manifest never claims real launch"),O->GetBoolField(TEXT("launched")));
    TestEqual(TEXT("Three explicit rung specs"),O->GetArrayField(TEXT("development_queue")).Num(),3);
    TestTrue(TEXT("Full refined spec saved"),IFileManager::Get().FileExists(*(Path/TEXT("run_spec_r4.json"))));
    TestFalse(TEXT("Queue export cannot overwrite"),StudioHome4Validation::ExportLadder(Parent,TEXT("ladder"),R,Path,Error));
    R[1].Refinement=R[0].Refinement;
    TestFalse(TEXT("Duplicate filenames cannot overwrite a rung inside a staged bundle"),StudioHome4Validation::ExportLadder(Parent,TEXT("bad-ladder"),R,Path,Error));
    TestFalse(TEXT("Invalid bundle publishes nothing"),IFileManager::Get().DirectoryExists(*(Parent/TEXT("bad-ladder"))));
    IFileManager::Get().DeleteDirectory(*Parent,false,true);return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ValidationNative, "Studio.Home4.Validation.NativeEvidenceSelectionAndScope",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ValidationNative::RunTest(const FString&)
{
    using namespace StudioHome4ValidationTestFixtures;
    if(!TestTrue(TEXT("Slate initialized"),FSlateApplication::IsInitialized()))return false;
    auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-validation-widget")/FGuid::NewGuid().ToString());
    M->EditCase(TEXT("Unit test selected recipe"),[](auto& D){D.Home4=Spec();});const auto Camera=M->Project.Camera;const int32 Frame=M->SelectedFrame;
    auto State=MakeShared<FStudioHome4ValidationState>();auto Panel=SNew(SStudioHome4Validation).Model(M).State(State);
    FStudioHeadlessSlate UI(*this,Panel,FVector2D(700,1000));
    if(!UI.Inspect(TEXT("home4-validation-compact"),{TEXT("Home4ReferenceImport"),TEXT("Home4ReferenceRun"),TEXT("Home4ReferenceOverlay"),TEXT("Home4ReferenceGate"),TEXT("Home4ValidationBuildLadder")}))return false;
    TestTrue(TEXT("No supplied evidence begins not evaluated"),UI.Text(TEXT("Home4ReferenceGate")).Contains(TEXT("not_evaluated")));
    FStudioHome4ReferenceEvidence E;FString Error;StudioHome4Validation::Parse(FixtureJSON(),Expect(),E,Error);
    auto Other=E.Series[0];Other.Id=TEXT("other");Other.Name=TEXT("Second unit test metric");E.Series.Add(Other);State->Evidence=MakeShared<FStudioHome4ReferenceEvidence>(E);
    Panel->Tick(FGeometry(),0,0);
    if(!UI.Press(TEXT("Home4ReferenceSeries1")))return false;
    TestEqual(TEXT("Series selection retained locally"),State->SelectedSeries,1);
    TestTrue(TEXT("Original labels and units shown"),UI.Text(TEXT("Home4ReferenceGate")).Contains(TEXT("Second unit test metric [L]")));
    TestTrue(TEXT("Gate explicitly belongs to evidence run"),UI.Text(TEXT("Home4ReferenceGate")).Contains(TEXT("current draft has no transferred gate")));
    if(!UI.Press(TEXT("Home4ValidationBuildLadder")))return false;
    TestEqual(TEXT("Native builder creates three actual specifications"),State->Ladder.Num(),3);
    TestTrue(TEXT("Measured cost attribution visible"),UI.Text(TEXT("Home4LadderRung2")).Contains(TEXT("unit-test measured rate oracle")));
    TestTrue(TEXT("Camera unaffected by selection and ladder building"),StudioView::CameraEquals(M->Project.Camera,Camera));
    TestEqual(TEXT("Playback frame unaffected"),M->SelectedFrame,Frame);
    M->EditCase(TEXT("Change selected recipe"),[](auto& D){D.Home4->RecipeId=TEXT("th01-hull");});Panel->Tick(FGeometry(),0,0);
    TestFalse(TEXT("Recipe scope change clears prior gate"),State->Evidence.IsValid());
    TestTrue(TEXT("Scope change clears development queue"),State->Ladder.IsEmpty());
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ReferenceScopedImport,"Studio.Home4.Validation.TransactionalImportAndProjectScope",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ReferenceScopedImport::RunTest(const FString&)
{
    if(!TestTrue(TEXT("Slate initialized"),FSlateApplication::IsInitialized()))return false;
    ADD_LATENT_AUTOMATION_COMMAND(StudioHome4ValidationTestFixtures::FScopedImport(*this));return true;
}
#endif
