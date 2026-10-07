#include "StudioHome4Authoring.h"
#include "StudioHome4Recipes.h"
#include "StudioHome4Readouts.h"
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4AuthoringTestsLocal
{
FStudioHome4Spec Box()
{
    FStudioHome4Spec S;S.Authoring.Primitive=TEXT("box");S.Authoring.PrimitiveSizeCells=FVector(4.5);S.Lattice.Extents=FIntVector(16);
    S.Geometry.InitialPositionCells=FVector(8);S.Geometry.BandCells=3;S.Authoring.WaterlineCells=8;
    S.Reference.LengthCells=4.5;S.Reference.SpeedCellsPerStep=.02;S.Fluids.RhoHeavy=1;S.Fluids.RhoLight=.1;S.Fluids.Gravity=.01;
    S.Fluids.NuHeavy=.01;S.Fluids.NuLight=.001;S.Fluids.Xi=4;S.Fluids.Mobility=.03;
    S.Units.DxMeters=.01;S.Units.DtSeconds=.001;S.Units.DensityReferenceKgM3=1000;return S;
}
FStudioHome4AuthoringPreview Build(const FStudioHome4Spec& S,int32 Budget=32768)
{FStudioHome4AuthoringRequest R;R.Spec=S;R.SampleBudget=Budget;return StudioHome4Authoring::Build(R,MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false));}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4AuthoringSDF,"Studio.Home4.Authoring.OriginalIndexSignedDistanceAndCutFractions",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4AuthoringSDF::RunTest(const FString&)
{
    using namespace StudioHome4AuthoringTestsLocal;const auto S=Box();const auto P=Build(S);TestTrue(*P.Error,P.IsValid());if(!P.IsValid())return false;
    const auto* Center=P.Cells.FindByPredicate([](const auto& C){return C.Index==FIntVector(8);});const auto* Outside=P.Cells.FindByPredicate([](const auto& C){return C.Index==FIntVector(5,8,8);});
    TestTrue(TEXT("Interior signed distance is actual nearest face"),Center&&Center->bInside&&FMath::IsNearlyEqual(Center->SignedDistance,-2.25,1e-9));
    TestTrue(TEXT("Exterior node retains its original index and distance"),Outside&&!Outside->bInside&&FMath::IsNearlyEqual(Outside->SignedDistance,.75,1e-9));
    const auto* Link=P.Links.FindByPredicate([](const auto& L){return L.Index==FIntVector(5,8,8)&&L.Direction==FIntVector(1,0,0);});
    TestTrue(TEXT("Cut fraction is exact segment/triangle intersection"),Link&&FMath::IsNearlyEqual(Link->Fraction,.75,1e-9)&&Link->Position.Equals(FVector(5.75,8,8),1e-9));
    const auto Sparse=Build(S,100);TestTrue(TEXT("Sampling is bounded explicitly"),Sparse.IsValid()&&Sparse.Cells.Num()<=100&&Sparse.Stride>1&&Sparse.RequestedCells==P.RequestedCells);
    for(const auto& C:Sparse.Cells)TestTrue(TEXT("Sampled nodes keep exact original integer positions"),C.Index.X>=0&&C.Index.Y>=0&&C.Index.Z>=0);
    auto Shift=S;Shift.Geometry.InitialPositionCells=FVector(9,8,8);const auto Q=Build(Shift);TestTrue(TEXT("Pose changes the immutable request and actual bounds"),Q.IsValid()&&Q.RequestSHA256!=P.RequestSHA256&&FMath::IsNearlyEqual(Q.Body.Min.X-P.Body.Min.X,1.,1e-12));
    TestEqual(TEXT("Building does not mutate the next-run request"),StudioHome4Config::Serialize(S),StudioHome4Config::Serialize(Box()));return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4AuthoringFloat,"Studio.Home4.Authoring.ClosedMeshFlotationAndHydrostaticStiffness",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4AuthoringFloat::RunTest(const FString&)
{
    using namespace StudioHome4AuthoringTestsLocal;auto S=Box();S.Geometry.BodyMass=40;S.Geometry.CenterOfGravity=FVector(8);S.Geometry.Float=true;
    const auto P=Build(S);TestTrue(*P.Error,P.IsValid());TestTrue(*P.HydrostaticError,P.Hydrostatics.IsSet());if(!P.Hydrostatics)return false;
    const auto& H=*P.Hydrostatics;TestTrue(TEXT("Closed box volume matches analytic geometry"),FMath::IsNearlyEqual(H.TotalVolume,FMath::Pow(4.5,3),1e-9));
    const double ExpectedHeave=(.5*H.TotalVolume-(40-.1*H.TotalVolume)/.9)/(4.5*4.5);
    TestTrue(TEXT("Actual displaced mass balances declared body mass"),H.bConverged&&FMath::Abs(H.DisplacedMass-40)<1e-5&&FMath::Abs(H.VerticalResidual)<1e-6);
    TestTrue(TEXT("Geometric equilibrium heave matches the analytical box"),FMath::IsNearlyEqual(H.Heave,ExpectedHeave,1e-6));
    TestTrue(TEXT("Waterplane derivative yields physical heave stiffness"),FMath::IsNearlyEqual(H.K33,.9*.01*4.5*4.5,1e-8));
    TestTrue(TEXT("Preview uses equilibrium mesh pose"),FMath::IsNearlyEqual(P.Body.Min.Z,5.75+H.Heave,1e-6));
    S.Geometry.BodyMass=200;const auto Impossible=Build(S);TestTrue(TEXT("Impossible float remains an explicit geometric error without fabricated equilibrium"),Impossible.IsValid()&&!Impossible.Hydrostatics&&Impossible.HydrostaticError.Contains(TEXT("no partially submerged")));
    auto Rotated=Box();Rotated.Geometry.BodyMass=40;Rotated.Geometry.CenterOfGravity=FVector(8);Rotated.Geometry.Float=true;
    Rotated.Geometry.InitialAttitudeDegrees=FVector(4,3,20);Rotated.Geometry.SinkCells=.1;
    const auto Equilibrium=Build(Rotated);FStudioHome4Spec Accepted;FString Error;
    TestTrue(*Equilibrium.HydrostaticError,Equilibrium.Hydrostatics.IsSet());
    TestTrue(*Error,StudioHome4Authoring::AcceptEquilibrium(Equilibrium,Accepted,Error));
    const auto Replay=Build(Accepted);TestTrue(*Replay.Error,Replay.IsValid());
    if(Equilibrium.IsValid()&&Replay.IsValid())for(int32 I=0;I<Equilibrium.Mesh->Positions.Num();++I)
        TestTrue(TEXT("Accepted arbitrary source attitude reproduces every equilibrium vertex"),Equilibrium.Mesh->Positions[I].Equals(Replay.Mesh->Positions[I],1e-9));
    TestTrue(TEXT("Accepted CoG follows geometric heave exactly once"),Equilibrium.Hydrostatics&&Accepted.Geometry.CenterOfGravity&&Accepted.Geometry.CenterOfGravity->Equals(Equilibrium.Hydrostatics->CenterOfGravity,1e-9));
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4AuthoringZones,"Studio.Home4.Authoring.AuthoredZoneProfilesPatchesAndMotion",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4AuthoringZones::RunTest(const FString&)
{
    using namespace StudioHome4AuthoringTestsLocal;auto S=Box();S.Authoring.ZoneUnits=TEXT("root-cells");FStudioHome4AuthoredZone Z;Z.Id=TEXT("rear");Z.Minimum=FVector(10,0,0);Z.Maximum=FVector(16);Z.Strength=.8;S.Authoring.Zones.Add(Z);
    FStudioHome4AuthoredPatch Patch;Patch.Id=TEXT("body-fine");Patch.Level=2;Patch.Origin=FVector(6);Patch.Extents=FIntVector(16);Patch.bFollowBody=true;Patch.BodyId=TEXT("body");S.Authoring.Patches.Add(Patch);
    S.Geometry.BodyMotion=TEXT("forced-heave");S.Geometry.HeaveAmplitudeCells=2;S.Geometry.MotionFrequencyCyclesPerStep=.01;S.Geometry.CenterOfGravity=FVector(8);
    const auto P=Build(S);TestTrue(*P.Error,P.IsValid());TestEqual(TEXT("Declared region requests are realized separately"),P.Regions.Num(),2);if(P.Regions.Num()!=2)return false;
    TestTrue(TEXT("Cubic profile has actual midpoint strength"),FMath::IsNearlyEqual(StudioHome4Authoring::ZoneWeight(P.Regions[0],FVector(13,8,8)),.1,1e-12));
    TestTrue(TEXT("Per-level damping scales by declared exponent"),FMath::IsNearlyEqual(StudioHome4Authoring::ZoneWeight(P.Regions[0],FVector(13,8,8),2),.025,1e-12));
    TestTrue(TEXT("Original fine patch spacing is dyadic, not root spacing"),P.Regions[1].Bounds.GetSize().Equals(FVector(4),1e-12));
    const auto Motion=StudioHome4Authoring::Motion(S,25);TestTrue(TEXT("Declared quarter-period heave moves the actual body"),Motion.TransformPosition(FVector(8)).Equals(FVector(8,8,10),1e-12));
    S.Authoring.ZoneUnits.Empty();TestFalse(TEXT("Unknown zone coordinates cannot be guessed into a preview"),Build(S).IsValid());return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4AuthoringSizing,"Studio.Home4.Authoring.ResizingAndExplicitMixedDimensions",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4AuthoringSizing::RunTest(const FString&)
{
    using namespace StudioHome4AuthoringTestsLocal;auto S=Box();S.Reference.Reynolds=9;S.Geometry.BodyMass=2;S.Geometry.InertiaDiagonal=FVector(3);S.Geometry.Stiffness.Init(1,36);S.Authoring.PreserveCahn=true;
    FStudioHome4Allocation A;A.Name=TEXT("root array");A.Nodes=4096;A.NodeScope=TEXT("root");S.Performance.Allocations.Add(A);const auto Before=StudioHome4Config::Derive(S);FString Error;
    TestTrue(*Error,StudioHome4Config::Resize(S,9,.02*FMath::Sqrt(3.),9,Error));
    TestTrue(TEXT("Resolution resizes actual extents and allocations"),S.Lattice.Extents==FIntVector(32)&&S.Performance.Allocations[0].Nodes==int64(32768));
    TestTrue(TEXT("Cahn and physical coordinates stay coherent"),FMath::IsNearlyEqual(*S.Fluids.Xi/ *S.Reference.LengthCells,*Before.Cahn,1e-12)&&S.Geometry.InitialPositionCells->Equals(FVector(16))&&FMath::IsNearlyEqual(*S.Units.DxMeters,.005,1e-12));
    TestTrue(TEXT("Mass, inertia and mixed K scale with their dimensions"),S.Geometry.BodyMass==16&&S.Geometry.InertiaDiagonal->Equals(FVector(96))&&FMath::IsNearlyEqual(S.Geometry.Stiffness[0],2.,1e-12)&&FMath::IsNearlyEqual(S.Geometry.Stiffness[3],4.,1e-12)&&FMath::IsNearlyEqual(S.Geometry.Stiffness[21],8.,1e-12));
    const auto Gradient=StudioHome4Config::ConvertUnits(2,EStudioHome4Quantity::Gradient,EStudioHome4UnitDisplay::Lattice,EStudioHome4UnitDisplay::Physical,S);
    TestTrue(TEXT("Explicit inverse-length source units convert"),Gradient&&FMath::IsNearlyEqual(*Gradient,400.,1e-9));
    const auto Force=StudioHome4Config::ConvertUnits(1,EStudioHome4Quantity::ForceDensity,EStudioHome4UnitDisplay::Lattice,EStudioHome4UnitDisplay::Physical,S);
    TestTrue(TEXT("Explicit force density uses rho dx/dt²"),Force&&FMath::IsNearlyEqual(*Force,1000*.005/FMath::Square(*S.Units.DtSeconds),1e-6));
    return !HasAnyErrors();
}
#endif
