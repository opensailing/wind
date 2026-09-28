#include "StudioStreamlines.h"
#include "StudioModel.h"
#include "StudioPlanarSurface.h"
#include "StudioSurfaceReconstruction.h"
#include "StudioVolume.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto StreamTestFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FStudioSeedObject StreamSeed(const FStudioFieldIdentity& I)
{FStudioSeedObject S;S.Name=TEXT("Inlet seeds");S.Source={I.Dataset,I.MetadataSHA256,I.PayloadSHA256};return S;}
// Analytic ODE inputs exercise mathematics only; they are never product CFD data.
class FStreamAnalyticField final : public IStudioField
{
public:
    bool bCircular=false,bZero=false,bMissingScalar=false,bGap=false;
    mutable int32 Samples=0;
    FStudioLoadCancellation CancelOnSample;
    bool IsValid() const override{return true;}
    TOptional<FStudioFieldIdentity> Identity() const override
    {FStudioFieldIdentity I;I.Dataset=TEXT("analytic-numerical-test");I.MetadataSHA256=FString::ChrN(64,'a');
        I.Interpolation=EStudioFieldInterpolation::SourceTriangles;I.SpatialDimensions=2;return I;}
    bool Sample(const FVector& P,FStudioFieldValue& Out) const override
    {
        if(++Samples==20&&CancelOnSample)CancelOnSample->store(true);
        Out.Velocity=bZero?FVector::ZeroVector:bCircular?FVector(-P.Z,0,P.X):FVector(3,0,4);
        Out.Pressure=2*P.X-3*P.Z;return true;
    }
    bool SampleScalar(const FVector& P,const FString& Id,double& Out) const override
    {if(bMissingScalar)return false;return IStudioField::SampleScalar(P,Id,Out);}
    bool SupportsSegment(const FVector& A,const FVector& B,const FStudioLoadCancellation&) const override
    {return !bGap||FMath::Max(A.X,B.X)<.045||FMath::Min(A.X,B.X)>.046;}
    bool IsSolid(const FVector&) const override{return false;}
    const TArray<FVector2D>& Boundary() const override{static TArray<FVector2D> Empty;return Empty;}
    const TArray<FIntVector>& BoundaryTriangles() const override{static TArray<FIntVector> Empty;return Empty;}
};
FString StreamJSON(const TSharedRef<FJsonObject>& O)
{FString Text;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&Text));return Text;}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioStreamCoverage,"Studio.Streamlines.ContinuousMeshCoverage",StreamTestFlags)
bool FStudioStreamCoverage::RunTest(const FString&)
{
    auto Points=MakeShared<FStudioPointGeometry,ESPMode::ThreadSafe>();
    Points->Positions={{-2,-2,0},{2,-2,0},{2,2,0},{-2,2,0},{-.5,-.5,0},{.5,-.5,0},{.5,.5,0},{-.5,.5,0}};
    const auto R=FStudioPlanarSurface::Create(Points,{{0,1,5},{0,5,4},{1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}});
    if(!TestTrue(*R.Error,R.Surface.IsValid()))return false;
    TestTrue(TEXT("Whole covered segment crosses multiple triangle edges"),R.Surface->SupportsSegment({-1.8,1},{1.8,1}));
    TestTrue(TEXT("Reverse traversal gives identical support"),R.Surface->SupportsSegment({1.8,1},{-1.8,1}));
    TestTrue(TEXT("Exact shared edge stays covered"),R.Surface->SupportsSegment({-2,-2},{.5,-.5}));
    TestFalse(TEXT("Valid endpoints cannot bridge an inner hole"),R.Surface->SupportsSegment({-1,0},{1,0}));
    TestFalse(TEXT("Valid start cannot escape outer mesh"),R.Surface->SupportsSegment({1,0},{3,0}));
    // A very narrow interior gap falls between endpoint and midpoint samples.
    auto GapPoints=MakeShared<FStudioPointGeometry,ESPMode::ThreadSafe>();
    GapPoints->Positions={{0,0,0},{.234,0,0},{.234,1,0},{0,1,0},{.23400001,0,0},{1,0,0},{1,1,0},{.23400001,1,0}};
    const auto Gap=FStudioPlanarSurface::Create(GapPoints,{{0,1,2},{0,2,3},{4,5,6},{4,6,7}});
    if(!TestTrue(TEXT("Numerical gap geometry built"),Gap.Surface.IsValid()))return false;
    FStudioSurfaceLocation Location;
    for(double X:{.1,.5,.9})TestTrue(TEXT("Sparse point checks miss the gap"),Gap.Surface->Locate({X,.5},Location));
    TestFalse(TEXT("Complete support detects narrow unsampled gap"),Gap.Surface->SupportsSegment({.1,.5},{.9,.5}));
    const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
    TestFalse(TEXT("Cancelled geometry check cannot claim coverage"),R.Surface->SupportsSegment({-1,1},{1,1},Cancel));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioStreamSeeds,"Studio.Streamlines.DeterministicSeedGeometry",StreamTestFlags)
