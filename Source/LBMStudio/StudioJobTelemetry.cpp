#include "StudioJobTelemetry.h"
#include "StudioJobs.h"

namespace
{
    bool Nonnegative(const TOptional<double>& Value)
    { return !Value || (FMath::IsFinite(*Value) && *Value >= 0); }

    bool Valid(const FStudioJobMeasurement& S)
    {
        if(S.Reporter.TrimStartAndEnd().IsEmpty() || S.Reporter.Len()>256 || S.Host.Len()>256 || S.Device.Len()>256 ||
           !FMath::IsFinite(S.WallSeconds) || S.WallSeconds<0 ||
           !FMath::IsFinite(S.SampleAgeSeconds) || S.SampleAgeSeconds<0 ||
           (S.CompletedSteps && *S.CompletedSteps<0) || !Nonnegative(S.PhysicalSeconds) ||
           (S.StopAfterSteps && *S.StopAfterSteps<=0) || !Nonnegative(S.StopAtPhysicalSeconds) ||
           (S.StopAtPhysicalSeconds && *S.StopAtPhysicalSeconds<=0) ||
           !Nonnegative(S.DeviceUtilizationPercent) || (S.DeviceUtilizationPercent && *S.DeviceUtilizationPercent>100))return false;
        if(S.HostResidentBytes && S.Host.TrimStartAndEnd().IsEmpty())return false;
        if((S.DeviceUsedBytes || S.DeviceTotalBytes || S.DeviceUtilizationPercent) && S.Device.TrimStartAndEnd().IsEmpty())return false;
        if(S.DeviceTotalBytes && (!*S.DeviceTotalBytes || (S.DeviceUsedBytes && *S.DeviceUsedBytes>*S.DeviceTotalBytes)))return false;
        return S.StopRule==EStudioJobStopRule::Unknown ||
            (S.StopRule==EStudioJobStopRule::FirstReportedLimit && (S.StopAfterSteps || S.StopAtPhysicalSeconds));
    }

    template <typename ReadValue>
    TOptional<double> Rate(const TArray<FStudioJobTelemetryRecord>& History,uint64 After,ReadValue Read)
    {
        const auto& Last=History.Last().Measurement;
        const auto EndValue=Read(Last);
        if(!EndValue || Last.Sequence<=After)return {};
        const FStudioJobMeasurement* First=&Last;
        for(int32 I=History.Num()-2;I>=0;--I)
        {
            const auto& S=History[I].Measurement;
            if(S.Sequence<=After || Last.WallSeconds-S.WallSeconds>10. || !Read(S))break;
            First=&S;
        }
        const double Duration=Last.WallSeconds-First->WallSeconds;
        if(Duration<1.)return {};
        const double Result=double(*EndValue-*Read(*First))/Duration;
        return FMath::IsFinite(Result) && Result>=0 ? TOptional<double>(Result) : TOptional<double>();
    }

    TOptional<double> Remaining(double Left,const TOptional<double>& RateValue)
    {
        if(Left<=0)return 0.;
        if(!RateValue || *RateValue<=0)return {};
        const double Result=Left / *RateValue;
        return FMath::IsFinite(Result) ? TOptional<double>(Result) : TOptional<double>();
    }
}

void FStudioJobController::BreakTelemetryRates()
{ RateAfterSequence=Measurements.IsEmpty()?0:Measurements.Last().Measurement.Sequence; }

bool FStudioJobController::AcceptTelemetry(const FStudioJobMeasurement& S)
{
    if(!Caps.bTelemetry || Caps.bControlHarness || !Record || S.RunId!=Record->GetId() ||
       !S.Sequence || !StateSequence || S.StateSequence!=StateSequence ||
       (Current!=EStudioJobState::Running && Current!=EStudioJobState::Paused && !IsTerminal(Current)) || !Valid(S))return false;
    if((StepHighWater && S.CompletedSteps && *S.CompletedSteps<*StepHighWater) ||
       (PhysicalHighWater && S.PhysicalSeconds && *S.PhysicalSeconds<*PhysicalHighWater))return false;
    if(!Measurements.IsEmpty())
    {
        const auto& Previous=Measurements.Last();
        const auto& P=Previous.Measurement;
        if(S.Sequence<=P.Sequence || S.WallSeconds<=P.WallSeconds ||
           LastClock-S.SampleAgeSeconds<Previous.ReceivedAt-P.SampleAgeSeconds ||
           (IsTerminal(Current) && P.StateSequence==StateSequence))return false;
        // No rate may bridge missing/stale transport, a different producer, or
        // a resource attribution change. Keep original samples for inspection.
        if(LastClock-Previous.ReceivedAt+P.SampleAgeSeconds>Caps.TelemetryStaleSeconds ||
           S.WallSeconds-P.WallSeconds>Caps.TelemetryStaleSeconds ||
           S.SampleAgeSeconds>Caps.TelemetryStaleSeconds ||
           S.Reporter!=P.Reporter || S.Host!=P.Host || S.Device!=P.Device)BreakTelemetryRates();
    }
    if(S.CompletedSteps)StepHighWater=S.CompletedSteps;
    if(S.PhysicalSeconds)PhysicalHighWater=S.PhysicalSeconds;
    Measurements.Add({S,LastClock});
    if(Measurements.Num()>240)Measurements.RemoveAt(0,Measurements.Num()-240,EAllowShrinking::No);
    return true;
}

