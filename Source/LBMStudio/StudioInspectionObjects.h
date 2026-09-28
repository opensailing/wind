#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Paths and display titles do not identify scientific data. Both legacy files
 * are pinned; a point descriptor transitively pins its member arrays. */
struct FStudioInspectionSource
{
    FString Dataset, MetadataSHA256, PayloadSHA256;
    bool operator==(const FStudioInspectionSource& Other) const;
};

/** Every inspection object uses the recording's scene axes, in meters. A source
 * binding survives relocation and prevents reuse against a different recording. */
struct FStudioInspectionObject
{
    FGuid Id = FGuid::NewGuid();
    FString Name;
    FStudioInspectionSource Source;
    bool bVisible = true;
    bool Equals(const FStudioInspectionObject& Other) const;
};

struct FStudioSliceObject : FStudioInspectionObject
{
    FVector Origin = FVector::ZeroVector;
    FVector Normal = FVector::RightVector;
    double Opacity = .65;
    bool operator==(const FStudioSliceObject& Other) const;
};

enum class EStudioProbeKind : uint8 { Point, Line };
enum class EStudioProbeMethod : uint8 { Interpolated, OriginalPoint };

struct FStudioProbeObject : FStudioInspectionObject
{
    EStudioProbeKind Kind = EStudioProbeKind::Point;
    EStudioProbeMethod Method = EStudioProbeMethod::Interpolated;
    FVector A = FVector::ZeroVector, B = FVector::ZeroVector;
    int32 Samples = 64;
    // Empty follows the presented scalar. Sampling never silently chooses a
    // different field when this explicit field is unavailable.
    FString Field;
    // Stable original ID, not a decimated glyph index or a nearest-neighbour
    // interpolation. Kept as a decimal string in JSON to preserve all 64 bits.
    TOptional<int64> PointId;
    bool operator==(const FStudioProbeObject& Other) const;
};

enum class EStudioRulerKind : uint8 { Distance, Angle };
struct FStudioRulerObject : FStudioInspectionObject
{
    EStudioRulerKind Kind = EStudioRulerKind::Distance;
    FVector A = FVector::ZeroVector, B = FVector::ZeroVector, C = FVector::ZeroVector;
    FString Unit = TEXT("m");
    bool operator==(const FStudioRulerObject& Other) const;
};

enum class EStudioSeedKind : uint8 { Inlet, Plane, Line, Points };
/** Saved positions use scene meters. Plane: A=center, B/C=full span vectors.
 * Line: A/B=endpoints. Points: exact user-selected locations, never snapped.
 * Inlet: chosen domain face, inset to its first supported sampling region. */
struct FStudioSeedObject : FStudioInspectionObject
{
    EStudioSeedKind Kind = EStudioSeedKind::Inlet;
    int32 Count = 84;
    int32 InletAxis = 0;
    bool bUpperFace = false;
    FVector A = FVector::ZeroVector, B = FVector::ForwardVector, C = FVector::UpVector;
    TArray<FVector> Points;
    bool operator==(const FStudioSeedObject& Other) const;
};

struct FStudioInspectionObjects
{
    TArray<FStudioSliceObject> Slices;
    TArray<FStudioProbeObject> Probes;
    TArray<FStudioRulerObject> Rulers;
    TArray<FStudioSeedObject> Seeds;
    bool operator==(const FStudioInspectionObjects& Other) const;
};

namespace StudioInspectionObjects
{
    constexpr int32 CurrentVersion = 2;
    constexpr int32 MaxObjectsPerKind = 128;
    constexpr int32 MaxLineSamples = 1024;
    constexpr int32 MaxSeedsPerObject = 512;
    constexpr int32 MaxTotalSeeds = 4096;
    bool IsValid(const FStudioInspectionSource& Source);
    bool IsValid(const FStudioInspectionObjects& Objects, FString& Error);
    TSharedRef<FJsonObject> ToJSON(const FStudioInspectionObjects& Objects);
    /** Validates a complete candidate before replacing Out. Never repairs or
     * normalizes saved coordinates, normals, IDs or field names. */
    bool FromJSON(const TSharedPtr<FJsonObject>& JSON, FStudioInspectionObjects& Out, FString& Error);

    /** A convex polygon in normal-facing winding. Empty for a miss, corner-only
     * contact, line-only contact or invalid input. Coordinates remain meters. */
    TArray<FVector> SlicePolygon(const FStudioSliceObject& Slice, const FBox& Bounds);
    bool SliceRange(const FVector& UnitNormal, const FBox& Bounds, double& Minimum, double& Maximum);
    bool MoveSlice(FStudioSliceObject& Slice, double SignedPositionMeters);
    /** Forward ray/plane intersection only. Out is unchanged for a miss. */
    bool IntersectSlice(const FStudioSliceObject& Slice, const FVector& RayOrigin,
        const FVector& RayDirection, FVector& Out);
    TArray<FVector> ProbeLocations(const FStudioProbeObject& Probe);
    /** Distances are returned in the selected unit; angles at B in degrees.
     * An angle with a zero-length arm is unavailable, never zero. */
    TOptional<double> Measurement(const FStudioRulerObject& Ruler);
}
