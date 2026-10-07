#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4Authoring.h"
/** Interactive, source-pinned geometric request preview shared across HOME4 setup pages. */
class SStudioHome4Authoring final:public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4Authoring){}
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4AuthoringSession>,Session)
        SLATE_ARGUMENT(FString,Page)
    SLATE_END_ARGS()
    void Construct(const FArguments&);
    void Tick(const FGeometry&,double,float)override;
private:
    TSharedPtr<FStudioHome4AuthoringSession> Session;
    FString Page,Mode=TEXT("mesh"),TimeText=TEXT("0");double Step=0;
    int32 SelectedLink=0;
    FString Detail()const;
    void PinSource();void ApplyEquilibrium();
};
