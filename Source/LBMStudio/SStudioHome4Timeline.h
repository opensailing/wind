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
        SLATE_ARGUMENT(TFunction<TOptional<FGuid>()>,FieldSourceRun)
        SLATE_ARGUMENT(TFunction<const TArray<FStudioFrame>*()>,Frames)
        SLATE_ARGUMENT(TFunction<void(int32)>,Review)
        SLATE_ARGUMENT(TFunction<void(const FStudioHome4OutputEvent&)>,WarmStart)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    FVector2D ComputeDesiredSize(float)const override{return FVector2D(400,88);}
    int32 OnPaint(const FPaintArgs&,const FGeometry&,const FSlateRect&,FSlateWindowElementList&,int32,const FWidgetStyle&,bool)const override;
    bool SupportsKeyboardFocus()const override{return true;}
    FReply OnKeyDown(const FGeometry&,const FKeyEvent&)override;
    FReply OnMouseButtonDown(const FGeometry&,const FPointerEvent&)override;
    TOptional<TPair<int64,int64>> RetainedStepExtent()const;
    TOptional<FStudioHome4OutputEvent> SelectedOutputEvent()const;
    TArray<int64> RetainedGuardSteps()const;
    /** Shared native menu content; callers may host it in a popup or virtual window. */
    TSharedPtr<SWidget> MakeRestartMenu(const FStudioHome4OutputEvent& Event);
private:
    TArray<FStudioHome4OutputEvent> Events()const;
    const TArray<FStudioFrame>* MatchingFieldFrames()const;
    static bool SameEvent(const FStudioHome4OutputEvent& A,const FStudioHome4OutputEvent& B);
    int32 SelectedIndex(const TArray<FStudioHome4OutputEvent>& Events)const;
    FVector2D EventPosition(const FStudioHome4OutputEvent& Event,const FGeometry& Geometry,const TPair<int64,int64>& Extent)const;
    void Activate(int32 Index);
    void RestartMenu(int32 Index,const FVector2D& Position);
    TFunction<const FStudioHome4TelemetryStream*()> Telemetry;
    TFunction<TOptional<FGuid>()> SourceRun;
    TFunction<TOptional<FGuid>()> FieldSourceRun;
    TFunction<const TArray<FStudioFrame>*()> Frames;
    TFunction<void(int32)> Review;
    TFunction<void(const FStudioHome4OutputEvent&)> WarmStart;
    TOptional<FStudioHome4OutputEvent> Selected;
};
