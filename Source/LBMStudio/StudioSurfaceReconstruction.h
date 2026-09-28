#pragma once

#include "CoreMinimal.h"
#include "StudioPlanarSurface.h"

/** Supplemental display topology, immutable after verified publication. */
struct FStudioSurfaceReconstruction
{
    FString MetadataSHA256, Title, Method, BoundaryOrigin;
    TSharedPtr<const FStudioPlanarSurface, ESPMode::ThreadSafe> Surface;
    TArray<int32> BoundaryRows;
    TArray<FVector2D> Boundary;
    TArray<FString> Limitations;
};

struct FStudioSurfaceLoadResult
{
    TSharedPtr<const FStudioSurfaceReconstruction, ESPMode::ThreadSafe> Reconstruction;
    FString Error;
};

namespace StudioSurfaceReconstructions
{
    /** Worker-only. Source descriptor/geometry must come from the verified point
     * reader. Binds every topology index to its exact original source row; never
     * reorders, rescales, repairs or creates scientific values. No implicit discovery.
     */
    FStudioSurfaceLoadResult Load(const FString& Path, const FStudioPointRecordingDescriptor& Source,
        TSharedRef<const FStudioPointGeometry, ESPMode::ThreadSafe> Geometry,
        const FStudioLoadCancellation& Cancellation = {}, const FString& ExpectedMetadataSHA256 = FString());
}
