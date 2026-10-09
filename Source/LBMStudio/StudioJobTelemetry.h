#pragma once
#include "CoreMinimal.h"

enum class EStudioJobStopRule : uint8 { Unknown, FirstReportedLimit };
enum class EStudioTelemetryStatus : uint8 { Unavailable, Current, Stale, Disconnected, Final };

/** One complete adapter measurement, never a patch over an older sample.
 * Sequence increases for the entire run. StateSequence identifies the latest
 * authoritative State event; samples from a previous state are rejected.
 * WallSeconds is measured solver elapsed wall time, including pauses, and must
 * increase across reconnects. SampleAgeSeconds is its age when PollTelemetry
 * delivers it (the adapter accounts for transport/queue delay).
 * Counters are cumulative for this run, not command acknowledgements or replay
 * frames. Missing optionals mean unavailable, including resource measurements.
 * Limits and their relationship are backend reported, not inferred from the
 * requested case or from a Completed event. All strings are bounded to 256 chars. */
struct FStudioJobMeasurement
{
    FGuid RunId;
    uint64 Sequence = 0, StateSequence = 0;
    FString Reporter, Host, Device;
    double WallSeconds = 0, SampleAgeSeconds = 0;
    TOptional<int64> CompletedSteps;
    TOptional<double> PhysicalSeconds;
    EStudioJobStopRule StopRule = EStudioJobStopRule::Unknown;
    TOptional<int64> StopAfterSteps;
    TOptional<double> StopAtPhysicalSeconds;
    TOptional<uint64> HostResidentBytes, DeviceUsedBytes, DeviceTotalBytes;
    TOptional<double> DeviceUtilizationPercent;
};

struct FStudioJobTelemetryRecord
{
    FStudioJobMeasurement Measurement;
    double ReceivedAt = 0;
};

/** Session-only view. Stale/disconnected samples retain their original values
 * and age for inspection, but have no derived progress, rates or ETA. Final
 * means a sample explicitly references the terminal state, not assumed 100%.
 * Rates use at least one second of contiguous, current Running measurements
 * within the last ten solver wall seconds. ETA assumes those rates continue. */
struct FStudioJobTelemetryView
{
    FString BackendId;
    EStudioTelemetryStatus Status = EStudioTelemetryStatus::Unavailable;
    TOptional<FStudioJobMeasurement> Sample;
    double AgeSeconds = 0;
    TOptional<double> StepProgress, PhysicalProgress, Progress;
    TOptional<double> StepsPerSecond, PhysicalSecondsPerSecond, EstimatedRemainingSeconds;
};
