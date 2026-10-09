#include "StudioMonitorChart.h"
#include "Rendering/DrawElements.h"
#include "Framework/Application/SlateApplication.h"
#include "Fonts/FontMeasure.h"
#include "Styling/CoreStyle.h"
#include "StudioProbeHistory.h"
#include "StudioProbeProfile.h"

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
TArray<FString> ValueTickLabels(const FStudioMonitorPlot& Plot,int32 Intervals)
{
    TArray<FString> Labels;
    for(int32 Precision=Plot.bLogY?2:3;Precision<=17;++Precision)
    {
        Labels.Reset();bool Distinct=true;
        for(int32 I=0;I<=Intervals;++I)
        {
            const double AxisValue=Plot.ValueMinimum+(Plot.ValueMaximum-Plot.ValueMinimum)*double(I)/Intervals;
            const double Value=Plot.bLogY?FMath::Pow(10.,AxisValue):AxisValue;
            const FString Label=FString::Printf(TEXT("%.*g"),Precision,Value);
            if(I&&Label==Labels.Last())Distinct=false;Labels.Add(Label);
        }
        if(Distinct)break;
    }
    return Labels;
}
}
void SStudioMonitorChart::Construct(const FArguments& A)
{Model=A._Model;Binding=A._Binding;bCompact=A._Compact;SelectedSeries=A._Series;Residual=A._Residual;SetClipping(EWidgetClipping::ClipToBounds);SetCanTick(true);}
TSharedPtr<const FStudioHistory,ESPMode::ThreadSafe> SStudioMonitorChart::Source() const
{return Binding?Binding->Source():Residual.Get(false)?Model->ResidualHistory():Model->MonitorHistory();}
const FStudioMonitorSettings& SStudioMonitorChart::Settings() const
{return Binding?Binding->Settings():Residual.Get(false)?Model->Project.Residual.Chart:Model->Project.Monitor;}
uint64 SStudioMonitorChart::SourceRevision() const
{return Binding?Binding->Revision():Residual.Get(false)?Model->ResidualRevision:Model->MonitorRevision;}
void SStudioMonitorChart::UpdateSettings(const FStudioMonitorSettings& Value)
{if(Binding)Binding->Update(Value);else if(Residual.Get(false))Model->UpdateResidualSettings(Value);else Model->UpdateMonitorSettings(Value);}
TArray<FString> SStudioMonitorChart::VisibleSeries() const
{
    auto Series=Settings().Series;
    if(bCompact&&!Series.IsEmpty())
    {
        if(Residual.Get(false)){if(Series.Num()>3)Series.SetNum(3);}
        else Series={Series.Contains(SelectedSeries.Get(FString()))?SelectedSeries.Get():Series[0]};
    }
    return Series;
}
void SStudioMonitorChart::Tick(const FGeometry&,double,float)
{
    if(Source()!=History){HoverSample=INDEX_NONE;SetToolTipText(FText::GetEmpty());if(Binding&&Binding->SelectSample)Binding->SelectSample(INDEX_NONE);}
    if(Source()!=History||Revision!=SourceRevision()||CachedSeries!=SelectedSeries.Get(FString()))Invalidate(EInvalidateWidgetReason::Paint);
}
FVector2D SStudioMonitorChart::ComputeDesiredSize(float) const{return bCompact?FVector2D(210,110):FVector2D(500,320);}
FSlateRect SStudioMonitorChart::PlotRect(const FGeometry& G) const
{const int32 Columns=FMath::Max(1,int32((G.GetLocalSize().X-16)/(bCompact?52:190)));
    const bool Legend=!bCompact||Residual.Get(false);
    const double Top=Legend?8+16*FMath::DivideAndRoundUp(VisibleSeries().Num(),Columns):8;
    const auto Measure=FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
    double Left=bCompact?60:64;
    for(const auto& Label:StudioMonitorChartPrivate::ValueTickLabels(Plot,bCompact?2:4))
        Left=FMath::Max(Left,double(Measure->Measure(Label,StudioMonitorChartPrivate::Font(bCompact?8:9)).X)+14);
    return FSlateRect(Left,Top,FMath::Max(Left+1,G.GetLocalSize().X-14),FMath::Max(Top+1,G.GetLocalSize().Y-(bCompact?40:44)));}
