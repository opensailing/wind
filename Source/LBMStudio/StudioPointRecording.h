#pragma once

#include "CoreMinimal.h"
#include "StudioRecording.h"

/** Version 3 keeps optional source fields separate. A point recording supplies no cells or solid boundary. */
struct FStudioPointArrayDescriptor
{
    FString Path, SHA256;
    int64 ByteLength = 0;
    TArray<uint32> FrameCRC32;
};

struct FStudioPointFieldDescriptor
{
    FString Id, Label, Unit, Origin, Expression, Vector, Component;
    bool bStatic = false;
    double Minimum = 0, Maximum = 0;
    FStudioPointArrayDescriptor Array;
};

struct FStudioPointRecordingDescriptor
{
    FString Id, Title, SourceURL, MetadataSHA256, CoordinateUnit, TimeUnit, TimeOrigin, DefaultScalar;
    int32 SpatialDimensions = 0, PointCount = 0;
    FBox SourceBounds = FBox(ForceInit);
    FStudioPointArrayDescriptor Coordinates, PointIds;
    TArray<FStudioFrame> Frames;
    TArray<FString> FrameLabels, Limitations;
    TArray<FStudioPointFieldDescriptor> Fields;
    const FStudioPointFieldDescriptor* FindField(const FString& Id) const;
};

/** Source axes and float64 coordinates, before any view transform or extrusion. */
struct FStudioPointGeometry
{
    TArray<FVector> Positions;
    TArray<int64> PointIds;
};

struct FStudioPointMemory;
struct FStudioPointValues
{
    FStudioPointValues(int32 Count, const TSharedRef<FStudioPointMemory, ESPMode::ThreadSafe>& InMemory);
    ~FStudioPointValues();
    FStudioPointValues(const FStudioPointValues&) = delete;
    FStudioPointValues& operator=(const FStudioPointValues&) = delete;
    TArray<double> Values;
private:
    TSharedRef<FStudioPointMemory, ESPMode::ThreadSafe> Memory;
};

/** Snapshots retain only requested scalar arrays. Absent/unrequested fields return nullptr. */
struct FStudioPointFrame
{
    FStudioPointFrame();
    ~FStudioPointFrame();
    FStudioPointFrame(const FStudioPointFrame&) = delete;
    FStudioPointFrame& operator=(const FStudioPointFrame&) = delete;
    int32 Ordinal = 0;
    TSharedPtr<const FStudioPointRecordingDescriptor, ESPMode::ThreadSafe> Descriptor;
    TSharedPtr<const FStudioPointGeometry, ESPMode::ThreadSafe> Geometry;
    TMap<FString, TSharedPtr<const FStudioPointValues, ESPMode::ThreadSafe>> Fields;
    const TArray<double>* FindValues(const FString& FieldId) const;
};

struct FStudioPointReadResult
{
    TSharedPtr<const FStudioPointFrame, ESPMode::ThreadSafe> Frame;
    FString Error;
};

struct FStudioPointReadOptions
{
    int64 CacheBytes = 8LL * 1024 * 1024;
    // Value bytes in cached, evicted-but-pinned and in-flight arrays together.
    // Allocator/object overhead and the fixed-size metadata parser are additional.
    int64 LiveArrayBytes = 32LL * 1024 * 1024;
    // Geometry value bytes: positions + IDs + temporary ID uniqueness copy.
    // Allocator overhead is additional; Stats reports retained geometry allocations.
    int64 GeometryBytes = 128LL * 1024 * 1024;
};

struct FStudioPointReadStats
{
    int64 CacheBudgetBytes = 0;
    int64 CacheBytes = 0, LiveArrayBytes = 0, PeakLiveArrayBytes = 0, GeometryBytes = 0;
    int32 CachedArrays = 0;
    uint64 Loads = 0, Hits = 0;
};
struct FStudioPointLiveStats
{
    int32 Readers = 0, Snapshots = 0, Arrays = 0;
    int64 AllocatedValueBytes = 0;
};

struct FStudioPointRecordingData;
class FStudioPointRecording
{
public:
    explicit FStudioPointRecording(TSharedRef<FStudioPointRecordingData, ESPMode::ThreadSafe> InData);
    const FStudioPointRecordingDescriptor& Descriptor() const;
    /** Verified immutable coordinates/IDs; no file access or scalar-frame allocation. */
    TSharedRef<const FStudioPointGeometry, ESPMode::ThreadSafe> Geometry() const;
    /** Worker-only; requests serialize cancellably. No mutable cursor or frame is shared. */
    FStudioPointReadResult ReadFrame(int32 Ordinal, const TArray<FString>& FieldIds,
        const FStudioLoadCancellation& Cancellation = {}) const;
    FStudioPointReadStats Stats() const;
private:
    TSharedRef<FStudioPointRecordingData, ESPMode::ThreadSafe> Data;
};

struct FStudioPointOpenResult
{
    TSharedPtr<FStudioPointRecording, ESPMode::ThreadSafe> Recording;
    FString Error;
};

namespace StudioPointRecordings
{
    FStudioPointLiveStats LiveStats();
    /** Worker-only. Verifies every array SHA-256 and source notices before publication.
     * Metadata SHA-256 pins all member identities for future save/relink support.
     * Version 3 currently accepts static point positions in 2D/3D, scalar arrays and
     * explicit vector components. Connectivity and spatial interpolation are unavailable.
     */
    FStudioPointOpenResult Open(const FString& DescriptorPath,
        const FStudioPointReadOptions& Options = {}, const FStudioLoadCancellation& Cancellation = {},
        const FString& ExpectedMetadataSHA256 = FString());
}
