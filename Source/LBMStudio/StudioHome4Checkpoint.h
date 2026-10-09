#pragma once
#include "CoreMinimal.h"
#include "StudioHome4Archive.h"
#include "StudioHome4Config.h"
#include "Async/Future.h"

class FStudioModel;
class FStudioHome4Session;

enum class EStudioHome4CheckpointState : uint8 { Unavailable, Inspecting, Rejected, GridMismatch, GridCompatible, Cancelled };
struct FStudioHome4CheckpointPatch
{
    FString Id, BodyId, AxisOrder;
    int32 Level = 0;
    FVector OriginXYZ = FVector::ZeroVector;
    FIntVector DimensionsXYZ = FIntVector::ZeroValue;
    bool bFollowBody = false;
    TArray<FString> RequiredMembers;
};
/** Frontend attestation only. This is not a solver STATE_KEYS or numerical load validation. */
struct FStudioHome4CheckpointInspection
{
    EStudioHome4CheckpointState State = EStudioHome4CheckpointState::Unavailable;
    FStudioHome4ArchiveSource Source;
    FString OriginalRunSpecJSON, AxisOrder, Error;
    TArray<FStudioHome4ArchiveMember> Members;
    TArray<FString> RequiredMembers, UndeclaredMembers;
    TOptional<FIntVector> DimensionsXYZ;
    TOptional<int32> OriginalStep;
    int32 LevelCount = 0;
    TArray<FStudioHome4CheckpointPatch> Patches;
    bool bCancelled = false;
};

/** Immutable lease on a privately copied checkpoint. Queue jobs must retain the lease.
 * The eventual adapter must verify SHA256 on its worker immediately before loading
 * PreparedPath and must enforce its actual STATE_KEYS, grid and nonfinite contract. */
class FStudioHome4PreparedCheckpoint final
{
public:
    FStudioHome4PreparedCheckpoint(FString InDirectory, FString InPreparedPath, FString InSpecIdentity,
        FStudioHome4CheckpointInspection InInspection);
    ~FStudioHome4PreparedCheckpoint();
    FStudioHome4PreparedCheckpoint(const FStudioHome4PreparedCheckpoint&) = delete;
    FStudioHome4PreparedCheckpoint& operator=(const FStudioHome4PreparedCheckpoint&) = delete;
    const FString& SourcePath() const { return Inspection.Source.Path; }
    const FString& PreparedPath() const { return Path; }
    const FString& SHA256() const { return Inspection.Source.SHA256; }
    const FString& SpecIdentity() const { return Identity; }
    FIntVector DimensionsXYZ() const { return Inspection.DimensionsXYZ.Get(FIntVector::ZeroValue); }
    const FStudioHome4CheckpointInspection& Metadata() const { return Inspection; }
private:
    const FString Directory, Path, Identity;
    const FStudioHome4CheckpointInspection Inspection;
};
struct FStudioHome4CheckpointPreparation
{
    FStudioHome4CheckpointInspection Inspection;
    TSharedPtr<const FStudioHome4PreparedCheckpoint, ESPMode::ThreadSafe> Lease;
};
namespace StudioHome4Checkpoints
{
    /** Explicit frontend extension on original run_spec, never inferred from viz fields:
     * frontendRestartAttestation={version:1,kind:"checkpoint",axisOrder:"zyx",
     * dimensionsXYZ:[Nx,Ny,Nz],componentAxis:"last",levelCount:1,patches:[],requiredMembers:[...],
     * scalarMembers:{rho:"...",ux:"...",uy:"...",uz:"...",phi:"...",solid:"..."},
     * tensorMembers:{S:"...",a3:"...",Jphi:"...",Pphi:"..."}}.
     * Each refined patch declares id/bodyId/level/originXYZ/followBody plus the
     * same grid/axes/member/family fields. IDs, origins, grids, levels and motion
     * match the exact next-run authored patch spec, including declared level counts.
     * Origins use source XYZ root cells; level d grid spacing is 2^(-d) root cells.
     * Tensor final component counts are 6,7,3,6 respectively. The producer declares
     * its full required state; absent attestation remains unavailable. Extra members
     * are retained with unknown restart status. Driver semantic validity is separate. */
    FStudioHome4CheckpointInspection Inspect(const FString& Path, const FStudioHome4Spec& NextSpec,
        const FStudioLoadCancellation& Cancellation = {});
    /** Worker-only: pin bounded bytes, check original identity, inspect the private copy. */
    FStudioHome4CheckpointPreparation Prepare(const FString& Path, const FStudioHome4Spec& NextSpec,
        const FStudioLoadCancellation& Cancellation = {});
#if WITH_DEV_AUTOMATION_TESTS
    FStudioHome4CheckpointPreparation PrepareWithCopyBoundaryForAutomation(const FString& Path, const FStudioHome4Spec& NextSpec,
        const FStudioLoadCancellation& Cancellation, TFunction<void()> BeforeOriginalVerify);
#endif
}

/** Shared owner-thread session. Tick once per workspace. Workers hold no UI/model
 * pointers; project, case, draft and cancellation generations reject stale results. */
class FStudioHome4CheckpointSession final
{
public:
    FStudioHome4CheckpointSession(TSharedPtr<FStudioModel> InModel, TSharedPtr<FStudioHome4Session> InEditor);
    ~FStudioHome4CheckpointSession();
    bool Start(const FString& Path, const FStudioHome4Spec& FrozenNextSpec, FString& Error);
    void Tick();
    void Cancel();
    bool IsBusy() const { return Pending.IsValid(); }
    FString Status();
    EStudioHome4CheckpointState State();
    const FStudioHome4CheckpointInspection* Metadata();
    /** Zero file IO. An exact currently scoped draft must match the inspected request. */
    bool ValidateForSubmit(const FStudioHome4Spec& Spec,
        TSharedPtr<const FStudioHome4PreparedCheckpoint, ESPMode::ThreadSafe>& OutLease, FString& Error);
private:
    void Scope();
    void Invalidate(const FString& Reason);
    TWeakPtr<FStudioModel> Model;
    TWeakPtr<FStudioHome4Session> Editor;
    FGuid ProjectId, CaseId;
    FString DraftIdentity, Notice;
    uint64 Generation = 0, PendingGeneration = 0;
    EStudioHome4CheckpointState CurrentState = EStudioHome4CheckpointState::Unavailable;
    FStudioLoadCancellation Cancellation;
    TFuture<FStudioHome4CheckpointPreparation> Pending;
    TOptional<FStudioHome4CheckpointInspection> Inspected;
    TSharedPtr<const FStudioHome4PreparedCheckpoint, ESPMode::ThreadSafe> Prepared;
};
