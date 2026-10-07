#include "SStudioHome4Allocations.h"
#include "StudioTheme.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
void SStudioHome4Allocations::Construct(const FArguments& Args)
{
    Session=Args._Session;SetCanTick(true);
    ChildSlot[SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,14,0,8)[StudioUI::Label(TEXT("Allocation inventory"),13,StudioUI::Text,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[SNew(STextBlock).Font(StudioUI::Font(10)).ColorAndOpacity(StudioUI::Muted).AutoWrapText(true)
            .Text(FText::FromString(TEXT("List solver arrays and buffers to estimate memory. New rows start with the current root cell count when available; edit Nodes for each array, including refined patches. Blank nodes leaves the estimate unknown. Counts stay fixed when the lattice changes. Edits remain pending until Apply.")))]
        +SVerticalBox::Slot().AutoHeight()[SNew(SScrollBox).Orientation(Orient_Horizontal)
            +SScrollBox::Slot()[SNew(SBox).MinDesiredWidth(590)[SAssignNew(Rows,SVerticalBox)]]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,8)[SNew(SButton).Tag(TEXT("Home4AllocationAdd")).ButtonStyle(&StudioUI::ButtonStyle()).ContentPadding(FMargin(9,6))
            .IsEnabled_Lambda([this]{return Session->AllocationCount()<256;})
            .OnClicked_Lambda([this]{Session->AddAllocation();Refresh();return FReply::Handled();})[StudioUI::Label(TEXT("Add array"),10,StudioUI::Cyan)]]];
    Refresh();
}
void SStudioHome4Allocations::Tick(const FGeometry&,double,float)
{if(Count!=Session->AllocationCount())Refresh();}
void SStudioHome4Allocations::Refresh()
{
    Count=Session->AllocationCount();Rows->ClearChildren();
    const TCHAR* Labels[]={TEXT("Array"),TEXT("Nodes"),TEXT("Components"),TEXT("Bytes/value"),TEXT("Buffers")};
    for(int32 Row=0;Row<Count;++Row)
    {
        auto Line=SNew(SHorizontalBox);
        for(int32 Column=0;Column<5;++Column)Line->AddSlot().FillWidth(Column==0?2:1).Padding(0,0,6,6)
            [SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[StudioUI::Label(Labels[Column],9,StudioUI::Muted)]
                +SVerticalBox::Slot().AutoHeight().Padding(0,4)[SNew(SEditableTextBox).Style(&StudioUI::InputStyle()).Font(StudioUI::Font(10))
                    .Tag(FName(*FString::Printf(TEXT("Home4Allocation%d.%d"),Row,Column)))
                    .Text_Lambda([this,Row,Column]{return FText::FromString(Session->AllocationValue(Row,Column));})
                    .HintText(FText::FromString(Column==1?TEXT("Unknown"):TEXT("")))
                    .OnTextChanged_Lambda([this,Row,Column](const FText& T){Session->SetAllocation(Row,Column,T.ToString());})]];
        Line->AddSlot().AutoWidth().VAlign(VAlign_Center)[SNew(SButton).ButtonStyle(&StudioUI::ButtonStyle()).ContentPadding(FMargin(8,6))
            .OnClicked_Lambda([this,Row]{Session->RemoveAllocation(Row);Refresh();return FReply::Handled();})[StudioUI::Label(TEXT("Remove"),9)]];
        Rows->AddSlot().AutoHeight()[Line];
    }
}
