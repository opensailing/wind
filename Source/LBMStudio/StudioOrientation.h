#pragma once
#include "StudioProject.h"

/** A view direction points from the retained focus toward the camera. */
namespace StudioOrientation
{
    bool IsDirection(const FIntVector& Direction);
    TArray<FIntVector> Directions();
    FString Label(const FIntVector& Direction);
    FStudioCameraState Align(const FStudioCameraState& Camera,const FIntVector& Direction);

    struct FRegion
    {
        FIntVector Direction;
        TArray<FVector2D> Polygon;
        TArray<FVector2D> FaceOutline; // Populated only by the face-center region.
        FVector2D Center;
        double Depth=0;
        bool bFaceCenter=false;
    };
    /** Orthographic UI projection only; it does not transform the CFD scene. */
    TArray<FRegion> Regions(const FQuat& CameraOrientation,const FVector2D& Size);
    int32 Hit(const TArray<FRegion>& Regions,const FVector2D& Position);
}
