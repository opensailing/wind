#pragma once
#include "CoreMinimal.h"
#include "StudioRecording.h"

class IStudioField;

struct FStudioMeshSummary
{
    int32 Triangles=0;
    bool bDerived=false;
    FString Notice;
    bool bFieldFillHidden=false;
};

/** Exact topology in scene meters; no synthetic lattice or point triangulation. */
struct FStudioMeshDisplayData
{
    TArray<FVector> PositionsMeters;
    TArray<int32> Indices;
    int32 TriangleCount=0;
    bool bDerived=false;
    FString Notice;
};

namespace StudioMeshDisplay
{
    // Bounds both CPU staging and the expanded procedural GPU mesh. Large
    // sources fail explicitly instead of presenting a silently decimated mesh.
    constexpr int32 MaximumTriangles=131072;
    FStudioMeshDisplayData Build(const IStudioField& Field,const FStudioLoadCancellation& Cancellation={});
}
