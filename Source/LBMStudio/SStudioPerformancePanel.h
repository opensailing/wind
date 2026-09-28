#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioPerformance.h"
#include "StudioJobTelemetry.h"

class FStudioModel;
class SScrollBox;

/** A local Solve inspector; the visible flow continues to render alongside it. */
class SStudioPerformancePanel final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioPerformancePanel) {}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model)
        SLATE_ARGUMENT(TSharedPtr<FStudioPerformanceHistory>,History)
        SLATE_EVENT(FSimpleDelegate,OnClose)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    FReply OnKeyDown(const FGeometry& Geometry,const FKeyEvent& Event) override;
    bool SupportsKeyboardFocus() const override {return true;}
    void ResumeReadings();
private:
    const TArray<FStudioPerformanceSample>& Samples() const;
    FStudioJobTelemetryView JobView() const;
    FString AppValue(FName Key) const;
    FString JobValue(FName Key) const;
    void ToggleReadings();
    TWeakPtr<FStudioModel> Model;
    TSharedPtr<FStudioPerformanceHistory> History;
    TSharedPtr<SScrollBox> Scroll;
    FSimpleDelegate Close;
    TArray<FStudioPerformanceSample> Frozen;
    FStudioJobTelemetryView FrozenJob;
    FString FrozenState;
    FString FrozenAt;
    bool bPaused=false;
};
