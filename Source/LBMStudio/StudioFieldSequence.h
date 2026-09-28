#pragma once
#include "StudioFieldExportTask.h"

class IStudioSolver;

/** Inclusive original ordinals. The retained reader is independent from the
 * current project, camera and playback cursor. No temporal interpolation. */
struct FStudioFieldSequenceRequest
{
    TSharedPtr<const IStudioSolver,ESPMode::ThreadSafe> Source;
    int32 FirstOrdinal=0,LastOrdinal=0;
    TArray<FString> Scalars;
    EStudioExportCoordinates Coordinates=EStudioExportCoordinates::Source;
};
struct FStudioFieldSequenceResult
{
    bool bSuccess=false,bCancelled=false;
    FString Error,Path;
    int32 CompletedFrames=0,FailedOrdinal=INDEX_NONE;
    int64 Bytes=0;
    TOptional<FStudioFieldIdentity> FirstIdentity,LastIdentity;
};
struct FStudioFieldSequenceProgress
{
    EStudioFieldExportState State=EStudioFieldExportState::Complete;
    int32 CompletedFrames=0,TotalFrames=0;
    int64 FrameCompleted=0,FrameTotal=0;
};
namespace StudioFieldSequence
{
    constexpr int32 MaximumFrames=100000;
    bool Validate(const FStudioFieldSequenceRequest& Request,FString& Error);
    /** Worker-only. Directory must be caller-owned private staging. A failed
     * call leaves partial staging for its owner to discard, never to publish.
     * Retains one frame plus at most one additional scalar snapshot at a time.
     * Writes frame_<original ordinal>.vtp and a relative flow.pvd collection. */
    FStudioFieldSequenceResult Write(const FStudioFieldSequenceRequest& Request,const FString& Directory,
        const FStudioLoadCancellation& Cancellation={},TFunction<void(int32,int64,int64)> Progress={});
}
struct FStudioFieldSequenceWork;
/** One cancellable worker. Atomically publishes a NEW directory below an
 * existing user-selected parent. Existing files/directories are never replaced,
 * including a destination created while the export is running. */
class FStudioFieldSequenceTask
{
public:
    ~FStudioFieldSequenceTask();
    bool Start(FStudioFieldSequenceRequest Request,const FString& Parent,const FString& Name,FString& Error);
    bool Cancel();
    void Shutdown();
    bool IsBusy() const{return Pending.IsValid();}
    FStudioFieldSequenceProgress Progress() const;
    TOptional<FStudioFieldSequenceResult> Poll();
#if WITH_DEV_AUTOMATION_TESTS
    TFunction<void()> BeforePublishForAutomation;
#endif
private:
    TSharedPtr<FStudioFieldSequenceWork,ESPMode::ThreadSafe> Work;
    TFuture<FStudioFieldSequenceResult> Pending;
    bool bShutdown=false;
};
