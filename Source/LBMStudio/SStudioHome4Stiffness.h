#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4Session.h"
class SVerticalBox;
class SStudioHome4Stiffness final:public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4Stiffness){} SLATE_ARGUMENT(TSharedPtr<FStudioHome4Session>,Session) SLATE_END_ARGS()
    void Construct(const FArguments&);void Tick(const FGeometry&,double,float)override;
private:TSharedPtr<FStudioHome4Session> Session;TSharedPtr<SVerticalBox> Rows;int32 Display=-1;TMap<FString,FString> Pending;void Refresh();bool Commit();
};
