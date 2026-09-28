#pragma once
#include "StudioProject.h"

/** Restoration is transactional: both verified sources and original fields, or
 * no sources/fields. It never changes the live Solve model or substitutes data. */
struct FStudioComparisonRestoreResult
{
    FGuid ProjectId;
    FStudioSavedComparison Saved;
    TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> PrimarySource,SecondarySource;
    TOptional<FStudioComparisonResult> Pair;
    FString Error;
    bool bCancelled=false;
    bool Matches(const FGuid& CurrentProject,const FStudioSavedComparison& Current) const;
};

namespace StudioSavedComparisons
{
    constexpr int32 MaxEntries=64;
    bool IsValid(const FStudioSavedComparison& Saved,FString& Error);
    bool IsValid(const TArray<FStudioSavedComparison>& Saved,FString& Error);
    bool Equals(const FStudioSavedComparison& A,const FStudioSavedComparison& B);
    bool Equals(const TArray<FStudioSavedComparison>& A,const TArray<FStudioSavedComparison>& B);
    /** Freeze a verified pair and its independent poses. The source reference
     * must describe the same data/reconstruction that actually produced it. */
    bool Create(const FString& Name,const FStudioComparisonResult& Pair,
        const FStudioCameraState& PrimaryCamera,const FStudioCameraState& SecondaryCamera,
        bool bSharedRange,const TArray<FStudioRecordingReference>& References,
        FStudioSavedComparison& Out,FString& Error);
    TArray<TSharedPtr<FJsonValue>> ToJSON(const TArray<FStudioSavedComparison>& Saved);
    bool FromJSON(const TArray<TSharedPtr<FJsonValue>>& Array,TArray<FStudioSavedComparison>& Out,FString& Error);
    int64 StoredBytes(const TArray<FStudioSavedComparison>& Saved);
    /** Worker-only. Current matching reference paths may repair a relocation,
     * but stored source/reconstruction hashes and original frames remain fixed. */
    FStudioComparisonRestoreResult Restore(const FGuid& ProjectId,const FStudioSavedComparison& Saved,
        const TArray<FStudioRecordingReference>& CurrentReferences,const FStudioLoadCancellation& Cancellation={});
}

/** One worker and no queued requests. Cancel also wins after completion before
 * Poll. Shutdown joins and releases the worker's source/snapshot ownership. */
class FStudioComparisonRestoreTask
{
public:
    ~FStudioComparisonRestoreTask();
    bool Start(FGuid ProjectId,FStudioSavedComparison Saved,TArray<FStudioRecordingReference> References,FString& Error);
    void Cancel();
    void Shutdown();
    bool IsBusy() const {return Pending.IsValid();}
    TOptional<FStudioComparisonRestoreResult> Poll();
private:
    TFuture<FStudioComparisonRestoreResult> Pending;
    FStudioLoadCancellation Cancellation;
    bool bShutdown=false;
};
