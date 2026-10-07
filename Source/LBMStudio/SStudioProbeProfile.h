#pragma once

#include "Widgets/SLeafWidget.h"
#include "StudioProbeProfile.h"

class SStudioProbeProfile final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioProbeProfile){}
        SLATE_ATTRIBUTE(TSharedPtr<const FStudioProbeProfile>,Profile)
        SLATE_ARGUMENT(TFunction<FString(double,const FString&)>,Format)
        SLATE_ARGUMENT(TFunction<FString(double,const FString&)>,FormatTooltip)
        SLATE_ATTRIBUTE(uint64,FormatRevision)
        SLATE_ARGUMENT(FLinearColor,Background)
        SLATE_ARGUMENT(FLinearColor,Accent)
        SLATE_ARGUMENT(FLinearColor,TextColor)
        SLATE_ARGUMENT(FLinearColor,MutedColor)
        SLATE_ARGUMENT(FLinearColor,GridColor)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    FVector2D ComputeDesiredSize(float) const override {return FVector2D(280,190);}
    int32 OnPaint(const FPaintArgs&,const FGeometry&,const FSlateRect&,FSlateWindowElementList&,int32,const FWidgetStyle&,bool) const override;
    void Tick(const FGeometry&,double,float) override;
    bool SupportsKeyboardFocus() const override {return true;}
    FReply OnKeyDown(const FGeometry&,const FKeyEvent&) override;
    FReply OnMouseMove(const FGeometry&,const FPointerEvent&) override;
    FReply OnMouseButtonDown(const FGeometry&,const FPointerEvent&) override;
    void OnMouseLeave(const FPointerEvent&) override;
    int32 SelectedSample() const {return Selected;}
private:
#if WITH_DEV_AUTOMATION_TESTS
    friend class FStudioSurfaceSequenceCommand;
#endif
    void SelectAt(const FGeometry&,const FVector2D&);
    TFunction<FString(double,const FString&)> Format,FormatTooltip;
    TAttribute<uint64> FormatRevision;
    uint64 LastFormatRevision=MAX_uint64;
    FString Readout(double Value,const FString& Unit) const;
    double Left(const FStudioProbeProfile& P) const;
    TAttribute<TSharedPtr<const FStudioProbeProfile>> Profile;
    TSharedPtr<const FStudioProbeProfile> PaintedProfile;
    FLinearColor Background,Accent,TextColor,MutedColor,GridColor;
    int32 Selected=INDEX_NONE;
};
