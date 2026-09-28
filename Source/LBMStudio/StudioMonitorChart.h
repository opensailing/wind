#pragma once
#include "Widgets/SLeafWidget.h"
#include "StudioModel.h"

/** Native chart of immutable original history rows. Cached reduction depends on
 * chart width and monitor revision, independently of flow rendering/playback. */
class SStudioMonitorChart final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioMonitorChart){} SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model)
        SLATE_ARGUMENT(bool,Compact) SLATE_ATTRIBUTE(bool,Residual) SLATE_ATTRIBUTE(FString,Series) SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    void Tick(const FGeometry&,double,float) override;
    FVector2D ComputeDesiredSize(float) const override;
    int32 OnPaint(const FPaintArgs&,const FGeometry&,const FSlateRect&,FSlateWindowElementList&,int32,const FWidgetStyle&,bool) const override;
    bool SupportsKeyboardFocus() const override { return !bCompact; }
    FReply OnMouseMove(const FGeometry&,const FPointerEvent&) override;
    FReply OnMouseWheel(const FGeometry&,const FPointerEvent&) override;
    FReply OnMouseButtonDown(const FGeometry&,const FPointerEvent&) override;
    FReply OnMouseButtonUp(const FGeometry&,const FPointerEvent&) override;
    FReply OnKeyDown(const FGeometry&,const FKeyEvent&) override;
    void OnMouseLeave(const FPointerEvent&) override;
private:
    TSharedPtr<const FStudioHistory,ESPMode::ThreadSafe> Source() const;
    const FStudioMonitorSettings& Settings() const;
    uint64 SourceRevision() const;
    void UpdateSettings(const FStudioMonitorSettings& Value);
    TArray<FString> VisibleSeries() const;
    void Refresh(const FGeometry&) const;
    FSlateRect PlotRect(const FGeometry&) const;
    void ChangeWindow(double Minimum,double Maximum);
    TSharedPtr<FStudioModel> Model;
    bool bCompact=false;
    TAttribute<FString> SelectedSeries;
    TAttribute<bool> Residual;
    mutable FString CachedSeries;
    mutable FStudioMonitorPlot Plot;
    mutable uint64 Revision=MAX_uint64;
    mutable int32 Width=0;
    mutable TSharedPtr<const FStudioHistory,ESPMode::ThreadSafe> History;
    int32 HoverSample=INDEX_NONE;
    double DragX=0,DragMinimum=0,DragMaximum=1;
};
