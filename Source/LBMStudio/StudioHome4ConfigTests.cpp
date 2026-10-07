#include "StudioHome4Config.h"
#include "StudioCase.h"
#include "StudioHome4JSON.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
    constexpr EAutomationTestFlags Home4Flags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
    FStudioHome4Spec Home4Complete()
    {
        FStudioHome4Spec S;S.RecipeId=TEXT("test-only");S.LineageId=TEXT("independent-test-fixture");
        S.Reference.LengthCells=100.;S.Reference.SpeedCellsPerStep=.02;
        S.Fluids.RhoHeavy=3.;S.Fluids.RhoLight=1.;S.Fluids.NuHeavy=.1;S.Fluids.NuLight=.01;
        S.Fluids.Sigma=.006;S.Fluids.Xi=5.;S.Fluids.Mobility=.02;S.Fluids.Gravity=.0001;
        S.Fluids.PhaseST=1.;S.Fluids.PhaseSD1=1.5;S.Fluids.PhaseSD2=1.5;S.Fluids.PhaseSXY=1.5;
        S.Lattice.Extents=FIntVector(100,20,20);S.Run.Backend=EStudioHome4Backend::Metal;
        S.Run.ExtensionImported=true;S.Run.Steps=1000;
        S.Units.DxMeters=.01;S.Units.DtSeconds=.001;S.Units.DensityReferenceKgM3=1000.;return S;
    }
    bool Home4Has(const FStudioHome4Derived& D,const TCHAR* Field,EStudioHome4IssueSeverity Severity)
    {return D.Issues.ContainsByPredicate([&](const FStudioHome4Issue& I){return I.Field==Field&&I.Severity==Severity;});}
    double Home4Value(const TOptional<double>& N){return N.IsSet()?*N:std::numeric_limits<double>::quiet_NaN();}
    bool Home4Near(double A,double B){return FMath::IsFinite(A)&&FMath::IsFinite(B)&&FMath::Abs(A-B)<=1.e-9*FMath::Max(1.,FMath::Abs(B));}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4Formulas,"Studio.Home4.Config.DimensionlessFormulas",Home4Flags)
bool FStudioHome4Formulas::RunTest(const FString&)
{
    const auto S=Home4Complete();const auto D=StudioHome4Config::Derive(S);
    TestFalse(TEXT("Known feasible configuration has no launch blocks"),D.HasBlockingIssues());
    TestTrue(TEXT("Mach uses D3Q27 sound speed"),Home4Near(Home4Value(D.Mach),.02*FMath::Sqrt(3.)));
    TestTrue(TEXT("Re=UL/nu"),Home4Near(Home4Value(D.Reynolds),20.));
    TestTrue(TEXT("Fr=U/sqrt(gL)"),Home4Near(Home4Value(D.Froude),.2));
    TestTrue(TEXT("Bo=rho*g*L^2/sigma"),Home4Near(Home4Value(D.Bond),500.));
    TestTrue(TEXT("We=rho*U^2*L/sigma"),Home4Near(Home4Value(D.Weber),20.));
    TestTrue(TEXT("Ca uses dynamic viscosity rho*nu"),Home4Near(Home4Value(D.Capillary),1.));
    TestTrue(TEXT("Pe=UL/M"),Home4Near(Home4Value(D.Peclet),100.));
    TestTrue(TEXT("Cn=xi/L"),Home4Near(Home4Value(D.Cahn),.05));
    TestTrue(TEXT("At uses both densities"),Home4Near(Home4Value(D.Atwood),.5));
    TestTrue(TEXT("Heavy tau"),Home4Near(Home4Value(D.TauHeavy),.8));
    TestTrue(TEXT("Light tau"),Home4Near(Home4Value(D.TauLight),.53));
    TestTrue(TEXT("Kn=cs*(tau-.5)/L"),Home4Near(Home4Value(D.Knudsen),.003/FMath::Sqrt(3.)));
    TestTrue(TEXT("Wake wavelength"),Home4Near(Home4Value(D.WakeWavelength),8*PI));
    auto Target=S;Target.Reference.SpeedCellsPerStep.Reset();Target.Fluids.NuHeavy.Reset();Target.Fluids.Sigma.Reset();Target.Fluids.Mobility.Reset();Target.Fluids.Xi.Reset();Target.Fluids.RhoLight.Reset();
    Target.Reference.Mach=D.Mach;Target.Reference.Reynolds=D.Reynolds;Target.Reference.Bond=D.Bond;Target.Reference.Peclet=D.Peclet;Target.Reference.Cahn=D.Cahn;Target.Reference.Atwood=D.Atwood;
    const FString Before=StudioHome4Config::Serialize(Target);const auto Resolved=StudioHome4Config::Derive(Target);
    TestTrue(TEXT("Dimensionless requests derive lattice values"),Home4Near(Home4Value(Resolved.Speed),.02)&&Home4Near(Home4Value(Resolved.NuHeavy),.1)&&Home4Near(Home4Value(Resolved.Sigma),.006)&&Home4Near(Home4Value(Resolved.Mobility),.02)&&Home4Near(Home4Value(Resolved.Xi),5)&&Home4Near(Home4Value(Resolved.RhoLight),1));
    TestEqual(TEXT("Derivation preserves requests exactly"),StudioHome4Config::Serialize(Target),Before);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4UnitsTest,"Studio.Home4.Config.UnitConversionsAndInverses",Home4Flags)