bool FStudioStreamSeeds::RunTest(const FString&)
{
    FStreamAnalyticField Field;auto Seed=StreamSeed(*Field.Identity());const FBox Bounds(FVector(-2,-1,-3),FVector(8,1,2));
    FString Error;TArray<FVector> P,Again;
    for(int32 Dimensions:{2,3})for(int32 Count:{1,17,512})
    {
        Seed.Count=Count;
        TestTrue(*Error,StudioStreamlines::Seeds(Seed,Bounds,Dimensions,0,P,Error));
        TestTrue(TEXT("Exact requested inlet count"),P.Num()==Count);
        TestTrue(TEXT("Repeat generation succeeds"),StudioStreamlines::Seeds(Seed,Bounds,Dimensions,0,Again,Error));
        TestTrue(TEXT("Deterministic positions"),P==Again);TSet<FVector> Unique;
        for(const auto& V:P){Unique.Add(V);TestTrue(TEXT("Seed remains inside domain"),Bounds.IsInside(V));if(Dimensions==2)TestEqual(TEXT("2D stays on original plane"),V.Y,0.);}
        TestEqual(TEXT("Distinct inlet positions"),Unique.Num(),Count);
    }
    Seed.bUpperFace=true;TestTrue(TEXT("Upper inlet accepted"),StudioStreamlines::Seeds(Seed,Bounds,3,0,P,Error));
    TestTrue(TEXT("Upper face follows chosen domain boundary"),P[0].X>7.9);
    Seed.InletAxis=1;const auto Kept=P;
    TestFalse(TEXT("Spanwise inlet cannot manufacture a 2D plane"),StudioStreamlines::Seeds(Seed,Bounds,2,0,P,Error));
    TestTrue(TEXT("Rejected seed edit retains prior positions"),P==Kept);
    Seed.Kind=EStudioSeedKind::Line;Seed.A=FVector(-1,0,-1);Seed.B=FVector(2,0,1);Seed.Count=17;
    TestTrue(TEXT("Line built"),StudioStreamlines::Seeds(Seed,Bounds,2,0,P,Error));
    TestEqual(TEXT("First endpoint exact"),P[0],Seed.A);TestEqual(TEXT("Last endpoint exact"),P.Last(),Seed.B);
    Seed.Kind=EStudioSeedKind::Plane;Seed.A=FVector(1,0,0);Seed.B=FVector(2,0,1);Seed.C=FVector(-1,0,2);
    TestTrue(TEXT("Arbitrary plane rectangle built"),StudioStreamlines::Seeds(Seed,Bounds,2,0,P,Error));
    TestEqual(TEXT("Plane count exact despite grid shape"),P.Num(),17);
    for(const auto& V:P)TestEqual(TEXT("Oblique rectangle stays on its specified plane"),V.Y,0.);
    Seed.Kind=EStudioSeedKind::Points;Seed.Points={FVector(1,.123,2),FVector(-1,.456,-2)};
    TestTrue(TEXT("Selected points retained"),StudioStreamlines::Seeds(Seed,Bounds,2,0,P,Error));
    TestTrue(TEXT("No silent snap onto source plane"),P==Seed.Points);
    const FVector Duplicate=Seed.Points[0];Seed.Points.Add(Duplicate);
    TestFalse(TEXT("Duplicate selected locations rejected"),StudioStreamlines::Seeds(Seed,Bounds,2,0,P,Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioStreamIntegration,"Studio.Streamlines.BoundedInstantaneousIntegration",StreamTestFlags)
bool FStudioStreamIntegration::RunTest(const FString&)
{
    FStreamAnalyticField Field;auto Seed=StreamSeed(*Field.Identity());Seed.Kind=EStudioSeedKind::Points;Seed.Points={FVector::ZeroVector};
    const FBox Bounds(FVector(-5,-1,-5),FVector(5,1,5));FStudioStreamlineSettings S;S.Direction=EStudioStreamDirection::Both;
    S.StepFraction=.013;S.MaximumLength=.071;S.MaximumSteps=100;S.WorkBudget=1000;
    FString Error;FStudioStreamlineOutput Out;
    if(!TestTrue(*Error,StudioStreamlines::Build(Field,Bounds,{Seed},S,TEXT("pressure"),Out,Error))||!TestEqual(TEXT("Both directions are separate branches"),Out.Paths.Num(),2))return false;
    const double Length=.71;
    TestTrue(TEXT("Forward branch follows normalized real velocity"),Out.Paths[0].PositionsMeters.Last().Equals(FVector(.6,0,.8)*Length,1.e-12));
    TestTrue(TEXT("Backward branch reverses velocity"),Out.Paths[1].PositionsMeters.Last().Equals(FVector(-.6,0,-.8)*Length,1.e-12));
    for(const auto& Path:Out.Paths)
    {
        TestTrue(TEXT("Final partial step obeys exact length cap"),FMath::IsNearlyEqual(Path.LengthMeters,Length,1.e-12));
        TestTrue(TEXT("Length termination recorded"),Path.End==EStudioStreamEnd::LengthLimit);
        for(int32 I=0;I<Path.PositionsMeters.Num();++I)TestEqual(TEXT("Scalar remains independent of integration direction"),Path.Scalars[I],2*Path.PositionsMeters[I].X-3*Path.PositionsMeters[I].Z);
    }
    S.WorkBudget=3;
    TestTrue(TEXT("Budgeted trace succeeds"),StudioStreamlines::Build(Field,Bounds,{Seed},S,TEXT("pressure"),Out,Error));
    TestTrue(TEXT("Global work and allocation bound"),Out.Attempts==3&&Out.Segments==3&&Out.bBudgetExhausted);
    TestTrue(TEXT("Both branches get a turn under budget"),Out.Paths[0].PositionsMeters.Num()==3&&Out.Paths[1].PositionsMeters.Num()==2);
    S.WorkBudget=1000;S.MaximumSteps=2;
    TestTrue(TEXT("Step-limited trace"),StudioStreamlines::Build(Field,Bounds,{Seed},S,TEXT("pressure"),Out,Error));
    for(const auto& Path:Out.Paths)TestTrue(TEXT("Per-branch step cap honored"),Path.PositionsMeters.Num()==3&&Path.End==EStudioStreamEnd::StepLimit);
    S.MaximumSteps=100;S.Direction=EStudioStreamDirection::Forward;Field.bGap=true;
    TestTrue(TEXT("Unsupported interior returns explicit termination"),StudioStreamlines::Build(Field,Bounds,{Seed},S,TEXT("pressure"),Out,Error));
    TestTrue(TEXT("Midpoint and endpoint validity cannot jump a hole"),Out.Segments==0&&Out.Paths[0].End==EStudioStreamEnd::CoverageUnavailable);
    Field.bGap=false;Field.bZero=true;
    TestTrue(TEXT("Stationary field is handled"),StudioStreamlines::Build(Field,Bounds,{Seed},S,TEXT("pressure"),Out,Error));
    TestTrue(TEXT("Zero velocity invents no flow direction"),Out.Segments==0&&Out.Paths[0].End==EStudioStreamEnd::Stagnation);
    Field.bZero=false;Field.bMissingScalar=true;
    TestTrue(TEXT("Missing scalar is explicit"),StudioStreamlines::Build(Field,Bounds,{Seed},S,TEXT("pressure"),Out,Error));
    TestTrue(TEXT("No zero fallback scalar"),Out.Segments==0&&Out.Paths[0].End==EStudioStreamEnd::MissingScalar);
    Field.bMissingScalar=false;Field.Samples=0;Field.CancelOnSample=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    const auto Before=Out;
    TestFalse(TEXT("Mid-trace cancellation prevents publication"),StudioStreamlines::Build(Field,Bounds,{Seed},S,TEXT("pressure"),Out,Error,Field.CancelOnSample));
    TestTrue(TEXT("Previous output retained atomically"),Out.Segments==Before.Segments&&Out.Paths[0].End==Before.Paths[0].End);
    Field.CancelOnSample.Reset();Field.bCircular=true;Seed.Points={FVector(1,0,0)};S.MaximumLength=.1;S.MaximumSteps=4096;
    S.StepFraction=.01;TestTrue(TEXT("Coarse circular ODE trace"),StudioStreamlines::Build(Field,Bounds,{Seed},S,TEXT("pressure"),Out,Error));
    const FVector Exact(FMath::Cos(1.),0,FMath::Sin(1.));const double Coarse=(Out.Paths[0].PositionsMeters.Last()-Exact).Size();
    S.StepFraction=.005;TestTrue(TEXT("Fine circular ODE trace"),StudioStreamlines::Build(Field,Bounds,{Seed},S,TEXT("pressure"),Out,Error));
    const double Fine=(Out.Paths[0].PositionsMeters.Last()-Exact).Size();
    TestTrue(TEXT("Midpoint integration converges toward analytic arc at second order"),Fine<Coarse*.3&&Fine<.0002);
    Seed.Points[0].Y=.01;
    TestTrue(TEXT("Off-plane position reported"),StudioStreamlines::Build(Field,Bounds,{Seed},S,TEXT("pressure"),Out,Error));
    TestTrue(TEXT("2D is never extruded into scientific traces"),Out.Segments==0&&Out.Paths[0].End==EStudioStreamEnd::OutsideDomain);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioStreamPersistence,"Studio.Streamlines.PersistenceMigrationAndHistory",StreamTestFlags)
bool FStudioStreamPersistence::RunTest(const FString&)
{
    FStreamAnalyticField Field;auto Seed=StreamSeed(*Field.Identity());Seed.Kind=EStudioSeedKind::Points;Seed.Points={FVector(1.25,0,-.75),FVector(-.125,0,2.25)};
    FStudioProject P;P.View.InspectionObjects.Seeds.Add(Seed);P.View.StreamlineSettings.Direction=EStudioStreamDirection::Both;
    P.View.StreamlineSettings.bAutomaticSeeds=false;P.View.StreamlineSettings.AutomaticSeedCount=173;
    P.View.StreamlineSettings.StepFraction=.001234567890123;P.View.StreamlineSettings.WorkBudget=2345;
    FStudioProject Loaded;FString Error;
    TestTrue(*Error,StudioProjectIO::Parse(StudioProjectIO::Serialize(P),Loaded,Error));
    TestTrue(TEXT("Seed identities and exact selected positions persist"),Loaded.View.InspectionObjects==P.View.InspectionObjects);
    TestTrue(TEXT("All streamline controls persist"),Loaded.View.StreamlineSettings==P.View.StreamlineSettings);
    TSharedPtr<FJsonObject> JSON;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StudioProjectIO::Serialize(P)),JSON);
    const auto View=JSON->GetObjectField(TEXT("view"));View->RemoveField(TEXT("streamlineSettings"));
    TestFalse(TEXT("New document requires complete streamline settings"),StudioProjectIO::Parse(StreamJSON(JSON.ToSharedRef()),Loaded,Error));
    JSON->SetNumberField(TEXT("version"),14);const auto Objects=View->GetObjectField(TEXT("inspectionObjects"));
    Objects->SetNumberField(TEXT("version"),1);Objects->RemoveField(TEXT("seeds"));
    TestTrue(TEXT("Schema14 migrates without inventing saved seeds"),StudioProjectIO::Parse(StreamJSON(JSON.ToSharedRef()),Loaded,Error));
    TestTrue(TEXT("Prior view gets safe tracing defaults"),Loaded.View.StreamlineSettings==FStudioStreamlineSettings()&&Loaded.View.InspectionObjects.Seeds.IsEmpty());
    auto Settings=StudioStreamlines::ToJSON(P.View.StreamlineSettings);auto Kept=P.View.StreamlineSettings;
    for(double Bad:{0.,65537.,3.5,std::numeric_limits<double>::infinity()})
    {
        Settings->SetNumberField(TEXT("workBudget"),Bad);TestFalse(TEXT("Unbounded/fractional work rejected"),StudioStreamlines::FromJSON(Settings,Kept));
        TestTrue(TEXT("Invalid settings preserve prior state"),Kept==P.View.StreamlineSettings);
    }
    FStudioInspectionState Before,After;After.Display=P.View;FStudioViewHistory History;History.Record(TEXT("Streamline seeds"),Before,After);
    FStudioInspectionState Restored;FString Label;
    TestTrue(TEXT("Shared view history undoes all seed settings together"),History.Restore(false,After,Restored,Label)&&Restored.Equals(Before));
    TestTrue(TEXT("Redo restores exact positions and parameters"),History.Restore(true,Before,Restored,Label)&&Restored.Equals(After));
    auto Rename=After.Display;Rename.InspectionObjects.Seeds[0].Name=TEXT("Renamed seeds");
    TestTrue(TEXT("Renaming does not retrace field"),StudioView::RenderEquals(After.Display,Rename));
    Rename.InspectionObjects.Seeds[0].Points[0].X+=.1;
    TestFalse(TEXT("Changing a seed invalidates field geometry"),StudioView::RenderEquals(After.Display,Rename));
    auto Duplicate=P.View.InspectionObjects;Duplicate.Seeds.Add(Seed);
    TestFalse(TEXT("Duplicate stable object identities rejected"),StudioInspectionObjects::IsValid(Duplicate,Error));
    TestTrue(TEXT("History accounts for selected point arrays"),History.StoredBytes()>int64(sizeof(FStudioInspectionState))*2);
    FStudioModel M(FPaths::ProjectSavedDir()/TEXT("Automation/StreamlineModel")/FGuid::NewGuid().ToString());
    FStudioSeedObject ModelSeed;ModelSeed.Name=TEXT("Seed set");ModelSeed.Kind=EStudioSeedKind::Line;ModelSeed.B=FVector(0,0,1);
    if(!TestTrue(TEXT("Model creates and selects a source-bound seed set"),M.AddSeed(ModelSeed)&&M.SelectedInspectionObject==ModelSeed.Id))return false;
    TestFalse(TEXT("Adding saved seeds switches off automatic inlet"),M.StreamlineSettings.bAutomaticSeeds);
    TestTrue(TEXT("Creation and mode are one undo step"),M.UndoView()&&M.InspectionObjects.Seeds.IsEmpty()&&M.StreamlineSettings.bAutomaticSeeds);
    TestTrue(TEXT("Redo restores seed and saved mode together"),M.RedoView()&&M.FindSeed(ModelSeed.Id)&&!M.StreamlineSettings.bAutomaticSeeds);
    TestTrue(TEXT("Seed model pins current recording"),M.FindSeed(ModelSeed.Id)->Source==M.InspectionSource());
    const auto Original=*M.FindSeed(ModelSeed.Id);const int32 Frame=M.SelectedFrame;const auto Camera=M.Project.Camera;
    const auto Intent=M.RenderIntentRevision;
    TestTrue(TEXT("Named seed set participates in shared object operations"),M.RenameInspectionObject(ModelSeed.Id,TEXT("Inlet study")));
    TestEqual(TEXT("Label change keeps in-flight geometry valid"),M.RenderIntentRevision,Intent);
    TestTrue(TEXT("Model edits trigger geometry invalidation"),M.EditSeed(ModelSeed.Id,[](auto& O){O.Count=17;})&&M.RenderIntentRevision>Intent);
    TestTrue(TEXT("Seed edit undo restores count"),M.UndoView()&&M.FindSeed(ModelSeed.Id)->Count==Original.Count);
    TestTrue(TEXT("Seed edit redo restores count"),M.RedoView()&&M.FindSeed(ModelSeed.Id)->Count==17);
    TestFalse(TEXT("Seed source cannot be relabelled by edit"),M.EditSeed(ModelSeed.Id,[](auto& O){O.Source.MetadataSHA256=FString::ChrN(64,'f');}));
    TestFalse(TEXT("Seed stable ID cannot be changed"),M.EditSeed(ModelSeed.Id,[](auto& O){O.Id=FGuid::NewGuid();}));
    TestTrue(TEXT("Duplicate seed set receives fresh identity"),M.DuplicateInspectionObject(ModelSeed.Id)&&M.SelectedInspectionObject!=ModelSeed.Id);
    const auto Copy=M.SelectedInspectionObject;
    TestTrue(TEXT("Duplicate retains exact source and geometry"),M.FindSeed(Copy)->Source==Original.Source&&M.FindSeed(Copy)->B==Original.B);
    TestTrue(TEXT("Seed visibility is undoable"),M.SetInspectionObjectVisible(Copy,false)&&M.UndoView()&&M.FindSeed(Copy)->bVisible);
    TestTrue(TEXT("Seed deletion clears selection"),M.DeleteInspectionObject(Copy)&&!M.SelectedInspectionObject.IsValid());
    TestTrue(TEXT("Delete undo restores stable seed ID"),M.UndoView()&&M.FindSeed(Copy));
    auto Saved=M.Solver;M.Solver=MakeShared<FRecordedSolver,ESPMode::ThreadSafe>(StudioRecordings::PathForId(TEXT("MeshGraphNets_Airfoil_test010")));
    TestFalse(TEXT("Inactive source seeds cannot be edited"),M.EditSeed(ModelSeed.Id,[](auto& O){O.Count=18;}));
    M.Solver=Saved;
    TestEqual(TEXT("Seed operations never scrub replay"),M.SelectedFrame,Frame);
    TestTrue(TEXT("Seed operations never move camera"),StudioView::CameraEquals(M.Project.Camera,Camera));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioStreamRecorded,"Studio.Streamlines.RecordedFieldsAndSourceIsolation",StreamTestFlags)
