#include "SStudioHome4Sizing.h"
#include "StudioHome4Recipes.h"
#include "StudioTheme.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Rendering/DrawElements.h"
#include "Widgets/SLeafWidget.h"

namespace StudioHome4SizingLocal
{
class STradePlot final:public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(STradePlot){} SLATE_ARGUMENT(TSharedPtr<FStudioHome4Session>,Session) SLATE_END_ARGS()
    void Construct(const FArguments& A){Session=A._Session;SetToolTipText(FText::FromString(TEXT("Resolution vs lattice Mach. Curves hold the requested Reynolds number at τ=0.51, 0.6 and 1.0. Recipe points appear only where their supplied parameters determine all coordinates; no validation pass is implied.")));}
    FVector2D ComputeDesiredSize(float) const override{return FVector2D(400,165);}
    int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle&,bool) const override
    {
        FStudioHome4Spec Spec;FString Error;if(!Session->Build(Spec,Error))return Layer;
        const auto D=StudioHome4Config::Derive(Spec);const FVector2D Size=G.GetLocalSize();
        auto Point=[&](double L,double Ma){return FVector2D(36+(Size.X-48)*FMath::Clamp((FMath::Log2(FMath::Max(16.,L))-4)/9,0.,1.),Size.Y-24-(Size.Y-36)*FMath::Clamp(Ma/.3,0.,1.));};
        auto Line=[&](const TArray<FVector2D>& Points,FLinearColor Color,float Width=1){FSlateDrawElement::MakeLines(Out,Layer,G.ToPaintGeometry(),Points,ESlateDrawEffect::None,Color,true,Width);};
        Line({Point(16,0),Point(8192,0)},StudioUI::Muted);Line({Point(16,0),Point(16,.3)},StudioUI::Muted);
        Line({Point(16,.1),Point(8192,.1)},StudioUI::Amber);
        const auto Re=Spec.Reference.Reynolds.IsSet()?Spec.Reference.Reynolds:D.Reynolds;
        if(Re.IsSet()&&Re.GetValue()>0)
        {
            for(const double Tau:{.51,.6,1.})
            {
                TArray<FVector2D> Points;
                for(int32 I=0;I<=80;++I){const double L=FMath::Pow(2.,4+9.*I/80);const double Ma=Re.GetValue()*(Tau-.5)/(FMath::Sqrt(3.)*L);if(Ma<=.3)Points.Add(Point(L,Ma));}
                if(Points.Num()>1)Line(Points,FLinearColor(.2f,.45f,.7f));
            }
        }
        auto Mark=[&](double L,double Ma,FLinearColor Color){if(L<16||L>8192||Ma<0||Ma>.3)return;const auto P=Point(L,Ma);Line({P+FVector2D(-4,0),P+FVector2D(4,0)},Color,2);Line({P+FVector2D(0,-4),P+FVector2D(0,4)},Color,2);};
        for(const auto& R:StudioHome4Recipes::All()){const auto V=StudioHome4Config::Derive(R.Template);if(R.Template.Reference.LengthCells.IsSet()&&V.Mach.IsSet())Mark(R.Template.Reference.LengthCells.GetValue(),V.Mach.GetValue(),StudioUI::Muted);}
        if(Spec.Reference.LengthCells.IsSet()&&D.Mach.IsSet())Mark(Spec.Reference.LengthCells.GetValue(),D.Mach.GetValue(),StudioUI::Cyan);
        FSlateDrawElement::MakeText(Out,Layer+1,G.ToPaintGeometry(FVector2D(Size.X-40,15),FSlateLayoutTransform(FVector2D(40,Size.Y-16))),TEXT("16          Reference length L (cells, log₂)          8192"),StudioUI::Font(9),ESlateDrawEffect::None,StudioUI::Muted);
        FSlateDrawElement::MakeText(Out,Layer+1,G.ToPaintGeometry(FVector2D(40,15),FSlateLayoutTransform(FVector2D(0,0))),TEXT("Ma 0.3"),StudioUI::Font(9),ESlateDrawEffect::None,StudioUI::Muted);
        return Layer+1;
    }
