#pragma once

#include "StudioCase.h"
#include "StudioMeshImport.h"
#include "StudioCameraPlacement.h"

struct FStudioDomainGeometryEntry
{
    FGuid Id;
    FString Name,Error;
    FBox Bounds=FBox(ForceInit);
    bool bClosedSurface=false;
    bool bInPreview=false;
};

/** Bounds and preview come only from hash-verified original case geometry. */
struct FStudioDomainGeometry
{
    FString Key;
    TArray<FStudioDomainGeometryEntry> Objects;
    FBox Bounds=FBox(ForceInit);
    TSharedPtr<const FStudioImportedMesh,ESPMode::ThreadSafe> Preview;
    // One stable case patch ID per original preview triangle; no inferred patches.
    TArray<FGuid> TriangleTargets;
    TMap<FGuid,FBox> PatchBounds;
    bool bCancelled=false;
    bool bPreviewLimited=false;
    bool Complete() const;
};

struct FStudioDomainEdit
{
    FStudioDomain Saved;
    FString Minimum[3],Maximum[3],FaceNames[6];
    FString Dimensions[3];
    bool bDimensionInput[3]={false,false,false};
    FString Padding[6]; // Physical distances: -X,+X,-Y,+Y,-Z,+Z.
    FString Error;
    int32 ErrorField=INDEX_NONE;
    void Reset(const FStudioDomain& Domain);
    bool IsDirty() const;
    bool Matches(const FStudioDomain& Domain) const;
    bool Build(FStudioDomain& Out);
    bool Fit(const FBox& GeometryBounds);
    void SetCoordinate(int32 Index,const FString& Text);
    void SetDimension(int32 Axis,const FString& Text);
    void RefreshDimensions();
};

namespace StudioDomain
{
    struct FFaceDrag
    {
        FStudioDomain Domain;
        FStudioCameraState Observer;
        FVector2D Viewport;
        int32 Face=INDEX_NONE;
        double ProjectionAspect=0,StartDistance=0;
    };
    bool BeginFaceDrag(const FStudioDomain& Domain,int32 Face,const FStudioCameraState& Observer,
        FVector2D Viewport,FVector2D Pixel,FFaceDrag& Out,double ProjectionAspect=0);
    bool DragFace(const FFaceDrag& Start,FVector2D Pixel,FStudioDomain& Out);
    FString GeometryKey(const FStudioCaseDraft& Case);
    FStudioDomainGeometry InspectGeometry(const FStudioCaseDraft& Case,const FStudioAssetCancellation& Cancel);
    bool Contains(const FStudioDomain& Domain,const FBox& Bounds);
    bool Padded(const FBox& Bounds,const double (&Padding)[6],FVector& Min,FVector& Max,FString& Error);
    FVector FaceCenter(const FStudioDomain& Domain,int32 Index);
    constexpr int32 MaximumPreviewVertices=500000;
    constexpr int32 MaximumPreviewTriangles=500000;
}