bool FStudioStreamRecorded::RunTest(const FString&)
{
    TArray<TSharedRef<IStudioSolver,ESPMode::ThreadSafe>> Sources;
    Sources.Add(MakeShared<FRecordedSolver,ESPMode::ThreadSafe>());
    const auto Point=StudioRecordings::Import(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json"),0,{});
    if(!TestTrue(*Point.Error,Point.Reference.IsSet()&&Point.Source.IsValid()))return false;
    const auto Surface=StudioRecordings::ImportReconstruction(*Point.Reference,FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_SurfaceFixture/reconstruction.json"),0,{});
    const auto VolumePoint=StudioRecordings::Import(FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_ReaderFixture/recording.json"),0,{});
    if(!TestTrue(*Surface.Error,Surface.Source.IsValid())||!TestTrue(*VolumePoint.Error,VolumePoint.Reference.IsSet()))return false;
    const auto Volume=StudioRecordings::ImportReconstruction(*VolumePoint.Reference,FPaths::ProjectContentDir()/TEXT("Samples/Cylinder3D_VolumeFixture/reconstruction.json"),0,{});
    if(!TestTrue(*Volume.Error,Volume.Source.IsValid()))return false;
    Sources.Add(Surface.Source.ToSharedRef());Sources.Add(Volume.Source.ToSharedRef());
    int32 SourceIndex=0;
    for(const auto& Solver:Sources)
    {
        const auto Field=Solver->CaptureViewField(0,TEXT("pressure"),true);const auto Identity=Field->Identity();
        if(!TestTrue(TEXT("Immutable authentic frame loaded"),Field->IsValid()&&Identity.IsSet()))return false;
        FBox Bounds=Solver->Descriptor().DisplayBounds;
        if(const auto Grid=Solver->VolumeReconstruction())Bounds=FBox(FVector(Grid->SourceBounds.Min.X,Grid->SourceBounds.Min.Z,Grid->SourceBounds.Min.Y),
            FVector(Grid->SourceBounds.Max.X,Grid->SourceBounds.Max.Z,Grid->SourceBounds.Max.Y));
        auto Seed=StreamSeed(*Identity);Seed.Kind=EStudioSeedKind::Points;
        // Distributed, supported locations inside the genuine field. No values
        // are authored or filled: both velocity and pressure come from its reader.
        for(int32 Z=1;Z<9;++Z)for(int32 X=1;X<9;++X)
        {
            FVector P=Bounds.Min+Bounds.GetSize()*FVector(X/9.,.5,Z/9.);if(Identity->SpatialDimensions==2)P.Y=Identity->SourceOffset.Y;
            FVector Velocity;double Pressure;
            if(Field->SampleVelocity(P,Velocity)&&Field->SampleScalar(P,TEXT("pressure"),Pressure)&&Field->SupportsSegment(P,P))Seed.Points.Add(P);
        }
        if(!TestTrue(TEXT("Recording has supported seed locations"),Seed.Points.Num()>8))return false;
        FStudioStreamlineSettings S;S.Direction=EStudioStreamDirection::Both;S.StepFraction=.001;S.MaximumSteps=40;S.WorkBudget=6000;
        FStudioStreamlineOutput Out;FString Error;
        if(!TestTrue(*Error,StudioStreamlines::Build(*Field,Bounds,{Seed},S,TEXT("pressure"),Out,Error)))return false;
        TestTrue(TEXT("Substantial authentic traces generated"),Out.Segments>100);
        TestTrue(TEXT("Result records exact immutable frame identity"),Out.Identity.IsSet()&&Out.Identity->Dataset==Identity->Dataset&&
            Out.Identity->MetadataSHA256==Identity->MetadataSHA256&&Out.Identity->PayloadSHA256==Identity->PayloadSHA256&&
            Out.Identity->ReconstructionSHA256==Identity->ReconstructionSHA256&&Out.Identity->Ordinal==Identity->Ordinal&&
            Out.Identity->Frame.Index==Identity->Frame.Index&&Out.Identity->Frame.Time==Identity->Frame.Time&&
            Out.Identity->Interpolation==Identity->Interpolation&&Out.Identity->SourceOffset==Identity->SourceOffset);
        TArray<double> Audit;int32 Interior=0;
        for(const auto& Path:Out.Paths)for(int32 I=0;I<Path.PositionsMeters.Num();++I)
        {
            const auto P=Path.PositionsMeters[I];double Value;
            if(!Field->SampleScalar(P,TEXT("pressure"),Value)){AddError(TEXT("Trace sample is outside recorded field"));return false;}
            TestEqual(TEXT("Raw pressure retained before color mapping"),Path.Scalars[I],Value);
            if(!I)continue;
            const auto A=Path.PositionsMeters[I-1];FVector D1,D2;
            Field->SampleVelocity(A,D1);D1/=D1.Size();
            const double Sign=Path.bBackward?-1.:1.,Step=S.StepFraction*Bounds.GetSize().GetMax();
            Field->SampleVelocity(A+D1*(Sign*Step*.5),D2);D2/=D2.Size();
            if(!P.Equals(A+D2*(Sign*Step),1.e-11)){AddError(TEXT("Recorded streamline deviated from supplied velocity"));return false;}
            for(double T:{.2,.4,.6,.8})
            {if(!Field->SampleScalar(FMath::Lerp(A,P,T),TEXT("pressure"),Value)){AddError(TEXT("Trace bridges a missing recorded region"));return false;}++Interior;}
            if(Audit.Num()<16000)Audit.Append({A.X,A.Y,A.Z,P.X,P.Y,P.Z,Path.Scalars[I],Sign});
        }
        TestTrue(TEXT("Many path interior positions checked against actual data"),Interior>400);
        const FString Folder=FPaths::ProjectDir()/TEXT("tmp/debug/streamline-recorded-audit");IFileManager::Get().MakeDirectory(*Folder,true);
        const FString File=Folder/FString::Printf(TEXT("source-%d.f64"),SourceIndex++);
        TestTrue(TEXT("Export traced segments for independent audit"),FFileHelper::SaveArrayToFile(TArrayView<const uint8>(reinterpret_cast<const uint8*>(Audit.GetData()),Audit.Num()*sizeof(double)),*File));
        const auto Later=Solver->CaptureViewField(2,TEXT("pressure"),true);FStudioStreamlineOutput Evolved;
        TestTrue(TEXT("Next actual frame traces independently"),StudioStreamlines::Build(*Later,Bounds,{Seed},S,TEXT("pressure"),Evolved,Error));
        TestEqual(TEXT("New result identifies its own ordinal"),Evolved.Identity->Ordinal,2);
        bool Changed=false;for(int32 I=0;I<FMath::Min(Out.Paths.Num(),Evolved.Paths.Num());++I)
            if(Out.Paths[I].Scalars!=Evolved.Paths[I].Scalars||Out.Paths[I].PositionsMeters!=Evolved.Paths[I].PositionsMeters){Changed=true;break;}
        TestTrue(TEXT("Transient recorded fields change the traces or sampled pressure"),Changed);
        Seed.Source.MetadataSHA256=FString::ChrN(64,'f');
        TestTrue(TEXT("Foreign seed binding handled"),StudioStreamlines::Build(*Field,Bounds,{Seed},S,TEXT("pressure"),Evolved,Error));
        TestTrue(TEXT("Cannot silently reuse another recording's seed set"),Evolved.Paths.IsEmpty()&&Evolved.Notices.Contains(Seed.Id));
    }
    const auto Raw=Point.Source->CaptureViewField(0,TEXT("pressure"),true);auto Seed=StreamSeed(*Raw->Identity());
    FStudioStreamlineOutput Out;FString Error;
    TestTrue(TEXT("Original point mode remains inspectable"),StudioStreamlines::Build(*Raw,Point.Source->Descriptor().DisplayBounds,{Seed},{},TEXT("pressure"),Out,Error));
    TestTrue(TEXT("Original points are never implicitly connected into streamlines"),Out.Paths.IsEmpty()&&Out.Notices.Contains(Seed.Id));
    return true;
}
#endif
