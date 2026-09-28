#pragma once

#include "StudioProbeSampling.h"
#include "Async/Future.h"

/** Immutable plot data, built once per sampled result. Gaps split traces;
 * missing values never enter the vertical range or become zeroes. */
struct FStudioProbeProfile
{
    FStudioProbeResult Snapshot;
    TArray<TArray<int32>> Segments;
    double LengthMeters=0,Minimum=0,Maximum=0;
    int32 ValidSamples=0;
    FVector2D NormalizedPosition(int32 Sample) const;
    FString RangeLabel(bool bMaximum) const;
    bool Matches(const FStudioProbeResult& Result) const;
};

namespace StudioProbeProfile
{
    TSharedPtr<const FStudioProbeProfile> Build(const FStudioProbeResult& Result);
    FString SampleStatus(EStudioProbeSampleStatus Status);
    /** RFC-style quoted text, UTF-8 output, round-trip double precision and
     * explicit empty cells/status for absent values/coordinates/point IDs. */
    bool CSV(const FStudioProbeResult& Snapshot,FString& Out,FString& Error);
}

struct FStudioProbeExportResult
{
    bool bSuccess=false;
    FString Path,Error,ProbeName;
    FStudioFrame Frame;
};

/** One bounded export at a time; writes own their frozen samples and never
 * touch a model, solver or renderer. Destruction joins an outstanding write. */
class FStudioProbeExportTask
{
public:
    ~FStudioProbeExportTask();
    bool Start(FStudioProbeResult Snapshot,const FString& Path);
    bool IsBusy() const {return Pending.IsValid();}
    TOptional<FStudioProbeExportResult> Poll();
private:
    TFuture<FStudioProbeExportResult> Pending;
};
