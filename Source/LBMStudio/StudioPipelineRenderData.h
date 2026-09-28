#pragma once
#include "StudioPipelineEvaluation.h"
#include "StudioSurfaceRenderData.h"

struct FStudioPipelineRenderMesh
{
    TArray<FVector> Vertices,Normals;
    TArray<FLinearColor> Colors;
    TArray<int32> Indices;
};
/** Bounded GPU data, separate from the complete float64 numerical output.
 * Positions use UE centimeters. Surfaces interpolate scalars before coloring;
 * contour surfaces alone use shape shading. No display extrusion or decimation. */
struct FStudioPipelineRenderData
{
    FStudioScalarSurfaceData Surface;
    FStudioPipelineRenderMesh Glyphs,Contour;
    FBox Bounds=FBox(ForceInit);
    FString Error;
    bool bCancelled=false;
};
namespace StudioPipelineRendering
{
    constexpr int32 MaxPoints=50000;
    constexpr int32 MaxLines=37500;
    constexpr int32 MaxVertices=900000;
    /** Worker-only, all geometry or none. Empty results and probe tables are
     * successful empty meshes; their numerical output remains available. */
    FStudioPipelineRenderData Build(const FStudioPipelineOutput& Output,const FStudioColorMapping& Mapping,
        const FStudioLoadCancellation& Cancellation={});
}
