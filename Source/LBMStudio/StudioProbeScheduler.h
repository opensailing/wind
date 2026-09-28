#pragma once

#include "Async/Future.h"
#include "StudioProbeSampling.h"

/** Owned by the game thread. One worker plus one replaceable latest request;
 * no queued work per playback tick, UObject access or source reads in the UI.
 * The worker owns only its immutable field and cancellation flag. */
class FStudioProbeScheduler
{
public:
    FStudioProbeScheduler() = default;
    ~FStudioProbeScheduler();
    FStudioProbeScheduler(const FStudioProbeScheduler&) = delete;
    FStudioProbeScheduler& operator=(const FStudioProbeScheduler&) = delete;

    void Submit(FStudioProbeRequest Request);
    void Tick();
    /** Immediately removes visible values; pending work cooperatively cancels. */
    void Clear();
    /** Cancels and joins owned work. Further submissions are ignored. */
    void Shutdown();
    const FStudioProbeResult* Result() const;
    bool HasPendingWork() const { return Pending.IsValid(); }
    uint64 StartedRequests() const { return Started; }
    uint64 CancelledRequests() const { return Cancelled; }
    uint64 DiscardedResults() const { return Discarded; }

private:
    void CancelPending();
    TOptional<FStudioProbeRequest> Desired, InFlight;
    TOptional<FStudioProbeResult> Completed;
    TFuture<FStudioProbeResult> Pending;
    FStudioLoadCancellation Cancellation;
    uint64 Started = 0, Cancelled = 0, Discarded = 0;
    bool bShutdown = false;
};
