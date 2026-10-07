#pragma once
#include "CoreMinimal.h"
#include "StudioHome4Config.h"

/** HOME4 science records, independent of job command/state acknowledgements.
 * The owning session supplies identity; the proposed driver JSONL has no run,
 * state sequence or wall clock envelope. No solver or job controller is mocked here. */
struct FStudioHome4Source
{
    FGuid RunId;
    FString SourceId;
};
/** Owner attachment for reports, separate from science records. An independently
 * assigned replay GUID is never an original solver-run identity. */
struct FStudioHome4TelemetryProvenance
{
    FGuid StreamRunId;
    TOptional<FGuid> OriginalRunId;
    bool bImportedReplay=false;
    FString SourceId, SourcePath, SourceSHA256;
    bool bCapturedPrefix=false, bCaptureCoversDisplayedData=true;
    int64 CapturedByteCount=0;
    TOptional<FGuid> AttachedProjectId, AttachedCaseId;
};

struct FStudioHome4CellFacts
{
    TOptional<FIntVector> Cell;
    TOptional<int32> Level;
    TOptional<double> Phi, Tau;
    TOptional<bool> Limiter, ForceThreshold, InBand, InSponge, InBeach, InCutLinkShell;
    FString Zone;
    FString PatchId; // Explicit original locator.patch_id; level alone may be ambiguous.
};

struct FStudioHome4MassLedger
{
    TOptional<double> PhiDrift, Injected;
    TArray<TOptional<double>> LevelDrifts, LevelInjections;
    // Change in |injected| between adjacent original measurements; missing
    // correction samples interrupt the comparison rather than inheriting it.
    TOptional<double> InjectionMagnitudeChange;
    TArray<TOptional<double>> LevelInjectionMagnitudeChanges;
};

/** Terms have the driver's units and time interval. Missing terms are unavailable.
 * Closure is W = D_near + D_far + D_air + Z_beach + Z_floor + dKE + dPE + res. */
struct FStudioHome4EnergyBudget
{
    TOptional<double> Work, DissipationNear, DissipationFar, DissipationAir;
    TOptional<double> BeachLoss, FloorLoss, DeltaKE, DeltaPE, Residual;
    bool IsComplete() const;
};

struct FStudioHome4Forces
{
    TOptional<double> Fx, Fy, Fz, My;
    TOptional<double> PressureFx, PressureFy, PressureFz;
    TOptional<double> ViscousFx, ViscousFy, ViscousFz;
    TOptional<double> MomentumFx, MomentumFy, MomentumFz, MomentumMy;
    TOptional<double> PressureMy, ViscousMy;
};

struct FStudioHome4Window
{
    TOptional<double> Fx, Fy, Fz, My;
    TOptional<double> PreviousFx, PreviousFy, PreviousFz, PreviousMy;
    TOptional<double> Start, End, PreviousStart, PreviousEnd;
    TOptional<double> AverageLength;
    FString AbscissaUnit, Epoch;
};

/** Optional measured work extension. Counts include actual level substeps.
 * elapsed_seconds and node_updates cover one window; cumulative_* cover this run.
 * transferred_bytes is supplied from the allocation/transfer accounting, never
 * inferred from a nominal bytes-per-node constant or a device peak. */
struct FStudioHome4Work
{
    TOptional<double> ElapsedSeconds, NodeUpdates, TransferredBytes;
    TOptional<double> CumulativeElapsedSeconds, CumulativeNodeUpdates;
};

/** Divisors already expressed in the original force/moment units. No benchmark
 * formula, body mass, projected area or draft is substituted for missing data. */
struct FStudioHome4Normalization
{
    TOptional<double> ForceDivisor, MomentDivisor;
    FString ForceLabel, MomentLabel;
};
/** Optional immutable original measurement conventions, independent of current case.
 * JSON source_metadata: force_units/energy_units/velocity_units/length_units are
 * lattice|physical|nondimensional. unit_map holds dx_m, dt_s, rho_kg_m3, rho_lattice,
 * length_cells, time_steps, speed_cells_step; conversions require their actual
 * dimensions. normalization/body_normalizations hold positive divisors and labels.
 * Optional declared_levels identifies expected root/MD ledger levels. Divergence
 * convention/unit/domain and device_peak_gbps/source retain their original basis.
 * A kind:source_metadata record supplies conventions without a numerical sample. */