bool FStudioHome4UnitsTest::RunTest(const FString&)
{
    auto S=Home4Complete();
    struct FExpected{EStudioHome4Quantity Q;double PhysicalScale;};
    const FExpected Cases[]={
        {EStudioHome4Quantity::Length,.01},{EStudioHome4Quantity::Time,.001},{EStudioHome4Quantity::Density,1000.},
        {EStudioHome4Quantity::Velocity,10.},{EStudioHome4Quantity::KinematicViscosity,.1},{EStudioHome4Quantity::Mobility,.1},
        {EStudioHome4Quantity::Pressure,100000.},{EStudioHome4Quantity::Acceleration,10000.},
        {EStudioHome4Quantity::SurfaceTension,1000.},{EStudioHome4Quantity::Force,10.},
        {EStudioHome4Quantity::Moment,.1},{EStudioHome4Quantity::Energy,.1},{EStudioHome4Quantity::StrainRate,1000.},
        {EStudioHome4Quantity::SquaredRate,1000000.},{EStudioHome4Quantity::SpecificDissipation,100000.}};
    for(const auto& C:Cases)
    {
        const auto P=StudioHome4Config::ConvertUnits(2.,C.Q,EStudioHome4UnitDisplay::Lattice,EStudioHome4UnitDisplay::Physical,S);
        TestTrue(TEXT("Dimension-specific SI scale"),Home4Near(Home4Value(P),2*C.PhysicalScale));
        TestTrue(TEXT("SI inverse"),P.IsSet()&&Home4Near(Home4Value(StudioHome4Config::ConvertUnits(*P,C.Q,EStudioHome4UnitDisplay::Physical,EStudioHome4UnitDisplay::Lattice,S)),2));
        const auto N=StudioHome4Config::ConvertUnits(2.,C.Q,EStudioHome4UnitDisplay::Lattice,EStudioHome4UnitDisplay::Nondimensional,S);
        TestTrue(TEXT("Nondimensional inverse"),N.IsSet()&&Home4Near(Home4Value(StudioHome4Config::ConvertUnits(*N,C.Q,EStudioHome4UnitDisplay::Nondimensional,EStudioHome4UnitDisplay::Lattice,S)),2));
    }
    TestTrue(TEXT("Speed normalization uses L/U reference time"),Home4Near(Home4Value(StudioHome4Config::ConvertUnits(.02,EStudioHome4Quantity::Velocity,EStudioHome4UnitDisplay::Lattice,EStudioHome4UnitDisplay::Nondimensional,S)),1.));
    S.Reference.TimeSteps=16000.;
    TestTrue(TEXT("Explicit benchmark time preserved"),Home4Near(Home4Value(StudioHome4Config::ConvertUnits(16000.,EStudioHome4Quantity::Time,EStudioHome4UnitDisplay::Lattice,EStudioHome4UnitDisplay::Nondimensional,S)),1.));
    S.Units.DxMeters.Reset();
    TestFalse(TEXT("Missing spacing cannot become a zero velocity"),StudioHome4Config::ConvertUnits(1.,EStudioHome4Quantity::Velocity,EStudioHome4UnitDisplay::Lattice,EStudioHome4UnitDisplay::Physical,S).IsSet());
    TestTrue(TEXT("Time conversion requires no length or density"),Home4Near(Home4Value(StudioHome4Config::ConvertUnits(1.,EStudioHome4Quantity::Time,EStudioHome4UnitDisplay::Lattice,EStudioHome4UnitDisplay::Physical,S)),.001));
    TestFalse(TEXT("Non-finite conversion rejected"),StudioHome4Config::ConvertUnits(std::numeric_limits<double>::infinity(),EStudioHome4Quantity::Length,EStudioHome4UnitDisplay::Lattice,EStudioHome4UnitDisplay::Physical,S).IsSet());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ValidationTest,"Studio.Home4.Config.MissingInvalidAndNumericalBoundaries",Home4Flags)
