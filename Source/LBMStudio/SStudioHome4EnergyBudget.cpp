#include "SStudioHome4EnergyBudget.h"
#include "StudioHome4Session.h"
#include "StudioModel.h"
#include "StudioColor.h"
#include "StudioTheme.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SLeafWidget.h"
#include "Rendering/DrawElements.h"
#include "Framework/Application/SlateApplication.h"
namespace StudioHome4EnergyWidgetPrivate
{
    const FString Group=TEXT("energy-budget");
    FString Vector(const FVector& V){return FString::Printf(TEXT("%.17g, %.17g, %.17g"),V.X,V.Y,V.Z);}
    bool Vector(const FString& Input,FVector& V)
    {TArray<FString> P;Input.ParseIntoArray(P,TEXT(","),false);if(P.Num()!=3)return false;for(int32 I=0;I<3;++I)if(!StudioColor::ParseNumber(P[I],V[I]))return false;return true;}
    FString ChoiceLabel(const FString& V)
    {
        if(V==TEXT("root-cells"))return TEXT("Root cell lengths");
        if(V==TEXT("body-lengths"))return TEXT("Reference body lengths");
        if(V==TEXT("physical-metres"))return TEXT("Physical metres");
        if(V==TEXT("body-CoG-local-XYZ"))return TEXT("Local XYZ about body centre of gravity");
        if(V==TEXT("follow-body"))return TEXT("Follow current body pose");
        if(V==TEXT("fixed-initial-body"))return TEXT("Keep initial body pose");
        if(V==TEXT("inside-box"))return TEXT("Integrate inside box");
        if(V==TEXT("shell-excluding-near"))return TEXT("Inside far, excluding near");
        if(V==TEXT("source-all-phases"))return TEXT("All source phases");
        return TEXT("Select an explicit convention");
    }
    class SBoxPreview final:public SLeafWidget
    {
    public:
        SLATE_BEGIN_ARGS(SBoxPreview){}SLATE_ARGUMENT(TFunction<TArray<FStudioHome4EnergyBudgetRegion>()>,Read)SLATE_END_ARGS()
        void Construct(const FArguments& A){Read=A._Read;}
        FVector2D ComputeDesiredSize(float)const override{return FVector2D(280,145);}
        int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& O,int32 L,const FWidgetStyle&,bool)const override
        {
            const auto Regions=Read();const auto Size=G.GetLocalSize();
            if(Regions.IsEmpty()){FSlateDrawElement::MakeText(O,L,G.ToPaintGeometry(FVector2D(1,1),FSlateLayoutTransform(FVector2D(0,10))),TEXT("No requested boxes; no default radii inferred"),StudioUI::Font(8),ESlateDrawEffect::None,StudioUI::Muted);return L+1;}
            for(int32 K=0;K<Regions.Num();++K)
            {
                const auto& R=Regions[K];const FVector V=R.Maximum-R.Minimum;const double Scale=FMath::Max(1.e-12,V.GetMax());
                const FVector2D Start(K*Size.X/Regions.Num()+10,20);const double W=Size.X/Regions.Num()-20,H=95;
                const FVector2D A=Start+FVector2D(0,H),B=A+FVector2D(W*V.X/Scale,0),C=B+FVector2D(0,-H*V.Z/Scale),D=C-FVector2D(W*V.X/Scale,0),Offset(W*.16*V.Y/Scale,-H*.16*V.Y/Scale);
                const FLinearColor Color=R.Role==TEXT("near")?StudioUI::Cyan:R.Role==TEXT("far")?StudioUI::Amber:StudioUI::Muted;
                for(const auto& P:TArray<TArray<FVector2D>>{{A,B,C,D,A},{A+Offset,B+Offset,C+Offset,D+Offset,A+Offset},{A,A+Offset},{B,B+Offset},{C,C+Offset},{D,D+Offset}})
                    FSlateDrawElement::MakeLines(O,L,G.ToPaintGeometry(),P,ESlateDrawEffect::None,Color,true,1.2);
                FSlateDrawElement::MakeText(O,L+1,G.ToPaintGeometry(FVector2D(1,1),FSlateLayoutTransform(FVector2D(Start.X,0))),R.Role+TEXT(" · ")+R.Units,StudioUI::Font(8),ESlateDrawEffect::None,Color);
            }
            return L+2;
        }
    private:TFunction<TArray<FStudioHome4EnergyBudgetRegion>()> Read;
    };
}
void SStudioHome4EnergyBudget::Construct(const FArguments& A)
{
    Editor=A._Editor?A._Editor:MakeShared<FStudioHome4Session>(A._Model);Sync();
    auto Rows=SNew(SVerticalBox);Rows->AddSlot().AutoHeight()[SNew(SButton).Tag(TEXT("Home4EnergyEdit"))
        .ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([this]{bOpen=!bOpen;return FReply::Handled();})[StudioUI::Label(TEXT("Define next-request energy-budget boxes"),9)]];
    Rows->AddSlot().AutoHeight().Padding(0,4)[SNew(STextBlock).Tag(TEXT("Home4EnergyStatus")).AutoWrapText(true).Font(StudioUI::Font(8)).ColorAndOpacity(StudioUI::Muted)
        .Text_Lambda([this]{return FText::FromString(Status);})];
    auto Body=SNew(SVerticalBox);
    Body->AddSlot().AutoHeight()[StudioUI::Label(TEXT("Explicit body-relative XYZ bounds. Near must lie inside far. Far is a shell excluding near. Enter the actual phase mask and exclusions for every box; near/far masks must match. Air overlap is not automatically interpreted as disjoint. Each box needs body-CoG local XYZ frame and follow-body or fixed-initial-body tracking. Empty definitions remain unknown; these requests compute no energy. Apply applies all retained HOME4 changes."),8,StudioUI::Muted)];
    for(const auto* Role:{TEXT("near"),TEXT("far"),TEXT("air")})
    {
        const FString Prefix=Role;
        Body->AddSlot().AutoHeight().Padding(0,6)[SNew(SCheckBox).Tag(FName(*(TEXT("Home4Energy.")+Prefix+TEXT(".enabled"))))
            .IsChecked_Lambda([this,Prefix]{return Value(Prefix+TEXT(".enabled"))==TEXT("true")?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
            .OnCheckStateChanged_Lambda([this,Prefix](ECheckBoxState C){Changed(Prefix+TEXT(".enabled"),C==ECheckBoxState::Checked?TEXT("true"):TEXT("false"));})[StudioUI::Label(Prefix+TEXT(" box"),9)]];
        for(const auto* Field:{TEXT("body"),TEXT("units"),TEXT("frame"),TEXT("tracking"),TEXT("region"),TEXT("phaseMask"),TEXT("minimum"),TEXT("maximum")})
        {
            const FString Key=Prefix+TEXT(".")+Field;
            const FString Caption=FString(Field)==TEXT("units")?TEXT("Coordinate units"):FString(Field)==TEXT("body")?TEXT("Explicit bound body ID"):FString(Field)==TEXT("frame")?TEXT("Origin and axes"):FString(Field)==TEXT("tracking")?TEXT("Body tracking"):FString(Field)==TEXT("region")?TEXT("Integration region"):FString(Field)==TEXT("phaseMask")?TEXT("Explicit phase mask / exclusions"):FString(Field)+TEXT(" XYZ");
            TArray<FString> Choices;
            if(FString(Field)==TEXT("units"))Choices={TEXT("root-cells"),TEXT("body-lengths"),TEXT("physical-metres")};
            if(FString(Field)==TEXT("frame"))Choices={TEXT("body-CoG-local-XYZ")};
            if(FString(Field)==TEXT("tracking"))Choices={TEXT("follow-body"),TEXT("fixed-initial-body")};
            if(FString(Field)==TEXT("region"))Choices={Prefix==TEXT("far")?TEXT("shell-excluding-near"):TEXT("inside-box")};
            TSharedPtr<SWidget> Control;
            if(Choices.IsEmpty())Control=SNew(SEditableTextBox).Tag(FName(*(TEXT("Home4Energy.")+Key))).Style(&StudioUI::InputStyle()).Font(StudioUI::Font(9))
                .IsEnabled_Lambda([this,Prefix]{return Value(Prefix+TEXT(".enabled"))==TEXT("true");})
                .Text_Lambda([this,Key]{return FText::FromString(Value(Key));}).OnTextChanged_Lambda([this,Key](const FText& V){Changed(Key,V.ToString());});
            else Control=SNew(SComboButton).Tag(FName(*(TEXT("Home4Energy.")+Key))).ButtonStyle(&StudioUI::ButtonStyle()).Method(EPopupMethod::UseCurrentWindow)
                .IsEnabled_Lambda([this,Prefix]{return Value(Prefix+TEXT(".enabled"))==TEXT("true");})
                .ToolTipText_Lambda([this,Key]{return FText::FromString(TEXT("Exact retained request value: ")+Value(Key));})
                .OnGetMenuContent_Lambda([this,Key,Choices]
                {
                    auto Menu=SNew(SVerticalBox);
                    for(const auto& Choice:Choices)Menu->AddSlot().AutoHeight()[SNew(SButton).Tag(FName(*(TEXT("Home4Energy.choice.")+Key+TEXT(".")+Choice))).ButtonStyle(&StudioUI::ButtonStyle())
                        .ToolTipText(FText::FromString(Choice)).OnClicked_Lambda([this,Key,Choice]{Changed(Key,Choice);FSlateApplication::Get().DismissAllMenus();return FReply::Handled();})[StudioUI::Label(StudioHome4EnergyWidgetPrivate::ChoiceLabel(Choice),9)]];
                    return StaticCastSharedRef<SWidget>(Menu);
                }).ButtonContent()[SNew(STextBlock).Font(StudioUI::Font(9)).Text_Lambda([this,Key]{return FText::FromString(StudioHome4EnergyWidgetPrivate::ChoiceLabel(Value(Key)));})];
            Body->AddSlot().AutoHeight().Padding(0,2)[SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1)[StudioUI::Label(Caption,8,StudioUI::Muted)]
                +SHorizontalBox::Slot().FillWidth(1)[Control.ToSharedRef()]];
        }
    }
    Body->AddSlot().AutoHeight().Padding(0,7)[SNew(StudioHome4EnergyWidgetPrivate::SBoxPreview).Tag(TEXT("Home4EnergyPreview")).Read([this]{return PreviewRegions();})];
    Body->AddSlot().AutoHeight()[StudioUI::Label(TEXT("Independent box diagrams show XYZ aspect ratios in each declared unit form. No shared world origin, body pose or cross-unit scale is inferred."),8,StudioUI::Muted)];
    auto Buttons=SNew(SHorizontalBox);
    auto Button=[&](const TCHAR* Caption,const TCHAR* Tag,TFunction<void()> Action){Buttons->AddSlot().AutoWidth().Padding(0,4,5,0)[SNew(SButton).Tag(Tag).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([Action]{Action();return FReply::Handled();})[StudioUI::Label(Caption,8)]];};
    Button(TEXT("Retain boxes"),TEXT("Home4EnergyRetain"),[this]{Retain();});Button(TEXT("Apply all HOME4 changes"),TEXT("Home4EnergyApply"),[this]{Apply();});Button(TEXT("Revert box text"),TEXT("Home4EnergyRevert"),[this]{RevertText();});
    Body->AddSlot().AutoHeight()[Buttons];Rows->AddSlot().AutoHeight()[SNew(SBox).Visibility_Lambda([this]{return bOpen?EVisibility::Visible:EVisibility::Collapsed;})[Body]];ChildSlot[Rows];
}
FString SStudioHome4EnergyBudget::Value(const FString& Key)const
{const auto* V=Draft.Find(Key);return V?*V:FString();}
void SStudioHome4EnergyBudget::Sync()
{
    Editor->Refresh();const auto Pending=Editor->Pending(StudioHome4EnergyWidgetPrivate::Group);
    if(!Pending.IsEmpty()){Draft=Pending;BaseSpec=Value(TEXT("_base"));bDirty=true;return;}
    FStudioHome4Spec S;FString Error;if(!Editor->Build(S,Error,true)){Status=Error;return;}
    const FString Current=StudioHome4Config::Serialize(S);if(Current==ShownSpec&&!bDirty)return;
    Draft.Reset();BaseSpec=Current;ShownSpec=Current;bDirty=false;
    for(const auto& R:S.Authoring.EnergyBudgetRegions){Draft.Add(R.Role+TEXT(".enabled"),TEXT("true"));Draft.Add(R.Role+TEXT(".body"),R.BodyId);Draft.Add(R.Role+TEXT(".units"),R.Units);Draft.Add(R.Role+TEXT(".frame"),R.Frame);Draft.Add(R.Role+TEXT(".tracking"),R.Tracking);Draft.Add(R.Role+TEXT(".region"),R.Region);Draft.Add(R.Role+TEXT(".phaseMask"),R.PhaseMask);Draft.Add(R.Role+TEXT(".minimum"),StudioHome4EnergyWidgetPrivate::Vector(R.Minimum));Draft.Add(R.Role+TEXT(".maximum"),StudioHome4EnergyWidgetPrivate::Vector(R.Maximum));}
    Status=TEXT("Retained next request: ")+StudioHome4EnergyBudget::Description(S.Authoring.EnergyBudgetRegions,&S);
}
void SStudioHome4EnergyBudget::Tick(const FGeometry&,double,float){Sync();}
void SStudioHome4EnergyBudget::Changed(const FString& Key,const FString& Input)
{
    // Slate broadcasts bound-text reloads as changes. A scope reset already
    // replaced Draft; loading those values must not recreate pending edits.
    if(Input.Equals(Value(Key),ESearchCase::CaseSensitive))return;
    if(Input.Len()>256){Status=TEXT("Budget input exceeds its 256-character bound; previous text retained.");return;}
    Draft.Add(Key,Input);Draft.Add(TEXT("_base"),BaseSpec);bDirty=true;Editor->RetainPending(StudioHome4EnergyWidgetPrivate::Group,Draft);Status=TEXT("Budget box text is pending. Retain or revert before queuing; Apply commits all retained HOME4 changes.");
}
TArray<FStudioHome4EnergyBudgetRegion> SStudioHome4EnergyBudget::PreviewRegions()const
{
    TArray<FStudioHome4EnergyBudgetRegion> R;
    for(const auto* Role:{TEXT("near"),TEXT("far"),TEXT("air")})
    {
        const FString Prefix=Role;if(Value(Prefix+TEXT(".enabled"))!=TEXT("true"))continue;
        FStudioHome4EnergyBudgetRegion B;B.Role=Role;B.BodyId=Value(Prefix+TEXT(".body"));B.Units=Value(Prefix+TEXT(".units"));B.Frame=Value(Prefix+TEXT(".frame"));B.Tracking=Value(Prefix+TEXT(".tracking"));B.Region=Value(Prefix+TEXT(".region"));B.PhaseMask=Value(Prefix+TEXT(".phaseMask"));
        if(!StudioHome4EnergyWidgetPrivate::Vector(Value(Prefix+TEXT(".minimum")),B.Minimum)||!StudioHome4EnergyWidgetPrivate::Vector(Value(Prefix+TEXT(".maximum")),B.Maximum))return {};
        R.Add(MoveTemp(B));
    }
    FString Error;if(!StudioHome4EnergyBudget::Validate(R,Error))return {};
    return R;
}
bool SStudioHome4EnergyBudget::Retain()
{
    Editor->Refresh();if(!bDirty){Status=TEXT("No pending box text. Retained request remains unchanged.");return true;}
    if(Editor->Pending(StudioHome4EnergyWidgetPrivate::Group).IsEmpty()){Sync();Status=TEXT("Scope changed; previous pending box text was cleared.");return false;}
    FStudioHome4Spec S;if(!Editor->Build(S,Status,true))return false;
    if(StudioHome4Config::Serialize(S)!=BaseSpec){Status=TEXT("The retained request changed while editing boxes. Revert box text before applying them to the new request.");return false;}
    S.Authoring.EnergyBudgetRegions.Reset();
    for(const auto* Role:{TEXT("near"),TEXT("far"),TEXT("air")})
    {
        const FString Prefix=Role;if(Value(Prefix+TEXT(".enabled"))!=TEXT("true"))continue;
        FStudioHome4EnergyBudgetRegion B;B.Role=Role;B.BodyId=Value(Prefix+TEXT(".body"));B.Units=Value(Prefix+TEXT(".units"));B.Frame=Value(Prefix+TEXT(".frame"));B.Tracking=Value(Prefix+TEXT(".tracking"));B.Region=Value(Prefix+TEXT(".region"));B.PhaseMask=Value(Prefix+TEXT(".phaseMask"));
        if(!StudioHome4EnergyWidgetPrivate::Vector(Value(Prefix+TEXT(".minimum")),B.Minimum)||!StudioHome4EnergyWidgetPrivate::Vector(Value(Prefix+TEXT(".maximum")),B.Maximum)){Status=TEXT("Enter all three finite minimum/maximum coordinates explicitly.");return false;}
        S.Authoring.EnergyBudgetRegions.Add(MoveTemp(B));
    }
    if(!StudioHome4EnergyBudget::ValidateRequest(S,Status))return false;
    Editor->RetainPending(StudioHome4EnergyWidgetPrivate::Group,{});
    if(Editor->HasPending()||!Editor->Replace(S,Status)){Editor->RetainPending(StudioHome4EnergyWidgetPrivate::Group,Draft);if(Status.IsEmpty())Status=TEXT("Retain or revert other pending HOME4 editor text first.");return false;}
    bDirty=false;ShownSpec.Empty();Sync();Status=TEXT("Budget boxes retained; Apply commits all retained HOME4 changes. Imported energy domains remain independent.");return true;
}
bool SStudioHome4EnergyBudget::Apply()
{
    if(bDirty&&!Retain())return false;
    if(!Editor->Apply()){Status=Editor->Status;return false;}
    Sync();Status=TEXT("All retained HOME4 changes applied, including requested energy boxes. No measured budget or gate was created.");return true;
}
void SStudioHome4EnergyBudget::RevertText()
{Editor->RetainPending(StudioHome4EnergyWidgetPrivate::Group,{});bDirty=false;ShownSpec.Empty();Sync();Status=TEXT("Pending box text reverted; retained request remains unchanged.");}
