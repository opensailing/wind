#include "StudioMonitorChart.h"
#include "Rendering/DrawElements.h"
#include "Framework/Application/SlateApplication.h"
#include "Fonts/FontMeasure.h"
#include "Styling/CoreStyle.h"

namespace StudioMonitorChartPrivate
{
const FLinearColor Ink(.75,.82,.88),Muted(.43,.55,.64),Grid(.065,.10,.13),Cyan(0,.72,.9),Amber(1,.60,.06);
FLinearColor Color(int32 Index)
{
    const FLinearColor Colors[]={FLinearColor(.10,.50,1),FLinearColor(.13,.88,.40),FLinearColor(1,.72,.08),
        FLinearColor(.76,.40,1),FLinearColor(1,.36,.28),FLinearColor(.10,.85,.85)};
    return Colors[Index%UE_ARRAY_COUNT(Colors)];
}
FSlateFontInfo Font(int32 Size){return FCoreStyle::GetDefaultFontStyle(TEXT("Regular"),Size);}
}
void SStudioMonitorChart::Construct(const FArguments& A)
{Model=A._Model;bCompact=A._Compact;SelectedSeries=A._Series;SetClipping(EWidgetClipping::ClipToBounds);SetCanTick(true);}
void SStudioMonitorChart::Tick(const FGeometry&,double,float)
{
    if(Model->MonitorHistory()!=History){HoverSample=INDEX_NONE;SetToolTipText(FText::GetEmpty());}
    if(Model->MonitorHistory()!=History||Revision!=Model->MonitorRevision||CachedSeries!=SelectedSeries.Get(FString()))Invalidate(EInvalidateWidgetReason::Paint);
}
FVector2D SStudioMonitorChart::ComputeDesiredSize(float) const{return bCompact?FVector2D(210,110):FVector2D(500,320);}
FSlateRect SStudioMonitorChart::PlotRect(const FGeometry& G) const
{return FSlateRect(bCompact?60:64,bCompact?8:24,FMath::Max(double(bCompact?61:65),G.GetLocalSize().X-14),FMath::Max(25.,G.GetLocalSize().Y-(bCompact?40:44)));}
void SStudioMonitorChart::Refresh(const FGeometry& G) const
{
    const auto Source=Model->MonitorHistory();const auto R=PlotRect(G);const int32 NewWidth=FMath::Clamp(int32(R.Right-R.Left),1,4096);
    const FString Series=SelectedSeries.Get(FString());
    if(Revision==Model->MonitorRevision&&History==Source&&Width==NewWidth&&CachedSeries==Series)return;
    Revision=Model->MonitorRevision;History=Source;Width=NewWidth;CachedSeries=Series;
    auto Settings=Model->Project.Monitor;
    if(bCompact&&!Settings.Series.IsEmpty())Settings.Series={Settings.Series.Contains(Series)?Series:Settings.Series[0]};
    Plot=History?StudioMonitor::BuildPlot(*History,Settings,Width):FStudioMonitorPlot();
}
int32 SStudioMonitorChart::OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle&,bool) const
{
    using namespace StudioMonitorChartPrivate;Refresh(G);
    const auto Size=G.GetLocalSize();const auto Measure=FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
    auto Text=[&](const FString& S,FVector2D At,int32 Points,FLinearColor Color)
    {FSlateDrawElement::MakeText(Out,Layer+2,G.ToPaintGeometry(Measure->Measure(S,Font(Points)),FSlateLayoutTransform(At)),S,Font(Points),ESlateDrawEffect::None,Color);};
    auto Line=[&](TArray<FVector2D> Points,FLinearColor Color,float Thickness=1.f)
    {FSlateDrawElement::MakeLines(Out,Layer+1,G.ToPaintGeometry(),Points,ESlateDrawEffect::None,Color,true,Thickness);};
    if(!History)
    {
        Text(Model->IsMonitorLoading()?TEXT("Verifying history…"):TEXT("No history selected"),FVector2D(8,Size.Y*.35),bCompact?9:13,Muted);
        if(!bCompact)Text(TEXT("Choose a published history in the source panel."),FVector2D(8,Size.Y*.35+25),10,Muted);
        return Layer+2;
    }
    if(!Plot.Error.IsEmpty())
    {Text(Plot.Error,FVector2D(8,Size.Y*.4),bCompact?8:11,Amber);return Layer+2;}
    auto R=PlotRect(G);const double W=R.Right-R.Left;
    // Increase precision until original-time ticks differ, then reduce their count
    // to fit measured native text. Endpoint labels may use separate rows at very
    // narrow widths; neither formatting nor layout changes source coordinates.
    TArray<FString> TimeLabels;TArray<double> TimeLabelX;int32 TimeIntervals=bCompact?2:4;bool StaggerTime=false;
    const int32 TimePoints=bCompact?8:9;
    for(;;)
    {
        for(int32 Precision=3;Precision<=17;++Precision)
        {
            TimeLabels.Reset();bool Distinct=true;
            for(int32 I=0;I<=TimeIntervals;++I)
            {
                const double Time=Plot.TimeMinimum+(Plot.TimeMaximum-Plot.TimeMinimum)*double(I)/TimeIntervals;
                const FString Label=FString::Printf(TEXT("%.*g"),Precision,Time);
                if(I&&Label==TimeLabels.Last())Distinct=false;TimeLabels.Add(Label);
            }
            if(Distinct)break;
        }
        bool Fits=true;double End=-6;TimeLabelX.Reset();
        for(int32 I=0;I<TimeLabels.Num();++I)
        {
            const double LabelWidth=Measure->Measure(TimeLabels[I],Font(TimePoints)).X;
            double At=FMath::Clamp(R.Left+W*double(I)/TimeIntervals-LabelWidth*.5,0.,FMath::Max(0.,Size.X-LabelWidth));
            if(TimeIntervals==1)At=I?FMath::Max(0.,Size.X-LabelWidth):0.;
            if(At<End+6)Fits=false;TimeLabelX.Add(At);End=At+LabelWidth;
        }
        if(Fits)break;
        if(TimeIntervals==1){StaggerTime=true;R.Bottom=FMath::Max(R.Top+1,R.Bottom-12);break;}
        --TimeIntervals;
    }
    const double H=R.Bottom-R.Top;
    auto X=[&](double Value){return R.Left+(Value-Plot.TimeMinimum)/(Plot.TimeMaximum-Plot.TimeMinimum)*W;};
    auto Y=[&](double Value){return R.Bottom-(Value-Plot.ValueMinimum)/(Plot.ValueMaximum-Plot.ValueMinimum)*H;};
    const int32 Ticks=bCompact?2:4;
    for(int32 I=0;I<=Ticks;++I)
    {
        const double Fraction=double(I)/Ticks,Value=Plot.ValueMinimum+(Plot.ValueMaximum-Plot.ValueMinimum)*Fraction;
        const double ValueY=Y(Value);Line({{R.Left,ValueY},{R.Right,ValueY}},Grid);
        const FString Label=Plot.bLogY?FString::Printf(TEXT("%.2g"),FMath::Pow(10.,Value)):FString::Printf(TEXT("%.3g"),Value);
        const auto Extent=Measure->Measure(Label,Font(bCompact?8:9));Text(Label,{R.Left-Extent.X-6,ValueY-Extent.Y*.5},bCompact?8:9,Muted);
    }
    for(int32 I=0;I<=TimeIntervals;++I)
    {
        const double Time=Plot.TimeMinimum+(Plot.TimeMaximum-Plot.TimeMinimum)*double(I)/TimeIntervals,TimeX=X(Time);
        Line({{TimeX,R.Top},{TimeX,R.Bottom}},Grid);
        Text(TimeLabels[I],{TimeLabelX[I],R.Bottom+5+(StaggerTime&&I?12:0)},TimePoints,Muted);
    }
    double LegendX=R.Left;
    for(int32 I=0;I<Plot.Traces.Num();++I)
    {
        const auto& Trace=Plot.Traces[I];const auto& C=*History->FindColumn(Trace.Id);const auto Tint=Color(I);
        TArray<FVector2D> Points;
        auto Flush=[&]
        {
            if(Points.Num()>1)Line(Points,Tint,bCompact?1.25f:1.75f);
            else if(Points.Num()==1)Line({Points[0]-FVector2D(1,0),Points[0]+FVector2D(1,0)},Tint,2.f);
            Points.Reset();
        };
        for(int32 Index:Trace.Samples)
        {
            if(Index==INDEX_NONE){Flush();continue;}
            const double V=Plot.bLogY?FMath::LogX(10.,C.Values[Index]):C.Values[Index];Points.Add({X(History->Times[Index]),Y(V)});
        }
        Flush();
        const FString Name=Trace.Id;const auto E=Measure->Measure(Name,Font(9));
        if(!bCompact&&LegendX+E.X+20<R.Right){Line({{LegendX,11},{LegendX+10,11}},Tint,2);Text(Name,{LegendX+14,4},9,Ink);LegendX+=E.X+30;}
    }
    {
        const FString Axis=(bCompact?TEXT("Time ("):TEXT("Original solver time ("))+History->TimeUnit+TEXT(")");
        const int32 Points=bCompact?8:10;const auto E=Measure->Measure(Axis,Font(Points));
        Text(Axis,{R.Left+(W-E.X)*.5,Size.Y-18},Points,Muted);
    }
    if(HoverSample!=INDEX_NONE&&History->Times.IsValidIndex(HoverSample)&&History->Times[HoverSample]>=Plot.TimeMinimum&&History->Times[HoverSample]<=Plot.TimeMaximum)
    {const double At=X(History->Times[HoverSample]);Line({{At,R.Top},{At,R.Bottom}},Cyan);}
    if(HasKeyboardFocus())Line({{1,1},{Size.X-1,1},{Size.X-1,Size.Y-1},{1,Size.Y-1},{1,1}},Cyan);
    return Layer+2;
}
void SStudioMonitorChart::ChangeWindow(double Minimum,double Maximum)
{
    const auto H=Model->MonitorHistory();if(!H||H->Times.Num()<2)return;
    const double Full=H->Times.Last()-H->Times[0],Span=FMath::Min(Maximum-Minimum,Full);
    if(!FMath::IsFinite(Span)||Span<Full*1.e-8)return;
    Minimum=FMath::Clamp(Minimum,H->Times[0],H->Times.Last()-Span);Maximum=Minimum+Span;
    auto S=Model->Project.Monitor;FString Error;
    if(StudioMonitor::SetTimeWindow(*H,Minimum,Maximum,S,Error))Model->UpdateMonitorSettings(S);
}
FReply SStudioMonitorChart::OnMouseMove(const FGeometry& G,const FPointerEvent& E)
{
    Refresh(G);if(!History||!Plot.Error.IsEmpty())return FReply::Unhandled();
    const auto R=PlotRect(G);const auto Local=G.AbsoluteToLocal(E.GetScreenSpacePosition());
    if(HasMouseCapture())
    {const double Shift=(DragX-Local.X)/(R.Right-R.Left)*(DragMaximum-DragMinimum);ChangeWindow(DragMinimum+Shift,DragMaximum+Shift);return FReply::Handled();}
    const double Fraction=FMath::Clamp((Local.X-R.Left)/(R.Right-R.Left),0.,1.);
    HoverSample=StudioMonitor::NearestSample(History->Times,Plot.TimeMinimum+(Plot.TimeMaximum-Plot.TimeMinimum)*Fraction);
    FString Tip=History->Title+FString::Printf(TEXT("\nOriginal sample %d · %.17g %s"),HoverSample,History->Times[HoverSample],*History->TimeUnit);
    for(const auto& Trace:Plot.Traces){const auto* C=History->FindColumn(Trace.Id);Tip+=FString::Printf(TEXT("\n%s: %.17g %s"),*C->Label,C->Values[HoverSample],*C->Unit);}
    SetToolTipText(FText::FromString(Tip));Invalidate(EInvalidateWidgetReason::Paint);return FReply::Handled();
}
FReply SStudioMonitorChart::OnMouseWheel(const FGeometry& G,const FPointerEvent& E)
{
    Refresh(G);if(bCompact||!History)return FReply::Unhandled();
    const auto R=PlotRect(G);const double F=FMath::Clamp((G.AbsoluteToLocal(E.GetScreenSpacePosition()).X-R.Left)/(R.Right-R.Left),0.,1.);
    const double At=Plot.TimeMinimum+(Plot.TimeMaximum-Plot.TimeMinimum)*F,Span=(Plot.TimeMaximum-Plot.TimeMinimum)*FMath::Pow(.8,E.GetWheelDelta());
    ChangeWindow(At-Span*F,At+Span*(1-F));return FReply::Handled();
}
FReply SStudioMonitorChart::OnMouseButtonDown(const FGeometry& G,const FPointerEvent& E)
{
    Refresh(G);if(bCompact||!History||E.GetEffectingButton()!=EKeys::LeftMouseButton)return FReply::Unhandled();
    DragX=G.AbsoluteToLocal(E.GetScreenSpacePosition()).X;DragMinimum=Plot.TimeMinimum;DragMaximum=Plot.TimeMaximum;
    return FReply::Handled().CaptureMouse(SharedThis(this)).SetUserFocus(SharedThis(this),EFocusCause::Mouse);
}
FReply SStudioMonitorChart::OnMouseButtonUp(const FGeometry&,const FPointerEvent& E)
{return HasMouseCapture()&&E.GetEffectingButton()==EKeys::LeftMouseButton?FReply::Handled().ReleaseMouseCapture():FReply::Unhandled();}
FReply SStudioMonitorChart::OnKeyDown(const FGeometry& G,const FKeyEvent& E)
{
    Refresh(G);if(bCompact||!History)return FReply::Unhandled();
    const double Span=Plot.TimeMaximum-Plot.TimeMinimum,Center=(Plot.TimeMinimum+Plot.TimeMaximum)*.5;
    if(E.GetKey()==EKeys::Home){auto S=Model->Project.Monitor;S.bManualTime=false;Model->UpdateMonitorSettings(S);}
    else if(E.GetKey()==EKeys::Left)ChangeWindow(Plot.TimeMinimum-Span*.1,Plot.TimeMaximum-Span*.1);
    else if(E.GetKey()==EKeys::Right)ChangeWindow(Plot.TimeMinimum+Span*.1,Plot.TimeMaximum+Span*.1);
    else if(E.GetKey()==EKeys::Add||E.GetKey()==EKeys::Equals)ChangeWindow(Center-Span*.4,Center+Span*.4);
    else if(E.GetKey()==EKeys::Subtract||E.GetKey()==EKeys::Hyphen)ChangeWindow(Center-Span*.625,Center+Span*.625);
    else return FReply::Unhandled();
    return FReply::Handled();
}
void SStudioMonitorChart::OnMouseLeave(const FPointerEvent& E)
{SLeafWidget::OnMouseLeave(E);HoverSample=INDEX_NONE;Invalidate(EInvalidateWidgetReason::Paint);}
