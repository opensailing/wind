#pragma once
#include "StudioSurfaceReconstruction.h"
#include "StudioColor.h"

/** GPU presentation data. Original float64 arrays remain in the immutable frame.
 * Texels hold unclamped normalized scalars: interpolate first, then color-map
 * in the pixel shader. Source-row texel centres are exact even in half UVs.
 */
struct FStudioScalarSurfaceData
{
    TArray<FVector> Vertices;
    TArray<int32> Indices;
    TArray<FVector2D> TextureCoordinates;
    TArray<float> Scalars;
    FIntPoint TextureSize=FIntPoint::ZeroValue;
    FString Error;
};

namespace StudioSurfaceRendering
{
    FStudioScalarSurfaceData Build(const FStudioPointFrame& Frame,const FStudioSurfaceReconstruction& Surface,
        const FString& FieldId,const FStudioColorMapping& Mapping,const FStudioLoadCancellation& Cancellation={});
}
