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
    FString Page,Mode=TEXT("mesh"),TimeText=TEXT("0"),DensityText;double Step=0;bool bPlaying=false;
    int32 SelectedLink=0;EStudioHome4UnitDisplay DensityDisplay=EStudioHome4UnitDisplay::Lattice;
    FString Detail()const;FString ReadoutTooltip()const;
    void PinSource();void ApplyEquilibrium();void AdoptMass();void AdoptStiffness();
};
