#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4EnergyBudget.h"
class FStudioModel;
class FStudioHome4Session;
class SVerticalBox;

/** Uses the same retained request editor as Run/Setup. Applied requests and
 * immutable imported energy domains remain visibly separate. */
class SStudioHome4EnergyBudget final:public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4EnergyBudget){}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4Session>,Editor)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    void Tick(const FGeometry&,double,float)override;
    bool Retain();
    bool Apply();
    void RevertText();
    FString StatusText()const{return Status;}
    TArray<FStudioHome4EnergyBudgetRegion> PreviewRegions()const;
private:
    void Sync();
    void Changed(const FString& Key,const FString& Value);
    FString Value(const FString& Key)const;
    TSharedPtr<FStudioHome4Session> Editor;
    TMap<FString,FString> Draft;
    FString BaseSpec,ShownSpec,Status;
    bool bDirty=false,bOpen=false;
};
