#pragma once
#include "CoreMinimal.h"
#include "StudioHome4Telemetry.h"
#include "Async/Future.h"
#include <atomic>

class FStudioModel;
/** Source coordinate values are retained verbatim. Unit names are explicit;
 * absence of a unit never borrows the current editable case. */
struct FStudioHome4SpatialValue { TOptional<double> Value; FString Unit; };
struct FStudioHome4SpatialMask { FString Kind, FieldId; TOptional<int64> Count; };
struct FStudioHome4SpatialPatch
{
    FString Id;
    int32 Level=0;
    TOptional<FVector> Origin, Spacing;
    TOptional<FIntVector> Extents;
    TOptional<int64> CellCount;
    TArray<FStudioHome4SpatialMask> Masks;
    TOptional<FBox> Bounds() const;
};
struct FStudioHome4SpatialLevel
{
    int32 Level=0;
    FStudioHome4SpatialValue Nu, Sigma, Mobility, Gravity, Tau, SpongeStrength;
    FStudioHome4SpatialValue BandDepth, Overlap, RestrictionMargin;
    TOptional<int64> Substeps;
    FStudioHome4Work Work;
    TOptional<bool> TauFloor, RootPhaseFree, EvenWrap;
    FString SneqMode;
};
struct FStudioHome4SpatialZone
{
    FString Id, Kind, ProfileAxis, ProfileCoordinateUnit, ProfileValueUnit;
    TOptional<FVector> Minimum, Maximum;
    TArray<double> ProfileX, ProfileValues;
    TMap<int32, FStudioHome4SpatialValue> LevelStrengths;
};
struct FStudioHome4SpatialGeometry
{
    FString BodyId, CADSource, SDFBackend, TessellationStatus, CacheStatus, CutLinkStatus, FlotationStatus;
    TOptional<int64> TessellationTriangles, CutLinks;
    TOptional<double> CutLinkFraction;
    TOptional<FVector> BoundsMinimum, BoundsMaximum;
    FStudioHome4SpatialValue EquilibriumHeave, RunningHeave, Trim, Draft, Buoyancy, Weight;
    FString ReferenceSource;
};
struct FStudioHome4SpatialEvidence
{
    FGuid RunId;
    /** Session attachment only; never imported from or written into source JSON. */
    TOptional<FGuid> AttachedProjectId, AttachedCaseId;
    FString CoordinateUnit, SourceId, SourcePath, SourceSHA256;
    TArray<uint8> OriginalBytes;
    TArray<FStudioHome4SpatialPatch> Patches;
    TArray<FStudioHome4SpatialLevel> Levels;
    TArray<FStudioHome4SpatialZone> Zones;
    TArray<FStudioHome4SpatialGeometry> Geometry;
    FString GateStatus() const { return TEXT("Recipe validation not evaluated"); }
};
struct FStudioHome4SpatialLocation
{
    FGuid RunId;
    FString SourceId, SourceSHA256, PatchId;
    int32 Level=0;
    FIntVector OriginalCell=FIntVector::ZeroValue;
};
namespace StudioHome4SpatialDiagnostics
{
    /** Strict schema LBMStudio.Home4SpatialDiagnostics v1. Root run_id GUID,
     * axis_order XYZ, coordinate_unit and source_id identify all optional arrays.
     * patches[] {id,level,origin,spacing,extents,cell_count,masks[{kind,field_id,count}]};
     * levels[] {level,nu/sigma/mobility/gravity/tau/sponge_strength/band_depth/overlap/
     * restriction_margin:{value,unit},substeps,performance,tau_floor,root_phase_free,
     * even_wrap,sneq_mode}; zones[] {id,kind,min,max,profile:{axis,coordinate_unit,
     * value_unit,x,values},level_strengths:[{level,value,unit}]}; geometry[] has
     * explicit adapter status, bounds, counts/fraction and flotation values.
     * Optional nested run_id must equal root. No grids are synthesized or stitched. */
    bool Parse(const FString& JSON,const TOptional<FGuid>& ExpectedRun,FStudioHome4SpatialEvidence& Out,FString& Error);
    bool Load(const FString& Path,const TOptional<FGuid>& ExpectedRun,FStudioHome4SpatialEvidence& Out,FString& Error,
        const TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe>& Cancel={});
    /** Recheck retained bytes/hash/run/source before an export; never reread a changed source path. */
    bool VerifyOriginal(const FStudioHome4SpatialEvidence& Evidence,FStudioHome4SpatialEvidence& Out,FString& Error);
#if WITH_DEV_AUTOMATION_TESTS
    bool LoadWithVerificationForAutomation(const FString& Path,const TOptional<FGuid>& ExpectedRun,FStudioHome4SpatialEvidence& Out,FString& Error,TFunction<void()> BeforeVerify);
#endif
    bool Locate(const FStudioHome4SpatialEvidence& Evidence,const FString& PatchId,const FIntVector& Cell,FStudioHome4SpatialLocation& Out,FString& Error);
}
/** Shared session controller: multiple native views use one identified import.
 * Workers own file access and bounded candidate data; publication is game-thread
 * Poll only. Project/case switches cancel and clear evidence before attachment. */
class FStudioHome4SpatialSession
{
public:
    explicit FStudioHome4SpatialSession(TSharedPtr<FStudioModel> Model={});
    ~FStudioHome4SpatialSession();
    bool BeginImport(const FString& Path,const TOptional<FGuid>& ExpectedRun={});
    void Cancel();
    void Poll();
    bool IsImporting() const { return Pending.IsValid(); }
    const TSharedPtr<const FStudioHome4SpatialEvidence,ESPMode::ThreadSafe>& Evidence() { Scope(); return Current; }
    FString Status() const { return Message; }
private:
    void Scope();
    struct FResult { TSharedPtr<FStudioHome4SpatialEvidence,ESPMode::ThreadSafe> Evidence; FString Error; };
    TWeakPtr<FStudioModel> Owner;
    FGuid ProjectId,CaseId,ImportProjectId,ImportCaseId;
    TSharedPtr<const FStudioHome4SpatialEvidence,ESPMode::ThreadSafe> Current;
    FString Message=TEXT("Import original spatial diagnostics to inspect measured patches, zones and adapter status.");
    TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe> Cancellation;
    TFuture<FResult> Pending;
};
