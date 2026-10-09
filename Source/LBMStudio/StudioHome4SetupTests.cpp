#include "StudioHome4Setup.h"
#include "Misc/AutomationTest.h"
#include <cmath>
#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4SetupTestsLocal
{
FStudioHome4Spec Spec()
{
    FStudioHome4Spec S;S.Authoring.Primitive=TEXT("box");S.Authoring.PrimitiveSizeCells=FVector(4);S.Authoring.BodyId=TEXT("setup-body");
    S.Reference.LengthCells=4;S.Reference.SpeedCellsPerStep=.02;S.Fluids.NuHeavy=.01;S.Fluids.NuLight=.005;S.Fluids.Sigma=.02;S.Fluids.Mobility=.04;S.Fluids.Gravity=.001;S.Fluids.Xi=4;S.Fluids.RhoHeavy=1;S.Fluids.RhoLight=.1;
    S.Geometry.InitialPositionCells=FVector(8);S.Geometry.CenterOfGravity=FVector(8);S.Lattice.Extents=FIntVector(16);S.Authoring.WaterlineCells=8;S.Authoring.ZoneUnits=TEXT("root-cells");
    S.Lattice.PadUp=1;S.Lattice.PadDown=2;S.Lattice.PadSide=.5;S.Lattice.Depth=2;S.Lattice.Air=2;
    S.Run.RampLength=2;S.Run.NoFrameAcceleration=false;return S;
}
FStudioHome4AuthoredPatch Patch(const TCHAR* Id,int32 Level,FVector Origin,FIntVector Extents)
{FStudioHome4AuthoredPatch P;P.Id=Id;P.BodyId=TEXT("setup-body");P.Level=Level;P.Origin=Origin;P.Extents=Extents;return P;}
FStudioHome4AuthoringPreview Preview(const FStudioHome4Spec& S)
{FStudioHome4AuthoringRequest R;R.ProjectId=FGuid::NewGuid();R.CaseId=FGuid::NewGuid();R.Spec=S;R.SampleBudget=4096;return StudioHome4Authoring::Build(R,MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false));}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4TankSetup,"Studio.Home4.Setup.PreparedTankTranslationAndBounds",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4TankSetup::RunTest(const FString&)
{
    using namespace StudioHome4SetupTestsLocal;auto S=Spec();FString Error;FStudioHome4Spec Next;
    FStudioHome4AuthoredZone Z;Z.Id=TEXT("hand-zone");Z.Minimum=FVector(6);Z.Maximum=FVector(10);Z.Strength=.2;S.Authoring.Zones.Add(Z);
    S.Authoring.Patches.Add(Patch(TEXT("patch"),1,FVector(6),FIntVector(4)));
    FStudioHome4Allocation A;A.Name=TEXT("root array");A.NodeScope=TEXT("root");A.Nodes=4096;S.Performance.Allocations.Add(A);
    auto P=Preview(S);TestTrue(*P.Error,P.IsValid());if(!P.IsValid())return false;
    TestTrue(*Error,StudioHome4Setup::Tank(P,Next,Error));
    TestTrue(TEXT("Independent body-plus-pad dimensions produce 16×8×16"),Next.Lattice.Extents&&*Next.Lattice.Extents==FIntVector(16,8,16));
    TestTrue(TEXT("Every positional quantity translates by independent −(2,4,0)"),Next.Geometry.InitialPositionCells.Get(FVector::ZeroVector)==FVector(6,4,8)&&Next.Geometry.CenterOfGravity.Get(FVector::ZeroVector)==FVector(6,4,8)&&Next.Authoring.Patches[0].Origin==FVector(4,2,6)&&Next.Authoring.Zones[0].Minimum==FVector(4,2,6));
    TestEqual(TEXT("Root-scoped array tracks the actual new root"),Next.Performance.Allocations[0].Nodes.Get(0),int64(2048));
    auto Rebuilt=Preview(Next);TestTrue(*Rebuilt.Error,Rebuilt.IsValid());TestTrue(TEXT("Prepared triangles retain exact translated body bounds"),Rebuilt.Body.Min.Equals(P.Body.Min-FVector(2,4,0),1.e-10)&&Rebuilt.Body.Max.Equals(P.Body.Max-FVector(2,4,0),1.e-10));
    const FString Before=StudioHome4Config::Serialize(Next);P.bCancelled=true;
    TestFalse(TEXT("Cancelled preview cannot lay out the body"),StudioHome4Setup::Tank(P,Next,Error));TestEqual(TEXT("Failure preserves output transaction"),StudioHome4Config::Serialize(Next),Before);
    P.bCancelled=false;P.Spec.Lattice.Depth=.1;P.RequestSHA256=StudioHome4Authoring::Fingerprint(P.Spec);
    TestFalse(TEXT("Depth that clips actual body is rejected"),StudioHome4Setup::Tank(P,Next,Error));
    P=Preview(S);P.RequestSHA256=TEXT("stale");TestFalse(TEXT("Wrong immutable request identity rejected"),StudioHome4Setup::Tank(P,Next,Error));
    S.Authoring.Patches.Reset();S.Authoring.Zones.Reset();S.Lattice.PadDown=1.125;S.Lattice.Depth=1.125;P=Preview(S);
    TestTrue(*Error,StudioHome4Setup::Tank(P,Next,Error));TestTrue(TEXT("Half-cell requested lengths round up on upper tank sides"),Next.Lattice.Extents&&*Next.Lattice.Extents==FIntVector(13,8,13)&&Next.Authoring.WaterlineCells.Get(0)==4.5);
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4PatchCounts,"Studio.Home4.Setup.ContiguousPatchCountsAndIndependentPhysics",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4PatchCounts::RunTest(const FString&)
{
    using namespace StudioHome4SetupTestsLocal;auto S=Spec();S.Multidomain.Levels=3;S.Multidomain.NoTauFloor=true;
    S.Authoring.Patches.Add(Patch(TEXT("coarse"),1,FVector(2),FIntVector(16)));S.Authoring.Patches.Add(Patch(TEXT("fine"),2,FVector(4),FIntVector(8)));
    FStudioHome4Allocation A;A.Name=TEXT("fine");A.NodeScope=TEXT("level:2");S.Performance.Allocations.Add(A);FString Error;FStudioHome4Spec Next;
    TestTrue(*Error,StudioHome4Setup::PatchCounts(S,Next,Error));
    TestTrue(TEXT("Node products are root 4096, level1 4096, level2 512"),Next.Multidomain.LevelCells==TArray<int64>{4096,4096,512});TestEqual(TEXT("Scoped allocation uses actual fine product"),Next.Performance.Allocations[0].Nodes.Get(0),int64(512));
    const auto D=StudioHome4Config::Derive(Next);TestEqual(TEXT("Physics table has all three levels"),D.Levels.Num(),3);
    if(D.Levels.Num()==3)
    {
        const auto& L=D.Levels[2];TestTrue(TEXT("Independent acoustic scaling of ν, σ, M, g, ξ and τ"),FMath::IsNearlyEqual(L.NuHeavy.Get(0),.04,1.e-12)&&FMath::IsNearlyEqual(L.Sigma.Get(0),.08,1.e-12)&&FMath::IsNearlyEqual(L.Mobility.Get(0),.16,1.e-12)&&FMath::IsNearlyEqual(L.Gravity.Get(0),.00025,1.e-12)&&L.Xi.Get(0)==16&&FMath::IsNearlyEqual(L.TauHeavy.Get(0),.62,1.e-12));
    }
    const FString Before=StudioHome4Config::Serialize(Next);auto Bad=S;Bad.Authoring.Patches.RemoveAt(0);TestFalse(TEXT("Missing intermediate level is not counted as zero"),StudioHome4Setup::PatchCounts(Bad,Next,Error));TestEqual(TEXT("Invalid topology leaves previous request intact"),StudioHome4Config::Serialize(Next),Before);
    Bad=S;Bad.Authoring.Patches[1].Level=-1;TestFalse(TEXT("Negative level rejected before indexing"),StudioHome4Setup::PatchCounts(Bad,Next,Error));
    Bad=S;Bad.Authoring.Patches[1].Origin=FVector(12);TestFalse(TEXT("Fine patch outside parent rejected"),StudioHome4Setup::PatchCounts(Bad,Next,Error));
    Bad=S;Bad.Authoring.Patches.Add(Patch(TEXT("overlap"),1,FVector(3),FIntVector(4)));TestFalse(TEXT("Overlapping same-level ownership is not double-counted"),StudioHome4Setup::PatchCounts(Bad,Next,Error));
    Bad=S;Bad.Authoring.Patches[0].bFollowBody=true;Bad.Authoring.Patches[0].BodyId=TEXT("another-body");TestFalse(TEXT("Following patch must bind exact body identity"),StudioHome4Setup::PatchCounts(Bad,Next,Error));
    StudioHome4Setup::InvalidatePatchCounts(Next);TestTrue(TEXT("Topology invalidation clears estimates instead of preserving old numbers"),Next.Multidomain.LevelCells.IsEmpty()&&!Next.Performance.Allocations[0].Nodes);
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ZoneSetup,"Studio.Home4.Setup.DeclaredWidthsUnitsAndReverseProfiles",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ZoneSetup::RunTest(const FString&)
{
    using namespace StudioHome4SetupTestsLocal;auto S=Spec();S.Lattice.Extents=FIntVector(160,80,40);S.Units.DxMeters=.01;S.Authoring.ZoneUnits=TEXT("physical-metres");
    S.Zones.Sponge=.2;S.Zones.XBeach=1.2;S.Zones.BeachY=.1;S.Zones.BeachGap=.5;S.Zones.ZoneStrength=.5;S.Zones.XBeachStrength=.4;S.Zones.FloorFriction=.1;
    FStudioHome4AuthoredZone Hand;Hand.Id=TEXT("hand-zone");Hand.Maximum=FVector(.1);S.Authoring.Zones.Add(Hand);FStudioHome4Spec Next;FString Error;TestTrue(*Error,StudioHome4Setup::Zones(S,Next,Error));TestEqual(TEXT("Actual regions include retained handmade plus five realized zones"),Next.Authoring.Zones.Num(),6);
    const auto* Sponge=Next.Authoring.Zones.FindByPredicate([](const auto& Z){return Z.Id==TEXT("frontend-width-sponge");});
    TestTrue(TEXT("Physical sponge .2m starts at root x140"),Sponge&&FMath::IsNearlyEqual(Sponge->Minimum.X*100,140.,1.e-10));
    const auto* Low=Next.Authoring.Zones.FindByPredicate([](const auto& Z){return Z.Id==TEXT("frontend-width-beach-y-low");});
    if(Low)
    {
        FStudioHome4PreviewRegion R;R.Bounds=FBox(Low->Minimum*100,Low->Maximum*100);R.Axis=Low->Axis;R.Profile=Low->Profile;R.Strength=Low->Strength;R.LevelExponent=Low->LevelExponent;
        TestEqual(TEXT("Low-side exterior has full damping"),StudioHome4Authoring::ZoneWeight(R,FVector(10,0,10)),.5);TestEqual(TEXT("Low-side interior has zero damping"),StudioHome4Authoring::ZoneWeight(R,FVector(10,10,10)),0.);
        TestTrue(TEXT("Independent cubic midpoint and level exponent"),FMath::IsNearlyEqual(StudioHome4Authoring::ZoneWeight(R,FVector(10,5,10),2),.015625,1.e-12));
    }
    else AddError(TEXT("Real side region missing"));
    auto Again=Next;Again.Zones.Sponge.Reset();TestTrue(*Error,StudioHome4Setup::Zones(Again,Next,Error));TestEqual(TEXT("Reconcile removes disabled generated sponge without duplicating handwritten region"),Next.Authoring.Zones.Num(),5);
    const FString Before=StudioHome4Config::Serialize(Next);S.Zones.ZoneStrength.Reset();TestFalse(TEXT("Unknown strength is not fabricated"),StudioHome4Setup::Zones(S,Next,Error));TestEqual(TEXT("Unknown-default failure preserves output"),StudioHome4Config::Serialize(Next),Before);
    S=Spec();S.Zones.Sponge=1;S.Zones.ZoneStrength=.5;S.Authoring.ZoneUnits=TEXT("body-lengths");TestTrue(*Error,StudioHome4Setup::Zones(S,Next,Error));TestEqual(TEXT("L-sized sponge width uses actual L=4"),Next.Authoring.Zones[0].Minimum.X,3.);
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4RampSetup,"Studio.Home4.Setup.CubicRampDerivativeAndFrameForce",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4RampSetup::RunTest(const FString&)
{
    using namespace StudioHome4SetupTestsLocal;auto S=Spec();const auto Start=StudioHome4Setup::Ramp(S,0),Mid=StudioHome4Setup::Ramp(S,200),End=StudioHome4Setup::Ramp(S,400);
    TestTrue(TEXT("Cubic endpoints and independent midpoint speed/acceleration"),Start&&Mid&&End&&Start->Speed==0&&Start->Acceleration==0&&End->Speed==.02&&End->Acceleration==0&&Mid->Duration==400&&FMath::IsNearlyEqual(Mid->Speed,.01,1.e-12)&&FMath::IsNearlyEqual(Mid->Acceleration,.000075,1.e-12));
    if(Mid)
    {
        const auto Before=StudioHome4Setup::Ramp(S,199.99),After=StudioHome4Setup::Ramp(S,200.01);
        TestTrue(TEXT("Frame acceleration equals independent finite difference derivative"),Before&&After&&FMath::IsNearlyEqual((After->Speed-Before->Speed)/.02,Mid->Acceleration,1.e-12));
        TestTrue(TEXT("Frame-force densities scale with each declared phase density"),FMath::IsNearlyEqual(Mid->HeavyFrameForceDensity.Get(0),.000075,1.e-12)&&FMath::IsNearlyEqual(Mid->LightFrameForceDensity.Get(0),.0000075,1.e-12));
    }
    S.Run.NoFrameAcceleration=true;const auto Off=StudioHome4Setup::Ramp(S,200);TestTrue(TEXT("Explicit no-frame flag retains ramp but drops frame force"),Off&&Off->Acceleration>0&&Off->HeavyFrameForceDensity&&*Off->HeavyFrameForceDensity==0);
    S.Run.NoFrameAcceleration.Reset();const auto Unknown=StudioHome4Setup::Ramp(S,200);TestTrue(TEXT("Absent frame choice leaves force unknown"),Unknown&&!Unknown->HeavyFrameForceDensity);
    TestFalse(TEXT("Nonfinite preview step rejected"),StudioHome4Setup::Ramp(S,std::nan("")).IsSet());
    S.Run.RampLength=0;const auto Instant=StudioHome4Setup::Ramp(S,0);TestTrue(TEXT("Zero ramp explicitly means instantaneous input with no finite impulse"),Instant&&Instant->Duration==0&&Instant->Speed==.02);
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4WaveSetup,"Studio.Home4.Setup.FiniteDepthWaveAndNamedPhaseFaces",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4WaveSetup::RunTest(const FString&)
{
    using namespace StudioHome4SetupTestsLocal;auto S=Spec();S.Authoring.WaveModel=TEXT("linear-gravity");S.Authoring.WaveLengthCells=16;S.Authoring.WaveDepthCells=4;S.Authoring.WaveAmplitudeCells=1;FString Error;
    const double Pi=std::acos(-1.0),K=Pi/8.0,Omega=std::sqrt(.001*K*std::tanh(Pi/2.0)),Period=2.0*Pi/Omega;
    const auto P=StudioHome4Setup::WaveProperties(S,Error);TestTrue(*Error,P.IsSet());if(!P)return false;
    TestTrue(TEXT("Independent finite-depth dispersion matches period"),FMath::IsNearlyEqual(P->WaveNumber,K,1.e-12)&&FMath::IsNearlyEqual(P->AngularFrequency,Omega,1.e-12)&&FMath::IsNearlyEqual(P->Period,Period,1.e-10)&&FMath::IsNearlyEqual(P->PhaseSpeed,Omega/K,1.e-12));
    const auto Crest=StudioHome4Setup::Wave(S,FVector::ZeroVector,0,Error),Quarter=StudioHome4Setup::Wave(S,FVector::ZeroVector,Period/4,Error),Trough=StudioHome4Setup::Wave(S,FVector(8,0,0),0,Error);
    TestTrue(TEXT("Independent crest, quarter-period waterline and half-wavelength trough"),Crest&&Quarter&&Trough&&FMath::IsNearlyEqual(*Crest,9.,1.e-12)&&FMath::IsNearlyEqual(*Quarter,8.,1.e-12)&&FMath::IsNearlyEqual(*Trough,7.,1.e-12));
    TArray<FVector> Curve;TestTrue(*Error,StudioHome4Setup::WaveCurve(S,0,64,Curve,Error));TestTrue(TEXT("Actual curve retains endpoint positions and geometric amplitude"),Curve.Num()==65&&Curve[0].Equals(FVector(0,8,9),1.e-12)&&Curve[32].Equals(FVector(8,8,7),1.e-12)&&Curve.Last().Equals(FVector(16,8,9),1.e-12));
    S.Authoring.WavePeriodSteps=Period*1.2;const auto Previous=Curve;TestFalse(TEXT("Conflicting declared period rejects curve"),StudioHome4Setup::WaveCurve(S,0,64,Curve,Error));TestTrue(TEXT("Failed wave curve preserves previous output"),Curve==Previous);S.Authoring.WavePeriodSteps.Reset();
    S.Authoring.WaveAxis=TEXT("y");TestTrue(*Error,StudioHome4Setup::WaveCurve(S,0,64,Curve,Error));TestTrue(TEXT("Y wave samples actual Y positions"),Curve.Num()==65&&Curve[32].Equals(FVector(8,8,7),1.e-12));
    S.Zones.PeriodicY=true;S.Zones.PinPhaseWalls=true;S.Zones.PhiTop=0;S.Zones.PhiBottom=1;TArray<FStudioHome4BoundaryFace> Faces;
    TestTrue(*Error,StudioHome4Setup::BoundaryFaces(S,Faces,Error));TestTrue(TEXT("Six named physical face constraints retain exact phase values"),Faces.Num()==6&&Faces[2].Constraint.Contains(TEXT("periodic pair"))&&Faces[4].Bounds.Min.Z==0&&Faces[5].Bounds.Max.Z==16&&Faces[4].Phase.Get(-1)==1&&Faces[5].Phase.Get(-1)==0);
    S.Zones.PhiTop.Reset();TestTrue(*Error,StudioHome4Setup::BoundaryFaces(S,Faces,Error));TestFalse(TEXT("Phase-wall unspecified value is never invented"),Faces[5].Phase.IsSet());
    return !HasAnyErrors();
}
#endif
