#include "SStudioHome4Timeline.h"
#include "StudioTheme.h"
#include "Rendering/DrawElements.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Application/SlateApplication.h"
void SStudioHome4Timeline::Construct(const FArguments& A)
{Telemetry=A._Telemetry;SourceRun=A._SourceRun;Frames=A._Frames;Review=A._Review;WarmStart=A._WarmStart;SetToolTipText(FText::FromString(TEXT("Original output events for this source run. Four lanes distinguish traces, slices, visualization snapshots and restart states. Select a marker to inspect its path. Only an exact visualization frame can move replay.")));}
TArray<FStudioHome4OutputEvent> SStudioHome4Timeline::Events()const
{
    TArray<FStudioHome4OutputEvent> Out;const auto T=Telemetry?Telemetry():nullptr;const auto Id=SourceRun?SourceRun():TOptional<FGuid>();
    const auto F=Frames?Frames():nullptr;
    if(T&&Id&&F&&!F->IsEmpty())for(const auto& E:T->OutputEvents())if(E.Source.RunId==*Id&&E.Step&&*E.Step>=(*F)[0].Index&&*E.Step<=F->Last().Index)Out.Add(E);
    Out.StableSort([](const auto& A,const auto& B){return *A.Step<*B.Step;});return Out;
}
int32 SStudioHome4Timeline::OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 L,const FWidgetStyle&,bool)const
{
    const auto Size=G.GetLocalSize();const auto E=Events();const auto F=Frames?Frames():nullptr;
    const TCHAR* Names[]={TEXT("Trace"),TEXT("Slice"),TEXT("Viz"),TEXT("Restart")};
    const FLinearColor Colors[]={StudioUI::Muted,StudioUI::Cyan,FLinearColor(.25,.6,.9),StudioUI::Amber};
    for(int32 Row=0;Row<4;++Row)
    {
        FSlateDrawElement::MakeText(Out,L,G.ToPaintGeometry(FVector2D(58,14),FSlateLayoutTransform(FVector2D(0,Row*17))),Names[Row],StudioUI::Font(8),ESlateDrawEffect::None,Colors[Row]);
        FSlateDrawElement::MakeLines(Out,L,G.ToPaintGeometry(),{FVector2D(60,Row*17+7),FVector2D(Size.X-4,Row*17+7)},ESlateDrawEffect::None,StudioUI::Muted*.25f);
    }
    if(!F||F->Num()<1||E.IsEmpty())
    {FSlateDrawElement::MakeText(Out,L+1,G.ToPaintGeometry(FVector2D(Size.X-70,16),FSlateLayoutTransform(FVector2D(70,25))),TEXT("No original output events attached to this source run"),StudioUI::Font(9),ESlateDrawEffect::None,StudioUI::Muted);return L+1;}
    const double Low=(*F)[0].Index,Span=FMath::Max(1.,double(F->Last().Index)-Low);
    for(int32 I=0;I<E.Num();++I)
    {
        if(*E[I].Step<Low||*E[I].Step>F->Last().Index)continue;
        const double X=60+(Size.X-66)*(*E[I].Step-Low)/Span,Y=int32(E[I].Kind)*17+7;
        FSlateDrawElement::MakeLines(Out,L+1,G.ToPaintGeometry(),{FVector2D(X,Y-5),FVector2D(X,Y+5)},ESlateDrawEffect::None,Colors[int32(E[I].Kind)],true,I==Selected?4:2);
    }
    return L+1;
}
void SStudioHome4Timeline::Activate(int32 Index)
{
    const auto E=Events();if(!E.IsValidIndex(Index))return;Selected=Index;
    const auto& Event=E[Index];SetToolTipText(FText::FromString(FString::Printf(TEXT("Original step %lld · %s"),*Event.Step,*Event.Path)));
    if(Event.Kind==EStudioHome4OutputKind::Visualization&&Frames&&Review)
    {const auto F=Frames();for(int32 I=0;F&&I<F->Num();++I)if((*F)[I].Index==*Event.Step){Review(I);break;}}
    Invalidate(EInvalidateWidgetReason::Paint);
}
FReply SStudioHome4Timeline::OnKeyDown(const FGeometry&,const FKeyEvent& E)
{
    const auto Count=Events().Num();if(Count==0)return FReply::Unhandled();
    if(E.GetKey()==EKeys::Left)Activate(FMath::Max(0,Selected-1));
    else if(E.GetKey()==EKeys::Right)Activate(FMath::Min(Count-1,Selected+1));
    else if(E.GetKey()==EKeys::Home)Activate(0);else if(E.GetKey()==EKeys::End)Activate(Count-1);
    else if(E.GetKey()==EKeys::Enter)RestartMenu(Selected,GetCachedGeometry().LocalToAbsolute(FVector2D(60,60)));else return FReply::Unhandled();
    return FReply::Handled();
}
FReply SStudioHome4Timeline::OnMouseButtonDown(const FGeometry& G,const FPointerEvent& Event)
{
    if(Event.GetEffectingButton()!=EKeys::LeftMouseButton&&Event.GetEffectingButton()!=EKeys::RightMouseButton)return FReply::Unhandled();
    const auto E=Events();const auto F=Frames?Frames():nullptr;if(!F||F->IsEmpty())return FReply::Handled();
    const auto P=G.AbsoluteToLocal(Event.GetScreenSpacePosition());const double Span=FMath::Max(1.,double(F->Last().Index)-(*F)[0].Index);
    double Best=12;int32 Index=INDEX_NONE;
    for(int32 I=0;I<E.Num();++I)
    {const FVector2D At(60+(G.GetLocalSize().X-66)*(*E[I].Step-(*F)[0].Index)/Span,int32(E[I].Kind)*17+7);const double D=(P-At).Size();if(D<Best){Best=D;Index=I;}}
    if(Event.GetEffectingButton()==EKeys::RightMouseButton)RestartMenu(Index,Event.GetScreenSpacePosition());else Activate(Index);return FReply::Handled().SetUserFocus(SharedThis(this));
}

void SStudioHome4Timeline::RestartMenu(int32 Index,const FVector2D& Position)
{
    const auto E=Events();if(!WarmStart||!E.IsValidIndex(Index)||E[Index].Kind!=EStudioHome4OutputKind::Restart)return;
    const auto Event=E[Index];Selected=Index;FMenuBuilder Menu(true,nullptr);
    const TWeakPtr<SStudioHome4Timeline> Weak=SharedThis(this);
    Menu.AddMenuEntry(FText::FromString(TEXT("Warm start from this checkpoint")),FText::FromString(TEXT("Sets the pending restart path. The solver adapter must verify grid and moment-state compatibility before launch.")),FSlateIcon(),FUIAction(FExecuteAction::CreateLambda([Weak,Event]{
        if(const auto Self=Weak.Pin())
        {const auto Current=Self->Events();if(Current.ContainsByPredicate([&](const auto& V){return V.Kind==EStudioHome4OutputKind::Restart&&V.Source.RunId==Event.Source.RunId&&V.Step==Event.Step&&V.Path==Event.Path;}))Self->WarmStart(Event);}
    })));
    FSlateApplication::Get().PushMenu(SharedThis(this),FWidgetPath(),Menu.MakeWidget(),Position,FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
}
