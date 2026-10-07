#pragma once
#include "StudioHome4Config.h"
#include <atomic>

/** A named definition supplied by the user. No G/Q/P numbers are built into the frontend. */
struct FStudioHome4TankZonePreset
{
    FString Name,SourceId,SourcePath,SourceSHA256;
    TArray<uint8> OriginalBytes;
    FStudioHome4Spec OriginalSpec;
};
namespace StudioHome4TankPresets
{
    constexpr int32 MaximumBytes=1024*1024;
    bool Parse(const FString& JSON,FStudioHome4TankZonePreset& Out,FString& Error);
    bool Load(const FString& Path,FStudioHome4TankZonePreset& Out,FString& Error,
        const TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe>& Cancel={},TFunction<void()> BeforeVerify={});
    /** Apply only the reviewed tank and zone payload; unrelated physics/body/output values remain current. */
    bool Apply(const FStudioHome4TankZonePreset&,const FStudioHome4Spec& Current,FStudioHome4Spec& Out,FString& Error);
    bool MatchesApplied(const FStudioHome4Spec&,const FStudioHome4TankZonePreset&);
    /** Export the exact retained original bytes to a new destination, never overwrite. */
    bool Export(const FStudioHome4TankZonePreset&,const FString& Destination,FString& Error);
    FString Review(const FStudioHome4TankZonePreset&);
}
