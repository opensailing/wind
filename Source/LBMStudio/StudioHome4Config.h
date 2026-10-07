#pragma once
#include "CoreMinimal.h"

class FJsonObject;

enum class EStudioHome4UnitDisplay : uint8 { Lattice, Physical, Nondimensional };
enum class EStudioHome4Backend : uint8 { Unknown, Metal, CUDA, PyTorch };
enum class EStudioHome4Quantity : uint8
{
    Dimensionless, Length, Time, Density, Velocity, KinematicViscosity, Pressure,
    Acceleration, SurfaceTension, Mobility, Force, Moment, Energy, StrainRate,
    SquaredRate, SpecificDissipation
};
enum class EStudioHome4IssueSeverity : uint8 { Information, Warning, Blocking };

struct FStudioHome4Units
{
    TOptional<double> DxMeters, DtSeconds, DensityReferenceKgM3;
    EStudioHome4UnitDisplay Display = EStudioHome4UnitDisplay::Lattice;
};
struct FStudioHome4Reference
{
    TOptional<double> LengthCells, SpeedCellsPerStep, TimeSteps;
    // Dimensionless requests derive missing lattice values; conflicting requests are reported.
    TOptional<double> Mach, Reynolds, Froude, Bond, Weber, Capillary, Peclet, Cahn, Atwood;
    // Benchmark anchors are independent of steady inlet/reference speed.
    TOptional<double> KeuleganCarpenter, Galileo, RotationalReynolds;
    TOptional<double> OscillationPeakSpeed, SpinSurfaceSpeed, WavePhaseSpeed, WaveSlope;
};
struct FStudioHome4Fluids
{
    TOptional<double> RhoHeavy, RhoLight, NuHeavy, NuLight, Sigma, Xi, Mobility, Gravity;
    TOptional<double> PhaseST, PhaseSD1, PhaseSD2, PhaseSXY;
    FString TauMethod, SurfaceTensionForm;
    TOptional<bool> GradientLimiter, ForceThresholding;
    TOptional<double> GradientLimitFactor, ForceThresholdFactor, LightForceFactor, LightPhaseCutoff;
};
struct FStudioHome4Geometry
{
    FString SourcePath, PatchClassification, SdfBackend, CptPath;
    TOptional<double> HeelDegrees, TrimDegrees, YawDegrees, SinkCells, BandCells, Refine;
    TOptional<bool> Float, NoEquilibrate;
    FString BodyMotion, RetabulationPolicy;
    TOptional<double> BodyMass, RetabulateEvery;
    TOptional<double> BodyFluidDensityRatio, BeamDraftRatio, SubmergenceChordRatio;
    // Explicit frontend sinusoidal motion request: amplitude*sin(2*pi*f*n+phase).
    // Translation uses source XYZ root cells; rotations use roll/pitch/yaw degrees.
    // These requests have no asserted solver/driver encoding.
    TOptional<double> HeaveAmplitudeCells, RollAmplitudeDegrees, PitchAmplitudeDegrees;
    TOptional<double> MotionFrequencyCyclesPerStep, MotionPhaseDegrees, SpinRadiansPerStep;
    TOptional<FVector> CenterOfGravity;
    TOptional<FVector> InitialPositionCells, InitialAttitudeDegrees;
    TOptional<FVector> InitialVelocityCellsPerStep, InitialAngularVelocityRadiansPerStep;
    TArray<double> Stiffness; // Empty or 36 row-major entries for a 6-DOF stiffness matrix.
};
struct FStudioHome4Lattice
{
    TOptional<FIntVector> Extents;
    TOptional<double> PadUp, PadDown, PadSide, Depth, Air;
    TOptional<int64> StreamwiseCells;
    TOptional<double> WidthLengthRatio, HeightLengthRatio;
};
struct FStudioHome4Zones
{
    TOptional<double> Sponge, XBeach, XBeachStrength, BeachY, BeachGap, ZoneStrength, FloorFriction;
    FString MassCorrection, PierceBoundary;
    TOptional<bool> PeriodicX, PeriodicY, PeriodicZ, PinPhaseWalls;
    TOptional<double> PhiTop, PhiBottom;
    FString Walls, Inlet, Outlet, WaveAbsorption;
};
struct FStudioHome4Multidomain
{
    TOptional<int64> Levels;
    TOptional<double> Z1, Z2, Margin, BandDepth, Overlap, RestrictionMargin, TauFloor;
    TOptional<bool> EvenWrap, RootPhaseFree, NoTauFloor, NoFullF2C, FixedCahnRefinement;
    TOptional<int64> MassFixEvery;
    FString SneqMode;
    TOptional<double> FinestMobility, RecipeCahn;
    // Explicit level cell counts permit memory/cost estimates without guessing topology.
    TArray<int64> LevelCells;
};
struct FStudioHome4Run
{
    EStudioHome4Backend Backend = EStudioHome4Backend::Unknown;
    FString Device, QueueTarget, Tag, OutDirectory, InitState, SaveState, VizDirectory;
    TOptional<bool> BodyOnCpu, NoGpuKernels, Smoke, NoFrameAcceleration;
    TOptional<bool> ExtensionImported, FallbackConfirmed;
    TOptional<double> Travel, RampLength, AverageLength;
    // Restart cadence is an explicit frontend request; the driver CLI mapping
    // remains unavailable until its actual checkpoint hook is verified.
    TOptional<int64> Steps, MeasureEvery, PrintEvery, SaveEvery, VizEvery, RestartEvery;
    TOptional<FIntVector> BlockShape;
};
struct FStudioHome4Allocation
{
    FString Name;
    TOptional<int64> Nodes;
    int64 Components = 1, BytesPerComponent = 4, Buffers = 1;
};
struct FStudioHome4Performance
{
    TArray<FStudioHome4Allocation> Allocations;
    TOptional<double> MeasuredMLUPS;
    FString MeasurementSource;
    TOptional<int64> AvailableBytes;
};

