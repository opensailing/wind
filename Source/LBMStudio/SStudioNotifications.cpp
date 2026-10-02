/*
THESIS: Acknowledge and revisit actual local warnings, errors and completion events.
OWN-WORLD: Reference navy Slate header/popup, compact CoreStyle type, thin outlines and cyan actions.
STORY: Open the bell, read a stable session history, mark read explicitly, revisit the owning context.
FIRST VIEWPORT: One 500x500 popup; title/Close, All/Unread, Refresh/Mark shown read, bounded scrolling rows.
FORM: Local Operate extension; sidebar navigation and existing Results/log owners remain authoritative.
FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, and DESIGN.md
*/
#include "SStudioNotifications.h"
#include "StudioModel.h"
#include "StudioTheme.h"
#include "Styling/CoreStyle.h"
#include "Brushes/SlateNoResource.h"
#include "InputCoreTypes.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"

namespace StudioNotificationUI
{
using namespace StudioUI;
TSharedRef<SButton> Action(FName Tag,const FString& Caption,TFunction<void()> Run)
{
    return SNew(SButton).Tag(Tag).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,7))
        .OnClicked_Lambda([Run]{Run();return FReply::Handled();})[Label(Caption)];
}
TSharedRef<STextBlock> Paragraph(FName Tag,TAttribute<FString> Value,FLinearColor Color=Text,int32 Size=10)
{
    return SNew(STextBlock).Tag(Tag).Font(Font(Size)).ColorAndOpacity(Color).AutoWrapText(true)
        .Text_Lambda([Value]{return FText::FromString(Value.Get());});
}
const FScrollBoxStyle& ScrollStyle()
{
    static const auto Style=FScrollBoxStyle(FCoreStyle::Get().GetWidgetStyle<FScrollBoxStyle>(TEXT("ScrollBox")))
        .SetTopShadowBrush(FSlateNoResource()).SetBottomShadowBrush(FSlateNoResource());
    return Style;
}
}
void SStudioNotifications::Construct(const FArguments& Args)
{
    using namespace StudioNotificationUI;
    M=Args._Model;Close=Args._OnClose;Reveal=Args._Reveal;
    auto All=Action(TEXT("NotificationsAll"),TEXT("All"),[this]{bUnreadOnly=false;Rebuild();});
    All->SetContent(SNew(STextBlock).Font(Font(10)).Text(FText::FromString(TEXT("All"))).ColorAndOpacity_Lambda([this]{return bUnreadOnly?Text:Cyan;}));
    UnreadButton=Action(TEXT("NotificationsUnread"),TEXT("Unread"),[this]{bUnreadOnly=true;Rebuild();});
    UnreadButton->SetContent(SNew(STextBlock).Font(Font(10)).Text(FText::FromString(TEXT("Unread"))).ColorAndOpacity_Lambda([this]{return bUnreadOnly?Cyan:Text;}));
    auto Mark=Action(TEXT("NotificationsMarkShown"),TEXT("Mark shown read"),[this]
    {
        // Use the captured rows, not the current producer cursor. Neither a
        // newly arrived event nor a row outside this snapshot is acknowledged.
        bool Expired=false;
        for(auto& Entry:Captured)if(!Entry.bRead)
        {if(M->SetNotificationRead(Entry.Sequence,true))Entry.bRead=true;else Expired=true;}
        Rebuild();ActionNotice=Expired?TEXT("Retained shown notifications marked read. Some entries expired; Refresh updates the list."):
            TEXT("Shown notifications marked read.");
    });
    Mark->SetEnabled(TAttribute<bool>::CreateLambda([this]{return Captured.ContainsByPredicate([](const auto& E){return !E.bRead;});}));
    ChildSlot[SNew(SBox).WidthOverride(500).HeightOverride(500)
        [SNew(SBorder).BorderImage(&PanelBrush).Padding(16)[SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Notifications"),16,Text,true)]
                +SHorizontalBox::Slot().AutoWidth()[Action(TEXT("NotificationsClose"),TEXT("Close"),[this]{Close.ExecuteIfBound();})]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth().Padding(0,0,5,0)[All]
                +SHorizontalBox::Slot().AutoWidth()[UnreadButton.ToSharedRef()]
                +SHorizontalBox::Slot().FillWidth(1)
                +SHorizontalBox::Slot().AutoWidth().Padding(0,0,5,0)[Action(TEXT("NotificationsRefresh"),TEXT("Refresh"),[this]{Capture();})]
                +SHorizontalBox::Slot().AutoWidth()[Mark]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[Paragraph(TEXT("NotificationsSummary"),TAttribute<FString>::CreateLambda([this]{return Summary();}),Muted,9)]
            +SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox).Tag(TEXT("NotificationsScroll")).Style(&ScrollStyle())
                .ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll).NavigationScrollPadding(10.f)
                +SScrollBox::Slot()[SAssignNew(Rows,SVerticalBox)]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,10,0,0)[Paragraph(TEXT("NotificationsStatus"),TAttribute<FString>::CreateLambda([this]
                {return ActionNotice.IsEmpty()?TEXT("Session history · Closing keeps unread items. New arrivals appear after Refresh."):ActionNotice;}),Muted,9)]]]];
    Capture();
}
void SStudioNotifications::Capture()
{
    Captured=M->Notifications().Snapshot();CapturedThrough=M->Notifications().LastSequence();CapturedEvicted=M->Notifications().EvictedCount();
    ActionNotice.Empty();Rebuild();
}
void SStudioNotifications::Rebuild()
{
    using namespace StudioNotificationUI;
    Rows->ClearChildren();int32 Shown=0;
    for(int32 I=Captured.Num()-1;I>=0;--I)if(!bUnreadOnly||!Captured[I].bRead)
    {Rows->AddSlot().AutoHeight().Padding(0,0,0,14)[Row(Captured[I])];++Shown;}
    if(!Shown)Rows->AddSlot().AutoHeight().Padding(0,16)[Paragraph(TEXT("NotificationsEmpty"),FString(bUnreadOnly?
        TEXT("No unread notifications in this snapshot."):TEXT("No warnings, errors or completion events in this session.")),Muted)];
}
FString SStudioNotifications::Summary() const
{
    int32 Unread=0;for(const auto& Entry:Captured)if(!Entry.bRead)++Unread;
    FString Value=FString::Printf(TEXT("%d retained · %d unread in snapshot"),Captured.Num(),Unread);
    const uint64 Pending=M->Notifications().LastSequence()-CapturedThrough;
    if(Pending)Value+=FString::Printf(TEXT(" · %llu new · Refresh"),Pending);
    if(CapturedEvicted)Value+=FString::Printf(TEXT(" · %llu older expired"),CapturedEvicted);
    return Value;
}
void SStudioNotifications::SetRead(uint64 Id,bool Value)
{
    if(!M->SetNotificationRead(Id,Value)){ActionNotice=TEXT("This notification has expired. Refresh the retained history.");return;}
    if(auto* Entry=Captured.FindByPredicate([Id](const auto& E){return E.Sequence==Id;}))Entry->bRead=Value;
    Rebuild();FSlateApplication::Get().SetKeyboardFocus(UnreadButton,EFocusCause::Navigation);
}
TSharedRef<SWidget> SStudioNotifications::Row(const FStudioNotification& Entry)
{
    using namespace StudioNotificationUI;
    const auto Id=Entry.Sequence;const FName MessageTag(*FString::Printf(TEXT("NotificationMessage_%llu"),Id));
    auto Open=Action(FName(*FString::Printf(TEXT("NotificationOpen_%llu"),Id)),Entry.Kind==EStudioNotificationKind::Completed?
        (Entry.RunId.IsValid()?TEXT("View run"):TEXT("View recording")):TEXT("Open log entry"),[this,Id]
        {
            FString Reason;if(!M->CanRevealNotification(Id,Reason)){ActionNotice=Reason;return;}
            // Revealing dismisses this popup. Keep owned values before the
            // callback and never read this panel after its menu is dismissed.
            const auto Model=M;const auto Action=Reveal;
            if(Action&&Action(Id))Model->SetNotificationRead(Id,true);
        });
    Open->SetEnabled(TAttribute<bool>::CreateLambda([this,Id]{FString Reason;return bool(Reveal)&&M->CanRevealNotification(Id,Reason);}));
    Open->SetToolTipText(TAttribute<FText>::CreateLambda([this,Id]{FString Reason;M->CanRevealNotification(Id,Reason);return FText::FromString(Reason);}));
    const FLinearColor Tint=Entry.Kind==EStudioNotificationKind::Completed?Cyan:Amber;
    const FString Context=Entry.ObservedUTC.ToString(TEXT("%H:%M:%S"))+TEXT(" UTC · ")+(Entry.ProjectId==M->Project.Id?TEXT("Current project"):TEXT("Another project"))+
        (Entry.bRead?TEXT(" · Read"):TEXT(" · Unread"));
    auto ContextText=Paragraph(NAME_None,Context,Muted,9);
    ContextText->SetToolTipText(FText::FromString(TEXT("Observed: ")+Entry.ObservedUTC.ToIso8601()+TEXT("\nProject: ")+Entry.ProjectId.ToString()+
        (Entry.RunId.IsValid()?TEXT("\nRun: ")+Entry.RunId.ToString():FString())+(Entry.SourceReference.IsEmpty()?FString():TEXT("\nSource: ")+Entry.SourceReference)));
    return SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight()[Label(StudioNotifications::Title(Entry),11,Tint,true)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,3,0,6)[ContextText]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Paragraph(MessageTag,Entry.Message+(Entry.bTruncated?TEXT(" [truncated]"):TEXT("")))]
        +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[Open]
            +SHorizontalBox::Slot().AutoWidth()[Action(FName(*FString::Printf(TEXT("NotificationRead_%llu"),Id)),Entry.bRead?TEXT("Mark unread"):TEXT("Mark read"),
                [this,Id,Read=Entry.bRead]{SetRead(Id,!Read);})]];
}
FReply SStudioNotifications::OnPreviewKeyDown(const FGeometry&,const FKeyEvent& Event)
{
    if(Event.GetKey()==EKeys::Escape&&Close.IsBound()){Close.Execute();return FReply::Handled();}
    return FReply::Unhandled();
}
