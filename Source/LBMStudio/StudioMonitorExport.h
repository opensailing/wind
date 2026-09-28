#pragma once
#include "StudioMonitor.h"
#include "Async/Future.h"

struct FStudioMonitorExportResult
{
    bool bSuccess=false;
    FString Path,Error,Source;
    int32 Samples=0;
};
/** Freeze immutable source and settings before opening a destination panel.
 * The worker exports original rows, regardless of display decimation/log gaps.
 * Only one bounded write can be outstanding; destruction joins the write.
 */
class FStudioMonitorExportTask
{
public:
    ~FStudioMonitorExportTask();
    bool Start(TSharedPtr<const FStudioHistory,ESPMode::ThreadSafe> History,FStudioMonitorSettings Settings,const FString& Path);
    bool IsBusy() const { return Pending.IsValid(); }
    TOptional<FStudioMonitorExportResult> Poll();
private:
    TFuture<FStudioMonitorExportResult> Pending;
};
namespace StudioMonitorExport
{
    /** UTF-8 CSV: source identity, interpretation/payload hashes, original time,
     * column units/origin/expression and round-trip values for every selected row. */
    bool CSV(const FStudioHistory& History,const FStudioMonitorSettings& Settings,FString& Out,int32& Rows,FString& Error);
}
