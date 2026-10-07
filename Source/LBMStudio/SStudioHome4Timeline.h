#pragma once
#include "Widgets/SLeafWidget.h"
#include "StudioHome4Telemetry.h"
#include "StudioRecording.h"
class SStudioHome4Timeline final:public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4Timeline){}
        SLATE_ARGUMENT(TFunction<const FStudioHome4TelemetryStream*()>,Telemetry)
        SLATE_ARGUMENT(TFunction<TOptional<FGuid>()>,SourceRun)
        SLATE_ARGUMENT(TFunction<const TArray<FStudioFrame>*()>,Frames)
        SLATE_ARGUMENT(TFunction<void(int32)>,Review)
        SLATE_ARGUMENT(TFunction<void(const FStudioHome4OutputEvent&)>,WarmStart)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    FVector2D ComputeDesiredSize(float)const override{return FVector2D(400,72);}
    int32 OnPaint(const FPaintArgs&,const FGeometry&,const FSlateRect&,FSlateWindowElementList&,int32,const FWidgetStyle&,bool)const override;
    bool SupportsKeyboardFocus()const override{return true;}
    FReply OnKeyDown(const FGeometry&,const FKeyEvent&)override;
    FReply OnMouseButtonDown(const FGeometry&,const FPointerEvent&)override;
private:
    TArray<FStudioHome4OutputEvent> Events()const;
    void Activate(int32 Index);
    void RestartMenu(int32 Index,const FVector2D& Position);
    TFunction<const FStudioHome4TelemetryStream*()> Telemetry;
    TFunction<TOptional<FGuid>()> SourceRun;
    TFunction<const TArray<FStudioFrame>*()> Frames;
    TFunction<void(int32)> Review;
    TFunction<void(const FStudioHome4OutputEvent&)> WarmStart;
    int32 Selected=INDEX_NONE;
};
