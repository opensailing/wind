#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4Session.h"

/** Coupled sizing controls preserve explicit Reynolds target. Every adjustment
 * stays in the retained draft until the parent Apply action commits it. */
class SStudioHome4Sizing final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4Sizing){}
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4Session>,Session)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    bool Adjust(int32 Axis,double Value);
private:
    TSharedPtr<FStudioHome4Session> Session;
    FString Notice;
    double Current(int32 Axis) const;
    bool Ready() const;
};
