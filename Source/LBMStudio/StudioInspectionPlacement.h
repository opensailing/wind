#pragma once

#include "StudioCameraPlacement.h"
#include "StudioInspectionObjects.h"

/** An uncommitted one-to-three-point gesture. The placement plane is frozen at
 * Begin; camera motion only changes the ray used to find the next point. */
struct FStudioInspectionPlacement
{
    FGuid ProjectId,ObjectId;
    FStudioInspectionSource Source;
    int32 ObjectsRevision=0,RequiredPoints=1,SpatialDimensions=2;
    FVector PlaneOrigin=FVector::ZeroVector,PlaneNormal=FVector::RightVector;
    TArray<FVector> Accepted;
    TOptional<FVector> Preview;

    static TOptional<FStudioInspectionPlacement> Begin(const FGuid& Project,const FGuid& Object,
        const FStudioInspectionSource& Source,int32 Revision,FVector Anchor,int32 Points,
        int32 Dimensions,FVector SourceOffset,const FStudioCameraState& Observer);
    bool IsCurrent(const FGuid& Project,const FGuid& Selection,const FStudioInspectionSource& CurrentSource,int32 Revision) const;
    /** Pixel is local to the visible flow panel. Empty clears hover; no field
     * samples or model mutation occur here. Returns whether preview changed. */
    bool UpdatePreview(const FStudioCameraState& Observer,FVector2D Size,
        const TOptional<FVector2D>& Pixel,double ProjectionAspect);
    bool AcceptPreview();
    bool IsComplete() const {return Accepted.Num()==RequiredPoints;}
};
