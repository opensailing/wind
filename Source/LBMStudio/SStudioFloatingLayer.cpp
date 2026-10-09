#include "SStudioFloatingLayer.h"
#include "StudioModel.h"
#include "StudioTheme.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Input/Reply.h"

namespace
{
const FSlateColorBrush PaneOutline(FLinearColor(.12,.18,.24));
const FSlateColorBrush PaneHeader(FLinearColor(.018,.032,.047));
}

class SStudioPaneHandle final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioPaneHandle){} SLATE_DEFAULT_SLOT(FArguments,Content) SLATE_END_ARGS()
    void Construct(const FArguments& Args){SetCursor(EMouseCursor::CardinalCross);ChildSlot[Args._Content.Widget];}
    TFunction<void(FVector2D)> Begin,Move;
    TFunction<void(bool)> End;
    TFunction<void(FVector2D)> Nudge;
    bool SupportsKeyboardFocus() const override{return true;}
    FReply OnMouseButtonDown(const FGeometry&,const FPointerEvent& E) override
    {
        if(E.GetEffectingButton()!=EKeys::LeftMouseButton)return FReply::Unhandled();
        Pointer=E.GetPointerIndex();User=E.GetUserIndex();bDragging=true;Begin(E.GetScreenSpacePosition());
        return FReply::Handled().CaptureMouse(SharedThis(this)).SetUserFocus(SharedThis(this),EFocusCause::Mouse);
    }
    FReply OnMouseMove(const FGeometry&,const FPointerEvent& E) override
    {
        if(!bDragging||!HasMouseCapture()||E.GetPointerIndex()!=Pointer||E.GetUserIndex()!=User)return FReply::Unhandled();
        Move(E.GetScreenSpacePosition());return FReply::Handled();
    }
    FReply OnMouseButtonUp(const FGeometry&,const FPointerEvent& E) override
    {
        if(!bDragging||E.GetEffectingButton()!=EKeys::LeftMouseButton||E.GetPointerIndex()!=Pointer||E.GetUserIndex()!=User)return FReply::Unhandled();
        bDragging=false;End(true);return FReply::Handled().ReleaseMouseCapture();
    }
    void OnMouseCaptureLost(const FCaptureLostEvent&) override{if(bDragging){bDragging=false;End(false);}}
    FReply OnKeyDown(const FGeometry&,const FKeyEvent& E) override
    {
        if(E.GetKey()==EKeys::Escape&&bDragging){bDragging=false;End(false);return FReply::Handled().ReleaseMouseCapture();}
        if(bDragging)return FReply::Handled();
        if(E.IsControlDown()||E.IsCommandDown()||E.IsAltDown())return FReply::Unhandled();
        const double Step=E.IsShiftDown()?1.:10.;FVector2D Delta(0,0);
        if(E.GetKey()==EKeys::Left)Delta.X=-Step;else if(E.GetKey()==EKeys::Right)Delta.X=Step;
        else if(E.GetKey()==EKeys::Up)Delta.Y=-Step;else if(E.GetKey()==EKeys::Down)Delta.Y=Step;else return FReply::Unhandled();
        Nudge(Delta);return FReply::Handled();
    }
private:
    bool bDragging=false;uint32 Pointer=0;int32 User=0;
};