private:TSharedPtr<FStudioHome4Session> Session;
};
}
void SStudioHome4Sizing::Construct(const FArguments& Args)
{
    Session=Args._Session;auto Rows=SNew(SVerticalBox);
    Rows->AddSlot().AutoHeight().Padding(0,0,0,6)[StudioUI::Label(TEXT("Resolution, Mach & relaxation"),14,StudioUI::Text,true)];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,10)[SNew(STextBlock).Text(FText::FromString(TEXT("Hold Reynolds number and drag one control. Changes remain in this draft until Apply. The amber line is Ma=0.1; numerical feasibility is not a validation gate."))).Font(StudioUI::Font(10)).ColorAndOpacity(StudioUI::Muted).AutoWrapText(true)];
    Rows->AddSlot().AutoHeight()[SNew(StudioHome4SizingLocal::STradePlot).Session(Session)];
    const TCHAR* Labels[]={TEXT("Length L"),TEXT("Mach Ma"),TEXT("Heavy τ")};
    for(int32 I=0;I<3;++I)Rows->AddSlot().AutoHeight().Padding(0,7)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(120)[SNew(STextBlock).Font(StudioUI::Font(10)).ColorAndOpacity(StudioUI::Text).Text_Lambda([this,I,Name=FString(Labels[I])]{return FText::FromString(Name+FString::Printf(TEXT("  %.5g"),Current(I)));})]]
        +SHorizontalBox::Slot().FillWidth(1)[SNew(SSlider).Tag(FName(*FString::Printf(TEXT("Home4Size%d"),I))).IsEnabled_Lambda([this]{return Ready();})
            .Value_Lambda([this,I]{const double V=Current(I);return float(I==0?(FMath::Log2(FMath::Max(16.,V))-4)/9:I==1?V/.3:(V-.5001)/1.4999);})
            .OnValueChanged_Lambda([this,I](float V){Adjust(I,I==0?FMath::RoundToDouble(FMath::Pow(2.,4+9*V)):I==1?FMath::Max(.0001,.3*V):.5001+1.4999*V);})]];
    Rows->AddSlot().AutoHeight().Padding(0,7,0,18)[SNew(STextBlock).Font(StudioUI::Font(10)).ColorAndOpacity(StudioUI::Amber).AutoWrapText(true).Text_Lambda([this]{return FText::FromString(!Notice.IsEmpty()?Notice:Ready()?TEXT("Coupled controls ready. Light-fluid viscosity remains its independent input."):TEXT("Supply positive L, Ma (or U), and Re before using coupled controls."));})];
    ChildSlot[Rows];
}
double SStudioHome4Sizing::Current(int32 Axis) const
{
    FStudioHome4Spec S;FString E;if(!Session->Build(S,E))return 0;const auto D=StudioHome4Config::Derive(S);
    return Axis==0?S.Reference.LengthCells.Get(0):Axis==1?D.Mach.Get(0):D.TauHeavy.Get(0);
}
bool SStudioHome4Sizing::Ready() const
{
    FStudioHome4Spec S;FString E;if(!Session->Build(S,E))return false;const auto D=StudioHome4Config::Derive(S);
    return S.Reference.LengthCells.Get(0)>0&&D.Mach.Get(0)>0&&S.Reference.Reynolds.Get(D.Reynolds.Get(0))>0;
}
bool SStudioHome4Sizing::Adjust(int32 Axis,double Value)
{
    FStudioHome4Spec S;FString E;if(!Session->Build(S,E)||!Ready()||Axis<0||Axis>2||!FMath::IsFinite(Value))return false;
    const auto D=StudioHome4Config::Derive(S);const double Re=S.Reference.Reynolds.Get(D.Reynolds.Get(0));
    double L=S.Reference.LengthCells.GetValue(),Ma=D.Mach.GetValue();
    if(Axis==0)L=Value;else if(Axis==1)Ma=Value;else L=Re*(Value-.5)/(FMath::Sqrt(3.)*Ma);
    const double U=Ma/FMath::Sqrt(3.),Nu=U*L/Re;
    if(L<=0||L>1.e12||Ma<=0||Ma>.3||Nu<=0||!FMath::IsFinite(Nu)){Notice=TEXT("Sizing adjustment exceeds supported positive lattice bounds.");return false;}
    auto Set=[&](const TCHAR* K,double V){Session->Set(K,FString::Printf(TEXT("%.17g"),V));};
    Set(TEXT("reference.lengthCells"),L);Set(TEXT("reference.speedCellsPerStep"),U);Set(TEXT("reference.mach"),Ma);Set(TEXT("reference.reynolds"),Re);Set(TEXT("fluids.nuHeavy"),Nu);
    Notice=TEXT("Sizing draft updated; review dependent groups and Apply when ready.");return true;
}