struct FStudioHome4SourceMetadata
{
    TOptional<EStudioHome4UnitDisplay> ForceUnits, EnergyUnits, VelocityUnits, LengthUnits;
    FStudioHome4Spec UnitMap;
    FStudioHome4Normalization Normalization;
    TMap<FString, FStudioHome4Normalization> BodyNormalizations;
    FString DivergenceConvention, DivergenceUnit, DivergenceDomain;
    TOptional<double> DevicePeakGBps;
    FString DevicePeakSource;
    TArray<int32> DeclaredLevels;
    bool Equivalent(const FStudioHome4SourceMetadata& Other) const;
};
struct FStudioHome4Histogram
{
    TArray<double> BinEdges;
    TArray<int64> Counts;
    TOptional<double> PhiMinimum,PhiMaximum,ExpectedXi;
    FString ThicknessUnit,SamplingSource;
};
struct FStudioHome4PhaseEnergy
{
    TOptional<double> KE, PE, Surface;
};
struct FStudioHome4LevelMeasurement
{
    int32 Level = 0;
    TOptional<double> MassDrift, Injection, InjectionMagnitudeChange, ReportedMLUPS;
    FStudioHome4Work Work;
};
/** Optional bodies[] extension. state vectors use the source axes; attitude_deg
 * is roll/pitch/yaw in degrees. attitude heave values use declared length units.
 * Fits and stiffness retain explicit raw units and identified reference source;
 * neither their presence nor a reference value establishes agreement. */
struct FStudioHome4BodyMeasurement
{
    FString Id, Name, IntegratorStatus, ReferenceSource, AngularVelocityUnit;
    FStudioHome4Forces Forces;
    FStudioHome4Window Window;
    TOptional<FVector> Position, Velocity, AngularVelocity;
    TOptional<FVector> AttitudeDegrees, EquilibriumAttitudeDegrees, RunningAttitudeDegrees, ReferenceAttitudeDegrees;
    TOptional<double> EquilibriumHeave, RunningHeave, ReferenceHeave;
    TOptional<double> K33, K35, K55, AddedMass, Damping, ReferenceAddedMass, ReferenceDamping;
    FString StiffnessUnit, AddedMassUnit, DampingUnit, FitReferenceSource;
    FString K33Unit,K35Unit,K55Unit,StiffnessConvention;
    TOptional<int64> RetabulationEvery;
    FStudioHome4Work RetabulationWork;
    TOptional<double> QuasiStaticHeave,QuasiStaticPitchDegrees,FitFrequency;
    FString QuasiStaticMethod,QuasiStaticSource,FitMethod,FitFrequencyUnit;
    TOptional<double> FitWindowStart,FitWindowEnd;
    FString FitWindowUnit,FitEpoch;
};

struct FStudioHome4Sample
{
    FStudioHome4Source Source;
    uint64 RecordIndex = 0; // Local ingestion order, not a solver/state sequence.
    TSharedPtr<const FStudioHome4SourceMetadata> Metadata;
    TOptional<int64> Step;
    TOptional<double> LatticeTime, PhysicalTime, DimensionlessTime;
    FString Backend;
    TOptional<double> ReportedMLUPSInstant, ReportedMLUPSCumulative;
    TOptional<double> Mach, TauMinimum, MaximumSpeed, DivergenceNorm;
    TOptional<FIntVector> MaximumSpeedCell;
    FStudioHome4MassLedger Mass;
    TOptional<double> WaterKE, AirKE, SurfaceEnergy;
    FStudioHome4Forces Forces;
    FStudioHome4Window Window;
    TOptional<int64> LimiterCells, ThresholdCells;
    FStudioHome4EnergyBudget Budget;
    TMap<FString, FStudioHome4EnergyBudget> PhaseBudgets;
    TMap<FString, FStudioHome4PhaseEnergy> PhaseEnergies;
    FStudioHome4Histogram InterfaceThickness;
    TOptional<double> SpuriousSpeed;
    FString SpuriousMask,SpuriousUnit,SpuriousReferenceSource;
    TOptional<bool> SpuriousForcingFree,SpuriousAtRest;
    TOptional<double> SpuriousReferenceSpeed,SpuriousAbsoluteTolerance;
    TArray<FStudioHome4LevelMeasurement> Levels;
    TArray<FStudioHome4BodyMeasurement> Bodies;
    FStudioHome4Work Work;
    TOptional<bool> RestFullGravity;
    TOptional<bool> RestCondition; // Only explicit wb_rest.at_rest enables the gate.
    TOptional<double> RestMaxDynamicPressure;
    FStudioHome4CellFacts Trouble;
    bool bNonfinite = false;
};

enum class EStudioHome4OutputKind : uint8 { Trace, Slice, Visualization, Restart };
struct FStudioHome4OutputEvent
{
    FStudioHome4Source Source;
    uint64 RecordIndex = 0;
    TOptional<int64> Step;
    EStudioHome4OutputKind Kind = EStudioHome4OutputKind::Trace;
    FString Path; // Driver reported output; file existence is the owner's responsibility.
};

/** A guard asks the owner to act. It never certifies a stop or a new checkpoint.
 * LastGoodStep and RestartPath refer only to retained original measurements/events. */
struct FStudioHome4ActionRequest
{
    TOptional<int64> Step; // Exact triggering original step, retained after history eviction.
    FStudioHome4Source Source;
    uint64 RecordIndex = 0;
    bool bStop = true, bCheckpointLastGoodState = false, bLocateCell = false;
    TOptional<int64> LastGoodStep;
    TOptional<FString> RestartPath;
    FStudioHome4CellFacts Facts;
    FString Reason;
};

