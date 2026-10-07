#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4Session.h"
class SVerticalBox;
class SStudioHome4Allocations final:public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4Allocations){}
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4Session>,Session)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    void Tick(const FGeometry&,double,float)override;
private:
    void Refresh();
    TSharedPtr<FStudioHome4Session> Session;
    TSharedPtr<SVerticalBox> Rows;
    int32 Count=-1;
};
