#pragma once
#include "StudioView.h"

enum class EStudioCameraPlacementTool : uint8 { Move, Rotate };

/** Transient edit of one saved camera. Never part of a project or CFD frame. */
struct FStudioCameraPlacementDraft
{
    FGuid ProjectId,CameraId;
    int32 CollectionRevision=0;
    FStudioCameraState Camera;
    EStudioCameraPlacementTool Tool=EStudioCameraPlacementTool::Move;
};

namespace StudioCameraPlacement
{
    struct FRay { FVector Origin,Direction; };
    struct FLine { FVector A,B; int32 Axis=INDEX_NONE; };
    struct FDrag
    {
        FStudioCameraState Camera,Observer;
        FVector2D Viewport;
        EStudioCameraPlacementTool Tool=EStudioCameraPlacementTool::Move;
        int32 Axis=INDEX_NONE;
        double ProjectionAspect=0;
        double StartDistance=0;
        FVector StartDirection=FVector::ZeroVector;
    };
    /** Coordinates are meters; +X forward, +Y right, +Z up. Size is the displayed
     * panel. ProjectionAspect is the captured texture's ratio, or zero for Size. */
    bool Ray(const FStudioCameraState& Observer,FVector2D Size,FVector2D Pixel,FRay& Out,double ProjectionAspect=0);
    bool Project(const FStudioCameraState& Observer,FVector2D Size,const FVector& World,FVector2D& Out,double ProjectionAspect=0);
    /** Clips against all six observer planes before dividing by depth. */
    bool ProjectLine(const FStudioCameraState& Observer,FVector2D Size,FVector A,FVector B,FVector2D& OutA,FVector2D& OutB,double ProjectionAspect=0);
    double HandleScale(const FStudioCameraState& Observer,FVector2D Size,const FVector& Position);
    TArray<FLine> Handles(const FStudioCameraState& Camera,const FStudioCameraState& Observer,FVector2D Size,EStudioCameraPlacementTool Tool);
    /** Enabled clipping draws its actual near/far planes. Otherwise the cone ends at focus distance. */
    TArray<FLine> Frustum(const FStudioCameraState& Camera,double Aspect);
    int32 HitHandle(const TArray<FLine>& Lines,const FStudioCameraState& Observer,FVector2D Size,FVector2D Pixel,double ProjectionAspect=0);
    bool BeginDrag(const FStudioCameraState& Camera,const FStudioCameraState& Observer,FVector2D Size,
        FVector2D Pixel,EStudioCameraPlacementTool Tool,int32 Axis,FDrag& Out,double ProjectionAspect=0);
    bool Drag(const FDrag& Start,FVector2D Pixel,FStudioCameraState& Out);
}
