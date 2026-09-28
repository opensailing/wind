#pragma once

#include "Async/Future.h"
#include "StudioInspectionObjects.h"
#include "StudioPointRecording.h"

struct FStudioProbeMarkerQuery
{
    FGuid Id;
    int64 PointId = 0;
    bool operator==(const FStudioProbeMarkerQuery& Other) const { return Id==Other.Id&&PointId==Other.PointId; }
};
struct FStudioProbeMarkerRequest
{
    FGuid Project;
    FStudioInspectionSource Source;
    FVector Offset = FVector::ZeroVector;
    TSharedPtr<const FStudioPointGeometry,ESPMode::ThreadSafe> Geometry;
    TArray<FStudioProbeMarkerQuery> Queries;
    bool operator==(const FStudioProbeMarkerRequest& Other) const;
};
struct FStudioProbeMarkerResult
{
    FGuid Project;
    FStudioInspectionSource Source;
    TArray<FStudioProbeMarkerQuery> Queries;
    TMap<FGuid,FVector> Positions;
    uint64 Serial = 0;
    bool bCancelled = false;
    TOptional<FVector> Position(const FGuid& ProjectId,const FStudioProbeObject& Probe) const;
};

namespace StudioProbeMarkers
{
    /** One cancellable scan for all visible exact-ID probes. No field arrays,
     * interpolation, nearest-point substitution or frame I/O. */
    FStudioProbeMarkerResult Resolve(const FStudioProbeMarkerRequest& Request,const FStudioLoadCancellation& Cancellation = {});
}

/** One worker, one replaceable desired request. Static source geometry lets
 * markers survive playback/camera changes without retaining scalar frames. */
class FStudioProbeMarkerScheduler
{
public:
    ~FStudioProbeMarkerScheduler();
    void Submit(FStudioProbeMarkerRequest Request);
    void Tick();
    void Clear();
    const FStudioProbeMarkerResult* Result() const;
    uint64 StartedRequests() const { return Started; }
    bool HasPendingWork() const { return Pending.IsValid(); }
private:
    TOptional<FStudioProbeMarkerRequest> Desired,InFlight;
    TOptional<FStudioProbeMarkerResult> Completed;
    TFuture<FStudioProbeMarkerResult> Pending;
    FStudioLoadCancellation Cancellation;
    uint64 Started=0,Serial=0;
};