FStudioJobTelemetryView FStudioJobController::Telemetry() const
{
    FStudioJobTelemetryView V;V.BackendId=Caps.BackendId;
    if(Measurements.IsEmpty())return V;
    const auto& Last=Measurements.Last();const auto& S=Last.Measurement;
    V.Sample=S;V.AgeSeconds=S.SampleAgeSeconds+FMath::Max(0.,LastClock-Last.ReceivedAt);
    if(Current==EStudioJobState::Disconnected){V.Status=EStudioTelemetryStatus::Disconnected;return V;}
    const bool MatchingState=S.StateSequence==StateSequence;
    if(IsTerminal(Current) && MatchingState)V.Status=EStudioTelemetryStatus::Final;
    else if(!MatchingState || (Current!=EStudioJobState::Running && Current!=EStudioJobState::Paused) ||
            V.AgeSeconds>Caps.TelemetryStaleSeconds){V.Status=EStudioTelemetryStatus::Stale;return V;}
    else V.Status=EStudioTelemetryStatus::Current;

    if(S.StopAfterSteps && S.CompletedSteps)
        V.StepProgress=FMath::Clamp(double(*S.CompletedSteps)/double(*S.StopAfterSteps),0.,1.);
    if(S.StopAtPhysicalSeconds && S.PhysicalSeconds)
        V.PhysicalProgress=FMath::Clamp(*S.PhysicalSeconds / *S.StopAtPhysicalSeconds,0.,1.);
    if(S.StopRule==EStudioJobStopRule::FirstReportedLimit)
    {
        if((V.StepProgress && *V.StepProgress>=1.) || (V.PhysicalProgress && *V.PhysicalProgress>=1.))V.Progress=1.;
        else if((!S.StopAfterSteps || V.StepProgress) && (!S.StopAtPhysicalSeconds || V.PhysicalProgress))
            V.Progress=FMath::Max(V.StepProgress.Get(0.),V.PhysicalProgress.Get(0.));
    }
    if(Current!=EStudioJobState::Running)return V;
    V.StepsPerSecond=Rate(Measurements,RateAfterSequence,[](const auto& M){return M.CompletedSteps;});
    V.PhysicalSecondsPerSecond=Rate(Measurements,RateAfterSequence,[](const auto& M){return M.PhysicalSeconds;});
    if(S.StopRule!=EStudioJobStopRule::FirstReportedLimit)return V;
    if(V.Progress && *V.Progress>=1.){V.EstimatedRemainingSeconds=0.;return V;}
    TOptional<double> StepETA,PhysicalETA;
    if(S.StopAfterSteps && S.CompletedSteps)StepETA=Remaining(double(*S.StopAfterSteps-*S.CompletedSteps),V.StepsPerSecond);
    if(S.StopAtPhysicalSeconds && S.PhysicalSeconds)PhysicalETA=Remaining(*S.StopAtPhysicalSeconds-*S.PhysicalSeconds,V.PhysicalSecondsPerSecond);
    // Both active limits need estimates; otherwise a displayed total could
    // conceal an earlier stop at the unavailable limit.
    if((!S.StopAfterSteps || StepETA) && (!S.StopAtPhysicalSeconds || PhysicalETA))
        V.EstimatedRemainingSeconds=FMath::Min(StepETA.Get(TNumericLimits<double>::Max()),PhysicalETA.Get(TNumericLimits<double>::Max()));
    return V;
}
