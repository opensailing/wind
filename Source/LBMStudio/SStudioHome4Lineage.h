#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4Telemetry.h"
#include "StudioHome4Validation.h"
class FStudioModel;
class SVerticalBox;
/** Saved run identities plus session evidence. A next-run draft never supplies source measurements. */
class SStudioHome4Lineage final:public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4Lineage){}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model)
        SLATE_ARGUMENT(TFunction<const FStudioHome4TelemetryStream*()>,Telemetry)
        SLATE_ARGUMENT(TFunction<TSharedPtr<const FStudioHome4ReferenceEvidence>()>,Evidence)
        SLATE_EVENT(FSimpleDelegate,OnFields)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    void Tick(const FGeometry&,double,float) override;
private:
    TSharedPtr<FStudioModel> Model;
    TSharedPtr<SVerticalBox> Rows;
    TFunction<const FStudioHome4TelemetryStream*()> Telemetry;
    TFunction<TSharedPtr<const FStudioHome4ReferenceEvidence>()> Evidence;
    FSimpleDelegate OnFields;
    FString Signature;
    void Refresh();
};
