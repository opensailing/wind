#include "SStudioHome4Stiffness.h"
#include "StudioHome4Readouts.h"
#include "StudioModel.h"
#include "StudioColor.h"
#include "StudioTheme.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
namespace StudioHome4StiffnessLocal
{
EStudioHome4Quantity Quantity(int32 I)
{const int32 R=I/6,C=I%6;return R<3&&C<3?EStudioHome4Quantity::StiffnessTranslation:R>=3&&C>=3?EStudioHome4Quantity::StiffnessRotation:EStudioHome4Quantity::StiffnessCoupling;}
}
void SStudioHome4Stiffness::Construct(const FArguments& A)
{
    Session=A._Session;Pending=Session->Pending(TEXT("body.stiffness"));SetCanTick(true);
    ChildSlot[SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,15)[StudioUI::Label(TEXT("6-DOF stiffness · [x,y,z,roll,pitch,yaw]"),13,StudioUI::Text,true)]
        +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).AutoWrapText(true).Font(StudioUI::Font(9)).ColorAndOpacity(StudioUI::Muted).Text(FText::FromString(TEXT("K maps displacement/metre and angle/radian to force and moment. Translation–translation, coupled and rotation–rotation blocks have different dimensions. Each entry's tooltip retains LU, SI and ND. A blank matrix is unspecified.")))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,10)[SNew(SScrollBox).Orientation(Orient_Horizontal)+SScrollBox::Slot()[SNew(SBox).MinDesiredWidth(600)[SAssignNew(Rows,SVerticalBox)]]]
        +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Tag(TEXT("Home4Stiffness.retain")).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([this]{Commit();return FReply::Handled();})[StudioUI::Label(TEXT("Retain matrix"),10)]]
            +SHorizontalBox::Slot().AutoWidth().Padding(6,0)[SNew(SButton).Tag(TEXT("Home4Stiffness.clear")).ButtonStyle(&StudioUI::ButtonStyle()).OnClicked_Lambda([this]{Pending.Reset();Session->RetainPending(TEXT("body.stiffness"),Pending);Session->Set(TEXT("geometry.stiffness"),TEXT(""));Refresh();return FReply::Handled();})[StudioUI::Label(TEXT("Leave unspecified"),10)]]]];
    Refresh();
}
void SStudioHome4Stiffness::Tick(const FGeometry&,double,float)
{const auto M=Session->Owner();const int32 Next=M?int32(M->UnitDisplay):0;if(Next!=Display&&Pending.IsEmpty())Refresh();if(Session->Pending(TEXT("body.stiffness")).IsEmpty()&&!Pending.IsEmpty()){Pending.Reset();Refresh();}}
void SStudioHome4Stiffness::Refresh()
{
    FStudioHome4Spec S;FString Error;if(!Session->Build(S,Error,true))return;const auto M=Session->Owner();Display=M?int32(M->UnitDisplay):0;Rows->ClearChildren();
    const TCHAR* Axis[]={TEXT("x"),TEXT("y"),TEXT("z"),TEXT("roll"),TEXT("pitch"),TEXT("yaw")};
    for(int32 R=0;R<6;++R)
    {auto Row=SNew(SHorizontalBox);Row->AddSlot().AutoWidth().VAlign(VAlign_Center)[SNew(SBox).WidthOverride(45)[StudioUI::Label(Axis[R],9,StudioUI::Muted)]];
        for(int32 C=0;C<6;++C)
        {
            const int32 I=6*R+C;const FString Key=LexToString(I);const auto Q=StudioHome4StiffnessLocal::Quantity(I);const double Original=S.Geometry.Stiffness.IsValidIndex(I)?S.Geometry.Stiffness[I]:0;
            const auto Converted=StudioHome4Config::ConvertUnits(Original,Q,EStudioHome4UnitDisplay::Lattice,EStudioHome4UnitDisplay(Display),S);const auto Unit=Converted?EStudioHome4UnitDisplay(Display):EStudioHome4UnitDisplay::Lattice;
            const FString Text=Pending.Contains(Key)?Pending[Key]:S.Geometry.Stiffness.IsEmpty()?FString():FString::Printf(TEXT("%.9g"),Converted.Get(Original));
            Row->AddSlot().FillWidth(1).Padding(2,3)[SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[StudioUI::Label(FString(Axis[C])+TEXT(" · ")+StudioHome4Readouts::Unit(Q,Unit),8,StudioUI::Muted)]+SVerticalBox::Slot().AutoHeight()[SNew(SEditableTextBox).Tag(FName(*(TEXT("Home4Stiffness.")+Key))).Style(&StudioUI::InputStyle()).Font(StudioUI::Font(9)).Text(FText::FromString(Text)).HintText(FText::FromString(TEXT("0"))).ToolTipText(FText::FromString(StudioHome4Readouts::Tooltip(Original,Q,EStudioHome4UnitDisplay::Lattice,&S))).OnTextChanged_Lambda([this,Key,Unit](const FText& T){Pending.Add(Key,T.ToString());Pending.Add(Key+TEXT(".display"),LexToString(int32(Unit)));Session->RetainPending(TEXT("body.stiffness"),Pending);})]];
        }
        Rows->AddSlot().AutoHeight()[Row];
    }
    if(S.Geometry.Stiffness.Num()==36)
    {
        bool bSymmetric=true,bPositive=true;double Scale=1;for(double V:S.Geometry.Stiffness)Scale=FMath::Max(Scale,FMath::Abs(V));const double Eps=1e-10*Scale;
        for(int32 I=0;I<6;++I)for(int32 J=0;J<I;++J)if(FMath::Abs(S.Geometry.Stiffness[I*6+J]-S.Geometry.Stiffness[J*6+I])>Eps)bSymmetric=false;
        double L[6][6]={},D[6]={};for(int32 I=0;I<6&&bSymmetric;++I){L[I][I]=1;double Diagonal=S.Geometry.Stiffness[I*6+I];for(int32 K=0;K<I;++K)Diagonal-=L[I][K]*L[I][K]*D[K];D[I]=Diagonal;if(Diagonal<-Eps)bPositive=false;for(int32 J=I+1;J<6;++J){double V=S.Geometry.Stiffness[J*6+I];for(int32 K=0;K<I;++K)V-=L[J][K]*L[I][K]*D[K];if(FMath::Abs(Diagonal)<=Eps){if(FMath::Abs(V)>Eps)bPositive=false;}else L[J][I]=V/Diagonal;}}
        Rows->AddSlot().AutoHeight().Padding(0,8)[SNew(STextBlock).Tag(TEXT("Home4StiffnessChecks")).AutoWrapText(true).Font(StudioUI::Font(9)).ColorAndOpacity(bSymmetric&&bPositive?StudioUI::Muted:StudioUI::Amber).Text(FText::FromString(!bSymmetric?TEXT("K is not symmetric in its declared generalized coordinates; review nonconservative or asymmetric inputs."):!bPositive?TEXT("Symmetric K has a negative-energy direction; review restoring stability and axis/sign conventions."):TEXT("Symmetric nonnegative geometric K check. Zero modes remain unconstrained; this is not measured hydrodynamic stability.")))];
    }
}
bool SStudioHome4Stiffness::Commit()
{
    FStudioHome4Spec S;FString Error;if(!Session->Build(S,Error,true))return false;auto Matrix=S.Geometry.Stiffness;if(Matrix.IsEmpty())Matrix.Init(0,36);
    for(int32 I=0;I<36;++I)if(const auto* T=Pending.Find(LexToString(I)))
    {
        double V;if(!StudioColor::ParseNumber(*T,V)){Session->Status=TEXT("Keep finite matrix entries; pending text is retained.");return false;}
        const auto* D=Pending.Find(LexToString(I)+TEXT(".display"));const auto Unit=D?EStudioHome4UnitDisplay(FCString::Atoi(**D)):EStudioHome4UnitDisplay::Lattice;
        const auto Converted=StudioHome4Config::ConvertUnits(V,StudioHome4StiffnessLocal::Quantity(I),Unit,EStudioHome4UnitDisplay::Lattice,S);
        if(!Converted){Session->Status=TEXT("The stiffness input requires its declared next-run unit/reference map.");return false;}Matrix[I]=*Converted;
    }
    S.Geometry.Stiffness=Matrix;if(!Session->Replace(S,Error)){Session->Status=Error;return false;}Pending.Reset();Session->RetainPending(TEXT("body.stiffness"),Pending);Refresh();Session->Status=TEXT("Matrix retained with mixed component dimensions. Apply saves the case.");return true;
}
