// THESIS: Explicit commands keep replay and control targets clear during inspection.
// OWN-WORLD: Inherit the dense blue-black Slate log, fine borders and cyan focus.
// STORY: Discover or recall a command, inspect its text, send once, read the result.
// FIRST VIEWPORT: One compact command row below log entries; responses stay beside it.
// FORM: Local Operate extension; no extra navigation, command shell or scientific output.
// FINISH: Two-size native evidence, fresh finish review and recorded system are required.
#include "SStudioCommandInput.h"
#include "StudioModel.h"
#include "StudioTheme.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Text/STextBlock.h"

class SStudioCommandTextBox final : public SEditableTextBox
{
public:
    void Construct(const FArguments& Args)
    {SEditableTextBox::Construct(Args);EditableText->SetHintTextOpacity(1.f);}
};

void SStudioCommandInput::Construct(const FArguments& Args)
{
    using namespace StudioUI;
    Model=Args._Model;Execute=Args._Execute;Project=Args._Model->Project.Id;SetCanTick(true);
    ChildSlot[SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1)[Label(TEXT("Command"),10,Text,true)]
            +SHorizontalBox::Slot().AutoWidth()[Label(TEXT("Enter sends · Up / Down history · Tab completes · Esc clears"),9,Muted)]]
        +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,8,0)[SAssignNew(Input,SStudioCommandTextBox).Tag(TEXT("CommandInput"))
                .Style(&InputStyle()).Font(Font(10)).ForegroundColor(Text).ClearKeyboardFocusOnCommit(false)
                .SelectAllTextOnCommit(false).RevertTextOnEscape(false)
                .HintText(FText::FromString(TEXT("Type help, status, replay pause…")))
                .ToolTipText(FText::FromString(TEXT("Application commands only. Maximum 256 characters; no arguments. Job commands require Control harness mode.")))
                .OnTextChanged_Lambda([this](const FText& Value)
                {
                    Draft=Value.ToString();if(bSettingText)return;
                    History.Edited();bOversize=Draft.Len()>StudioCommands::InputLimit;
                    if(bOversize)
                    {
                        SetDraft(Draft.Left(StudioCommands::InputLimit));bSuccess=false;
                        Response=TEXT("Input exceeded 256 characters. Edit the shortened text before sending.");
                    }
                    const auto Matches=StudioCommands::Complete(Draft);Suggestions.Empty();
                    if(!Draft.TrimStartAndEnd().IsEmpty()&&!Matches.IsEmpty())
                    {
                        Suggestions=TEXT("Tab: ");for(int32 I=0;I<FMath::Min(4,Matches.Num());++I)Suggestions+=(I?TEXT(" · "):TEXT(""))+Matches[I];
                        if(Matches.Num()>4)Suggestions+=FString::Printf(TEXT(" · %d more"),Matches.Num()-4);
                    }
                })
                .OnTextCommitted_Lambda([this](const FText&,ETextCommit::Type Type){if(Type==ETextCommit::OnEnter)Submit();})
                .OnKeyDownHandler(this,&SStudioCommandInput::Key)]
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)[SNew(SButton).Tag(TEXT("CommandSend"))
                .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(12,7))
                .IsEnabled_Lambda([this]{return !Draft.TrimStartAndEnd().IsEmpty();})
                .OnClicked_Lambda([this]{Submit();return FReply::Handled();})[Label(TEXT("Send"),10,Cyan)]]
            +SHorizontalBox::Slot().AutoWidth()[SAssignNew(Menu,SComboButton).Tag(TEXT("CommandMenu"))
                .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,7)).OnGetMenuContent(this,&SStudioCommandInput::CommandsMenu)
                .ButtonContent()[Label(TEXT("Commands…"),10)]]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,5,0,0)[SNew(STextBlock).Tag(TEXT("CommandSuggestions")).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)
            .Visibility_Lambda([this]{return Suggestions.IsEmpty()?EVisibility::Collapsed:EVisibility::Visible;})
            .Text_Lambda([this]{return FText::FromString(Suggestions);})]
        +SVerticalBox::Slot().AutoHeight().Padding(0,6,0,0)[SNew(SBox).MaxDesiredHeight(100)
            .Visibility_Lambda([this]{return Response.IsEmpty()?EVisibility::Collapsed:EVisibility::Visible;})
            [SNew(SScrollBox)+SScrollBox::Slot()[SNew(STextBlock).Tag(TEXT("CommandResponse")).Font(Font(10)).AutoWrapText(true)
                .ColorAndOpacity_Lambda([this]{return FSlateColor(bSuccess?Cyan:Amber);})
                .Text_Lambda([this]{return FText::FromString(Response);})]]]];
}
void SStudioCommandInput::Tick(const FGeometry& Geometry,double Time,float Delta)
{
    SCompoundWidget::Tick(Geometry,Time,Delta);
    if(const auto M=Model.Pin();M&&Project!=M->Project.Id)
    {
        Project=M->Project.Id;History.Edited();SetDraft(TEXT(""));Response.Empty();Suggestions.Empty();bOversize=false;
    }
}
void SStudioCommandInput::SetDraft(const FString& Value,bool bFocus)
{
    Draft=Value;TGuardValue<bool> Guard(bSettingText,true);
    if(const auto Field=Input.Pin())
    {
        Field->SetText(FText::FromString(Draft));Field->GoTo(ETextLocation::EndOfDocument);
        if(bFocus)FSlateApplication::Get().SetKeyboardFocus(Field,EFocusCause::Navigation);
    }
}
FReply SStudioCommandInput::Key(const FGeometry&,const FKeyEvent& Event)
{
    if(Event.IsCommandDown()||Event.IsControlDown()||Event.IsAltDown())return FReply::Unhandled();
    const auto K=Event.GetKey();
    const FString Before=Draft;
    if(K==EKeys::Up)SetDraft(History.Previous(Draft));
    else if(K==EKeys::Down)SetDraft(History.Next(Draft));
    else if(K==EKeys::Tab&&!Draft.TrimStartAndEnd().IsEmpty()&&!StudioCommands::Complete(Draft).IsEmpty())SetDraft(History.Complete(Draft,Event.IsShiftDown()));
    else if(K==EKeys::Escape){History.Edited();SetDraft(TEXT(""));}
    else return FReply::Unhandled();
    Suggestions.Empty();
    // Empty history and Down outside recall can return the same truncated text.
    // Only an actual edit/selection may clear the oversized-paste send guard.
    if(Draft!=Before)bOversize=false;
    return FReply::Handled();
}
void SStudioCommandInput::Submit()
{
    const auto M=Model.Pin();if(!M)return;
    if(Project!=M->Project.Id){Project=M->Project.Id;History.Edited();SetDraft(TEXT(""));Response=TEXT("Project changed. Enter a command for the current project.");bSuccess=false;return;}
    if(bOversize){Response=TEXT("Input exceeded 256 characters. Edit the shortened text before sending.");bSuccess=false;return;}
    EStudioCommand Command;FString Result;
    const bool Parsed=StudioCommands::Parse(Draft,Command,Result);
    if(Parsed)History.Remember(Command);
    bSuccess=Parsed&&StudioCommands::Validate(*M,Command,Result)&&Execute&&Execute(Command,Result);
    const FString Name=Parsed?StudioCommands::Find(Command)->Name:TEXT("command");
    const bool bJob=Parsed&&Command>=EStudioCommand::JobSubmit;
    if(bSuccess&&bJob)Result=TEXT("Request sent. Use status or follow the log for the latest job state.");
    Response=(bSuccess?(bJob?TEXT("Sent · "):TEXT("Result · ")):TEXT("Cannot send · "))+Name+TEXT("\n")+Result;
    M->AddLog(Response,bSuccess?EStudioLogSeverity::Info:EStudioLogSeverity::Warning,EStudioLogSource::Application,
        bJob&&M->Job().Run()?M->Job().Run()->GetId():FGuid(),TEXT("Application command"));
    if(bSuccess){SetDraft(TEXT(""));Suggestions.Empty();}
}
TSharedRef<SWidget> SStudioCommandInput::CommandsMenu()
{
    using namespace StudioUI;
    auto Rows=SNew(SVerticalBox);
    Rows->AddSlot().AutoHeight().Padding(9,4,9,9)[Label(TEXT("Choose to fill the input; Send runs it."),10,Muted)];
    for(const auto& Spec:StudioCommands::Registry())
    {
        const FString Name=Spec.Name;
        const auto Choice=SNew(SButton).Tag(FName(TEXT("CommandChoice_")+Name.Replace(TEXT(" "),TEXT("_"))))
            .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,7)).HAlign(HAlign_Left)
            .OnClicked_Lambda([this,Name]{FSlateApplication::Get().DismissAllMenus();History.Edited();bOversize=false;Suggestions.Empty();SetDraft(Name,true);return FReply::Handled();})
            [SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(142)[Label(Name,10,Cyan)]]
                +SHorizontalBox::Slot().FillWidth(1)[Label(Spec.Description,10,Text)]];
        Rows->AddSlot().AutoHeight().Padding(0,2)[Choice];
        if(Spec.Id==EStudioCommand::Help)Menu.Pin()->SetMenuContentWidgetToFocus(Choice);
    }
    return SNew(SBorder).BorderImage(&PanelBrush).Padding(7)
        [SNew(SBox).WidthOverride(450).MaxDesiredHeight(360)[SNew(SScrollBox)+SScrollBox::Slot()[Rows]]];
}
