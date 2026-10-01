/*
THESIS: Author a consistent next-run flow request beside the recorded flow.
OWN-WORLD: Preserve compact native navy Slate rows, cyan values and amber recovery.
STORY: Enter velocity/pressure, inspect linked viscosity, calculate from reference
values, explicitly apply the draft, then save or submit the case.
FIRST VIEWPORT: Flow conditions lead Setup. Reference values expand in place;
calculator/actions share the section. Recorded camera and playback remain live.
FORM: Local Operate extension. One Materials owner, SI storage, explicit target
versus calculated Re. No numerical backend or pressure datum is implied.
FINISH: Two-size native captures, fresh scoped finish review and documentation.
*/
#include "SStudioFlowConditions.h"
#include "StudioModel.h"
#include "StudioTheme.h"
#include "StudioMaterials.h"
#include "StudioMenuButton.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/Input/SButton.h"

using namespace StudioUI;
namespace
{
class SFlowValueBox final : public SEditableTextBox
{
public:
    void Construct(const FArguments& Args)
    {
        SEditableTextBox::Construct(Args);
        // The muted foreground distinguishes unknown values without fading them twice.
        EditableText->SetHintTextOpacity(1.f);
    }
};
}
const TCHAR* SStudioFlowConditions::SaveGuard=TEXT("Apply or revert flow conditions before saving, replacing the project or starting a new control run.");
bool SStudioFlowConditions::Available() const
{const auto M=Model.Pin();return M&&!M->IsProjectOpenPending()&&!M->IsRecordingLoadPending();}
void SStudioFlowConditions::Refresh()
{
    const auto M=Model.Pin();if(!M)return;
    if(Project!=M->Project.Id || Edit.CaseId!=M->Project.Draft.Id)
    {Project=M->Project.Id;Edit.Reset(M->Project.Draft,false);Revision=M->Project.Draft.Revision;bConflict=false;Notice.Empty();Synchronize();return;}
    if(Revision==M->Project.Draft.Revision)return;
    Revision=M->Project.Draft.Revision;
    if(Edit.Matches(M->Project.Draft))return;
    if(Edit.IsDirty() || bConflict)bConflict=true;
    else {Edit.Reset(M->Project.Draft);Notice.Empty();Synchronize();}
}
bool SStudioFlowConditions::HasUnapplied(){Refresh();return bConflict||Edit.IsDirty();}
void SStudioFlowConditions::RequireResolution()
{Notice=SaveGuard;if(const auto M=Model.Pin())M->Notice=SaveGuard;Focus(0);}
void SStudioFlowConditions::Synchronize()
{
    bSynchronizing=true;
    for(int32 I=0;I<Edit.FieldCount;++I)if(const auto P=Inputs[I].Pin())P->SetText(FText::FromString(Edit.Values[I]));
    bSynchronizing=false;Computed=Edit.CalculatedReynolds();
}
void SStudioFlowConditions::Changed()
{Edit.Error.Empty();Edit.ErrorField=INDEX_NONE;if(Notice!=SaveGuard)Notice.Empty();Computed=Edit.CalculatedReynolds();}
void SStudioFlowConditions::Focus(int32 Field)
{
    if(Field<0 || Field>=Edit.FieldCount)Field=0;
    if(Field==Edit.Length || Field==Edit.Density)bAdvanced=true;
    if(const auto P=Inputs[Field].Pin())FSlateApplication::Get().SetKeyboardFocus(P,EFocusCause::Navigation);
}
void SStudioFlowConditions::Apply()
{
    if(!Available())return;Refresh();if(bConflict)return;
    const auto M=Model.Pin();
    if(!M->UpdateFlowConditions(Edit)){Focus(Edit.ErrorField);return;}
    Edit.Reset(M->Project.Draft);Revision=M->Project.Draft.Revision;Notice=TEXT("Flow conditions applied. Save to keep this case.");Synchronize();
}
void SStudioFlowConditions::Revert()
{
    if(!Available())return;const auto M=Model.Pin();Edit.Reset(M->Project.Draft);Revision=M->Project.Draft.Revision;
    bConflict=false;Notice=TEXT("Applied flow conditions restored.");Synchronize();if(M->Notice==SaveGuard)M->Notice=Notice;
}
void SStudioFlowConditions::Calculate(bool bSpeed)
{
    if(!Available())return;Refresh();if(bConflict)return;
    if(!(bSpeed?Edit.UseTargetSpeed():Edit.UseCalculatedReynolds()))
    {bAdvanced=true;if(const auto M=Model.Pin())M->Notice=Edit.Error;Focus(Edit.ErrorField);return;}
    Notice=bSpeed?TEXT("Inlet speed updated in draft; direction retained. Apply to use it."):TEXT("Calculated Re copied to target. Apply to use it.");Synchronize();
}
FString SStudioFlowConditions::Status() const
{
    return bConflict?TEXT("Flow conditions or linked viscosity changed. Revert before applying."):
        !Edit.Error.IsEmpty()?Edit.Error:!Notice.IsEmpty()?Notice:Edit.IsDirty()?TEXT("Unapplied flow conditions."):FString();
}
TSharedRef<SWidget> SStudioFlowConditions::Action(const TCHAR* Caption,FName Tag,TFunction<void()> Callback)
{
    return SNew(SButton).Tag(Tag).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7,6))
        .OnClicked_Lambda([Callback]{Callback();return FReply::Handled();})[Label(Caption,9)];
}
TSharedRef<SWidget> SStudioFlowConditions::Input(int32 Index)
{
    auto Box=SNew(SFlowValueBox).Tag(FName(*FString::Printf(TEXT("FlowValue%d"),Index)))
        .Style(&InputStyle()).Font(Font(10)).Text(FText::FromString(Edit.Values[Index]))
        .ForegroundColor_Lambda([this,Index]{return Edit.Values[Index].IsEmpty()?Muted:StudioUI::Text;})
        .HintText(FText::FromString(TEXT("Unspecified"))).SelectAllTextWhenFocused(true).ClearKeyboardFocusOnCommit(false)
        .IsEnabled_Lambda([this]{return Available();})
        .ToolTipText_Lambda([this,Index]{return FText::FromString(Edit.Values[Index]);})
        .OnTextChanged_Lambda([this,Index](const FText& V){if(!bSynchronizing){Edit.Values[Index]=V.ToString();Changed();}})
        .OnTextCommitted_Lambda([this](const FText&,ETextCommit::Type Type){if(Type==ETextCommit::OnEnter)Apply();});
    Inputs[Index]=Box;return Box;
}
TSharedRef<SWidget> SStudioFlowConditions::Unit(FStudioFlowConditionsEdit::EField Field)
{
    return SNew(SStudioMenuButton).Tag(FName(*FString::Printf(TEXT("FlowUnits%d"),int32(Field))))
        .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(5,4)).IsEnabled_Lambda([this]{return Available();})
        .OnGetMenuContent_Lambda([this,Field]
        {
            auto Items=SNew(SVerticalBox);
            for(int32 U=0;U<Edit.UnitCount(Field);++U)
                Items->AddSlot().AutoHeight()[Action(Edit.UnitLabel(Field,U),FName(*FString::Printf(TEXT("FlowUnit%d_%d"),int32(Field),U)),[this,Field,U]
                {if(Edit.ChangeUnit(Field,U)){Synchronize();Changed();}else if(const auto M=Model.Pin())M->Notice=Edit.Error;FSlateApplication::Get().DismissAllMenus();Focus(Field);})];
            return Items;
        })
        .ButtonContent()[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Muted)
            .Text_Lambda([this,Field]{return FText::FromString(Edit.UnitLabel(Field,Edit.Units[Field]));})];
}
TSharedRef<SWidget> SStudioFlowConditions::Field(int32 Index,const TCHAR* Caption)
{
    auto Line=SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(0,0,6,0)[Label(Caption,9,Muted)]
        +SHorizontalBox::Slot().FillWidth(1.05)[Input(Index)];
    if(Index!=Edit.Reynolds)Line->AddSlot().AutoWidth().Padding(4,0,0,0)[Unit(FStudioFlowConditionsEdit::EField(Index))];
    return SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,7)[Line]
        +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Amber).AutoWrapText(true)
            .Visibility_Lambda([this,Index]{return Edit.ErrorField==Index&&!Edit.Error.IsEmpty()?EVisibility::Visible:EVisibility::Collapsed;})
            .Text_Lambda([this]{return FText::FromString(Edit.Error);})];
}
void SStudioFlowConditions::Construct(const FArguments& A)
{
    Model=A._Model;Refresh();auto Rows=SNew(SVerticalBox);
    Rows->AddSlot().AutoHeight().Padding(0,0,0,9)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text_Lambda([this]{const auto M=Model.Pin();return FText::FromString(M&&M->HasActiveJob()?TEXT("Next-run case. Active run settings are frozen."):TEXT("Next-run case. Recorded fields keep their original physics."));})];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,5)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Inlet velocity · case XYZ"),9,Muted)]
        +SHorizontalBox::Slot().AutoWidth()[Unit(Edit.VelocityX)]];
    auto Velocity=SNew(SHorizontalBox);
    for(int32 I=0;I<3;++I)Velocity->AddSlot().FillWidth(1).Padding(I?4:0,0,0,0)
        [SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[Label(I==0?TEXT("X"):I==1?TEXT("Y"):TEXT("Z"),8,Muted)]
            +SVerticalBox::Slot().AutoHeight()[Input(I)]];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[Velocity];
    Rows->AddSlot().AutoHeight()[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Amber).AutoWrapText(true)
        .Visibility_Lambda([this]{return Edit.ErrorField>=0&&Edit.ErrorField<=2&&!Edit.Error.IsEmpty()?EVisibility::Visible:EVisibility::Collapsed;})
        .Text_Lambda([this]{return FText::FromString(Edit.Error);})];
    Rows->AddSlot().AutoHeight()[Field(Edit.Pressure,TEXT("Outlet pressure"))];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,9)[Label(TEXT("Pressure datum awaits solver definition."),8,Muted)];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,5)[Label(TEXT("Domain fluid · kinematic viscosity"),9,Muted)];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,7)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Text).AutoWrapText(true)
        .Text_Lambda([this]{const auto M=Model.Pin();const auto* F=M?Edit.Fluid(M->Project.Draft):nullptr;
            return FText::FromString(!F?TEXT("No fluid assigned to domain"):F->Name+TEXT(" · ")+(F->KinematicViscosity?StudioMaterials::ExactNumber(*F->KinematicViscosity)+TEXT(" m²/s"):TEXT("viscosity unspecified")));})];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,9)[Action(TEXT("Edit fluid in Materials…"),TEXT("FlowMaterials"),[Delegate=A._OnMaterials]{Delegate.ExecuteIfBound();})];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SButton).Tag(TEXT("FlowAdvanced")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7,5))
        .OnClicked_Lambda([this]{bAdvanced=!bAdvanced;return FReply::Handled();})
        [SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Cyan).Text_Lambda([this]{return FText::FromString(bAdvanced?TEXT("Hide reference values"):TEXT("Reference values…"));})]];
    Rows->AddSlot().AutoHeight()[SNew(SBox).Visibility_Lambda([this]{return bAdvanced?EVisibility::Visible:EVisibility::Collapsed;})
        [SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[Field(Edit.Length,TEXT("Reference length"))]
            +SVerticalBox::Slot().AutoHeight()[Field(Edit.Density,TEXT("Reference density"))]]];
    Rows->AddSlot().AutoHeight()[Field(Edit.Reynolds,TEXT("Target Reynolds (Re)"))];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,5)[SNew(STextBlock).Tag(TEXT("FlowCalculatedRe")).Font(Font(9)).ColorAndOpacity(Cyan).AutoWrapText(true)
        .Text_Lambda([this]{return FText::FromString(bConflict?TEXT("Re calculation paused · linked values changed"):
            Computed?FString::Printf(TEXT("Calculated Re: %.9g · |U| L / ν"),*Computed):TEXT("Calculated Re needs inlet, reference length and fluid viscosity."));})];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SHorizontalBox).IsEnabled_Lambda([this]{return Available()&&!bConflict;})
        +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,4,0)[Action(TEXT("Use calculated Re"),TEXT("FlowCalculate"),[this]{Calculate(false);})]
        +SHorizontalBox::Slot().FillWidth(1)[Action(TEXT("Set speed from Re"),TEXT("FlowTargetSpeed"),[this]{Calculate(true);})]];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text(FText::FromString(TEXT("Target Re is a request. Set speed changes the inlet draft while retaining its direction. Boundary assignments stay in Boundary Conditions.")))];
    auto ApplyButton=Action(TEXT("Apply flow"),TEXT("FlowApply"),[this]{Apply();});
    ApplyButton->SetEnabled(TAttribute<bool>::CreateLambda([this]{return Available()&&!bConflict&&Edit.IsDirty();}));
    auto RevertButton=Action(TEXT("Revert edits"),TEXT("FlowRevert"),[this]{Revert();});
    RevertButton->SetEnabled(TAttribute<bool>::CreateLambda([this]{return Available()&&(bConflict||Edit.IsDirty()||!Edit.Error.IsEmpty());}));
    Rows->AddSlot().AutoHeight().Padding(0,0,0,6)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,4,0)[ApplyButton]+SHorizontalBox::Slot().FillWidth(1)[RevertButton]];
    Rows->AddSlot().AutoHeight()[SNew(STextBlock).Tag(TEXT("FlowStatus")).Font(Font(9)).ColorAndOpacity(Amber).AutoWrapText(true)
        .Text_Lambda([this]{return FText::FromString(Status());})];
    ChildSlot[Rows];
}
