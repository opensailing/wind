#pragma once
#include "Widgets/SCompoundWidget.h"
class FStudioModel;
class SVerticalBox;

/** Explicit development controls. Simulated control counters and cadence notices
 * are independent of recorded frames and contain no solver fields or output files. */
class SStudioHome4RunControls final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4RunControls) {}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>, Model)
        SLATE_EVENT(FSimpleDelegate, OnSubmit)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    void Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime) override;
private:
    void Submit();
    void StepRange();
    void RunTo();
    FString CapabilityText() const;
    FString FrozenText() const;
    FString StatusText() const;
    void RefreshSchedule();
    TWeakPtr<FStudioModel> Model;
    FSimpleDelegate OnSubmit;
    TSharedPtr<SVerticalBox> ScheduleRows;
    FString StepDraft = TEXT("1"), TargetDraft, InputError;
    TArray<FString> OutputSizeDraft;
    FGuid ScheduleRun;
    int32 ScheduleCount = INDEX_NONE;
    uint64 ScheduleLastCommand = 0;
};