bool FStudioHome4ValidationTest::RunTest(const FString&)
{
    FString Error;FStudioHome4Spec Empty;
    TestTrue(TEXT("Incomplete request is persistable"),StudioHome4Config::Validate(Empty,Error));
    auto D=StudioHome4Config::Derive(Empty);
    TestFalse(TEXT("Missing speed is unknown"),D.Speed.IsSet());
    TestTrue(TEXT("Incomplete request blocks numerical launch"),D.HasBlockingIssues());
    auto S=Home4Complete();S.Reference.SpeedCellsPerStep=.3/FMath::Sqrt(3.);
    TestFalse(TEXT("Mach=0.3 boundary does not block"),StudioHome4Config::Derive(S).HasBlockingIssues());
    S.Reference.SpeedCellsPerStep=.300001/FMath::Sqrt(3.);
    TestTrue(TEXT("Above Mach=0.3 blocks"),Home4Has(StudioHome4Config::Derive(S),TEXT("reference.speedCellsPerStep"),EStudioHome4IssueSeverity::Blocking));
    S=Home4Complete();S.Fluids.NuLight=.003;
    TestTrue(TEXT("Small positive light tau margin warns"),Home4Has(StudioHome4Config::Derive(S),TEXT("fluids.nuLight"),EStudioHome4IssueSeverity::Warning));
    S.Fluids.NuLight=0.;TestTrue(TEXT("Zero viscosity blocks"),Home4Has(StudioHome4Config::Derive(S),TEXT("fluids.nu"),EStudioHome4IssueSeverity::Blocking));
    TestTrue(TEXT("Numerical infeasibility remains saveable"),StudioHome4Config::Validate(S,Error));
    S=Home4Complete();S.Fluids.RhoHeavy=1000.;S.Fluids.RhoLight=1.;
    TestTrue(TEXT("1000:1 requires safeguards"),Home4Has(StudioHome4Config::Derive(S),TEXT("fluids.safeguards"),EStudioHome4IssueSeverity::Blocking));
    S.Fluids.GradientLimiter=true;S.Fluids.ForceThresholding=true;
    TestFalse(TEXT("Both safeguards resolve ratio block"),Home4Has(StudioHome4Config::Derive(S),TEXT("fluids.safeguards"),EStudioHome4IssueSeverity::Blocking));
    S=Home4Complete();S.Reference.Mach=.2;
    TestTrue(TEXT("Conflicting dimensionless constraint blocks"),Home4Has(StudioHome4Config::Derive(S),TEXT("reference.mach"),EStudioHome4IssueSeverity::Blocking));
    S=Home4Complete();S.Run.Backend=EStudioHome4Backend::PyTorch;
    TestTrue(TEXT("Fallback requires acknowledgment"),Home4Has(StudioHome4Config::Derive(S),TEXT("run.fallbackConfirmed"),EStudioHome4IssueSeverity::Blocking));
    S.Run.FallbackConfirmed=true;TestFalse(TEXT("Acknowledged fallback resolves block"),Home4Has(StudioHome4Config::Derive(S),TEXT("run.fallbackConfirmed"),EStudioHome4IssueSeverity::Blocking));
    S=Home4Complete();S.Fluids.Xi=std::numeric_limits<double>::quiet_NaN();
    TestFalse(TEXT("Non-finite request rejected structurally"),StudioHome4Config::Validate(S,Error));
    S=Home4Complete();S.Units.DxMeters=0;TestFalse(TEXT("Zero unit-map scale is invalid"),StudioHome4Config::Validate(S,Error));
    S=Home4Complete();S.Run.Tag=TEXT("bad\nargument");TestFalse(TEXT("Control character in argv data rejected"),StudioHome4Config::Validate(S,Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4MDTest,"Studio.Home4.Config.MultidomainScalingAndCost",Home4Flags)
bool FStudioHome4MDTest::RunTest(const FString&)
{
    auto S=Home4Complete();S.Multidomain.Levels=3;S.Fluids.Mobility.Reset();S.Multidomain.FinestMobility=.08;
    S.Multidomain.LevelCells={40000,10000,1000};S.Performance.MeasuredMLUPS=10.;S.Performance.MeasurementSource=TEXT("explicit-test-workload");
    FStudioHome4Allocation A;A.Name=TEXT("D3Q27 two-buffer state");A.Nodes=51000;A.Components=27;A.BytesPerComponent=4;A.Buffers=2;S.Performance.Allocations.Add(A);
    auto D=StudioHome4Config::Derive(S);
    TestEqual(TEXT("Three MD rows"),D.Levels.Num(),3);
    if(D.Levels.Num()==3)
    {
        const auto& P=D.Levels[2];
        TestTrue(TEXT("Acoustic viscosity scaling"),Home4Near(Home4Value(P.NuHeavy),.4));
        TestTrue(TEXT("Surface tension scaling"),Home4Near(Home4Value(P.Sigma),.024));
        TestTrue(TEXT("Fine-level mobility supplied at fine depth"),Home4Near(Home4Value(P.Mobility),.08));
        TestTrue(TEXT("Gravity scales inversely"),Home4Near(Home4Value(P.Gravity),.000025));
        TestTrue(TEXT("Interface width fixed in root cells"),Home4Near(Home4Value(P.Xi),20.));
    }
    TestTrue(TEXT("Root mobility inverse from fine recipe"),Home4Near(Home4Value(D.Mobility),.02));
    TestTrue(TEXT("Explicit allocation byte count"),D.AllocationBytes.IsSet()&&*D.AllocationBytes==11016000ULL);
    TestTrue(TEXT("MD cost includes level substeps"),Home4Near(Home4Value(D.EstimatedSeconds),6.4));
    S.Fluids.Mobility=.08;
    TestTrue(TEXT("Wrong root mobility blocks"),Home4Has(StudioHome4Config::Derive(S),TEXT("multidomain.finestMobility"),EStudioHome4IssueSeverity::Blocking));
    S.Fluids.Mobility.Reset();S.Multidomain.TauFloor=.56;D=StudioHome4Config::Derive(S);
    TestTrue(TEXT("Explicit tau floor raises root light viscosity"),D.Levels.Num()>0&&Home4Near(Home4Value(D.Levels[0].NuLight),.02));
    S.Multidomain.NoTauFloor=true;D=StudioHome4Config::Derive(S);
    TestTrue(TEXT("Disabling tau floor preserves original physics"),D.Levels.Num()>0&&Home4Near(Home4Value(D.Levels[0].NuLight),.01));
    S.Multidomain.RecipeCahn=.025;
    TestTrue(TEXT("Fixed-Cn departure warns"),Home4Has(StudioHome4Config::Derive(S),TEXT("multidomain.recipeCahn"),EStudioHome4IssueSeverity::Warning));
    S.Performance.Allocations[0].Nodes.Reset();TestFalse(TEXT("Missing allocation size means unknown memory"),StudioHome4Config::Derive(S).AllocationBytes.IsSet());
    S.Performance.MeasuredMLUPS.Reset();TestFalse(TEXT("Missing measured speed means unknown cost"),StudioHome4Config::Derive(S).EstimatedSeconds.IsSet());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4MetalTest,"Studio.Home4.Config.MetalIndexAndAllocationOverflow",Home4Flags)
bool FStudioHome4MetalTest::RunTest(const FString&)
{
    auto S=Home4Complete();S.Multidomain.Levels=2;S.Multidomain.LevelCells={40000,159072862};
    TestFalse(TEXT("Last safe D3Q27 Metal cell count"),Home4Has(StudioHome4Config::Derive(S),TEXT("multidomain.levelCells"),EStudioHome4IssueSeverity::Blocking));
    S.Multidomain.LevelCells[1]=159072863;
    TestTrue(TEXT("First unsafe D3Q27 Metal cell count"),Home4Has(StudioHome4Config::Derive(S),TEXT("multidomain.levelCells"),EStudioHome4IssueSeverity::Blocking));
    S.Run.Backend=EStudioHome4Backend::CUDA;
    TestFalse(TEXT("CUDA size_t path has no Metal-only block"),Home4Has(StudioHome4Config::Derive(S),TEXT("multidomain.levelCells"),EStudioHome4IssueSeverity::Blocking));
    S.Lattice.Extents=FIntVector(1048576,1048576,1048576);S.Multidomain.Levels.Reset();S.Multidomain.LevelCells.Reset();
    auto D=StudioHome4Config::Derive(S);TestTrue(TEXT("Extents multiply in uint64"),D.RootCells.IsSet()&&*D.RootCells==(uint64(1)<<60));
    FStudioHome4Allocation A;A.Name=TEXT("bounded-large-allocation");A.Nodes=1000000000000LL;A.Components=1024;A.BytesPerComponent=16;A.Buffers=16;
    for(int32 I=0;I<80;++I)S.Performance.Allocations.Add(A);
    D=StudioHome4Config::Derive(S);TestFalse(TEXT("Overflow cannot wrap to cheap memory"),D.AllocationBytes.IsSet());
    TestTrue(TEXT("Allocation overflow reported"),Home4Has(D,TEXT("performance.allocations"),EStudioHome4IssueSeverity::Blocking));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4PersistenceTest,"Studio.Home4.Config.TransactionalJSONAndLegacyCases",Home4Flags)
bool FStudioHome4PersistenceTest::RunTest(const FString&)
{
    auto S=Home4Complete();S.Geometry.Stiffness.SetNumZeroed(36);S.Geometry.Stiffness[35]=.12345678901234567;
    S.Run.NoFrameAcceleration=false;S.Zones.FloorFriction=0.;S.Run.Steps=1000000000000LL;
    FString Error;FStudioHome4Spec Loaded;
    TestTrue(TEXT("Home4Complete request round trip"),StudioHome4Config::Parse(StudioHome4Config::Serialize(S),Loaded,Error));
    TestEqual(TEXT("Exact numeric and optional requests preserved"),StudioHome4Config::Serialize(Loaded),StudioHome4Config::Serialize(S));
    TestTrue(TEXT("Zero differs from missing"),Loaded.Zones.FloorFriction.IsSet()&&*Loaded.Zones.FloorFriction==0&&!Loaded.Zones.Sponge.IsSet());
    TestTrue(TEXT("Explicit false differs from unknown"),Loaded.Run.NoFrameAcceleration.IsSet()&&!(*Loaded.Run.NoFrameAcceleration)&&!Loaded.Geometry.Float.IsSet());
    const FString Before=StudioHome4Config::Serialize(Loaded);
    auto O=StudioHome4Config::ToJSON(S);O->GetObjectField(TEXT("run"))->SetNumberField(TEXT("steps"),1.5);
    TestFalse(TEXT("Fractional count rejected"),StudioHome4Config::FromJSON(O,Loaded,Error));TestEqual(TEXT("Invalid count leaves all output unchanged"),StudioHome4Config::Serialize(Loaded),Before);
    O=StudioHome4Config::ToJSON(S);O->GetObjectField(TEXT("units"))->SetStringField(TEXT("dxMeters"),TEXT("0.1"));
    TestFalse(TEXT("String is not a typed number"),StudioHome4Config::FromJSON(O,Loaded,Error));
    O=StudioHome4Config::ToJSON(S);O->GetObjectField(TEXT("run"))->SetStringField(TEXT("unknownFlag"),TEXT("never"));
    TestFalse(TEXT("Unknown version-one keys rejected"),StudioHome4Config::FromJSON(O,Loaded,Error));
    O=StudioHome4Config::ToJSON(S);O->SetNumberField(TEXT("version"),2);
    TestFalse(TEXT("Future schema rejected transactionally"),StudioHome4Config::FromJSON(O,Loaded,Error));
    O=StudioHome4Config::ToJSON(S);O->GetObjectField(TEXT("fluids"))->RemoveField(TEXT("nuLight"));
    TestTrue(TEXT("Missing optional numeric remains unknown"),StudioHome4Config::FromJSON(O,Loaded,Error)&&!Loaded.Fluids.NuLight.IsSet());
    FStudioCaseDraft Legacy,Case;const auto Old=StudioCaseIO::ToJSON(Legacy);
    TestFalse(TEXT("Old case JSON omits HOME4"),Old->HasField(TEXT("home4")));
    TestTrue(TEXT("Old case loads without solver reinterpretation"),StudioCaseIO::FromJSON(Old,Case,Error)&&!Case.Home4.IsSet());
    Case.Home4=S;const auto Frozen=FStudioRunRecord::Capture(TEXT("HOME4 request"),Case,EStudioRunOrigin::ControlHarness);
    const FString Captured=StudioCaseIO::Serialize(*Frozen.GetConfiguration());Case.Home4->Fluids.Xi=9.;
    TestEqual(TEXT("Run spec deeply frozen"),StudioCaseIO::Serialize(*Frozen.GetConfiguration()),Captured);
    FStudioCaseDraft Reopened;TestTrue(TEXT("HOME4 case persists"),StudioCaseIO::FromJSON(StudioCaseIO::ToJSON(Case),Reopened,Error)&&Reopened.Home4.IsSet());
    if(Reopened.Home4.IsSet())TestEqual(TEXT("Case contains exact modified requests"),StudioHome4Config::Serialize(*Reopened.Home4),StudioHome4Config::Serialize(*Case.Home4));
    auto Corrupt=StudioCaseIO::ToJSON(Case);Corrupt->GetObjectField(TEXT("home4"))->SetNumberField(TEXT("version"),99);
    const FString Kept=StudioCaseIO::Serialize(Reopened);TestFalse(TEXT("Nested invalid spec rolls entire case back"),StudioCaseIO::FromJSON(Corrupt,Reopened,Error));
    TestEqual(TEXT("Case rollback retains previous home4 and other owners"),StudioCaseIO::Serialize(Reopened),Kept);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ArgvTest,"Studio.Home4.Config.AppendixArgvAndShellQuoting",Home4Flags)
bool FStudioHome4ArgvTest::RunTest(const FString&)
{
    auto S=Home4Complete();S.Geometry.PatchClassification=TEXT("HKr");S.Run.Tag=TEXT("hull's $(never) _viz");
    S.Run.OutDirectory=TEXT("/tmp/results with spaces");S.Run.NoFrameAcceleration=true;S.Run.BodyOnCpu=false;
    S.Run.SaveState=TEXT("checkpoint-request");S.Geometry.HeelDegrees=5.;
    FStudioHome4DriverCommand C;FString Error;
    if(!TestTrue(TEXT("Command preview builds from requests"),StudioHome4Config::BuildHullDriverArgv(S,TEXT("/path/python"),TEXT("/path/run_hull_speed.py"),C,Error)))return false;
    TestEqual(TEXT("Executable is first independent argument"),C.Argv[0],FString(TEXT("/path/python")));
    const int32 Tag=C.Argv.Find(TEXT("--tag"));TestTrue(TEXT("Tag remains one literal argument"),Tag!=INDEX_NONE&&C.Argv.IsValidIndex(Tag+1)&&C.Argv[Tag+1]==S.Run.Tag);
    TestTrue(TEXT("Affirmative appendix switch encoded"),C.Argv.Contains(TEXT("--no_frame_accel")));
    TestFalse(TEXT("False switch is not fabricated as another flag"),C.Argv.Contains(TEXT("--body_on_cpu")));
    TestFalse(TEXT("Unsupported heel flag never invented"),C.Argv.Contains(TEXT("--heel")));
    TestFalse(TEXT("Unknown checkpoint arity never invented"),C.Argv.Contains(TEXT("--save_state")));
    TestTrue(TEXT("Unverified contracts reported"),C.MissingContracts.Num()>0);
    TestEqual(TEXT("Shell display safely quotes apostrophe, whitespace and shell syntax"),StudioHome4Config::ShellDisplay({TEXT("a'b"),TEXT("$(never)"),TEXT("")}),FString(TEXT("'a'\"'\"'b' '$(never)' ''")));
    const auto Kept=C.Argv;S.Run.Tag=TEXT("bad\nargument");
    TestFalse(TEXT("Invalid command is transactional"),StudioHome4Config::BuildHullDriverArgv(S,TEXT("python"),TEXT("driver"),C,Error));
    TestTrue(TEXT("Previous argument vector kept"),C.Argv==Kept);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4TypedRecipeInputsTest,"Studio.Home4.Config.TypedRecipeAndMotionRequests",Home4Flags)
bool FStudioHome4TypedRecipeInputsTest::RunTest(const FString&)
{
    auto S=Home4Complete();S.Reference.KeuleganCarpenter=5;S.Reference.Galileo=19.6;S.Reference.RotationalReynolds=100;
    S.Reference.OscillationPeakSpeed=.04;S.Reference.SpinSurfaceSpeed=.04;S.Reference.WavePhaseSpeed=.015;S.Reference.WaveSlope=.08;
    S.Geometry.BodyFluidDensityRatio=1.25;S.Geometry.BeamDraftRatio=2;S.Geometry.SubmergenceChordRatio=1.8;
    S.Geometry.HeaveAmplitudeCells=3;S.Geometry.RollAmplitudeDegrees=5;S.Geometry.PitchAmplitudeDegrees=2;
    S.Geometry.MotionFrequencyCyclesPerStep=.001;S.Geometry.MotionPhaseDegrees=-30;S.Geometry.SpinRadiansPerStep=-.002;
    S.Geometry.InitialPositionCells=FVector(1,2,3);S.Geometry.InitialAttitudeDegrees=FVector(4,5,6);
    S.Geometry.InitialVelocityCellsPerStep=FVector(.01,.02,.03);S.Geometry.InitialAngularVelocityRadiansPerStep=FVector(.001,.002,.003);
    S.Lattice.StreamwiseCells=100;S.Lattice.WidthLengthRatio=.25;S.Lattice.HeightLengthRatio=.30;
    FStudioHome4Spec Loaded;FString E;
    TestTrue(TEXT("Typed benchmark and body requests parse"),StudioHome4Config::Parse(StudioHome4Config::Serialize(S),Loaded,E));
    TestEqual(TEXT("All optional values and vector conventions persist"),StudioHome4Config::Serialize(Loaded),StudioHome4Config::Serialize(S));
    TestTrue(TEXT("Steady speed is independent of benchmark-specific speeds"),Home4Near(StudioHome4Config::Derive(S).Speed.Get(-1),.02));
    FStudioHome4DriverCommand C;
    TestTrue(TEXT("Command preview accepts typed frontend requests"),StudioHome4Config::BuildHullDriverArgv(S,TEXT("python"),TEXT("run_hull_speed.py"),C,E));
    TestTrue(TEXT("Motion request remains an explicit missing driver contract"),C.MissingContracts.ContainsByPredicate([](const FString& M){return M.StartsWith(TEXT("geometry.motionFrequencyCyclesPerStep:"));}));
    TestFalse(TEXT("No motion flag invented"),C.Argv.Contains(TEXT("--motion_frequency")));
    Loaded.Reference.SpeedCellsPerStep.Reset();Loaded.Reference.Mach.Reset();
    TestFalse(TEXT("Peak, surface and wave speeds cannot supply missing inlet speed"),StudioHome4Config::Derive(Loaded).Speed.IsSet());
    TestTrue(TEXT("Initial attitude has explicit degree metadata"),StudioHome4Config::Fields().ContainsByPredicate([](const FStudioHome4Field& F){return F.Section==TEXT("geometry")&&F.Key==TEXT("initialAttitudeDegrees")&&F.Unit==TEXT("degrees");}));
    S.Lattice.StreamwiseCells=101;
    TestTrue(TEXT("Conflicting explicit Nx blocks numerical launch"),Home4Has(StudioHome4Config::Derive(S),TEXT("lattice.streamwiseCells"),EStudioHome4IssueSeverity::Blocking));
    S.Lattice.StreamwiseCells=100;S.Geometry.InitialAttitudeDegrees=FVector(0,0,361);
    TestFalse(TEXT("Unbounded initial attitude rejected"),StudioHome4Config::Validate(S,E));
    S.Geometry.InitialAttitudeDegrees.Reset();S.Geometry.BodyFluidDensityRatio=0;
    TestFalse(TEXT("Body/fluid ratio must be positive"),StudioHome4Config::Validate(S,E));
    auto Legacy=StudioHome4Config::ToJSON(Home4Complete());Legacy->GetObjectField(TEXT("reference"))->RemoveField(TEXT("keuleganCarpenter"));
    Legacy->GetObjectField(TEXT("geometry"))->RemoveField(TEXT("initialPositionCells"));
    TestTrue(TEXT("Absent new fields remain unset in existing specs"),StudioHome4Config::FromJSON(Legacy,Loaded,E)&&!Loaded.Reference.KeuleganCarpenter&&!Loaded.Geometry.InitialPositionCells);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4StrictImportTest,"Studio.Home4.Config.StrictBoundedTransactionalFileImport",Home4Flags)
bool FStudioHome4StrictImportTest::RunTest(const FString&)
{
    auto S=Home4Complete();S.Run.Tag=TEXT("read-a");FStudioHome4Spec Loaded=Home4Complete();Loaded.Run.Tag=TEXT("prior");FString E;
    const FString Original=StudioHome4Config::Serialize(Loaded),JSON=StudioHome4Config::Serialize(S);
    TestFalse(TEXT("Decoded duplicate root key rejected"),StudioHome4Config::Parse(TEXT("{\"vers\\u0069on\":1,")+JSON.Mid(1),Loaded,E));
    TestEqual(TEXT("Duplicate key leaves previous spec"),StudioHome4Config::Serialize(Loaded),Original);
    TestFalse(TEXT("Nested decoded duplicate rejected before object construction"),StudioHome4JSON::Preflight(TEXT("{\"x\":{\"name\":1,\"n\\u0061me\":2}}")));
    FString Deep;for(int32 I=0;I<33;++I)Deep+=TEXT("[");Deep+=TEXT("0");for(int32 I=0;I<33;++I)Deep+=TEXT("]");
    TestFalse(TEXT("Depth beyond 32 rejected"),StudioHome4JSON::Preflight(Deep));
    TestFalse(TEXT("Multiple roots rejected"),StudioHome4JSON::Preflight(TEXT("{} {}")));
    TestFalse(TEXT("Lone escaped surrogate rejected"),StudioHome4JSON::Preflight(TEXT("{\"x\":\"\\ud800\"}")));
    TestTrue(TEXT("Valid escaped Unicode pair accepted"),StudioHome4JSON::Preflight(TEXT("{\"x\":\"\\ud83d\\ude00\"}")));
    const FString Dir=FPaths::ProjectDir()/TEXT("tmp/debug/home4-import-tests")/FGuid::NewGuid().ToString();
    IFileManager::Get().MakeDirectory(*Dir,true);const FString Path=Dir/TEXT("spec.json");
    TestTrue(TEXT("Write valid bounded fixture"),FFileHelper::SaveStringToFile(JSON,*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    TestTrue(TEXT("Valid UTF-8 spec imports"),StudioHome4Config::Load(Path,Loaded,E));
    const FString Kept=StudioHome4Config::Serialize(Loaded);const auto Timestamp=IFileManager::Get().GetTimeStamp(*Path);
    auto Replacement=S;Replacement.Run.Tag=TEXT("read-b");
    TestFalse(TEXT("Same-size rewrite with preserved timestamp detected by bytes"),StudioHome4Config::LoadWithReadBoundaryForAutomation(Path,Loaded,E,[&]{
        TestTrue(TEXT("Rewrite fixture"),FFileHelper::SaveStringToFile(StudioHome4Config::Serialize(Replacement),*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
        IFileManager::Get().SetTimeStamp(*Path,Timestamp);
    }));
    TestEqual(TEXT("Rewrite rejection preserves previous spec"),StudioHome4Config::Serialize(Loaded),Kept);
    TArray<uint8> Bad={0xc0,0xaf};TestTrue(TEXT("Write malformed UTF-8 fixture"),FFileHelper::SaveArrayToFile(Bad,*Path));
    TestFalse(TEXT("Overlong UTF-8 rejected before decoding"),StudioHome4Config::Load(Path,Loaded,E));
    TestEqual(TEXT("Malformed import preserves prior data"),StudioHome4Config::Serialize(Loaded),Kept);
    Bad.Init(uint8(' '),1024*1024+1);TestTrue(TEXT("Write oversized fixture"),FFileHelper::SaveArrayToFile(Bad,*Path));
    TestFalse(TEXT("Import reads no unbounded file"),StudioHome4Config::Load(Path,Loaded,E));
    TestFalse(TEXT("Missing source rejected"),StudioHome4Config::Load(Dir/TEXT("missing.json"),Loaded,E));
    TestEqual(TEXT("All rejected imports retain prior request"),StudioHome4Config::Serialize(Loaded),Kept);
    IFileManager::Get().DeleteDirectory(*Dir,false,true);return !HasAnyErrors();
}
#endif
