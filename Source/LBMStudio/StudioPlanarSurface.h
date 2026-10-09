#pragma once

#include "CoreMinimal.h"
#include "StudioPointRecording.h"

/** Source-row indices and barycentric weights for one reconstructed 2D sample. */
struct FStudioSurfaceLocation
{
    int32 Triangle = INDEX_NONE;
    FIntVector Rows = FIntVector::ZeroValue;
    FVector Weights = FVector::ZeroVector;
};

struct FStudioPlanarSurfaceResult;

/** Immutable XY interpolation index. This is display topology, never a claim of
 * original CFD connectivity. Holes must be excluded by the supplied triangles.
 * Coordinates stay in source meters; caller owns any scene-axis transform.
 */
class FStudioPlanarSurface
{
public:
    static FStudioPlanarSurfaceResult Create(
        TSharedRef<const FStudioPointGeometry, ESPMode::ThreadSafe> Geometry,
        TArray<FIntVector> Triangles, int64 MaxIndexBytes = 64LL * 1024 * 1024,
        const FStudioLoadCancellation& Cancellation = {});

    bool Locate(const FVector2D& SourcePosition, FStudioSurfaceLocation& Out) const;
    /** Returns false for outside/hole/missing/nonfinite values, never a zero fallback. */
    bool Sample(const FVector2D& SourcePosition, const TArray<double>& Values, double& Out) const;
    /** Full triangle-union coverage; no bridging holes between valid endpoints. */
    bool SupportsSegment(const FVector2D& A,const FVector2D& B,
        const FStudioLoadCancellation& Cancellation={}) const;
    int64 IndexBytes() const;
    int32 TriangleCount() const { return Faces.Num(); }
    const TArray<FIntVector>& Triangles() const { return Faces; }
    const FStudioPointGeometry& Geometry() const { return *Points; }

private:
    struct FNode
    {
        FBox2D Bounds = FBox2D(ForceInit);
        int32 First = 0, Count = 0, Left = INDEX_NONE, Right = INDEX_NONE;
    };
    explicit FStudioPlanarSurface(TSharedRef<const FStudioPointGeometry, ESPMode::ThreadSafe> Geometry);
    bool BuildNode(int32 First, int32 Count, int32& OutNode, const FStudioLoadCancellation& Cancellation);
    bool TriangleLocation(int32 Face, const FVector2D& P, FStudioSurfaceLocation& Out) const;
    FVector2D Point(int32 Row) const;

    TSharedRef<const FStudioPointGeometry, ESPMode::ThreadSafe> Points;
    TArray<FIntVector> Faces;
    TArray<int32> Order;
    TArray<FNode> Nodes;
};

struct FStudioPlanarSurfaceResult
{
    TSharedPtr<const FStudioPlanarSurface, ESPMode::ThreadSafe> Surface;
    FString Error;
};
