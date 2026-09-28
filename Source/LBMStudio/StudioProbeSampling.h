#pragma once

#include "StudioInspectionObjects.h"
#include "StudioRecording.h"

class IStudioField;

/** The scene supplies its presented snapshot and capture number. Object edits
 * are compared by value; a same-ID edit cannot publish an older query. */
struct FStudioProbeRequest
{
    FGuid ProjectId;
    uint64 PresentationId = 0;
    FStudioProbeObject Probe;
    FString DisplayedScalar;
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> Field;
};

enum class EStudioProbeStatus : uint8 { Ready, InvalidRequest, UnidentifiedFrame, SourceMismatch, FieldUnavailable, FieldLoadFailed, FrameMismatch, Cancelled };
enum class EStudioProbeSampleStatus : uint8 { Value, OutsideCoverage, OffPlane, MissingPoint, FieldUnavailable, NoInterpolation };

struct FStudioProbeSample
{
    TOptional<FVector> ScenePosition, SourcePosition;
    TOptional<double> Value;
    TOptional<int64> PointId;
    double DistanceAlongLineMeters = 0;
    EStudioProbeSampleStatus Status = EStudioProbeSampleStatus::OutsideCoverage;
};

/** No source/cursor reads occur when presenting a result. All labels and values
 * originate in one immutable recorded frame; missing values stay unset. */
struct FStudioProbeResult
{
    FGuid ProjectId;
    uint64 PresentationId = 0;
    FStudioProbeObject Probe;
    TWeakPtr<const IStudioField,ESPMode::ThreadSafe> SampledField;
    TOptional<FStudioFieldIdentity> Identity;
    FString Field, Label, Unit, Origin, Method, Message;
    EStudioProbeStatus Status = EStudioProbeStatus::InvalidRequest;
    TArray<FStudioProbeSample> Samples;
    bool Matches(const FStudioProbeRequest& Current) const;
};

namespace StudioProbeSampling
{
    /** Worker-only: bounded sampling, cancellation and original-ID lookup.
     * Optional arrays load from the same presented frame, without mutable
     * source/cursor access, nearest-point substitution or synthetic values. */
    FStudioProbeResult Evaluate(const FStudioProbeRequest& Request,const FStudioLoadCancellation& Cancellation = {});
}
