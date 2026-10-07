#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4Session.h"
class SVerticalBox;
class SStudioHome4Regions final:public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4Regions){} SLATE_ARGUMENT(TSharedPtr<FStudioHome4Session>,Session) SLATE_ARGUMENT(bool,Patches) SLATE_END_ARGS()
    void Construct(const FArguments&);
    void Tick(const FGeometry&,double,float)override;
private:
    TSharedPtr<FStudioHome4Session> Session;TSharedPtr<SVerticalBox> Rows;bool bPatches=false;int32 Selected=0,ShownCount=-1;
    TMap<FString,FString> Pending;
    FString PendingKey()const{return bPatches?TEXT("region.patches"):TEXT("region.zones");}
    void KeepPending();
    void Refresh();void Add();void Remove();bool Commit();
};
