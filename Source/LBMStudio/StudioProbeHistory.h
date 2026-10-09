#pragma once

#include "StudioProbeSampling.h"
#include "Async/Future.h"

class IStudioSolver;

/** Frozen source, probe and inclusive ordinal window. Scalar is resolved when
 * requested, including probes that normally follow the viewport scalar. */
struct FStudioProbeHistoryRequest
{
    FGuid ProjectId;
    FStudioProbeObject Probe;
    FString Scalar;
    int32 FirstOrdinal=0,LastOrdinal=0;
    TSharedPtr<const IStudioSolver,ESPMode::ThreadSafe> Source;
};

struct FStudioProbeHistoryFrame
{
    int32 Ordinal=INDEX_NONE;
    FStudioFrame Frame;
    TArray<FStudioProbeSample> Samples;
};

enum class EStudioProbeHistoryStatus : uint8 { Complete, InvalidRequest, ReadFailed, IdentityMismatch, Cancelled };

/** Completed immutable history owns copied samples, never recorded-frame arrays.
 * A failed/cancelled operation publishes no partial history. Spatial gaps retain
 * their original status; read/integrity failures are operation errors, not gaps. */
struct FStudioProbeHistoryResult
{
    FGuid ProjectId;
    FStudioProbeObject Probe;
    FString Scalar,Label,Unit,Origin,Method,SourceTitle,SourceURL,TimeNote,Error;
    int32 FirstOrdinal=0,LastOrdinal=0,FailedOrdinal=INDEX_NONE;
    EStudioProbeHistoryStatus Status=EStudioProbeHistoryStatus::InvalidRequest;
    TWeakPtr<const IStudioSolver,ESPMode::ThreadSafe> SampledSource;
    TOptional<FStudioFieldIdentity> Identity;
    TArray<FStudioProbeHistoryFrame> Frames;
    bool Matches(const FStudioProbeHistoryRequest& Current) const;
};

namespace StudioProbeHistory
{
    constexpr int32 MaxFrames=100000;
    constexpr int64 MaxSamples=1000000;
    /** Bounded metadata validation only; does not read fields or change a cursor. */
    bool Validate(const FStudioProbeHistoryRequest& Request,FString& Error);
    /** Worker-only, sequential original frames. One immutable field at a time;
     * existing reader budgets still apply. Completion count is advisory progress. */
    FStudioProbeHistoryResult Evaluate(const FStudioProbeHistoryRequest& Request,
        const FStudioLoadCancellation& Cancellation={},std::atomic<int32>* CompletedFrames=nullptr);
}

/** One explicit operation, with no per-tick retry or growing request queue.
 * Caller compares the result to current project/probe/source before publication.
 * Cancellation and shutdown cooperatively drain owned work. */
class FStudioProbeHistoryTask
{
public:
    ~FStudioProbeHistoryTask();
    bool Start(FStudioProbeHistoryRequest Request,FString& Error);
    void Cancel();
    void Shutdown();
    bool IsBusy() const { return Pending.IsValid(); }
    int32 CompletedFrames() const { return Progress?Progress->load(std::memory_order_relaxed):0; }
    int32 TotalFrames() const { return Total; }
    TOptional<FStudioProbeHistoryResult> Poll();
private:
    TFuture<FStudioProbeHistoryResult> Pending;
    FStudioLoadCancellation Cancellation;
    TSharedPtr<std::atomic<int32>,ESPMode::ThreadSafe> Progress;
    int32 Total=0;
    bool bShutdown=false;
};
