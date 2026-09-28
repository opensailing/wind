#pragma once
#include "StudioLog.h"
#include "Async/Future.h"

struct FStudioLogExportResult
{
    bool bSuccess=false;
    int32 Entries=0;
    FString Path,Error;
};
namespace StudioLogExport
{
    /** Exact visible snapshot, in observation order. UTC means application
     * observation time, not solver time. CSV quoting preserves multiline text. */
    bool CSV(const TArray<FStudioLogEntry>& Entries,FString& Out,FString& Error);
}
/** Freeze visible entries before the destination picker. One bounded atomic
 * background write at a time; destruction joins an outstanding write. */
class FStudioLogExportTask
{
public:
    ~FStudioLogExportTask();
    bool Start(TArray<FStudioLogEntry> Entries,const FString& Path);
    bool IsBusy() const { return Pending.IsValid(); }
    TOptional<FStudioLogExportResult> Poll();
private:
    TFuture<FStudioLogExportResult> Pending;
};
