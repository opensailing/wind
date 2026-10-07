#pragma once
#include "Widgets/SLeafWidget.h"
#include "StudioHome4SpatialDiagnostics.h"
#include "StudioPointRecording.h"
class AStudioScene;
struct FStudioHome4SpatialRegion { FString Name; FBox Bounds; FLinearColor Color; };
namespace StudioHome4SpatialView
{
    /** Readonly annotations in scene metres. No patch interpolation or stitching. */
    bool Regions(const FStudioHome4SpatialEvidence& Evidence,const FStudioPointStructuredGrid& Grid,
        TArray<FStudioHome4SpatialRegion>& Out,FString& Error);
}
class SStudioHome4SpatialOverlay final:public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4SpatialOverlay){}
        SLATE_ARGUMENT(AStudioScene*,Scene)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4SpatialSession>,Session)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    FVector2D ComputeDesiredSize(float) const override {return FVector2D::ZeroVector;}
    int32 OnPaint(const FPaintArgs&,const FGeometry&,const FSlateRect&,FSlateWindowElementList&,int32,const FWidgetStyle&,bool) const override;
private:
    TWeakObjectPtr<AStudioScene> Scene;
    TSharedPtr<FStudioHome4SpatialSession> Session;
};
