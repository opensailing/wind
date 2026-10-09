#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4Telemetry.h"
#include "StudioHome4Validation.h"
class FStudioModel;
class SVerticalBox;
class FStudioHome4Session;
/** Saved run identities plus session evidence. A next-run draft never supplies source measurements. */
class SStudioHome4Lineage final:public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4Lineage){}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4Session>,Session)
        SLATE_EVENT(FSimpleDelegate,OnAuthoring)
        SLATE_ARGUMENT(TFunction<const FStudioHome4TelemetryStream*()>,Telemetry)
        SLATE_ARGUMENT(TFunction<TSharedPtr<const FStudioHome4ReferenceEvidence>()>,Evidence)
        SLATE_EVENT(FSimpleDelegate,OnFields)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    void Tick(const FGeometry&,double,float) override;
private:
    TSharedPtr<FStudioModel> Model;
    TSharedPtr<FStudioHome4Session> Session;
    FSimpleDelegate OnAuthoring;
    TSharedPtr<SVerticalBox> Rows;
    TFunction<const FStudioHome4TelemetryStream*()> Telemetry;
    TFunction<TSharedPtr<const FStudioHome4ReferenceEvidence>()> Evidence;
    FSimpleDelegate OnFields;
    FString Signature;
    void Refresh();
};
