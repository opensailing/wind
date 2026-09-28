#include "SStudioProbeProfile.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Input/Reply.h"
#include "Framework/Application/SlateApplication.h"
#include "Fonts/FontMeasure.h"

namespace
{
FSlateFontInfo ProfileFont(int32 Size=9){return FCoreStyle::GetDefaultFontStyle(TEXT("Regular"),Size);}
double PlotLeft(const FStudioProbeProfile& P)
{
    const auto Measure=FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
    return FMath::Max(58.,6.+FMath::Max(Measure->Measure(P.RangeLabel(false),ProfileFont(8)).X,Measure->Measure(P.RangeLabel(true),ProfileFont(8)).X));
}
FVector2D PlotPoint(const FStudioProbeProfile& P,int32 I,const FVector2D& Size,double Left)
{
    const auto N=P.NormalizedPosition(I);
    return FVector2D(Left+N.X*FMath::Max(1.,Size.X-Left-10),28+(1-N.Y)*FMath::Max(1.,Size.Y-86));
}
}

void SStudioProbeProfile::Construct(const FArguments& A)
{
    Profile=A._Profile;Background=A._Background;Accent=A._Accent;TextColor=A._TextColor;MutedColor=A._MutedColor;GridColor=A._GridColor;SetCanTick(true);
    SetToolTipText(FText::FromString(TEXT("Line profile of the displayed frame. Hover or use Left/Right, Home/End to inspect each sample. Gaps are unavailable samples.")));
}
void SStudioProbeProfile::Tick(const FGeometry&,double,float)
{
    const auto Current=Profile.Get();if(Current==PaintedProfile)return;
    PaintedProfile=Current;Selected=Current?FMath::Clamp(Selected,INDEX_NONE,Current->Snapshot.Samples.Num()-1):INDEX_NONE;
    Invalidate(EInvalidateWidgetReason::Paint);
}
int32 SStudioProbeProfile::OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 L,const FWidgetStyle&,bool) const
{
    const auto Size=G.GetLocalSize();const auto* Brush=FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
    FSlateDrawElement::MakeBox(Out,L,G.ToPaintGeometry(),Brush,ESlateDrawEffect::None,Background);
    auto Text=[&](const FString& Value,FVector2D At,FLinearColor Color,int32 FontSize=9)
    {FSlateDrawElement::MakeText(Out,L+3,G.ToPaintGeometry(Size,FSlateLayoutTransform(At)),Value,ProfileFont(FontSize),ESlateDrawEffect::None,Color);};
    auto DrawLine=[&](TArray<FVector2D> Points,FLinearColor Color,float Width=1.f)
    {FSlateDrawElement::MakeLines(Out,L+1,G.ToPaintGeometry(),Points,ESlateDrawEffect::None,Color,true,Width);};
    const auto P=Profile.Get();
    if(!P){Text(TEXT("Waiting for displayed-frame samples…"),FVector2D(8,16),MutedColor);return L+3;}
    Text(TEXT("Line profile · ")+P->Snapshot.Label+TEXT(" (")+P->Snapshot.Unit+TEXT(")"),FVector2D(4,4),TextColor);
    const double Left=PlotLeft(*P),Bottom=Size.Y-58,Right=Size.X-10,Height=FMath::Max(1.,Bottom-28);
    for(int32 I=0;I<3;++I)DrawLine({FVector2D(Left,28+Height*I*.5),FVector2D(Right,28+Height*I*.5)},GridColor);
    DrawLine({FVector2D(Left,28),FVector2D(Left,Bottom),FVector2D(Right,Bottom)},MutedColor);
    if(P->ValidSamples)
    {
        if(P->Minimum==P->Maximum)Text(P->RangeLabel(false),FVector2D(2,28+Height*.5-5),MutedColor,8);
        else {Text(P->RangeLabel(true),FVector2D(2,23),MutedColor,8);Text(P->RangeLabel(false),FVector2D(2,Bottom-7),MutedColor,8);}
    }
    else Text(TEXT("No available samples"),FVector2D(64,28+Height*.4),MutedColor);
    for(const auto& Segment:P->Segments)
    {
        TArray<FVector2D> Points;Points.Reserve(Segment.Num());for(int32 I:Segment)Points.Add(PlotPoint(*P,I,Size,Left));
        if(Points.Num()>1)DrawLine(Points,Accent,1.8f);
        // Individual values remain visible when surrounded by missing samples.
        for(const auto& At:Points)FSlateDrawElement::MakeBox(Out,L+2,G.ToPaintGeometry(FVector2D(3,3),FSlateLayoutTransform(At-FVector2D(1.5,1.5))),Brush,ESlateDrawEffect::None,Accent);
    }
    for(int32 I=0;I<P->Snapshot.Samples.Num();++I)if(!P->Snapshot.Samples[I].Value.IsSet())
    {const double X=PlotPoint(*P,I,Size,Left).X;DrawLine({FVector2D(X,Bottom-2),FVector2D(X,Bottom+3)},MutedColor);}
    Text(TEXT("0"),FVector2D(Left-2,Bottom+5),MutedColor,8);
    Text(FString::Printf(TEXT("%.5g m"),P->LengthMeters),FVector2D(FMath::Max(60.,Right-66),Bottom+5),MutedColor,8);
    Text(TEXT("Distance from A"),FVector2D(Left+8,Bottom+20),MutedColor,8);
    if(P->Snapshot.Samples.IsValidIndex(Selected))
    {
        const auto& S=P->Snapshot.Samples[Selected];const auto At=PlotPoint(*P,Selected,Size,Left);
        DrawLine({FVector2D(At.X,28),FVector2D(At.X,Bottom)},Accent*.65f);
        const FString Value=S.Value.IsSet()?FString::Printf(TEXT("%.10g %s"),S.Value.GetValue(),*P->Snapshot.Unit):StudioProbeProfile::SampleStatus(S.Status).Replace(TEXT("_"),TEXT(" "));
        Text(FString::Printf(TEXT("%d/%d · %.6g m · %s"),Selected+1,P->Snapshot.Samples.Num(),S.DistanceAlongLineMeters,*Value),FVector2D(4,Size.Y-17),TextColor,8);
    }
    else Text(TEXT("Hover or use arrow keys to read samples"),FVector2D(4,Size.Y-17),MutedColor,8);
    if(HasKeyboardFocus())DrawLine({{1,1},{Size.X-1,1},{Size.X-1,Size.Y-1},{1,Size.Y-1},{1,1}},Accent);
    return L+3;
}
FReply SStudioProbeProfile::OnKeyDown(const FGeometry&,const FKeyEvent& E)
{
    const auto P=Profile.Get();if(!P)return FReply::Unhandled();
    const int32 Last=P->Snapshot.Samples.Num()-1;
    if(E.GetKey()==EKeys::Home)Selected=0;
    else if(E.GetKey()==EKeys::End)Selected=Last;
    else if(E.GetKey()==EKeys::Right)Selected=FMath::Min(Selected+1,Last);
    else if(E.GetKey()==EKeys::Left)Selected=FMath::Max(Selected-1,0);
    else return FReply::Unhandled();
    Invalidate(EInvalidateWidgetReason::Paint);return FReply::Handled();
}
void SStudioProbeProfile::SelectAt(const FGeometry& G,const FVector2D& At)
{
    const auto P=Profile.Get();if(!P)return;
    const double Left=PlotLeft(*P);const double X=FMath::Clamp((At.X-Left)/FMath::Max(1.,G.GetLocalSize().X-Left-10),0.,1.);
    Selected=FMath::Clamp(FMath::RoundToInt(X*(P->Snapshot.Samples.Num()-1)),0,P->Snapshot.Samples.Num()-1);
    Invalidate(EInvalidateWidgetReason::Paint);
}
FReply SStudioProbeProfile::OnMouseMove(const FGeometry& G,const FPointerEvent& E)
{if(!E.GetCursorDelta().IsNearlyZero())SelectAt(G,G.AbsoluteToLocal(E.GetScreenSpacePosition()));return FReply::Handled();}
FReply SStudioProbeProfile::OnMouseButtonDown(const FGeometry& G,const FPointerEvent& E)
{
    if(E.GetEffectingButton()!=EKeys::LeftMouseButton)return FReply::Unhandled();
    SelectAt(G,G.AbsoluteToLocal(E.GetScreenSpacePosition()));return FReply::Handled().SetUserFocus(SharedThis(this));
}
void SStudioProbeProfile::OnMouseLeave(const FPointerEvent& E)
{SLeafWidget::OnMouseLeave(E);if(!HasKeyboardFocus()){Selected=INDEX_NONE;Invalidate(EInvalidateWidgetReason::Paint);}}
