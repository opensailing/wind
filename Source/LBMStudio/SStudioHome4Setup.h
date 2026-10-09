#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4Setup.h"
class SVerticalBox;

/** Reviewed geometric setup actions operate on the same scoped retained draft as the page. */
class SStudioHome4Setup final:public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4Setup){}
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4Session>,Session)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4AuthoringSession>,Authoring)
        SLATE_ARGUMENT(FString,Page)
    SLATE_END_ARGS()
    void Construct(const FArguments&);
    void Tick(const FGeometry&,double,float)override;
private:
    TSharedPtr<FStudioHome4Session> Session;
    TSharedPtr<FStudioHome4AuthoringSession> Authoring;
    TSharedPtr<SVerticalBox> LevelRows,FaceRows;
    FString Page,ShownSHA,StepText=TEXT("0");
    int32 SelectedFace=0;double Step=0;bool bStepValid=true;
    bool Build(FStudioHome4Spec&,FString&);
    void LayoutTank();void RealizeZones();void DeriveCounts();
    void RefreshLevels(const FStudioHome4Spec&);void RefreshFaces(const FStudioHome4Spec&);
    void SetFaceChoice(const FString&,const FString&);
    FString RampDetails()const;FString WaveDetails()const;FString WidthDetails()const;
    FString RampTooltip()const;FString WaveTooltip()const;FString WidthTooltip()const;
};
