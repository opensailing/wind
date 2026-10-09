#pragma once
#include "CoreMinimal.h"

class AStudioScene;

/** Actual application counters. No solver, replay or GPU-utilization estimates. */
struct FStudioApplicationCounters
{
    FString Host,Device;
    TOptional<double> ProcessCPUSeconds;
    TOptional<uint64> FootprintBytes;
    TOptional<uint64> Captures;
    TOptional<double> LastCaptureSubmitMs,LastFieldBuildMs;
    FString BuildSource;
    int32 BuildFrame=INDEX_NONE,Workers=0;
    int64 MeshBytes=0,TextureBytes=0;
};

struct FStudioPerformanceSample
{
    double At=0,IntervalSeconds=0,UICadenceMs=0,UIUpdateMs=0;
    TOptional<double> ProcessCPUPercent,CapturesPerSecond;
    FStudioApplicationCounters Counters;
};

/** Owner-thread sampler. Reads platform counters at most once per second and
 * retains 120 samples. UI timings measure the root workspace tick cadence and
 * update work, not paint/GPU duration. A >2s tick gap starts a new interval. */
class FStudioPerformanceHistory
{
public:
    bool Observe(double Now,double UpdateMs,TFunctionRef<FStudioApplicationCounters()> Read);
    const TArray<FStudioPerformanceSample>& Samples() const {return History;}
private:
    TArray<FStudioPerformanceSample> History;
    double LastTick=-1,WindowStart=0,UpdateTotal=0;
    int32 Ticks=0;
    bool bContinuous=false;
};

namespace StudioPerformance
{
    FStudioApplicationCounters ReadCounters(const AStudioScene* Scene);
}
