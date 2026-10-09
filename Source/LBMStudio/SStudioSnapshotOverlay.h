#pragma once
#include "StudioSnapshot.h"
#include "Widgets/SLeafWidget.h"

/** Offscreen, value-only annotation pass. No application controls are drawn. */
class SStudioSnapshotOverlay final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioSnapshotOverlay){} SLATE_ARGUMENT(FStudioSnapshot,Snapshot) SLATE_END_ARGS()
    void Construct(const FArguments& Args){Snapshot=Args._Snapshot;}
    FVector2D ComputeDesiredSize(float) const override{return FVector2D(Snapshot.Options.Size);}
    int32 OnPaint(const FPaintArgs&,const FGeometry&,const FSlateRect&,FSlateWindowElementList&,int32,const FWidgetStyle&,bool) const override;
private:
    FStudioSnapshot Snapshot;
};
