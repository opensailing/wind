#include "SStudioHome4Panel.h"
#include "SStudioHome4Sizing.h"
#include "SStudioHome4Timeline.h"
#include "SStudioHome4SpatialOverlay.h"
#include "SStudioHome4Allocations.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "StudioHome4Readouts.h"
#include "StudioHeadlessSlate.h"
#include "StudioModel.h"
#include "StudioHome4Reports.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace
{
bool Home4Widget(FAutomationTestBase& Test,int32 Width)
{
    auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-ui")/FGuid::NewGuid().ToString());
    auto Session=MakeShared<FStudioHome4Session>(M);Session->ApplyRecipe(TEXT("th01-hull"));
    const auto Camera=M->Project.Camera;const auto Frame=M->SelectedFrame;
    auto Panel=SNew(SStudioHome4Panel).Model(M).Session(Session).Page(TEXT("Fluids & Interface"));
    FStudioHeadlessSlate UI(Test,Panel,FVector2D(Width,740));
    if(!UI.Type(TEXT("reference.speedCellsPerStep"),TEXT("0.04"))||!UI.Type(TEXT("reference.mach"),TEXT("0.9"))||!UI.Press(TEXT("Home4Apply")))return false;
    Test.TestEqual(TEXT("Applied source-independent lattice speed"),M->Project.Draft.Home4->Reference.SpeedCellsPerStep.Get(0),.04);
    Test.TestFalse(TEXT("Numerically infeasible draft still saved for correction"),Session->IsDirty());
    if(!UI.Type(TEXT("reference.mach"),TEXT("nan"))||!UI.Press(TEXT("Home4Apply")))return false;
    Test.TestTrue(TEXT("Invalid draft remains"),Session->IsDirty());Test.TestEqual(TEXT("Invalid draft text retained"),UI.Text(TEXT("reference.mach")),FString(TEXT("nan")));
    if(!UI.Press(TEXT("Home4Revert")))return false;
    Test.TestEqual(TEXT("Revert restores exact applied text"),UI.Text(TEXT("reference.mach")),FString(TEXT("0.90000000000000002")));
    Test.TestTrue(TEXT("Camera independent"),StudioView::CameraEquals(Camera,M->Project.Camera));Test.TestEqual(TEXT("Source frame independent"),M->SelectedFrame,Frame);
    return UI.Inspect(FString::Printf(TEXT("home4-form-%d"),Width),{TEXT("reference.speedCellsPerStep"),TEXT("reference.mach"),TEXT("Home4Apply"),TEXT("Home4Revert"),TEXT("Home4Status"),TEXT("Home4Feasibility")});
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4PanelCompact,"Studio.HeadlessUI.Home4.Compact",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4PanelCompact::RunTest(const FString&){return Home4Widget(*this,1040);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4PanelWide,"Studio.HeadlessUI.Home4.Wide",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4PanelWide::RunTest(const FString&){return Home4Widget(*this,1280);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ReportTest,"Studio.Home4.Reports.AtomicBundleAndEscaping",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ReportTest::RunTest(const FString&)
{
    const FString Root=FPaths::ProjectDir()/TEXT("tmp/debug/home4-reports")/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Root,true);
    auto M=MakeShared<FStudioModel>(Root/TEXT("model"));FStudioHome4Session Session(M);Session.ApplyRecipe(TEXT("th01-hull"));
    M->Project.Name=TEXT("Hull_1 & 50% {report}");FString Path,Error;
    TestTrue(TEXT("New atomic report"),StudioHome4Reports::Export(Root,TEXT("test_viz"),M->SnapshotProject(),nullptr,Path,Error));
    FString Text;TestTrue(TEXT("Latex file written"),FFileHelper::LoadFileToString(Text,*(Path/TEXT("report.tex"))));
    TestTrue(TEXT("LaTeX escaped"),Text.Contains(TEXT("Hull\\_1 \\& 50\\% \\{report\\}")));
    TestTrue(TEXT("No invented gate pass"),Text.Contains(TEXT("not evaluated")));
    TestFalse(TEXT("Published report cannot overwrite"),StudioHome4Reports::Export(Root,TEXT("test_viz"),M->SnapshotProject(),nullptr,Path,Error));
    TestFalse(TEXT("Traversal cannot export"),StudioHome4Reports::Export(Root,TEXT("../escape"),M->SnapshotProject(),nullptr,Path,Error));
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4SizingTest,"Studio.Home4.Readouts.CoupledSizingAndSourceTime",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4SizingTest::RunTest(const FString&)
{
    auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-sizing")/FGuid::NewGuid().ToString());
    auto Session=MakeShared<FStudioHome4Session>(M);Session->ApplyRecipe(TEXT("th01-hull"));
    Session->Set(TEXT("reference.lengthCells"),TEXT("128"));Session->Set(TEXT("reference.speedCellsPerStep"),TEXT("0.03"));
    Session->Set(TEXT("reference.reynolds"),TEXT("100"));Session->Set(TEXT("fluids.nuHeavy"),TEXT("0.0384"));
    auto UI=SNew(SStudioHome4Sizing).Session(Session);FStudioHome4Spec S;FString Error;
    const auto Before=StudioHome4Config::Serialize(M->Project.Draft.Home4.GetValue());
    TestTrue(TEXT("Mach adjustment accepted"),UI->Adjust(1,.06));TestTrue(TEXT("Draft valid"),Session->Build(S,Error));
    TestTrue(TEXT("Reynolds held by viscosity"),FMath::IsNearlyEqual(StudioHome4Config::Derive(S).Reynolds.Get(0),100.,1e-8));
    TestTrue(TEXT("Tau adjustment accepted"),UI->Adjust(2,.65));Session->Build(S,Error);
    TestTrue(TEXT("Heavy relaxation target reached"),FMath::IsNearlyEqual(StudioHome4Config::Derive(S).TauHeavy.Get(0),.65,1e-8));
    TestEqual(TEXT("Applied project remains unchanged until Apply"),StudioHome4Config::Serialize(M->Project.Draft.Home4.GetValue()),Before);
    TestFalse(TEXT("Invalid sizing rejected"),UI->Adjust(1,.9));
    FStudioHome4Spec Map;Map.Units.DxMeters=.01;Map.Units.DtSeconds=.1;Map.Reference.TimeSteps=10.;
    TestEqual(TEXT("Physical time retains source origin, t* uses step origin"),StudioHome4Readouts::Time(20,&Map,7.),FString(TEXT("Step 20 · 7 s · t* 2 1")));
    TestEqual(TEXT("No map means no invented lattice speed"),StudioHome4Readouts::Value(1,EStudioHome4Quantity::Velocity,EStudioHome4UnitDisplay::Physical,EStudioHome4UnitDisplay::Lattice,nullptr),FString(TEXT("Not supplied · source map required")));
    TestEqual(TEXT("Normalised pressure remains dimensionless"),StudioHome4Readouts::Value(.3,EStudioHome4Quantity::Dimensionless,EStudioHome4UnitDisplay::Lattice,EStudioHome4UnitDisplay::Physical,&Map),FString(TEXT("0.3 1")));
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4AllocationsTest,"Studio.Home4.Readouts.AllocationDraftApplyRevert",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4AllocationsTest::RunTest(const FString&)
{
    auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-allocation")/FGuid::NewGuid().ToString());
    auto Session=MakeShared<FStudioHome4Session>(M);Session->ApplyRecipe(TEXT("th01-hull"));
    Session->Set(TEXT("lattice.extents"),TEXT("16, 8, 4"));Session->AddAllocation();
    Session->SetAllocation(0,0,TEXT("Hydro D3Q27"));Session->SetAllocation(0,2,TEXT("27"));Session->SetAllocation(0,4,TEXT("2"));
    FStudioHome4Spec S;FString Error;
    TestTrue(TEXT("Complete inventory builds"),Session->Build(S,Error));
    TestEqual(TEXT("Explicit allocation memory"),StudioHome4Config::Derive(S).AllocationBytes.Get(0),uint64(16*8*4*27*4*2));
    Session->SetAllocation(0,1,TEXT(""));
    TestTrue(TEXT("Unknown allocation count is retained"),Session->Build(S,Error));
    TestFalse(TEXT("Missing count never invents memory"),StudioHome4Config::Derive(S).AllocationBytes.IsSet());
    Session->SetAllocation(0,1,TEXT("512"));
    TestTrue(TEXT("Allocation changes are pending"),Session->IsDirty()&&M->Project.Draft.Home4->Performance.Allocations.IsEmpty());
    TestTrue(TEXT("Inventory applies with other retained settings"),Session->Apply());
    Session->SetAllocation(0,3,TEXT("nan"));TestFalse(TEXT("Invalid inventory cannot replace applied case"),Session->Apply());
    TestEqual(TEXT("Invalid original text retained"),Session->AllocationValue(0,3),FString(TEXT("nan")));
    Session->Revert();TestEqual(TEXT("Revert restores applied bytes"),Session->AllocationValue(0,3),FString(TEXT("4")));
    Session->RemoveAllocation(0);TestTrue(TEXT("Removing inventory is pending"),Session->IsDirty());Session->Revert();
    TestEqual(TEXT("Revert restores removed original allocation"),Session->AllocationCount(),1);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4TimelineTest,"Studio.Home4.Readouts.FourOutputTimelineSourceIdentity",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4TimelineTest::RunTest(const FString&)
{
    // Artificial event records test navigation only; no synthetic CFD is installed.
    FStudioHome4TelemetryStream Stream;FStudioHome4Source Source{FGuid::NewGuid(),TEXT("timeline-test")};Stream.BeginRun(Source);
    const FTCHARToUTF8 Bytes(TEXT("{\"step\":100,\"trace\":\"trace.csv\",\"slice\":\"slice.npz\",\"snapshot\":\"frame100.npz\",\"checkpoint\":\"restart.npz\"}\n{\"step\":150,\"snapshot\":\"frame150.npz\"}\n{\"step\":999,\"snapshot\":\"outside.npz\"}\n"));
    TestEqual(TEXT("Original output events accepted"),Stream.AppendBytes(reinterpret_cast<const uint8*>(Bytes.Get()),Bytes.Length()).Accepted,3);
    TArray<FStudioFrame> Frames;FStudioFrame First;First.Index=100;Frames.Add(First);First.Index=200;Frames.Add(First);
    TOptional<FGuid> Run=Source.RunId;int32 ReviewCount=0,Ordinal=INDEX_NONE;
    auto Widget=SNew(SStudioHome4Timeline).Telemetry([&]{return &Stream;}).SourceRun([&]{return Run;}).Frames([&]{return &Frames;})
        .Review([&](int32 I){++ReviewCount;Ordinal=I;});
    auto Key=[&](FKey K){return Widget->OnKeyDown(FGeometry(),FKeyEvent(K,FModifierKeysState(),0,false,0,0));};
    Key(EKeys::Home);Key(EKeys::Right);TestEqual(TEXT("Trace and slice do not change replay"),ReviewCount,0);
    Key(EKeys::Right);TestEqual(TEXT("Exact source step opens original ordinal"),Ordinal,0);TestEqual(TEXT("One matching visualization reviewed"),ReviewCount,1);
    Key(EKeys::Right);Key(EKeys::End);TestEqual(TEXT("Restart and unmatched original step do not substitute a nearby frame"),ReviewCount,1);
    Run=FGuid::NewGuid();TestFalse(TEXT("Foreign run events are not navigable"),Key(EKeys::Home).IsEventHandled());
    Run.Reset();TestFalse(TEXT("Missing source identity cannot move replay"),Key(EKeys::Right).IsEventHandled());
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4SpatialOverlayTest,"Studio.Home4.Readouts.SpatialOverlayOriginalAffineAndRun",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4SpatialOverlayTest::RunTest(const FString&)
{
    FStudioHome4SpatialEvidence Evidence;Evidence.RunId=FGuid::NewGuid();Evidence.CoordinateUnit=TEXT("lattice");
    FStudioHome4SpatialPatch Patch;Patch.Id=TEXT("fine");Patch.Level=2;Patch.Origin=FVector(10,20,30);Patch.Spacing=FVector(.25,.5,.75);Patch.Extents=FIntVector(3,5,7);Evidence.Patches.Add(Patch);
    FStudioHome4SpatialZone Zone;Zone.Id=TEXT("beach");Zone.Kind=TEXT("velocity relaxation");Zone.Minimum=FVector(0,1,2);Zone.Maximum=FVector(4,5,6);Evidence.Zones.Add(Zone);
    FStudioPointStructuredGrid Grid;Grid.SourceRunId=Evidence.RunId.ToString();Grid.Units.DxMeters=.01;TArray<FStudioHome4SpatialRegion> Regions;FString Error;
    TestTrue(TEXT("Original source map places separate regions"),StudioHome4SpatialView::Regions(Evidence,Grid,Regions,Error));
    TestEqual(TEXT("Both original regions retained separately"),Regions.Num(),2);
    TestTrue(TEXT("Original XYZ maps to scene XZY in metres"),Regions[0].Bounds.Min.Equals(FVector(.1,.3,.2),1e-12));
    TestTrue(TEXT("Fine spacing never substituted with root spacing"),Regions[0].Bounds.Max.Equals(FVector(.105,.345,.22),1e-12));
    Grid.SourceRunId=FGuid::NewGuid().ToString();TestFalse(TEXT("Foreign run hides annotations"),StudioHome4SpatialView::Regions(Evidence,Grid,Regions,Error));TestTrue(TEXT("No stale annotations after rejection"),Regions.IsEmpty());
    Grid.SourceRunId=Evidence.RunId.ToString();Grid.Units.DxMeters.Reset();TestFalse(TEXT("Missing original conversion hides annotations"),StudioHome4SpatialView::Regions(Evidence,Grid,Regions,Error));
    Evidence.CoordinateUnit=TEXT("m");TestTrue(TEXT("Physical source does not require guessed lattice map"),StudioHome4SpatialView::Regions(Evidence,Grid,Regions,Error));
    Evidence.CoordinateUnit=TEXT("unspecified driver cells");TestFalse(TEXT("Unknown convention stays unavailable"),StudioHome4SpatialView::Regions(Evidence,Grid,Regions,Error));return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4AllocationLayoutTest,"Studio.Home4.Readouts.AllocationCompactAndLiveEstimate",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4AllocationLayoutTest::RunTest(const FString&)
{
    auto M=MakeShared<FStudioModel>(FPaths::ProjectDir()/TEXT("tmp/debug/home4-allocation-ui")/FGuid::NewGuid().ToString());
    auto Session=MakeShared<FStudioHome4Session>(M);Session->ApplyRecipe(TEXT("th01-hull"));Session->Set(TEXT("lattice.extents"),TEXT("128, 128, 128"));Session->AddAllocation();
    Session->SetAllocation(0,0,TEXT("Population"));Session->SetAllocation(0,2,TEXT("27"));
    {
        auto Table=SNew(SStudioHome4Allocations).Session(Session);FStudioHeadlessSlate UI(*this,Table,FVector2D(610,300));
        if(!UI.Type(TEXT("Home4Allocation0.4"),TEXT("2"))||!UI.Inspect(TEXT("home4-allocation-compact"),{TEXT("Home4Allocation0.0"),TEXT("Home4Allocation0.1"),TEXT("Home4Allocation0.2"),TEXT("Home4Allocation0.3"),TEXT("Home4Allocation0.4")}))return false;
    }
    auto Panel=SNew(SStudioHome4Panel).Model(M).Session(Session).Page(TEXT("Lattice"));FStudioHeadlessSlate UI(*this,Panel,FVector2D(1040,740));UI.Layout();
    const auto Before=UI.Text(TEXT("Home4FeasibilitySummary"));Session->SetAllocation(0,4,TEXT("4"));UI.Layout();
    TestTrue(TEXT("Allocation edits refresh live estimate before Apply"),UI.Text(TEXT("Home4FeasibilitySummary"))!=Before);
    Session->SetAllocation(0,4,TEXT("nan"));UI.Layout();TestTrue(TEXT("Invalid allocation suppresses stale estimate"),UI.Text(TEXT("Home4FeasibilitySummary")).Contains(TEXT("Correct the draft")));
    return !HasAnyErrors();
}
#endif
