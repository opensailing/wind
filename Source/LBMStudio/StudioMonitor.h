#pragma once
#include "CoreMinimal.h"
#include "StudioHistory.h"

/** Saved chart choices. Source ID and interpretation hash never imply a spatial association. */
struct FStudioMonitorSettings
{
    FString HistoryId, MetadataSHA256;
    TArray<FString> Series;
    bool bLogY = false;
    bool bManualTime = false;
    double TimeMinimum = 0, TimeMaximum = 1;
};

struct FStudioMonitorTrace
{
    FString Id, Label, Unit;
    // Original source indices, in order. INDEX_NONE breaks a log trace at invalid samples.
    TArray<int32> Samples;
    int32 OmittedNonPositive = 0;
};

struct FStudioMonitorPlot
{
    double TimeMinimum = 0, TimeMaximum = 1;
    // Log axes store log10 bounds; source values remain untouched.
    double ValueMinimum = 0, ValueMaximum = 1;
    bool bLogY = false;
    TArray<FStudioMonitorTrace> Traces;
    FString Unit, Error;
    int32 FirstSample = 0, LastSample = INDEX_NONE;
};

namespace StudioMonitor
{
    TSharedRef<FJsonObject> ToJSON(const FStudioMonitorSettings& Settings);
    bool FromJSON(const TSharedPtr<FJsonObject>& Object, FStudioMonitorSettings& Out, FString& Error);
    bool Validate(const FStudioMonitorSettings& Settings, FString& Error);
    bool ValidateSource(const FStudioMonitorSettings& Settings, const FStudioHistory& History, FString& Error);
    FStudioMonitorSettings Defaults(const FStudioHistory& History);
    /** Original sample nearest the requested physical time; never interpolates values. */
    int32 NearestSample(const TArray<double>& Times, double Time);
    /** Pixel buckets retain first/minimum/maximum/last in temporal order. No smoothing.
     * Width is bounded; returned geometry contains at most four points per occupied
     * bucket plus at most one gap before each point. All points refer to original rows.
     */
    FStudioMonitorPlot BuildPlot(const FStudioHistory& History, const FStudioMonitorSettings& Settings, int32 PixelWidth);
    bool SetTimeWindow(const FStudioHistory& History, double Minimum, double Maximum,
        FStudioMonitorSettings& Settings, FString& Error);
}
