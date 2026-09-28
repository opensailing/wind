#pragma once

#include "StudioProbeHistory.h"
#include "StudioMonitor.h"

namespace StudioProbeMonitor
{
    /** Keep the full immutable samples for provenance/export; chart columns use
     * NaN only for explicit spatial gaps, never a substituted numerical value. */
    TSharedPtr<const FStudioHistory,ESPMode::ThreadSafe> MakeHistory(
        TSharedPtr<const FStudioProbeHistoryResult,ESPMode::ThreadSafe> Result,FString& Error);
    /** Long-form selected sample rows, including original frame links, both
     * coordinate systems, exact point IDs, statuses and round-trip values. */
    bool CSV(const FStudioHistory& History,const FStudioMonitorSettings& Settings,
        FString& Out,int32& Rows,FString& Error);
}

/** Session-only derived history. Saved probes persist in the project; generation
 * and chart selection are explicit. Changes to project/source/probe invalidate
 * results immediately; camera/playback changes do not invalidate the request. */
class FStudioProbeMonitorSession
{
public:
    void Select(FStudioProbeHistoryRequest Request);
    void SetRange(int32 FirstOrdinal,int32 LastOrdinal);
    bool Generate();
    void Cancel();
    void Clear();
    void Tick(const FGuid& Project,const TSharedPtr<const IStudioSolver,ESPMode::ThreadSafe>& Source,
        const FStudioInspectionObjects& Objects);
    bool UpdateSettings(const FStudioMonitorSettings& Value);
    const TOptional<FStudioProbeHistoryRequest>& Selection() const { return Selected; }
    TSharedPtr<const FStudioHistory,ESPMode::ThreadSafe> History() const { return Loaded; }
    const FStudioMonitorSettings& Settings() const { return Chart; }
    bool IsBusy() const { return Task.IsBusy(); }
    int32 CompletedFrames() const { return Task.CompletedFrames(); }
    int32 TotalFrames() const { return Task.TotalFrames(); }
    FString Notice;
    uint64 Revision=0;
private:
    void DropHistory();
    FStudioProbeHistoryTask Task;
    TOptional<FStudioProbeHistoryRequest> Selected;
    TSharedPtr<const FStudioHistory,ESPMode::ThreadSafe> Loaded;
    FStudioMonitorSettings Chart;
};
