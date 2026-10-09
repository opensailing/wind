#pragma once

#include "StudioSnapshot.h"
#include "StudioFieldExportTask.h"
#include "StudioMovie.h"

class IStudioSolver;

/** Exact original ordinal selection and a pixel-free copy of the presented
 * view. The producer renders these frames in an independent scene. No frame
 * interpolation, camera animation or live cursor mutation. Optional movie
 * presentation rate is separate from the original physical timeline. */
struct FStudioImageSequenceRequest
{
    TSharedPtr<const IStudioSolver,ESPMode::ThreadSafe> Source;
    int32 FirstOrdinal=0,LastOrdinal=0,Stride=1;
    FStudioSnapshot View;
    FStudioMovieOptions Movie;
};

enum class EStudioImageSequencePhase : uint8 { Preparing,AwaitingImage,Encoding,FinalizingMovie,Publishing,Complete };
struct FStudioImageSequenceProgress
{
    EStudioFieldExportState State=EStudioFieldExportState::Complete;
    EStudioImageSequencePhase Phase=EStudioImageSequencePhase::Complete;
    int32 CompletedFrames=0,TotalFrames=0,RequestedOrdinal=INDEX_NONE;
    int64 Bytes=0;
};
struct FStudioImageSequenceResult
{
    bool bSuccess=false,bCancelled=false;
    FString Path,Error;
    FGuid Project;
    int32 CompletedFrames=0,FailedOrdinal=INDEX_NONE;
    int64 Bytes=0;
};

namespace StudioImageSequence
{
    constexpr int32 MaximumFrames=100000;
    constexpr int64 MaximumImageBytes=128LL*1024*1024;
    /** Worker-safe validation; inspects the original timeline without reading fields. */
    bool Validate(const FStudioImageSequenceRequest& Request,FString& Error);
    /** Checks the requested original identity and frozen view, not pixel fidelity.
     * Rendering fidelity requires separate GPU acceptance against original fields. */
    bool Matches(const FStudioImageSequenceRequest& Request,int32 Ordinal,
        const FStudioSnapshot& Image,FString& Error);
}

struct FStudioImageSequenceWork;
/** One dedicated worker with one image handoff and no queue. Game-thread calls
 * TakeFrameRequest once, renders offscreen, then Submit or FailFrame. Encoding,
 * disk writes and cleanup stay off the game thread. Cancel wakes a waiting
 * worker; destruction cancels and joins without requiring another UI tick.
 * A complete sequence publishes exclusively into a NEW directory. Each PNG
 * has full snapshot metadata; frames.jsonl preserves original steps/times and
 * sequence.json describes the frozen view and exact selection. */
class FStudioImageSequenceTask
{
public:
    ~FStudioImageSequenceTask();
    bool Start(FStudioImageSequenceRequest Request,const FString& Parent,const FString& Name,FString& Error);
    TOptional<int32> TakeFrameRequest();
    bool Submit(FStudioSnapshot&& Image,FString& Error);
    bool FailFrame(const FString& Error);
    bool Cancel();
    void Shutdown();
    bool IsBusy() const {return Pending.IsValid();}
    FStudioImageSequenceProgress Progress() const;
    TOptional<FStudioImageSequenceResult> Poll();
#if WITH_DEV_AUTOMATION_TESTS
    TFunction<void()> BeforePublishForAutomation;
#endif
private:
    TSharedPtr<FStudioImageSequenceWork,ESPMode::ThreadSafe> Work;
    TFuture<FStudioImageSequenceResult> Pending;
    bool bShutdown=false;
};
