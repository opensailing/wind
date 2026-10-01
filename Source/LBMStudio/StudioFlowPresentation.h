#pragma once
#include "StudioView.h"

class IStudioField;
struct FStudioStreamlineOutput;
namespace StudioFlowPresentation
{
    /** Camera-only wing and near-wake region, clamped to supplied display bounds.
     * Does not crop geometry, source sampling, streamline integration or exports. */
    FBox OverviewBounds(const IStudioField& Field,const FBox& Bounds);
    /** Bounded current-domain sampling for a frozen custom display range.
     * Keeps source metadata and values intact. Returns false unless a 2D field supplies a wing boundary and scalar samples. */
    bool Overview(const IStudioField& Field,const FBox& Bounds,const FString& Scalar,
        double Aspect,const FStudioInspectionState& Current,FStudioInspectionState& Out);
    struct FDirectionMarker
    {
        FVector PositionMeters,Direction;
        double Scalar=0,LengthMeters=0,RadiusMeters=0;
    };
    /** Bounded glyphs sampled from recorded velocity, including backward traces.
     * Marker size means direction only. Cancellation returns no partial markers. */
    TArray<FDirectionMarker> DirectionMarkers(const IStudioField& Field,const FStudioStreamlineOutput& Streams,
        const FBox& Bounds,const FStudioLoadCancellation& Cancel={});
    struct FSlice
    {
        TArray<FVector> Positions;
        TArray<FVector2D> ScalarOpacity;
        TArray<FVector2D> Velocity;
        TArray<int32> Indices;
    };
    /** Exact original 2D triangles clipped to display bounds. Interpolates the
     * original selected nodal scalar before applying the palette in the shader.
     * Speed uses interpolated original velocity components, then magnitude. */
    FSlice OriginalSlice(const IStudioField& Field,const FBox& Bounds,double Y,
        const FStudioColorMapping& Mapping,const FString& Scalar,const FStudioLoadCancellation& Cancel);
}
