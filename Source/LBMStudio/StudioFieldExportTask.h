#pragma once
#include "StudioVTKExport.h"
#include "Async/Future.h"

enum class EStudioFieldExportState:uint8 { Writing,Publishing,Complete,Cancelled };
struct FStudioFieldExportProgress
{
    EStudioFieldExportState State=EStudioFieldExportState::Complete;
    int64 Completed=0,Total=0;
};
struct FStudioFieldExportWork;
struct FStudioPipelineExportRequest;
/** One private staging write; no queue. Cancellation is accepted until the
 * atomic destination replacement starts. Never reports a committed file as
 * cancelled. A failed or cancelled write leaves an existing destination intact.
 * Owned staging files are removed on every outcome; Shutdown cancels/joins and
 * releases the pinned snapshot before returning. */
class FStudioFieldExportTask
{
public:
    ~FStudioFieldExportTask();
    bool Start(FStudioFieldExportRequest Request,const FString& Path);
    /** Publish already evaluated geometry/table using the same atomic lifecycle. */
    bool Start(FStudioPipelineExportRequest Request,const FString& Path);
    bool Cancel();
    void Shutdown();
    bool IsBusy() const{return Pending.IsValid();}
    FStudioFieldExportProgress Progress() const;
    TOptional<FStudioFieldExportResult> Poll();
#if WITH_DEV_AUTOMATION_TESTS
    TFunction<void()> BeforePublishForAutomation;
#endif
private:
    using FWriter=TFunction<FStudioFieldExportResult(FArchive&,const FStudioLoadCancellation&,TFunction<void(int64,int64)>)>;
    bool StartWriter(FWriter Writer,const FString& Path);
    TSharedPtr<FStudioFieldExportWork,ESPMode::ThreadSafe> Work;
    TFuture<FStudioFieldExportResult> Pending;
    bool bShutdown=false;
};