enum class EStudioHome4Health : uint8 { Unavailable, Healthy, Warning };
struct FStudioHome4HealthSignal
{
    FString Id, Label;
    EStudioHome4Health Status = EStudioHome4Health::Unavailable;
    TOptional<double> Value, Threshold;
    FString Reason, Remedy;
};

struct FStudioHome4DiagnosticPolicy
{
    FString BodyId; // Empty selects original aggregate forces/windows, never another body.
    // The mass gate is fixed by the design note: |drift| < 1e-4.
    TOptional<double> BudgetAbsoluteTolerance;
    TOptional<double> ForceRelativeTolerance, ForceAbsoluteTolerance, ForceReferenceMagnitude;
    TOptional<double> WindowRelativeTolerance, WindowAbsoluteTolerance, WindowReferenceMagnitude;
    TOptional<double> RestPressureTolerance;
    TOptional<double> MaximumSpeedTrigger;
    // Select a component explicitly; the proposed schema only supplies mea_Fx.
    enum class EComponent : uint8 { Fx, Fy, Fz, My };
    EComponent ForceComponent = EComponent::Fx, WindowComponent = EComponent::Fx;
};

struct FStudioHome4MeasuredPerformance
{
    TOptional<double> MLUPSInstant, MLUPSCumulative, GigabytesPerSecond;
};

class FStudioHome4Diagnostics
{
public:
    /** Five signals in mass, budget, forces, steady-window, WB-rest order. */
    static TArray<FStudioHome4HealthSignal> Evaluate(const FStudioHome4Sample& Sample,
        const FStudioHome4DiagnosticPolicy& Policy);
    static FStudioHome4MeasuredPerformance Performance(const FStudioHome4Sample& Sample);
    static TOptional<FStudioHome4ActionRequest> TroubleAction(const FStudioHome4Sample& Sample,
        const FStudioHome4DiagnosticPolicy& Policy);
};

struct FStudioHome4TailLimits
{
    int32 MaxBytesPerAppend = 65536, MaxLinesPerAppend = 64, MaxLineBytes = 16384;
    int32 MaxHistory = 240, MaxOutputEvents = 240, MaxActionRequests = 64;
};

struct FStudioHome4TailResult
{
    int32 ConsumedBytes = 0, CompleteLines = 0, Accepted = 0, Malformed = 0, Unknown = 0;
    int32 Oversized = 0, Regressing = 0;
};

/** Bounded UTF-8 JSONL tail. AppendBytes returns the consumed prefix; the owner
 * retains/retries any suffix. Partial lines are retained up to MaxLineBytes.
 * ResetTail discards a partial line after file truncation/replacement but keeps
 * this run's monotonic baselines and retained history. BeginRun explicitly starts
 * a new run and clears all baselines. Missing values never inherit old values.
 * Unknown JSON fields are ignored; known fields require strict finite types/ranges.
 * No file IO, control acknowledgement, automatic restart, or fake CFD data. */
class FStudioHome4TelemetryStream
{
public:
    explicit FStudioHome4TelemetryStream(const FStudioHome4TailLimits& InLimits = {});
    bool BeginRun(const FStudioHome4Source& InSource);
    void ResetTail();
    FStudioHome4TailResult AppendBytes(const uint8* Bytes, int32 NumBytes);
    const TArray<FStudioHome4Sample>& History() const { return Samples; }
    const TArray<FStudioHome4OutputEvent>& OutputEvents() const { return Outputs; }
    const TArray<FStudioHome4ActionRequest>& ActionRequests() const { return Actions; }
    const TOptional<FStudioHome4Sample>& Latest() const { return LatestSample; }
    const TOptional<FStudioHome4Sample>& LastGoodSample() const { return GoodSample; }
    const TOptional<FStudioHome4OutputEvent>& LastRestart() const { return Restart; }
    const TSharedPtr<const FStudioHome4SourceMetadata>& OriginalMetadata() const { return SourceMetadata; }
    int32 BufferedBytes() const { return Pending.Num(); }
    void SetDiagnosticPolicy(const FStudioHome4DiagnosticPolicy& InPolicy) { Policy = InPolicy; }
private:
    enum class ELineResult : uint8 { Accepted, Malformed, Unknown, Regressing };
    ELineResult ParseLine();
    FStudioHome4TailLimits Limits;
    FStudioHome4Source Source;
    FStudioHome4DiagnosticPolicy Policy;
    bool bActive = false, bDiscardLine = false;
    uint64 RecordIndex = 0;
    TArray<uint8> Pending;
    TArray<FStudioHome4Sample> Samples;
    TArray<FStudioHome4OutputEvent> Outputs;
    TArray<FStudioHome4ActionRequest> Actions;
    TOptional<FStudioHome4Sample> LatestSample, GoodSample;
    TOptional<FStudioHome4OutputEvent> Restart;
    TOptional<int64> LastStep;
    TOptional<double> LastLatticeTime, LastPhysicalTime, LastDimensionlessTime;
    TOptional<double> LastCumulativeElapsed, LastCumulativeUpdates;
    TMap<int32, FStudioHome4Work> LastLevelWork;
    TSharedPtr<const FStudioHome4SourceMetadata> SourceMetadata;
};
