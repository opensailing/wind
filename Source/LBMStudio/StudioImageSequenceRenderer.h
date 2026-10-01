#pragma once
#include "StudioImageSequence.h"
#include "StudioSnapshotSource.h"
#include "StudioProbeMarkers.h"
#include "StudioProbeSampling.h"

class AStudioScene;
class UWorld;

struct FStudioImageSequencePreparedFrame
{
    TSharedPtr<FStudioSnapshotSource,ESPMode::ThreadSafe> Snapshot;
    FStudioProbeMarkerResult Markers;
    TOptional<FStudioProbeResult> Probe;
    FString Error;
};
namespace StudioImageSequence
{
    /** Worker-only original field/velocity read and source-bound annotations.
     * No scene, UObject, live cursor or solver-error mutation. */
    FStudioImageSequencePreparedFrame Prepare(const FStudioImageSequenceRequest& Request,int32 Ordinal,
        const FStudioLoadCancellation& Cancellation);
}

/** Game-thread controller for the writer's single image handoff. Owns one
 * reusable independent scene, one preparation future and a frozen view.
 * Tick while active, including when its menu or Solve workspace is hidden.
 * Minimized windows suspend capture, preserving the pending exact frame.
 * Shutdown cancels/joins preparation and writing and destroys owned rendering
 * resources; it never advances or changes the live Solve model. */
class FStudioImageSequenceRenderer
{
public:
    ~FStudioImageSequenceRenderer();
    bool Start(UWorld* World,FStudioImageSequenceRequest Request,const FString& Parent,const FString& Name,FString& Error);
    void Tick();
    bool Cancel();
    void Shutdown();
    bool IsBusy() const {return bActive;}
    FStudioImageSequenceProgress Progress() const;
    TOptional<FStudioImageSequenceResult> Poll();
#if WITH_DEV_AUTOMATION_TESTS
    TFunction<void(const FStudioSnapshot&)> CapturedForAutomation;
    TFunction<void()> BeforePublishForAutomation;
    AStudioScene* SceneForAutomation() const;
#endif
private:
    void Release();
    void Fail(const FString& Error);
    FStudioImageSequenceRequest Frozen;
    FStudioImageSequenceTask Writer;
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<AStudioScene> Scene;
    TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe> Visible;
    TFuture<FStudioImageSequencePreparedFrame> Preparing;
    FStudioLoadCancellation Cancellation;
    TOptional<FStudioImageSequencePreparedFrame> Prepared;
    TOptional<FStudioImageSequenceResult> Completed;
    FStudioImageSequenceProgress LastProgress;
    int32 Ordinal=INDEX_NONE;
    double LastTick=0,RenderSeconds=0;
    bool bActive=false,bShutdown=false;
};