void SStudioMonitorChart::Refresh(const FGeometry& G) const
{
    const auto NewSource=Source();const auto R=PlotRect(G);const int32 NewWidth=FMath::Clamp(int32(R.Right-R.Left),1,4096);
    const FString Series=SelectedSeries.Get(FString());
    if(Revision==SourceRevision()&&History==NewSource&&Width==NewWidth&&CachedSeries==Series)return;
    Revision=SourceRevision();History=NewSource;Width=NewWidth;CachedSeries=Series;
    auto ChartSettings=Settings();ChartSettings.Series=VisibleSeries();
    Plot=History?StudioMonitor::BuildPlot(*History,ChartSettings,Width):FStudioMonitorPlot();
    // New bounds can require wider value labels. Reduce against the actual plot
    // width so labels, source-coordinate interactions and geometry stay aligned.
    const auto Measured=PlotRect(G);const int32 MeasuredWidth=FMath::Clamp(int32(Measured.Right-Measured.Left),1,4096);
    if(History&&MeasuredWidth!=Width){Width=MeasuredWidth;Plot=StudioMonitor::BuildPlot(*History,ChartSettings,Width);}
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
        Text((Binding?Binding->Loading():Residual.Get(false)?Model->IsResidualLoading():Model->IsMonitorLoading())?TEXT("Verifying history…"):Settings().HistoryId.IsEmpty()?TEXT("No history selected"):TEXT("History unavailable"),FVector2D(8,Size.Y*.35),bCompact?9:13,Muted);
        if(!bCompact)Text(TEXT("Choose history to load or locate a source."),FVector2D(8,Size.Y*.35+25),10,Muted);
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
    const int32 Ticks=bCompact?FMath::Clamp(int32(H/22),1,2):4;
    const auto ValueLabels=ValueTickLabels(Plot,Ticks);
    for(int32 I=0;I<=Ticks;++I)
    {
        const double Fraction=double(I)/Ticks,Value=Plot.ValueMinimum+(Plot.ValueMaximum-Plot.ValueMinimum)*Fraction;
        const double ValueY=Y(Value);Line({{R.Left,ValueY},{R.Right,ValueY}},Grid);
        const FString& Label=ValueLabels[I];
        const auto Extent=Measure->Measure(Label,Font(bCompact?8:9));Text(Label,{R.Left-Extent.X-6,ValueY-Extent.Y*.5},bCompact?8:9,Muted);
    }
    for(int32 I=0;I<=TimeIntervals;++I)
    {
        const double Time=Plot.TimeMinimum+(Plot.TimeMaximum-Plot.TimeMinimum)*double(I)/TimeIntervals,TimeX=X(Time);
        Line({{TimeX,R.Top},{TimeX,R.Bottom}},Grid);
        Text(TimeLabels[I],{TimeLabelX[I],R.Bottom+5+(StaggerTime&&I?12:0)},TimePoints,Muted);
    }
    const int32 LegendColumns=FMath::Max(1,int32((Size.X-16)/(bCompact?52:190)));
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
        if(!bCompact||Residual.Get(false))
        {
            FString Name=History->ProbeHistory?C.Label:Trace.Id;
            if(Residual.Get(false))Name=Name.Replace(TEXT(".InitialFirst"),bCompact?TEXT(" i"):TEXT(" · first initial"))
                .Replace(TEXT(".FinalLast"),bCompact?TEXT(" f"):TEXT(" · last final"));
            const double Cell=(Size.X-16)/LegendColumns,X0=8+(I%LegendColumns)*Cell,Y0=4+(I/LegendColumns)*16;
            while(Name.Len()>1&&Measure->Measure(Name,Font(bCompact?8:9)).X>Cell-22)Name=Name.LeftChop(Name.EndsWith(TEXT("…"))?2:1)+TEXT("…");
            Line({{X0,Y0+7},{X0+10,Y0+7}},Tint,2);Text(Name,{X0+14,Y0},bCompact?8:9,Ink);
        }
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
    const auto H=Source();if(!H||H->Times.Num()<2)return;
    const double Full=H->Times.Last()-H->Times[0],Span=FMath::Min(Maximum-Minimum,Full);
    if(!FMath::IsFinite(Span)||Span<Full*1.e-8)return;
    Minimum=FMath::Clamp(Minimum,H->Times[0],H->Times.Last()-Span);Maximum=Minimum+Span;
    auto S=Settings();FString Error;
    if(StudioMonitor::SetTimeWindow(*H,Minimum,Maximum,S,Error))UpdateSettings(S);
}
FReply SStudioMonitorChart::OnMouseMove(const FGeometry& G,const FPointerEvent& E)
{
    Refresh(G);if(!History||!Plot.Error.IsEmpty())return FReply::Unhandled();
    const auto R=PlotRect(G);const auto Local=G.AbsoluteToLocal(E.GetScreenSpacePosition());
    if(HasMouseCapture())
    {const double Shift=(DragX-Local.X)/(R.Right-R.Left)*(DragMaximum-DragMinimum);ChangeWindow(DragMinimum+Shift,DragMaximum+Shift);return FReply::Handled();}
    const double Fraction=FMath::Clamp((Local.X-R.Left)/(R.Right-R.Left),0.,1.);
    SelectSample(StudioMonitor::NearestSample(History->Times,Plot.TimeMinimum+(Plot.TimeMaximum-Plot.TimeMinimum)*Fraction));
    return FReply::Handled();
}
void SStudioMonitorChart::SelectSample(int32 Index)
{
    if(!History||!History->Times.IsValidIndex(Index))return;
    HoverSample=Index;if(Binding&&Binding->SelectSample)Binding->SelectSample(Index);
    FString Tip=History->Title+FString::Printf(TEXT("\nOriginal sample %d · %.17g %s"),HoverSample,History->Times[HoverSample],*History->TimeUnit);
    if(History->bResiduals)Tip+=FString::Printf(TEXT(" · Time line %d"),History->TimeSourceLines[HoverSample]);
    for(const auto& Trace:Plot.Traces)
    {
        const auto* C=History->FindColumn(Trace.Id);
        if(FMath::IsFinite(C->Values[HoverSample]))Tip+=FString::Printf(TEXT("\n%s: %.17g %s"),*C->Label,C->Values[HoverSample],*C->Unit);
        else Tip+=TEXT("\n")+C->Label+TEXT(": unavailable");
        if(History->ProbeHistory)
        {
            const int32 Position=History->Columns.IndexOfByPredicate([&](const auto& Column){return Column.Id==C->Id;});
            Tip+=TEXT(" · ")+StudioProbeProfile::SampleStatus(History->ProbeHistory->Frames[HoverSample].Samples[Position].Status);
        }
        if(History->bResiduals)Tip+=FString::Printf(TEXT(" · log line %d"),C->SourceLines[HoverSample]);
    }
    if(History->ProbeHistory)
    {
        const auto& F=History->ProbeHistory->Frames[HoverSample];
        Tip+=FString::Printf(TEXT("\nFrame %d · source step %d"),F.Ordinal+1,F.Frame.Index);
    }
    SetToolTipText(FText::FromString(Tip));Invalidate(EInvalidateWidgetReason::Paint);
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
    if(History->ProbeHistory&&(E.GetKey()==EKeys::Up||E.GetKey()==EKeys::Down))
    {
        SelectSample(FMath::Clamp(HoverSample==INDEX_NONE?0:HoverSample+(E.GetKey()==EKeys::Down?1:-1),0,History->Times.Num()-1));
        return FReply::Handled();
    }
    if(History->ProbeHistory&&E.GetKey()==EKeys::Enter&&HoverSample!=INDEX_NONE&&Binding&&Binding->RevealSample)
    {Binding->RevealSample(HoverSample);return FReply::Handled();}
    const double Span=Plot.TimeMaximum-Plot.TimeMinimum,Center=(Plot.TimeMinimum+Plot.TimeMaximum)*.5;
    if(E.GetKey()==EKeys::Home){auto S=Settings();S.bManualTime=false;UpdateSettings(S);}
    else if(E.GetKey()==EKeys::Left)ChangeWindow(Plot.TimeMinimum-Span*.1,Plot.TimeMaximum-Span*.1);
    else if(E.GetKey()==EKeys::Right)ChangeWindow(Plot.TimeMinimum+Span*.1,Plot.TimeMaximum+Span*.1);
    else if(E.GetKey()==EKeys::Add||E.GetKey()==EKeys::Equals)ChangeWindow(Center-Span*.4,Center+Span*.4);
    else if(E.GetKey()==EKeys::Subtract||E.GetKey()==EKeys::Hyphen)ChangeWindow(Center-Span*.625,Center+Span*.625);
    else return FReply::Unhandled();
    return FReply::Handled();
}
void SStudioMonitorChart::OnMouseLeave(const FPointerEvent& E)
{SLeafWidget::OnMouseLeave(E);HoverSample=INDEX_NONE;Invalidate(EInvalidateWidgetReason::Paint);}
