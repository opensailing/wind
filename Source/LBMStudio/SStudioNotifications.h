#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioNotifications.h"

class FStudioModel;
class SVerticalBox;
class SButton;

/** A stable captured list: new events appear only after explicit Refresh.
 * Opening the bell acknowledges nothing and never replaces a live project. */
class SStudioNotifications final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioNotifications) {}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model)
        SLATE_EVENT(FSimpleDelegate,OnClose)
        SLATE_ARGUMENT(TFunction<bool(uint64)>,Reveal)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    FReply OnPreviewKeyDown(const FGeometry&,const FKeyEvent&) override;
private:
    void Capture();
    void Rebuild();
    void SetRead(uint64 Id,bool Value);
    FString Summary() const;
    TSharedRef<SWidget> Row(const FStudioNotification& Entry);
    TSharedPtr<FStudioModel> M;
    TSharedPtr<SVerticalBox> Rows;
    TSharedPtr<SButton> UnreadButton;
    FSimpleDelegate Close;
    TFunction<bool(uint64)> Reveal;
    TArray<FStudioNotification> Captured;
    uint64 CapturedThrough=0,CapturedEvicted=0;
    bool bUnreadOnly=false;
    FString ActionNotice;
};
