#pragma once
#include "CoreMinimal.h"
#include "StudioHome4Config.h"

class FJsonObject;

/** Authoring values use SI units. Unset properties remain unknown, not zero. */
struct FStudioMaterial
{
    FGuid Id = FGuid::NewGuid();
    FString Name = TEXT("Fluid");
    bool bSolid = false;
    TOptional<double> Density;
    TOptional<double> KinematicViscosity;
    TOptional<double> ThermalConductivity;
    TOptional<double> SpecificHeat;
};

struct FStudioSurfacePatch
{
    FGuid Id = FGuid::NewGuid();
    FString Name = TEXT("Surface");
};

/** References an asset; large mesh buffers never live in the project JSON. */
struct FStudioGeometryAsset
{
    FGuid Id = FGuid::NewGuid();
    FString Name = TEXT("Geometry");
    FString SourcePath;
    FString SourceSHA256;
    FString Format = TEXT("stl");
    double MetersPerSourceUnit = 1;
    FVector Translation = FVector::ZeroVector;
    FQuat Rotation = FQuat::Identity;
    FVector Scale = FVector::OneVector;
    FGuid MaterialId;
    TArray<FStudioSurfacePatch> Patches;
};

struct FStudioDomain
{
    FGuid Id = FGuid::NewGuid();
    // Initial authoring box, independent of any recording's computational domain.
    FVector Min = FVector(-1,-1,-1);
    FVector Max = FVector(1,1,1);
    FGuid FluidMaterialId;
    // Fixed order: -X, +X, -Y, +Y, -Z, +Z. IDs survive changes to bounds.
    TArray<FGuid> Faces;
    TArray<FString> FaceNames;
    FStudioDomain();
};

enum class EStudioBoundaryType : uint8 { Unassigned, VelocityInlet, PressureOutlet, NoSlip, Slip, Symmetry, Periodic };
struct FStudioBoundaryCondition
{
    FGuid Id = FGuid::NewGuid();
    FString Name = TEXT("Boundary");
    FGuid TargetId; // Domain face or imported surface patch.
    FGuid PairedTargetId; // Reciprocal periodic partner, otherwise invalid.
    EStudioBoundaryType Type = EStudioBoundaryType::Unassigned;
    TOptional<FVector> Velocity;
    TOptional<double> Pressure;
    TOptional<double> Temperature;
};

struct FStudioCaseSetup
{
    TOptional<FVector> InletVelocity;
    TOptional<double> OutletPressure;
    TOptional<double> ReferenceLength;
    TOptional<double> ReferenceDensity;
    TOptional<double> ReynoldsNumber;
    TOptional<double> RelaxationTime;
    TOptional<double> TimeStep;
    TOptional<double> CFLTarget;
    FIntVector LatticeResolution = FIntVector(256,128,128);
    // Solver-owned identifiers; empty until a backend/model is selected.
    FString BackendId;
    FString CollisionModel;
    FString TurbulenceModel;
    bool bThermal = false;
    int64 MaxSteps = 2000000;
    TOptional<double> MaxPhysicalTime;
    int64 OutputInterval = 500;
    bool bCheckpoints = false;
    int64 CheckpointInterval = 10000;
};

struct FStudioCaseDraft
{
    FGuid Id = FGuid::NewGuid();
    FString Name = TEXT("Untitled case");
    int64 Revision = 0;
    FStudioDomain Domain;
    TArray<FStudioGeometryAsset> Geometry;
    TArray<FStudioMaterial> Materials;
    TArray<FStudioBoundaryCondition> Boundaries;
    FStudioCaseSetup Setup;
    // Optional solver-specific requests. Existing cases do not acquire invented HOME4 physics.
    TOptional<FStudioHome4Spec> Home4;
};

enum class EStudioRunOrigin : uint8 { PublishedRecording, ControlHarness, Solver, ImportedRecording };

/** Original recording identity only. No editable next-run properties are copied here. */
struct FStudioRecordedRunProvenance
{
    FString RunId, RecipeId, LineageId, ManifestPath, ManifestSHA256;
    TArray<FString> OriginalArchives;
    FString OriginalTag,OriginalBackend,MeasurementSource,MeasurementSHA256,GateStatus,GateSourceSHA256;
    FString ParentRunId,ParentSpecSHA256;
    TOptional<double> MeasuredMLUPS;
    TOptional<FStudioHome4Spec> OriginalRunSpec; // Complete strict source request only; never editable next-run context.

};

/** Value object with no mutators; configuration is a deep, const snapshot. */
class FStudioRunRecord
{
public:
    FGuid GetId() const { return Id; }
    const FString& GetName() const { return Name; }
    EStudioRunOrigin GetOrigin() const { return Origin; }
    const FString& GetBackendId() const { return BackendId; }
    const FString& GetDatasetId() const { return DatasetId; }
    const FStudioCaseDraft* GetConfiguration() const { return Configuration.Get(); }
    const TOptional<FStudioRecordedRunProvenance>& GetProvenance() const { return Provenance; }
    FStudioRunRecord WithProvenance(const FStudioRecordedRunProvenance& Source) const;
    /** Recording import deliberately has no invented case configuration. */
    static FStudioRunRecord Recording(const FString& Name, const FString& DatasetId, bool bPublished = true);
    /** Captures settings only. Does not claim a job was started or computed fields. */
    static FStudioRunRecord Capture(const FString& Name, const FStudioCaseDraft& Draft, EStudioRunOrigin Origin);
    TSharedRef<FJsonObject> ToJSON() const;
    static bool FromJSON(const TSharedPtr<FJsonObject>& Object, FStudioRunRecord& Out, FString& Error);
private:
    TOptional<FStudioRecordedRunProvenance> Provenance;
    FGuid Id = FGuid::NewGuid();
    FString Name;
    EStudioRunOrigin Origin = EStudioRunOrigin::PublishedRecording;
    FString BackendId;
    FString DatasetId;
    TSharedPtr<const FStudioCaseDraft> Configuration;
};

namespace StudioCaseIO
{
    TSharedRef<FJsonObject> ToJSON(const FStudioCaseDraft& Draft);
    bool FromJSON(const TSharedPtr<FJsonObject>& Object, FStudioCaseDraft& Out, FString& Error);
    FString Serialize(const FStudioCaseDraft& Draft);
    /** Structural persistence validation only, never a claim of solver readiness. */
    bool Validate(const FStudioCaseDraft& Draft, FString& Error);
}
