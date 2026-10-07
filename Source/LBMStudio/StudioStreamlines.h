#pragma once
#include "CoreMinimal.h"
#include "StudioInspectionObjects.h"
#include "StudioRecording.h"

class IStudioField;
enum class EStudioStreamDirection : uint8 { Forward, Backward, Both };
enum class EStudioStreamMethod : uint8 { Midpoint, DormandPrince45 };
struct FStudioStreamlineSettings
{
    bool bAutomaticSeeds = true;
    bool bDirectionMarkers = false; // Display only; retained views keep plain traces by default.
    int32 AutomaticSeedCount = 84;
    EStudioStreamDirection Direction = EStudioStreamDirection::Forward;
    // Distances are relative to the longest domain side, so the same view
    // settings remain usable when switching between meter and millimeter cases.
    double StepFraction = .00625; // RK45 initial/maximum arc step; legacy midpoint fixed step.
    double MaximumLength = 2.;
    double WidthFraction = .00175; // Full tube diameter, not radius.
    int32 MaximumSteps = 512;
    int32 WorkBudget = 32768; // Total trials, including rejected/failed steps, all branches.
    FString VelocityField = TEXT("velocity");
    EStudioStreamMethod Method = EStudioStreamMethod::DormandPrince45;
    double AbsoluteToleranceFraction = 1.e-6;
    double RelativeTolerance = 1.e-4;
    double MinimumStepFraction = 1.e-8;
    bool operator==(const FStudioStreamlineSettings& Other) const;
};

/** Small immutable summary retained alongside the captured geometry. */
struct FStudioStreamlineSummary
{
    int32 Seeds=0,Lines=0,Segments=0,Attempts=0;
    int32 RejectedAttempts=0,VelocityEvaluations=0,ScalarEvaluations=0,SupportEvaluations=0;
    EStudioStreamMethod Method=EStudioStreamMethod::DormandPrince45;
    double WidthMeters=0;
    bool bBudgetExhausted=false,bAutomaticSeeds=true;
    FString Notice;
    TMap<FGuid,FString> SeedNotices;
};

enum class EStudioStreamEnd : uint8
{ OutsideDomain, MissingVelocity, MissingScalar, Stagnation, CoverageUnavailable, LengthLimit, StepLimit, WorkLimit, AccuracyLimit };
struct FStudioStreamlinePath
{
    FGuid SeedId;
    int32 SeedIndex = 0;
    bool bBackward = false;
    TArray<FVector> PositionsMeters;
    TArray<double> Scalars;
    double LengthMeters = 0;
    // Arc parameter for RK45; LengthMeters remains the rendered polyline length.
    double IntegrationLengthMeters = 0;
    int32 Attempts=0,RejectedAttempts=0;
    EStudioStreamEnd End = EStudioStreamEnd::WorkLimit;
};
struct FStudioStreamlineOutput
{
    TOptional<FStudioFieldIdentity> Identity;
    FString Scalar;
    TArray<FStudioStreamlinePath> Paths;
    TMap<FGuid,FString> Notices;
    int32 SeedCount = 0, Attempts = 0, Segments = 0;
    int32 RejectedAttempts=0,VelocityEvaluations=0,ScalarEvaluations=0,SupportEvaluations=0;
    EStudioStreamMethod Method=EStudioStreamMethod::DormandPrince45;
    double WidthMeters = 0;
    bool bBudgetExhausted = false;
};

namespace StudioStreamlines
{
    constexpr int32 MaximumWork = 65536;
    /** Seed domain is the attached grid coverage, or the supplied recording bounds. */
    FBox DomainBounds(const IStudioField& Field,const FBox& RecordingBounds);
    bool IsValid(const FStudioStreamlineSettings& Settings);
    TSharedRef<FJsonObject> ToJSON(const FStudioStreamlineSettings& Settings);
    bool FromJSON(const TSharedPtr<FJsonObject>& JSON,FStudioStreamlineSettings& Out);
    /** Original grids use bounded deterministic supported liquid cell centers.
     * Other sources use supported domain faces, spaced by their
     * projected length/area across recorded flow. Backward traces use outflow;
     * forward/both use inflow. No valid inflow returns an empty set, never a
     * substitute direction. Invalid/cancelled work leaves Out unchanged. */
    bool AutomaticSeeds(const IStudioField& Field,const FBox& Bounds,int32 Count,
        EStudioStreamDirection Direction,TArray<FVector>& Out,FString& Error,
        const FStudioLoadCancellation& Cancellation={});
    /** Exact deterministic seed locations. Custom positions are never projected
     * onto a 2D recording or moved to a nearby supported interpolation cell. */
    bool Seeds(const FStudioSeedObject& Seed,const FBox& Bounds,int32 Dimensions,
        double SourcePlaneY,TArray<FVector>& Out,FString& Error);
    /** Adaptive Dormand-Prince 5(4), or migrated midpoint arc-length integration of one immutable instantaneous velocity
     * field. This does not integrate time and never produces pathlines.
     * Round-robin work gives every seed/direction a turn before longer paths.
     * Invalid/cancelled work leaves Out unchanged; unsupported data is explicit. */
    bool Build(const IStudioField& Field,const FBox& Bounds,const TArray<FStudioSeedObject>& Seeds,
        const FStudioStreamlineSettings& Settings,const FString& Scalar,FStudioStreamlineOutput& Out,
        FString& Error,const FStudioLoadCancellation& Cancellation={});
}
