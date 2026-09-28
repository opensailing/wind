#pragma once

#include "StudioRecording.h"
#include "Async/Future.h"

class IStudioSolver;
class IStudioField;

enum class EStudioTimeAlignment : uint8 { Unset, RecordedTime, ElapsedFromStart, ManualOffset };
enum class EStudioTimeMatch : uint8 { Exact, Nearest };

/** Alignment is an explicit interpretation of the two original timelines.
 * Manual offset uses B's recorded time + offset = A's recorded time.
 * Nearest matching never extrapolates beyond B's aligned time range; an exact
 * tie selects the earlier original frame. No temporal interpolation occurs. */
struct FStudioComparisonAlignment
{
    EStudioTimeAlignment Mode=EStudioTimeAlignment::Unset;
    EStudioTimeMatch Match=EStudioTimeMatch::Exact;
    double SecondaryOffsetSeconds=0;
    double MaximumMismatchSeconds=0;
    bool operator==(const FStudioComparisonAlignment& Other) const;
};

enum class EStudioComparisonStatus : uint8
{ Ready, InvalidRequest, InvalidTimeline, NoMatch, ReadFailed, IdentityMismatch, Cancelled };

struct FStudioComparisonFrames
{
    EStudioComparisonStatus Status=EStudioComparisonStatus::InvalidRequest;
    FString Error;
    int32 PrimaryOrdinal=INDEX_NONE,SecondaryOrdinal=INDEX_NONE;
    FStudioFrame PrimaryFrame,SecondaryFrame;
    double PrimaryAlignedTime=0,SecondaryAlignedTime=0;
    /** Secondary aligned time minus primary aligned time, in seconds. */
    double MismatchSeconds=0;
};

struct FStudioComparisonRequest
{
    FGuid ProjectId;
    TSharedPtr<const IStudioSolver,ESPMode::ThreadSafe> Primary,Secondary;
    int32 PrimaryOrdinal=INDEX_NONE;
    FString Scalar;
    FStudioComparisonAlignment Alignment;
};

/** One original immutable scalar snapshot with its own provenance and topology.
 * These pairs support side-by-side inspection, not pointwise subtraction:
 * equal scalar IDs/units do not establish matching meshes or physical cases. */
struct FStudioComparisonSide
{
    FString Title,SourceURL,TimeNote;
    FStudioScalarDescriptor Scalar;
    FStudioFieldIdentity Identity;
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> Field;
};

struct FStudioComparisonResult
{
    FGuid ProjectId;
    FString Scalar;
    FStudioComparisonAlignment Alignment;
    FStudioComparisonFrames Frames;
    FStudioComparisonSide Primary,Secondary;
    TWeakPtr<const IStudioSolver,ESPMode::ThreadSafe> PrimarySource,SecondarySource;
    bool Matches(const FStudioComparisonRequest& Current) const;
};

namespace StudioComparison
{
    constexpr int32 MaxTimelineFrames=1000000;
    /** Metadata-only matching. Original source steps are never used as ordinals.
     * Exact matching means equal represented timestamps, without hidden epsilon.
     * Call off Slate for large timelines; validation is linear and bounded. */
    FStudioComparisonFrames Align(const FStudioRecordingDescriptor& Primary,
        const FStudioRecordingDescriptor& Secondary,int32 PrimaryOrdinal,
        const FStudioComparisonAlignment& Alignment,const FStudioLoadCancellation& Cancellation={});
    /** Worker-only paired reads. Both snapshots publish together, with verified
     * frame/scalar/source/reconstruction identity. Failure owns its error and
     * releases any partial pair, leaving both playback readers untouched. */
    FStudioComparisonResult Evaluate(const FStudioComparisonRequest& Request,
        const FStudioLoadCancellation& Cancellation={});
}

/** One bounded worker, no implicit retries or queued scrubs. The owner must
 * match the completed request against current project/source/settings before
 * publication. Cancellation also wins after completion but before Poll. */
class FStudioComparisonTask
{
public:
    ~FStudioComparisonTask();
    bool Start(FStudioComparisonRequest Request,FString& Error);
    void Cancel();
    void Shutdown();
    bool IsBusy() const { return Pending.IsValid(); }
    TOptional<FStudioComparisonResult> Poll();
private:
    TFuture<FStudioComparisonResult> Pending;
    FStudioLoadCancellation Cancellation;
    bool bShutdown=false;
};
