#pragma once
#include "CoreMinimal.h"
#include "StudioPointRecording.h"
#include "StudioColor.h"

/** A verified display grid from original source rows (identity weights for supplied structured nodes).
 * No CFD values are stored here. Source-space XYZ maps to scene XZY only at presentation. */
struct FStudioVolumeStencil
{
    int32 Rows[4] = {INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE};
    double Weights[4] = {};
};

struct FStudioVolumeReconstruction
{
    FString MetadataSHA256, SourceMetadataSHA256, Title, Method;
    TArray<FString> Limitations;
    FIntVector Dimensions = FIntVector::ZeroValue;
    FBox SourceBounds = FBox(ForceInit);
    FVector2D CylinderCenter = FVector2D::ZeroVector;
    double CylinderRadius = 0;
    TSharedPtr<const FStudioPointGeometry, ESPMode::ThreadSafe> Geometry;
    TArray<FStudioVolumeStencil> Stencils;
    // 0 = no support, 1 = fluid, 2 = explicitly classified solid.
    TArray<uint8> Classification;
    // Present only for original structured samples. Static Classification is topology, never a frame mask.
    TSharedPtr<const FStudioPointStructuredGrid,ESPMode::ThreadSafe> OriginalGrid;
    FVector Position(int32 Index) const;
    bool Sample(const FVector& SourcePosition, const TArray<double>& OriginalValues, double& Out) const;
    bool IsSolid(const FVector& SourcePosition) const;
    bool ContainsSolid(const FBox& SourceRegion) const;
    /** Conservative whole-region coverage for display cells. Every overlapping
     * interpolation cell must have eight fluid nodes and avoid the solid.
     * This may omit a boundary cell; it never bridges an unsupported interior. */
    bool SupportsRegion(const FBox& SourceRegion,const FStudioLoadCancellation& Cancellation={}) const;
};

struct FStudioVolumeLoadResult
{
    TSharedPtr<const FStudioVolumeReconstruction, ESPMode::ThreadSafe> Volume;
    FString Error;
};

struct FStudioVolumeRenderData
{
    FIntVector Dimensions = FIntVector::ZeroValue;
    FBox SourceBounds = FBox(ForceInit);
    // R = unclamped normalized scalar, G = validity. Interpolate scalar before
    // applying a palette. CPU source precision remains in the immutable frame.
    TArray<FVector2f> Texels;
    FVector2D CylinderCenter = FVector2D::ZeroVector;
    double CylinderRadius = 0;
    double MaximumTransportError = 0;
    FString Error;
};

struct FStudioVolumeIsosurface
{
    TArray<FVector> PositionsMeters;
    TArray<int32> Indices;
    FString Error;
};

namespace StudioVolumes
{
    constexpr int32 MaximumVoxels = 2 * 1024 * 1024;
    FStudioVolumeLoadResult Load(const FString& Path, const FStudioPointRecordingDescriptor& Source,
        TSharedRef<const FStudioPointGeometry, ESPMode::ThreadSafe> Geometry,
        const FStudioLoadCancellation& Cancellation = {}, const FString& ExpectedMetadata = {});
    FStudioVolumeLoadResult OriginalSource(const FStudioPointRecordingDescriptor& Source,
        TSharedRef<const FStudioPointGeometry,ESPMode::ThreadSafe> Geometry,const FStudioLoadCancellation& Cancellation={});
    /** Frame-specific classes: 0 unsupported, 1 valid, 2 source solid. Bounded to one byte/node. */
    TArray<uint8> SourceMask(const FStudioPointFrame& Frame,const FStudioVolumeReconstruction& Volume,
        const FString& Field,bool bVelocity,FString& Error,const FStudioLoadCancellation& Cancellation={},
        TOptional<bool> AirMaskOverride={});
    bool SourceNodeSupported(const FStudioPointFrame& Frame,const FStudioVolumeReconstruction& Volume,
        const FString& Field,int32 Row,bool bVelocity=false);
    bool SampleSource(const FStudioPointFrame& Frame,const FStudioVolumeReconstruction& Volume,
        const TArray<uint8>& Mask,const FString& Field,const FVector& SourcePosition,double& Out);
    bool SupportsSourceRegion(const FStudioVolumeReconstruction& Volume,const TArray<uint8>& Mask,
        const FBox& SourceRegion,const FStudioLoadCancellation& Cancellation={});
    bool IsSourceSolid(const FStudioPointFrame& Frame,const FStudioVolumeReconstruction& Volume,const FVector& SourcePosition);
    FStudioVolumeRenderData Build(const FStudioPointFrame& Frame, const FStudioVolumeReconstruction& Volume,
        const FString& Field, const FStudioColorMapping& Mapping, const FStudioLoadCancellation& Cancellation = {},
        TOptional<bool> AirMaskOverride={});
    FStudioVolumeIsosurface Isosurface(const FStudioVolumeRenderData& Grid, double NormalizedValue,
        const FStudioLoadCancellation& Cancellation = {});
    /** Robust slab intersection; direction is a unit vector and lengths use the
     * same units as the box. Includes rays starting within the clipped volume. */
    bool Intersect(const FVector& Origin, const FVector& Direction, const FBox& Bounds,
        double MaximumDistance, double& Entry, double& Exit);
}
