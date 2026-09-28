#pragma once
#include "StudioDomain.h"

namespace StudioBoundarySelection
{
    struct FFaceHandle
    {
        FGuid Target;
        int32 Face=INDEX_NONE;
        FVector Position=FVector::ZeroVector;
        FVector2D Pixel=FVector2D::ZeroVector;
        double Depth=0;
    };
    struct FPatchHit
    {
        FGuid Target;
        FVector Position=FVector::ZeroVector;
        int32 Triangle=INDEX_NONE;
        double Distance=0;
    };
    /** Domain faces use visible, labeled handles so the enclosing box cannot
     * intercept every click intended for an imported surface. */
    TArray<FFaceHandle> FaceHandles(const FStudioDomain& Domain,const FStudioCameraState& Observer,
        FVector2D Size,double ProjectionAspect=0);
    FGuid HitFaceHandle(const TArray<FFaceHandle>& Handles,FVector2D Pixel);
    /** Nearest visible original triangle. No collision proxy, inferred patch,
     * field sample, or ray test against a fabricated extrusion is used. */
    bool PickPatch(const FStudioDomainGeometry& Geometry,const FStudioCameraState& Observer,
        FVector2D Size,FVector2D Pixel,FPatchHit& Out,double ProjectionAspect=0);
}