/** HOME4 requests are a value object; absent values never become invented solver defaults. */
struct FStudioHome4Spec
{
    int32 Version = 1;
    FString RecipeId, LineageId;
    FStudioHome4Units Units;
    FStudioHome4Reference Reference;
    FStudioHome4Fluids Fluids;
    FStudioHome4Geometry Geometry;
    FStudioHome4Lattice Lattice;
    FStudioHome4Zones Zones;
    FStudioHome4Multidomain Multidomain;
    FStudioHome4Run Run;
    FStudioHome4Performance Performance;
};
struct FStudioHome4Issue
{
    EStudioHome4IssueSeverity Severity = EStudioHome4IssueSeverity::Information;
    FString Field, Message;
};
struct FStudioHome4LevelPhysics
{
    int32 Depth = 0;
    double Scale = 1;
    TOptional<double> NuHeavy, NuLight, Sigma, Mobility, Gravity, Xi, TauHeavy, TauLight, SpongeStrength;
};
struct FStudioHome4Derived
{
    TOptional<double> Speed, RhoHeavy, RhoLight, Gravity, NuHeavy, NuLight, Sigma, Mobility, Xi;
    TOptional<double> Mach, Reynolds, Froude, Bond, Weber, Capillary, Peclet, Cahn, Atwood;
    TOptional<double> TauHeavy, TauLight, TauHeavyMargin, TauLightMargin, Knudsen, WakeWavelength;
    TOptional<uint64> RootCells, TotalCells, AllocationBytes;
    TOptional<double> EstimatedSeconds;
    TArray<FStudioHome4LevelPhysics> Levels;
    TArray<FStudioHome4Issue> Issues;
    bool HasBlockingIssues() const;
};
struct FStudioHome4DriverCommand
{
    TArray<FString> Argv;
    FString Display;
    TArray<FString> MissingContracts;
};
enum class EStudioHome4FieldType : uint8 { Number, Integer, Boolean, String, NumberVector, IntegerVector, NumberArray, IntegerArray };
/** Stable JSON binding contract for retained native editors. Numeric bounds are persistence bounds. */
struct FStudioHome4Field
{
    FString Section, Key, Page, Label, Unit, Help;
    EStudioHome4FieldType Type = EStudioHome4FieldType::Number;
    bool bOptional = true;
    double Minimum = -1.e12, Maximum = 1.e12;
    TArray<FString> Choices;
};

namespace StudioHome4Config
{
    const TArray<FStudioHome4Field>& Fields();
    /** Structural persistence validation allows incomplete and numerically infeasible drafts. */
    bool Validate(const FStudioHome4Spec& Spec, FString& Error);
    TSharedRef<FJsonObject> ToJSON(const FStudioHome4Spec& Spec);
    bool FromJSON(const TSharedPtr<FJsonObject>& Object, FStudioHome4Spec& Out, FString& Error);
    FString Serialize(const FStudioHome4Spec& Spec);
    bool Parse(const FString& Text, FStudioHome4Spec& Out, FString& Error);
    /** Fixed bounded UTF-8 original file read. Failed/changed imports retain Out. */
    bool Load(const FString& Path, FStudioHome4Spec& Out, FString& Error);
#if WITH_DEV_AUTOMATION_TESTS
    /** Exercise replacement/growth at the actual read/verification boundary without a timing race. */
    bool LoadWithReadBoundaryForAutomation(const FString& Path, FStudioHome4Spec& Out, FString& Error, TFunction<void()> BeforeVerify);
#endif
    FStudioHome4Derived Derive(const FStudioHome4Spec& Spec);
    /** Converts an actual quantity, never normalised p_star/p_d into physical pressure.
     * Nondimensional bases use heavy density, L and explicit reference time (or L/U).
     * Reference time is independent of U when a benchmark supplies t0/T explicitly. */
    TOptional<double> ConvertUnits(double Value, EStudioHome4Quantity Quantity,
        EStudioHome4UnitDisplay From, EStudioHome4UnitDisplay To, const FStudioHome4Spec& Spec);
    /** Produces appendix-supported arguments only. No process is launched, no shell is evaluated. */
    bool BuildHullDriverArgv(const FStudioHome4Spec& Spec, const FString& PythonExecutable,
        const FString& DriverPath, FStudioHome4DriverCommand& Out, FString& Error);
    FString ShellDisplay(const TArray<FString>& Argv);
}
