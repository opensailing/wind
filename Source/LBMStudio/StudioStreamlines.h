#pragma once
#include "CoreMinimal.h"
#include "StudioInspectionObjects.h"
#include "StudioRecording.h"

class IStudioField;
enum class EStudioStreamDirection : uint8 { Forward, Backward, Both };
struct FStudioStreamlineSettings
{
    bool bAutomaticSeeds = true;
    bool bDirectionMarkers = false; // Display only; retained views keep plain traces by default.
    int32 AutomaticSeedCount = 84;
    EStudioStreamDirection Direction = EStudioStreamDirection::Forward;
    // Distances are relative to the longest domain side, so the same view
    // settings remain usable when switching between meter and millimeter cases.
    double StepFraction = .00625;
    double MaximumLength = 2.;
    double WidthFraction = .00175; // Full tube diameter, not radius.
    int32 MaximumSteps = 512;
    int32 WorkBudget = 32768; // Total attempted integration steps, all branches.
    FString VelocityField = TEXT("velocity");
    bool operator==(const FStudioStreamlineSettings& Other) const;
};

/** Small immutable summary retained alongside the captured geometry. */
struct FStudioStreamlineSummary
{
    int32 Seeds=0,Lines=0,Segments=0,Attempts=0;
    double WidthMeters=0;
    bool bBudgetExhausted=false,bAutomaticSeeds=true;
    FString Notice;
    TMap<FGuid,FString> SeedNotices;
};

enum class EStudioStreamEnd : uint8
{ OutsideDomain, MissingVelocity, MissingScalar, Stagnation, CoverageUnavailable, LengthLimit, StepLimit, WorkLimit };
struct FStudioStreamlinePath
{
    FGuid SeedId;
    int32 SeedIndex = 0;
    bool bBackward = false;
    TArray<FVector> PositionsMeters;
    TArray<double> Scalars;
    double LengthMeters = 0;
    EStudioStreamEnd End = EStudioStreamEnd::WorkLimit;
};
struct FStudioStreamlineOutput
{
    TOptional<FStudioFieldIdentity> Identity;
    FString Scalar;
    TArray<FStudioStreamlinePath> Paths;
    TMap<FGuid,FString> Notices;
    int32 SeedCount = 0, Attempts = 0, Segments = 0;
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
    /** Exact deterministic seed locations. Custom positions are never projected
     * onto a 2D recording or moved to a nearby supported interpolation cell. */
    bool Seeds(const FStudioSeedObject& Seed,const FBox& Bounds,int32 Dimensions,
        double SourcePlaneY,TArray<FVector>& Out,FString& Error);
    /** Arc-length midpoint integration of one immutable instantaneous velocity
     * field. This does not integrate time and never produces pathlines.
     * Round-robin work gives every seed/direction a turn before longer paths.
     * Invalid/cancelled work leaves Out unchanged; unsupported data is explicit. */
    bool Build(const IStudioField& Field,const FBox& Bounds,const TArray<FStudioSeedObject>& Seeds,
        const FStudioStreamlineSettings& Settings,const FString& Scalar,FStudioStreamlineOutput& Out,
        FString& Error,const FStudioLoadCancellation& Cancellation={});
}
