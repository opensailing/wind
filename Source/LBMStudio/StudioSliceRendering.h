#pragma once
#include "StudioInspectionObjects.h"
#include "StudioColor.h"
#include "StudioRecording.h"
class IStudioField;

struct FStudioSliceRenderData
{
    TArray<FVector> PositionsMeters;
    TArray<int32> Indices;
    // Only indexed vertices carry scientific samples; unused grid vertices
    // have finite placeholders for transport and must never be queried/exported.
    TArray<double> Scalars;
    TArray<float> Opacities;
    TMap<FGuid,FString> Notices;
    TSet<FGuid> RenderedSlices;
};
namespace StudioSliceRendering
{
    // Shared sample-grid budget across all visible, matching-source planes.
    constexpr int32 MaximumVertices=65536;
    FStudioSliceRenderData Build(const IStudioField& Field,const FBox& Bounds,
        const TArray<FStudioSliceObject>& Slices,const FString& Scalar,
        const FStudioLoadCancellation& Cancellation={},TOptional<bool> AirMaskOverride={});
}