class SStudioFloatingPane final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioFloatingPane){} SLATE_END_ARGS()
    TWeakPtr<SStudioFloatingLayer> Owner;
    FName Id;FString Title;FVector2D Anchor,Offset;bool Stretch=false;
    SConstraintCanvas::FSlot* Slot=nullptr;
    void Construct(const FArguments&){}
    FStudioFloatingPaneState& State() const{return DragPreview.IsSet()?DragPreview.GetValue():Owner.Pin()->Model->FloatingPanes.FindOrAdd(Id);}
    FVector2D At() const{return StudioFloatingPanes::Position(State(),Owner.Pin()->ViewSize(),GetDesiredSize(),Anchor,Offset);}
    void SetAt(FVector2D P)
    {
        const auto Room=StudioFloatingPanes::Travel(Owner.Pin()->ViewSize(),GetDesiredSize());auto& S=State();
        S.bMoved=true;S.Position=FVector2D(Room.X>0?FMath::Clamp((P.X-8)/Room.X,0.,1.):0,Room.Y>0?FMath::Clamp((P.Y-8)/Room.Y,0.,1.):0);
        Invalidate(EInvalidateWidgetReason::Layout);
    }
    FReply OnPreviewMouseButtonDown(const FGeometry&,const FPointerEvent&) override
    {Owner.Pin()->Raise(this);return FReply::Unhandled();}
    void Initialize(TSharedRef<SWidget> Content)
    {
        using namespace StudioUI;
        SetTag(FName(*(TEXT("FloatingPane_")+Id.ToString())));
        auto Handle=SNew(SStudioPaneHandle).Tag(FName(*(TEXT("PaneDrag_")+Id.ToString())))
            .ToolTipText(FText::FromString(TEXT("Drag to move · Arrow keys move · Shift for fine movement · Esc cancels drag")))
            [SNew(SBorder).BorderImage(&PaneHeader).Padding(5,4)
                [SNew(STextBlock).Text(FText::FromString(TEXT(":: ")+Title)).Font(Font(8)).ColorAndOpacity(Muted)]];
        Handle->Begin=[this](FVector2D P){Owner.Pin()->Raise(this);DragPreview=State();Start=Owner.Pin()->GetCachedGeometry().AbsoluteToLocal(P);StartAt=At();};
        Handle->Move=[this](FVector2D P){SetAt(StartAt+Owner.Pin()->GetCachedGeometry().AbsoluteToLocal(P)-Start);};
        Handle->End=[this](bool Commit){if(Commit&&DragPreview.IsSet())Owner.Pin()->Model->FloatingPanes.Add(Id,DragPreview.GetValue());
            DragPreview.Reset();if(Commit)Owner.Pin()->Save();Invalidate(EInvalidateWidgetReason::Layout);};
        Handle->Nudge=[this](FVector2D Delta){Owner.Pin()->Raise(this);SetAt(At()+Delta);Owner.Pin()->Save();};
        ChildSlot[SNew(SBorder).BorderImage(&PaneOutline).Padding(1)
            [SNew(SBox).MaxDesiredWidth_Lambda([this]{return FMath::Max(60.,Owner.Pin()->ViewSize().X-16);})
            .MaxDesiredHeight_Lambda([this]{return FMath::Max(24.,Owner.Pin()->ViewSize().Y-16);})
            [SNew(SVerticalBox)
                +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
                    +SHorizontalBox::Slot().FillWidth(1)[Handle]
                    +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Tag(FName(*(TEXT("PaneToggle_")+Id.ToString())))
                        .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(5,1))
                        .ToolTipText_Lambda([this]{return FText::FromString((State().bMinimized?TEXT("Restore "):TEXT("Minimize "))+Title);})
                        .OnClicked_Lambda([this]{auto& S=State();S.bMinimized=!S.bMinimized;Owner.Pin()->Raise(this);Owner.Pin()->Save();return FReply::Handled();})
                        [SNew(STextBlock).Text_Lambda([this]{return FText::FromString(State().bMinimized?TEXT("+ "):TEXT("− "));}).Font(Font(10)).ColorAndOpacity(Text)]]]
                +SVerticalBox::Slot().AutoHeight()[SNew(SBox).Tag(FName(*(TEXT("PaneBody_")+Id.ToString())))
                    .Visibility_Lambda([this]{return State().bMinimized?EVisibility::Collapsed:EVisibility::Visible;})
                    .WidthOverride_Lambda([this]()->FOptionalSize{return Stretch?FOptionalSize(FMath::Max(180.,Owner.Pin()->ViewSize().X-134)):FOptionalSize();})
                    [SNew(SBorder).BorderImage(&PanelBrush).Padding(0)[Content]]]]]];
    }
private:
    FVector2D Start,StartAt;mutable TOptional<FStudioFloatingPaneState> DragPreview;
};
void SStudioFloatingLayer::Construct(const FArguments& Args)
{Model=Args._Model;SetTag(TEXT("FloatingViewportPanes"));SetVisibility(EVisibility::SelfHitTestInvisible);SetClipping(EWidgetClipping::ClipToBounds);ChildSlot[SAssignNew(Canvas,SConstraintCanvas)];}
FVector2D SStudioFloatingLayer::ViewSize() const
{const auto S=GetCachedGeometry().GetLocalSize();return S.X>0&&S.Y>0?S:FVector2D(900,500);}
void SStudioFloatingLayer::AddPane(FName Id,const FString& Title,TSharedRef<SWidget> Content,FVector2D Anchor,FVector2D Offset,bool Stretch)
{
    auto Pane=SNew(SStudioFloatingPane);Pane->Owner=SharedThis(this);Pane->Id=Id;Pane->Title=Title;Pane->Anchor=Anchor;Pane->Offset=Offset;Pane->Stretch=Stretch;
    Pane->Initialize(Content);Panes.Add(Pane);
    const TWeakPtr<SStudioFloatingPane> Weak=Pane;
    Canvas->AddSlot().Expose(Pane->Slot).AutoSize(true).Alignment(FVector2D::ZeroVector).ZOrder(++Front)
        .Offset_Lambda([Weak]{const auto P=Weak.Pin()->At();return FMargin(P.X,P.Y,0,0);})[Pane];
}
void SStudioFloatingLayer::Raise(SStudioFloatingPane* Pane)
{if(Pane->Slot)Pane->Slot->SetZOrder(++Front);}
void SStudioFloatingLayer::Save(){Model->SaveSession();}
