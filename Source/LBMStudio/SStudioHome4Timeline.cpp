#include "SStudioHome4Timeline.h"
#include "StudioTheme.h"
#include "Rendering/DrawElements.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"

void SStudioHome4Timeline::Construct(const FArguments& A)
{
    Telemetry=A._Telemetry;SourceRun=A._SourceRun;FieldSourceRun=A._FieldSourceRun;
    Frames=A._Frames;Review=A._Review;WarmStart=A._WarmStart;
    SetToolTipText(FText::FromString(TEXT("Original output events for this source run. Four lanes distinguish traces, slices, visualization snapshots and restart states. Select a marker to inspect its path. Only an exact visualization frame from the same original run can move replay.")));
}
TArray<FStudioHome4OutputEvent> SStudioHome4Timeline::Events()const
{
    TArray<FStudioHome4OutputEvent> Out;
    const auto T=Telemetry?Telemetry():nullptr;const auto Id=SourceRun?SourceRun():TOptional<FGuid>();
    if(T&&Id&&Id->IsValid())for(const auto& E:T->OutputEvents())if(E.Source.RunId==*Id&&E.Step)Out.Add(E);
    Out.StableSort([](const auto& A,const auto& B){return *A.Step<*B.Step;});return Out;
}
const TArray<FStudioFrame>* SStudioHome4Timeline::MatchingFieldFrames()const
{
    const auto Log=SourceRun?SourceRun():TOptional<FGuid>();
    const auto Field=FieldSourceRun?FieldSourceRun():TOptional<FGuid>();
    return Log&&Log->IsValid()&&Field&&Field->IsValid()&&*Log==*Field&&Frames?Frames():nullptr;
}
TOptional<TPair<int64,int64>> SStudioHome4Timeline::RetainedStepExtent()const
{
    TOptional<TPair<int64,int64>> Extent;
    auto Include=[&](int64 Step){if(!Extent)Extent=TPair<int64,int64>(Step,Step);else{Extent->Key=FMath::Min(Extent->Key,Step);Extent->Value=FMath::Max(Extent->Value,Step);}};
    const auto T=Telemetry?Telemetry():nullptr;const auto Id=SourceRun?SourceRun():TOptional<FGuid>();
    if(!Id||!Id->IsValid())return {};
    if(T)
    {
        for(const auto& S:T->History())if(S.Source.RunId==*Id&&S.Step)Include(*S.Step);
        for(const auto& E:T->OutputEvents())if(E.Source.RunId==*Id&&E.Step)Include(*E.Step);
        for(const auto& A:T->ActionRequests())if(A.Source.RunId==*Id&&A.Step)Include(*A.Step);
    }
    if(const auto F=MatchingFieldFrames())for(const auto& Frame:*F)Include(Frame.Index);
    return Extent;
}
bool SStudioHome4Timeline::SameEvent(const FStudioHome4OutputEvent& A,const FStudioHome4OutputEvent& B)
{
    return A.Source.RunId==B.Source.RunId&&A.Source.SourceId==B.Source.SourceId&&A.RecordIndex==B.RecordIndex&&A.Kind==B.Kind&&A.Step==B.Step&&A.Path==B.Path;
}
int32 SStudioHome4Timeline::SelectedIndex(const TArray<FStudioHome4OutputEvent>& E)const
{return Selected?E.IndexOfByPredicate([&](const auto& V){return SameEvent(V,*Selected);}):INDEX_NONE;}
TOptional<FStudioHome4OutputEvent> SStudioHome4Timeline::SelectedOutputEvent()const
{return SelectedIndex(Events())!=INDEX_NONE?Selected:TOptional<FStudioHome4OutputEvent>();}
TArray<int64> SStudioHome4Timeline::RetainedGuardSteps()const
{
    TArray<int64> Steps;const auto T=Telemetry?Telemetry():nullptr;const auto Id=SourceRun?SourceRun():TOptional<FGuid>();
    if(T&&Id&&Id->IsValid())for(const auto& Action:T->ActionRequests())
    {
        if(Action.Source.RunId!=*Id)continue;
        // LastGoodStep is recovery context, never the guard's occurrence step.
        if(Action.Step)Steps.AddUnique(*Action.Step);
    }
    Steps.Sort();return Steps;
}
FVector2D SStudioHome4Timeline::EventPosition(const FStudioHome4OutputEvent& E,const FGeometry& G,const TPair<int64,int64>& Extent)const
{
    // Steps are nonnegative retained int64 values; subtract before converting so
    // adjacent large solver steps do not collapse from double rounding.
    const double Fraction=Extent.Key==Extent.Value?.5:double(*E.Step-Extent.Key)/double(Extent.Value-Extent.Key);
    return FVector2D(60+FMath::Max(0.,double(G.GetLocalSize().X)-66)*Fraction,int32(E.Kind)*17+7);
}
int32 SStudioHome4Timeline::OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 L,const FWidgetStyle&,bool)const
{
    const auto Size=G.GetLocalSize();const auto E=Events();const auto Extent=RetainedStepExtent();const int32 Selection=SelectedIndex(E);
    const TCHAR* Names[]={TEXT("Trace"),TEXT("Slice"),TEXT("Viz"),TEXT("Restart")};
    const FLinearColor Colors[]={StudioUI::Muted,StudioUI::Cyan,FLinearColor(.25,.6,.9),StudioUI::Amber};
    for(int32 Row=0;Row<4;++Row)
    {
        FSlateDrawElement::MakeText(Out,L,G.ToPaintGeometry(FVector2D(58,14),FSlateLayoutTransform(FVector2D(0,Row*17))),Names[Row],StudioUI::Font(8),ESlateDrawEffect::None,Colors[Row]);
        FSlateDrawElement::MakeLines(Out,L,G.ToPaintGeometry(),{FVector2D(60,Row*17+7),FVector2D(Size.X-4,Row*17+7)},ESlateDrawEffect::None,StudioUI::Muted*.25f);
    }
    const auto Guards=RetainedGuardSteps();
    const FString Bounds=Extent?FString::Printf(TEXT("Original steps %lld–%lld%s"),Extent->Key,Extent->Value,Guards.IsEmpty()?TEXT(""):TEXT(" · red ×: guard")):TEXT("Original step extent unavailable");
    FSlateDrawElement::MakeText(Out,L+1,G.ToPaintGeometry(FVector2D(Size.X,16),FSlateLayoutTransform(FVector2D(0,70))),Bounds,StudioUI::Font(8),ESlateDrawEffect::None,StudioUI::Muted);
    if(!Extent||E.IsEmpty())
        FSlateDrawElement::MakeText(Out,L+1,G.ToPaintGeometry(FVector2D(Size.X-70,16),FSlateLayoutTransform(FVector2D(70,25))),TEXT("No original output events attached to this source run"),StudioUI::Font(9),ESlateDrawEffect::None,StudioUI::Muted);
    if(Extent)for(int32 I=0;I<E.Num();++I)
    {
        const auto At=EventPosition(E[I],G,*Extent);
        FSlateDrawElement::MakeLines(Out,L+1,G.ToPaintGeometry(),{At-FVector2D(0,5),At+FVector2D(0,5)},ESlateDrawEffect::None,Colors[int32(E[I].Kind)],true,I==Selection?4:2);
    }
    if(Extent)for(const int64 Step:Guards)
    {
        FStudioHome4OutputEvent Guard;Guard.Step=Step;const auto At=EventPosition(Guard,G,*Extent);
        const FLinearColor Red(.95,.25,.25);
        FSlateDrawElement::MakeLines(Out,L+2,G.ToPaintGeometry(),{At-FVector2D(4,4),At+FVector2D(4,4)},ESlateDrawEffect::None,Red,true,2);
        FSlateDrawElement::MakeLines(Out,L+2,G.ToPaintGeometry(),{At-FVector2D(4,-4),At+FVector2D(4,-4)},ESlateDrawEffect::None,Red,true,2);
    }
    return L+2;
}
void SStudioHome4Timeline::Activate(int32 Index)
{
    const auto E=Events();if(!E.IsValidIndex(Index)){Selected.Reset();return;}Selected=E[Index];
    const auto& Event=E[Index];SetToolTipText(FText::FromString(FString::Printf(TEXT("Original step %lld · %s"),*Event.Step,*Event.Path)));
    if(Event.Kind==EStudioHome4OutputKind::Visualization&&Review)
        if(const auto F=MatchingFieldFrames())for(int32 I=0;I<F->Num();++I)if((*F)[I].Index==*Event.Step){Review(I);break;}
    Invalidate(EInvalidateWidgetReason::Paint);
}
FReply SStudioHome4Timeline::OnKeyDown(const FGeometry&,const FKeyEvent& E)
{
    const auto Current=Events();const int32 Count=Current.Num(),Index=SelectedIndex(Current);
    if(Count==0){Selected.Reset();return FReply::Unhandled();}
    if(E.GetKey()==EKeys::Left)Activate(FMath::Max(0,Index-1));
    else if(E.GetKey()==EKeys::Right)Activate(FMath::Min(Count-1,Index+1));
    else if(E.GetKey()==EKeys::Home)Activate(0);else if(E.GetKey()==EKeys::End)Activate(Count-1);
    else if(E.GetKey()==EKeys::Enter)RestartMenu(Index,GetCachedGeometry().LocalToAbsolute(FVector2D(60,60)));else return FReply::Unhandled();
    return FReply::Handled();
}
FReply SStudioHome4Timeline::OnMouseButtonDown(const FGeometry& G,const FPointerEvent& Event)
{
    if(Event.GetEffectingButton()!=EKeys::LeftMouseButton&&Event.GetEffectingButton()!=EKeys::RightMouseButton)return FReply::Unhandled();
    const auto E=Events();const auto Extent=RetainedStepExtent();if(!Extent)return FReply::Handled();
    const auto P=G.AbsoluteToLocal(Event.GetScreenSpacePosition());double Best=12;int32 Index=INDEX_NONE;
    for(int32 I=0;I<E.Num();++I){const double D=(P-EventPosition(E[I],G,*Extent)).Size();if(D<Best){Best=D;Index=I;}}
    if(Event.GetEffectingButton()==EKeys::RightMouseButton)RestartMenu(Index,Event.GetScreenSpacePosition());
    else
    {
        Activate(Index);
        if(Index==INDEX_NONE)
        {
            const auto T=Telemetry?Telemetry():nullptr;const auto Id=SourceRun?SourceRun():TOptional<FGuid>();
            if(T&&Id&&Id->IsValid())for(const auto& Action:T->ActionRequests())
            {
                if(Action.Source.RunId!=*Id||!Action.Step)continue;
                FStudioHome4OutputEvent Guard;Guard.Step=Action.Step;
                if((P-EventPosition(Guard,G,*Extent)).Size()<12)
                {SetToolTipText(FText::FromString(FString::Printf(TEXT("Original guard at step %lld · %s"),*Action.Step,*Action.Reason)));break;}
            }
        }
    }
    return FReply::Handled().SetUserFocus(SharedThis(this));
}
TSharedPtr<SWidget> SStudioHome4Timeline::MakeRestartMenu(const FStudioHome4OutputEvent& Event)
{
    const auto Current=Events();if(!WarmStart||Event.Kind!=EStudioHome4OutputKind::Restart||!Current.ContainsByPredicate([&](const auto& V){return SameEvent(V,Event);}))return {};
    Selected=Event;Invalidate(EInvalidateWidgetReason::Paint);
    const TWeakPtr<SStudioHome4Timeline> Weak=SharedThis(this);
    return SNew(SButton).Tag(TEXT("Home4TimelineWarmStart"))
        .ToolTipText(FText::FromString(TEXT("Sets the pending restart path. The solver adapter must verify grid and moment-state compatibility before launch.")))
        .OnClicked_Lambda([Weak,Event]{
            if(const auto Self=Weak.Pin())
            {
                const auto Latest=Self->Events();
                if(Self->WarmStart&&Latest.ContainsByPredicate([&](const auto& V){return SameEvent(V,Event);}))Self->WarmStart(Event);
            }
            FSlateApplication::Get().DismissAllMenus();return FReply::Handled();
        })
        [SNew(STextBlock).Text(FText::FromString(TEXT("Warm start from this checkpoint")))];
}
void SStudioHome4Timeline::RestartMenu(int32 Index,const FVector2D& Position)
{
    const auto E=Events();if(!E.IsValidIndex(Index))return;
    if(const auto Menu=MakeRestartMenu(E[Index]))
        FSlateApplication::Get().PushMenu(SharedThis(this),FWidgetPath(),Menu.ToSharedRef(),Position,FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
}
