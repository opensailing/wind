#pragma once

#include "StudioCameraPlacement.h"
#include "StudioInspectionObjects.h"

struct FStudioProbeMarkerResult;

namespace StudioInspectionOverlay
{
    struct FLine { FGuid Object; FVector A,B; float Width=2; };
    struct FMarker { FGuid Object; FVector Position; FString Label; };
    struct FGeometry { TArray<FLine> Lines; TArray<FMarker> Markers; };

    /** Shared visible annotation geometry for painting and picking. Original-ID
     * probes use only a current resolved marker, with no field reads. */
    FGeometry Build(const FStudioInspectionObjects& Objects,const FStudioInspectionSource& Source,
        const FGuid& Project,const FGuid& Selected,const FBox& Bounds,const FStudioProbeMarkerResult* Markers,
        int32 Dimensions=3,double SourcePlaneY=0,const FBox& SeedBounds=FBox(ForceInit));
    bool ProjectMarker(const FStudioCameraState& Camera,FVector2D Size,const FVector& World,
        FVector2D& Pixel,double ProjectionAspect=0);
    /** Picks visible markers and clipped edges within seven Slate units. Empty
     * space and slice interiors do not capture the camera gesture. */
    FGuid Pick(const FGeometry& Geometry,const FStudioCameraState& Camera,FVector2D Size,
        FVector2D Pixel,const FGuid& Selected,double ProjectionAspect=0);
}
