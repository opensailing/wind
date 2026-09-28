#include "StudioLattice.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr EAutomationTestFlags LatticeFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FString LatticeDirectory()
{const FString Dir=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/LatticeModel")/FGuid::NewGuid().ToString());IFileManager::Get().MakeDirectory(*Dir,true);return Dir;}
FStudioCaseDraft LatticeCube(bool Closed,FAutomationTestBase* Test)
{
    // Explicit geometric fixture for occupancy counts. It contains no CFD.
    FString OBJ=TEXT("# Lattice geometry test; no CFD\nv -0.6 -0.6 -0.6\nv 0.6 -0.6 -0.6\nv 0.6 0.6 -0.6\nv -0.6 0.6 -0.6\nv -0.6 -0.6 0.6\nv 0.6 -0.6 0.6\nv 0.6 0.6 0.6\nv -0.6 0.6 0.6\nf 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 4 8 7 3\nf 1 5 8 4\n");
    if(Closed)OBJ+=TEXT("f 2 3 7 6\n");
    const FString Path=LatticeDirectory()/TEXT("cube.obj");Test->TestTrue(TEXT("Write explicit geometry fixture"),FFileHelper::SaveStringToFile(OBJ,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);const auto Source=StudioMeshImport::Read(Path,Cancel);
    FStudioMeshImportOptions Options;Options.Name=Closed?TEXT("Closed cube"):TEXT("Open cube");Options.MetersPerUnit=1;
    FStudioGeometryAsset Asset;FString Error;FStudioCaseDraft Case;
    Test->TestTrue(TEXT("Original geometry imports"),StudioMeshImport::MakeAsset(Source,Options,Asset,Error));Case.Geometry.Add(Asset);
    Case.Domain.Min=FVector(-1.25);Case.Domain.Max=FVector(1.25);Case.Setup.LatticeResolution=FIntVector(5);return Case;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioLatticeLayoutTest,"Studio.Lattice.PhysicalCellCoverageAndSpacingDraft",LatticeFlags)
bool FStudioLatticeLayoutTest::RunTest(const FString&)
{
    FStudioDomain Domain;Domain.Min=FVector(-2,-1,-.5);Domain.Max=FVector(2,1,.5);FString Error;FStudioLatticeLayout Grid;
    TestTrue(TEXT("Physical layout accepts valid counts"),StudioLattice::Layout(Domain,FIntVector(4,2,1),Grid,Error));
    TestEqual(TEXT("Exact total cell count"),Grid.Cells,uint64(8));TestEqual(TEXT("Per-axis spacing in meters"),Grid.Spacing,FVector::OneVector);
    FVector Center;FBox Box;TestTrue(TEXT("First actual cell exists"),Grid.Cell(FIntVector::ZeroValue,Center,Box));
    TestEqual(TEXT("First physical center"),Center,FVector(-1.5,-.5,0));TestEqual(TEXT("First cell begins at domain minimum"),Box.Min,Domain.Min);
    Grid.Cell(FIntVector(3,1,0),Center,Box);TestEqual(TEXT("Last cell ends at domain maximum"),Box.Max,Domain.Max);const FVector Kept=Center;
    TestFalse(TEXT("Out-of-grid cell rejected"),Grid.Cell(FIntVector(4,1,0),Center,Box));TestEqual(TEXT("Failed lookup cannot replace physical coordinates"),Center,Kept);
    FStudioLatticeEdit Edit;Edit.Reset(FIntVector(4,2,1));FIntVector Built(7);TestFalse(TEXT("Saved counts are clean"),Edit.IsDirty());
    for(const TCHAR* Invalid:{TEXT("0"),TEXT("-1"),TEXT("1.5"),TEXT("1048577"),TEXT("nan"),TEXT(""),TEXT("20cells")})
    {
        Edit.Counts[1]=Invalid;TestFalse(TEXT("Invalid count rejected"),Edit.Build(Built));TestEqual(TEXT("Offending axis identified"),Edit.ErrorField,1);
        TestEqual(TEXT("Unapplied text retained"),Edit.Counts[1],FString(Invalid));TestEqual(TEXT("Failed draft cannot replace counts"),Built,FIntVector(7));
    }
    Edit.Reset(FIntVector(4,2,1));Edit.RequestedSpacing=TEXT("0.6");TestTrue(TEXT("Spacing derives covering counts"),Edit.UseSpacing(Domain));
    TestTrue(TEXT("Derived counts are an unapplied draft"),Edit.IsDirty());TestTrue(TEXT("Derived draft builds"),Edit.Build(Built));TestEqual(TEXT("Ceiling per axis"),Built,FIntVector(7,4,2));
    StudioLattice::Layout(Domain,Built,Grid,Error);TestTrue(TEXT("Every actual cell spacing is at most requested"),Grid.Spacing.GetMax()<=.6);
    Edit.RequestedSpacing=TEXT("1e-20");const FString Before=Edit.Counts[0];TestFalse(TEXT("Unrepresentable requested resolution rejected"),Edit.UseSpacing(Domain));TestEqual(TEXT("Failed spacing retains draft counts"),Edit.Counts[0],Before);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioLatticeSamplesTest,"Studio.Lattice.BoundedOriginalCellSampling",LatticeFlags)
bool FStudioLatticeSamplesTest::RunTest(const FString&)
{
    FStudioDomain Domain;FStudioLatticeLayout Grid;FString Error;FStudioLatticePreviewSettings Settings;Settings.Axis=-1;
    TestTrue(TEXT("Largest structurally supported layout"),StudioLattice::Layout(Domain,FIntVector(StudioLattice::MaximumResolution),Grid,Error));
    TestEqual(TEXT("Cell product is exact beyond floating-point integer range"),Grid.Cells,uint64(1)<<60);
    FStudioLatticeSamplePlan Plan;TestTrue(TEXT("Huge domain produces bounded sample plan"),StudioLattice::SamplePlan(Grid,Settings,Plan,Error));
    TestEqual(TEXT("Bounded preview cells"),Plan.Samples,32768);TestTrue(TEXT("Sampling explicitly disclosed"),Plan.bSampled);
    TestEqual(TEXT("First original index preserved"),Plan.Index(0),FIntVector::ZeroValue);TestEqual(TEXT("Last original index preserved"),Plan.Index(Plan.Samples-1),FIntVector(1048575));
    uint64 Previous=0;
    for(int32 I=0;I<Plan.Samples;++I)
    {
        const auto Index=Plan.Index(I);const uint64 Linear=uint64(Index.X)+uint64(Index.Y)*1048576ULL+uint64(Index.Z)*(1ULL<<40);
        if(I&&!TestTrue(TEXT("Original samples remain unique and ordered"),Linear>Previous))return false;Previous=Linear;
    }
    TestEqual(TEXT("Invalid sample returns invalid cell"),Plan.Index(Plan.Samples),FIntVector(INDEX_NONE));
    Settings.MaximumSamples=1;StudioLattice::SamplePlan(Grid,Settings,Plan,Error);TestEqual(TEXT("One sample uses a real middle cell"),Plan.Index(0),FIntVector(0,0,524288));
    StudioLattice::Layout(Domain,FIntVector(256,128,128),Grid,Error);Settings.Axis=2;Settings.Layer=127;Settings.MaximumSamples=32768;
    TestTrue(TEXT("Full actual layer is representable"),StudioLattice::SamplePlan(Grid,Settings,Plan,Error));TestFalse(TEXT("Full layer is not labelled sampled"),Plan.bSampled);
    TestEqual(TEXT("First layer cell"),Plan.Index(0),FIntVector(0,0,127));TestEqual(TEXT("Last layer cell"),Plan.Index(Plan.Samples-1),FIntVector(255,127,127));
    Settings.Layer=128;TestFalse(TEXT("Missing layer rejected"),StudioLattice::SamplePlan(Grid,Settings,Plan,Error));
    FStudioLatticeLayout Invalid;Invalid.Cells=1;Invalid.Bounds=FBox(Domain.Min,Domain.Max);Settings.Layer=0;
    TestFalse(TEXT("Incomplete layouts cannot divide by zero"),StudioLattice::SamplePlan(Invalid,Settings,Plan,Error));
    Invalid=Grid;Invalid.Cells+=1;TestFalse(TEXT("Inconsistent total count rejected"),StudioLattice::SamplePlan(Invalid,Settings,Plan,Error));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioLatticeBackendTest,"Studio.Lattice.DeclaredBackendRulesAndCaseTransactions",LatticeFlags)
bool FStudioLatticeBackendTest::RunTest(const FString&)
{
    FStudioModel M(LatticeDirectory());M.Pause();const auto Camera=M.Project.Camera;const int32 Frame=M.SelectedFrame;const uint64 Intent=M.RenderIntentRevision;
    M.EditCase(TEXT("Explicit test adapter identity"),[](auto& C){C.Setup.BackendId=TEXT("lattice-capability-test");});
    auto Check=StudioLattice::Validate(M.Project.Draft);TestFalse(TEXT("No invented backend memory"),Check.EstimatedBytes.IsSet());TestFalse(TEXT("Unknown grid conventions cannot be accepted"),Check.Compatible());
    FStudioLatticeCapabilities Backend;Backend.BackendId=M.Project.Draft.Setup.BackendId;
    TestFalse(TEXT("A backend name alone does not attest its rules"),StudioLattice::Validate(M.Project.Draft,&Backend).bBackendKnown);
    Backend.bLayoutRulesSupplied=true;Backend.bUniformSpacingRequired=true;Backend.MaximumCells=10000000;Backend.BytesPerCell=152;Backend.FixedBytes=1024; // Test-only declaration.
    TestFalse(TEXT("Explicit cubic-spacing rule detects anisotropic layout"),StudioLattice::Validate(M.Project.Draft,&Backend).Compatible());
    M.Project.Runs.Add(FStudioRunRecord::Capture(TEXT("Frozen before lattice edit"),M.Project.Draft,EStudioRunOrigin::ControlHarness));
    const FString Frozen=StudioCaseIO::Serialize(*M.Project.Runs.Last().GetConfiguration());const auto Before=M.Project.Draft.Setup.LatticeResolution;
    TestTrue(TEXT("Lattice resolution applies transactionally"),M.UpdateLatticeResolution(FIntVector(64)));
    Check=StudioLattice::Validate(M.Project.Draft,&Backend);TestTrue(TEXT("Declared rules accept compatible spacing"),Check.Compatible());
    TestTrue(TEXT("Declared memory estimate provided"),Check.EstimatedBytes.IsSet());if(Check.EstimatedBytes.IsSet())TestEqual(TEXT("Exact backend-declared byte estimate"),Check.EstimatedBytes.GetValue(),uint64(39846912));
    TestTrue(TEXT("Lattice edit is undoable"),M.UndoCase());TestEqual(TEXT("Original counts restored"),M.Project.Draft.Setup.LatticeResolution,Before);TestTrue(TEXT("Lattice edit is redoable"),M.RedoCase());
    const FString Saved=StudioCaseIO::Serialize(M.Project.Draft);TestFalse(TEXT("Invalid counts cannot alter case"),M.UpdateLatticeResolution(FIntVector(0)));TestEqual(TEXT("Rejected edit retains complete case"),StudioCaseIO::Serialize(M.Project.Draft),Saved);
    TestEqual(TEXT("Frozen run keeps earlier lattice"),StudioCaseIO::Serialize(*M.Project.Runs.Last().GetConfiguration()),Frozen);
    TestTrue(TEXT("Flow camera remains exact"),StudioView::CameraEquals(Camera,M.Project.Camera));TestEqual(TEXT("Source frame retained"),M.SelectedFrame,Frame);TestEqual(TEXT("No CFD requested"),M.RenderIntentRevision,Intent);
    FStudioProject Read;FString Error;TestTrue(TEXT("Saved lattice reopens"),StudioProjectIO::Parse(StudioProjectIO::Serialize(M.SnapshotProject()),Read,Error));TestEqual(TEXT("Reopened resolution exact"),Read.Draft.Setup.LatticeResolution,FIntVector(64));
    auto Large=M.Project.Draft;Large.Setup.LatticeResolution=FIntVector(1048576);Backend.BytesPerCell=16;Backend.FixedBytes=0;Backend.MaximumCells.Reset();
    Check=StudioLattice::Validate(Large,&Backend);TestFalse(TEXT("Overflow cannot wrap into a small memory estimate"),Check.EstimatedBytes.IsSet());TestFalse(TEXT("Overflow issue remains visible"),Check.Issues.IsEmpty());
    Backend.BackendId=TEXT("wrong-backend");TestFalse(TEXT("Another backend's rules cannot be used"),StudioLattice::Validate(Large,&Backend).bBackendKnown);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioLatticeOccupancyTest,"Studio.Lattice.OriginalClosedSurfaceOccupancy",LatticeFlags)
bool FStudioLatticeOccupancyTest::RunTest(const FString&)
{
    const auto Case=LatticeCube(true,this);auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    const auto Geometry=MakeShared<FStudioDomainGeometry,ESPMode::ThreadSafe>(StudioDomain::InspectGeometry(Case,Cancel));
    if(!TestTrue(TEXT("Closed original source verified"),Geometry->Complete()&&Geometry->Objects.Num()==1&&Geometry->Objects[0].bClosedSurface))return false;
    auto Progress=MakeShared<FStudioLatticePreviewProgress,ESPMode::ThreadSafe>();FStudioLatticePreviewSettings Settings;Settings.Axis=-1;
    const auto Result=StudioLattice::Preview(Case,Geometry,Settings,Cancel,Progress);
    TestTrue(TEXT("Full occupancy preview complete"),Result.Complete());TestEqual(TEXT("125 actual cells classified"),Result.Samples.Num(),125);
    TestEqual(TEXT("Geometric surface overlaps"),Result.Surface,26);TestEqual(TEXT("Known enclosed center cell"),Result.Inside,1);TestEqual(TEXT("Known outside cells"),Result.Outside,98);TestEqual(TEXT("No uncertain cells for this checked cube"),Result.Unknown,0);
    TestEqual(TEXT("Measured worker progress reaches actual count"),Progress->Completed.load(),125);TestEqual(TEXT("Worker reports completion"),Progress->Stage.load(),3);
    for(const auto& Cell:Result.Samples)if(Cell.Kind==EStudioLatticeCell::Surface)TestEqual(TEXT("Surface cell refers to original patch"),Cell.SurfacePatch,Case.Geometry[0].Patches[0].Id);
    const auto Again=StudioLattice::Preview(Case,Geometry,Settings,Cancel,Progress);TestEqual(TEXT("Same inputs retain identity"),Again.Key,Result.Key);
    TestEqual(TEXT("Deterministic count"),Again.Inside,Result.Inside);if(Again.Samples.Num()==Result.Samples.Num())for(int32 I=0;I<Again.Samples.Num();++I)
        if(!TestTrue(TEXT("Deterministic original cell classification"),Again.Samples[I].Index==Result.Samples[I].Index&&Again.Samples[I].Kind==Result.Samples[I].Kind))return false;
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioLatticeUnknownTest,"Studio.Lattice.OpenMissingStaleAndCancelledGeometry",LatticeFlags)
bool FStudioLatticeUnknownTest::RunTest(const FString&)
{
    auto Case=LatticeCube(false,this);auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    const auto Geometry=MakeShared<FStudioDomainGeometry,ESPMode::ThreadSafe>(StudioDomain::InspectGeometry(Case,Cancel));
    if(!TestTrue(TEXT("Open original source verified but not declared closed"),Geometry->Complete()&&Geometry->Objects.Num()==1&&!Geometry->Objects[0].bClosedSurface))return false;
    auto Progress=MakeShared<FStudioLatticePreviewProgress,ESPMode::ThreadSafe>();FStudioLatticePreviewSettings Settings;Settings.Axis=-1;
    auto Result=StudioLattice::Preview(Case,Geometry,Settings,Cancel,Progress);
    TestTrue(TEXT("Open surface can be previewed honestly"),Result.Complete());TestEqual(TEXT("Open shell invents no occupied interior"),Result.Inside,0);
    TestTrue(TEXT("Open shell retains unknown regions"),Result.Unknown>0);TestTrue(TEXT("Original surface overlaps still classified"),Result.Surface>0);
    auto Missing=MakeShared<FStudioDomainGeometry,ESPMode::ThreadSafe>(*Geometry);Missing->Objects[0].Error=TEXT("Missing source fixture");Missing->Preview.Reset();Missing->TriangleTargets.Reset();Missing->PatchBounds.Reset();
    Result=StudioLattice::Preview(Case,Missing,Settings,Cancel,Progress);TestTrue(TEXT("Missing geometry yields an explicit uncertain preview"),Result.Complete());TestEqual(TEXT("Missing geometry cannot become empty fluid"),Result.Unknown,125);
    Case.Geometry[0].Translation.X+=1;Result=StudioLattice::Preview(Case,Geometry,Settings,Cancel,Progress);TestFalse(TEXT("Stale source transform rejected"),Result.Complete());TestFalse(TEXT("Stale source gives recovery detail"),Result.Error.IsEmpty());
    *Cancel=true;Result=StudioLattice::Preview(Case,Geometry,Settings,Cancel,Progress);TestTrue(TEXT("Cancellation acknowledged"),Result.bCancelled);TestTrue(TEXT("Cancelled preview publishes no cells"),Result.Samples.IsEmpty());
    *Cancel=false;FStudioCaseDraft Empty;Empty.Setup.LatticeResolution=FIntVector(2);Result=StudioLattice::Preview(Empty,nullptr,Settings,Cancel,Progress);
    TestTrue(TEXT("Empty applied domain can preview its actual cells"),Result.Complete());TestEqual(TEXT("No geometry means outside geometry, not a CFD classification"),Result.Outside,8);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioLatticeTaskTest,"Studio.Lattice.WorkerOwnershipCancellationAndStaleResults",LatticeFlags)
bool FStudioLatticeTaskTest::RunTest(const FString&)
{
    FStudioModel M(LatticeDirectory());M.Pause();M.EditCase(TEXT("Empty geometry test"),[](auto& C){C.Geometry.Reset();C.Setup.LatticeResolution=FIntVector(64);});
    const auto Camera=M.Project.Camera;const int32 Frame=M.SelectedFrame;const uint64 Intent=M.RenderIntentRevision;
    auto Drain=[&]
    {
        const double Deadline=FPlatformTime::Seconds()+10.;
        while((M.IsBuildingLatticePreview()||M.IsReadingDomainGeometry())&&FPlatformTime::Seconds()<Deadline){M.Tick(0);FPlatformProcess::Sleep(.001f);}
        return TestFalse(TEXT("All authoring workers drained"),M.IsBuildingLatticePreview()||M.IsReadingDomainGeometry());
    };
    FStudioLatticePreviewSettings Settings;Settings.Axis=-1;Settings.MaximumSamples=2048;
    TestTrue(TEXT("First preview starts"),M.RequestLatticePreview(Settings));
    TestFalse(TEXT("A second worker cannot overlap the retained future"),M.RequestLatticePreview(Settings));
    M.CancelLatticePreview();if(!Drain())return false;
    TestFalse(TEXT("Cancelled completion cannot publish"),M.LatticePreview.IsValid());
    TestTrue(TEXT("Worker can restart after draining"),M.RequestLatticePreview(Settings));
    TestTrue(TEXT("Applied count change succeeds during computation"),M.UpdateLatticeResolution(FIntVector(32)));
    if(!Drain())return false;TestFalse(TEXT("Obsolete count result discarded"),M.LatticePreview.IsValid());
    TestTrue(TEXT("Current preview starts"),M.RequestLatticePreview(Settings));if(!Drain())return false;
    if(!TestTrue(TEXT("Current immutable result publishes"),M.LatticePreview&&M.LatticePreview->Complete()))return false;
    TestEqual(TEXT("Published layout uses current counts"),M.LatticePreview->Plan.Layout.Resolution,FIntVector(32));
    TestEqual(TEXT("Bounded result count"),M.LatticePreview->Samples.Num(),2048);
    const auto Retained=M.LatticePreview;
    M.EditCase(TEXT("Unrelated face label"),[](auto& C){C.Domain.FaceNames[0]=TEXT("Inlet label");});
    TestTrue(TEXT("Unrelated metadata keeps same occupancy result"),M.LatticePreview==Retained);
    TestTrue(TEXT("Flow camera remains exact"),StudioView::CameraEquals(Camera,M.Project.Camera));
    TestEqual(TEXT("Source frame remains exact"),M.SelectedFrame,Frame);
    TestEqual(TEXT("Authoring requests no CFD render"),M.RenderIntentRevision,Intent);
    TestTrue(TEXT("Next preview starts"),M.RequestLatticePreview(Settings));
    M.NewProject(TEXT("Replacement case"));M.Pause();if(!Drain())return false;
    TestFalse(TEXT("Previous project cannot publish into replacement"),M.LatticePreview.IsValid());
    TestTrue(TEXT("Geometry recheck starts"),M.RequestDomainGeometry());if(!Drain())return false;
    Settings.Axis=2;Settings.Layer=0;TestTrue(TEXT("Replacement project can preview"),M.RequestLatticePreview(Settings));if(!Drain())return false;
    TestTrue(TEXT("Replacement owns a valid preview"),M.LatticePreview&&M.LatticePreview->Complete());
    // NewProject intentionally resets playback; verify preview alone leaves it unchanged.
    const int32 ReplacementFrame=M.SelectedFrame;const uint64 ReplacementIntent=M.RenderIntentRevision;
    TestTrue(TEXT("Preview can be explicitly recomputed"),M.RequestLatticePreview(Settings));if(!Drain())return false;
    TestEqual(TEXT("Preview leaves source frame unchanged"),M.SelectedFrame,ReplacementFrame);
    TestEqual(TEXT("Preview requests no CFD render"),M.RenderIntentRevision,ReplacementIntent);
    M.NewProject(TEXT("Clear completed authoring results"));
    TestFalse(TEXT("New project immediately clears completed occupancy"),M.LatticePreview.IsValid());
    TestFalse(TEXT("New project immediately releases checked geometry"),M.DomainGeometry.IsValid());
    TestTrue(TEXT("Geometry check invalidates occupancy"),M.RequestDomainGeometry());
    TestFalse(TEXT("Previous occupancy cleared before source recheck"),M.LatticePreview.IsValid());M.CancelDomainGeometry();Drain();
    return true;
}
#endif
