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
/** One private staging write; no queue. Cancellation is accepted until the
 * atomic destination replacement starts. Never reports a committed file as
 * cancelled. A failed or cancelled write leaves an existing destination intact.
 * Owned staging files are removed on every outcome; Shutdown cancels/joins and
 * releases the pinned snapshot before returning. */
class FStudioFieldExportTask
{
public:
    ~FStudioFieldExportTask();
    bool Start(FStudioVTKExportRequest Request,const FString& Path);
    bool Cancel();
    void Shutdown();
    bool IsBusy() const{return Pending.IsValid();}
    FStudioFieldExportProgress Progress() const;
    TOptional<FStudioVTKExportResult> Poll();
#if WITH_DEV_AUTOMATION_TESTS
    TFunction<void()> BeforePublishForAutomation;
#endif
private:
    TSharedPtr<FStudioFieldExportWork,ESPMode::ThreadSafe> Work;
    TFuture<FStudioVTKExportResult> Pending;
    bool bShutdown=false;
};
