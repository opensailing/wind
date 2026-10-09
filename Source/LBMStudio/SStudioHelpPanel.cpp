/*
THESIS: Help answers the current task without changing the working case or view.
OWN-WORLD: Inherit the reference's navy Slate panels, compact Roboto type and cyan selection.
STORY: Read workspace guidance, find working keys, copy a frozen diagnostic snapshot, then return.
FIRST VIEWPORT: One header icon opens a 540 by 500 popup with a title, Close and four local categories.
FORM: Local Read extension of the approved desktop interface; no visual-world replacement.
FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, and DESIGN.md
*/
#include "SStudioHelpPanel.h"
#include "StudioHelp.h"
#include "StudioModel.h"
#include "StudioScene.h"
#include "StudioTheme.h"
#include "Misc/EngineVersion.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Brushes/SlateNoResource.h"

namespace StudioHelpUI
{
using namespace StudioUI;
TSharedRef<SWidget> Paragraph(FName Tag,TAttribute<FString> Value)
{
    return SNew(STextBlock).Tag(Tag).Font(Font(11)).ColorAndOpacity(Text).AutoWrapText(true)
        .Text_Lambda([Value]{return FText::FromString(Value.Get());});
}
TSharedRef<SButton> Action(FName Tag,const FString& TextValue,TFunction<void()> Callback)
{
    return SNew(SButton).Tag(Tag).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(10,7))
        .OnClicked_Lambda([Callback]{Callback();return FReply::Handled();})[Label(TextValue,10)];
}
const FEditableTextBoxStyle& DiagnosticStyle()
{
    static const FEditableTextBoxStyle Style=FEditableTextBoxStyle(InputStyle())
        .SetBackgroundImageReadOnly(InputStyle().BackgroundImageNormal);
    return Style;
}
const FScrollBoxStyle& ScrollStyle()
{
    static const FScrollBoxStyle Style=FScrollBoxStyle(FCoreStyle::Get().GetWidgetStyle<FScrollBoxStyle>(TEXT("ScrollBox")))
        .SetTopShadowBrush(FSlateNoResource()).SetBottomShadowBrush(FSlateNoResource());
    return Style;
}
}
void SStudioHelpPanel::Construct(const FArguments& Args)
{
    using namespace StudioHelpUI;
    M=Args._Model;Scene=Args._Scene;Results=Args._OnResults;Close=Args._OnClose;Copy=Args._CopyText;Page=Args._Page;
    if(!Copy)Copy=[](const FString& TextValue){FPlatformApplicationMisc::ClipboardCopy(*TextValue);};
    auto Tabs=SNew(SHorizontalBox);
    const TCHAR* Names[]={TEXT("This workspace"),TEXT("Shortcuts"),TEXT("Diagnostics"),TEXT("About")};
    for(int32 I=0;I<4;++I)
        Tabs->AddSlot().AutoWidth().Padding(0,0,5,0)
        [SNew(SButton).Tag(FName(*FString::Printf(TEXT("HelpPage%d"),I))).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,7))
            .OnClicked_Lambda([this,I]{Show(EStudioHelpPage(I));return FReply::Handled();})
            [SNew(STextBlock).Text(FText::FromString(Names[I])).Font(Font(10))
                .ColorAndOpacity_Lambda([this,I]{return int32(Page)==I?Cyan:Text;})]];
    ChildSlot[SNew(SBox).WidthOverride(540).HeightOverride(500)
        [SNew(SBorder).BorderImage(&PanelBrush).Padding(16)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)
            [SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Help"),16,Text,true)]
                +SHorizontalBox::Slot().AutoWidth()[Action(TEXT("HelpClose"),TEXT("Close"),[this]{Close.ExecuteIfBound();})]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,16)[Tabs]
            +SVerticalBox::Slot().FillHeight(1)[SAssignNew(Content,SBox)]]]];
    Show(Page);
}
void SStudioHelpPanel::Show(EStudioHelpPage Value)
{
    Page=Value;
    if(Page==EStudioHelpPage::Diagnostics&&DiagnosticSnapshot.IsEmpty())RefreshDiagnostics();
    Content->SetContent(Body());
}
FReply SStudioHelpPanel::OnPreviewKeyDown(const FGeometry&,const FKeyEvent& Event)
{
    if(Event.GetKey()==EKeys::Escape&&Close.IsBound())
    {Close.Execute();return FReply::Handled();}
    return FReply::Unhandled();
}
void SStudioHelpPanel::RefreshDiagnostics()
{
    DiagnosticSnapshot=StudioHelp::Diagnostics(*M,Scene.Get());CopyNotice.Empty();
}
TSharedRef<SWidget> SStudioHelpPanel::Body()
{
    using namespace StudioHelpUI;
    auto Rows=SNew(SVerticalBox);
    if(Page==EStudioHelpPage::Workspace)
    {
        Rows->AddSlot().AutoHeight().Padding(0,0,0,10)
            [SNew(STextBlock).Tag(TEXT("HelpWorkspaceTitle")).Font(Font(13,true)).ColorAndOpacity(Text)
                .Text_Lambda([this]{return FText::FromString(StudioHelp::WorkspaceName(M->Workspace));})];
        Rows->AddSlot().AutoHeight().Padding(0,0,0,20)
            [Paragraph(TEXT("HelpGuidance"),TAttribute<FString>::CreateLambda([this]{return StudioHelp::Guidance(M->Workspace,M->Project.bControlHarness,M->Project.Draft.Home4.IsSet());}))];
        Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[Label(TEXT("Source provenance"),12,Text,true)];
        Rows->AddSlot().AutoHeight().Padding(0,0,0,12)[Paragraph(TEXT("HelpProvenanceHint"),
            FString(TEXT("Results → Recording → Source and identity shows the published source, units, topology and hashes. Changing case requests does not recompute a recording.")))];
        auto Link=Action(TEXT("HelpResults"),TEXT("Open Results"),[this]{Results.ExecuteIfBound();});
        Link->SetEnabled(Results.IsBound());Rows->AddSlot().AutoHeight()[Link];
    }
    else if(Page==EStudioHelpPage::Shortcuts)
    {
#if PLATFORM_MAC
        const FString Modifier=TEXT("Command");
#else
        const FString Modifier=TEXT("Ctrl");
#endif
        Rows->AddSlot().AutoHeight().Padding(0,0,0,12)[Paragraph(TEXT("HelpShortcutScope"),
            FString(TEXT("Camera keys apply while the flow viewport has focus. Text fields keep their usual editing shortcuts.")))];
        const TPair<FString,FString> Keys[]={
            {TEXT("F1"),TEXT("Open Help")},{Modifier+TEXT(" + S"),TEXT("Save project")},
            {Modifier+TEXT(" + Shift + S"),TEXT("Save project as…")},{Modifier+TEXT(" + O"),TEXT("Open project")},
            {Modifier+TEXT(" + N"),TEXT("New project")},{Modifier+TEXT(" + Alt + Z"),TEXT("Undo view in Solve")},
            {Modifier+TEXT(" + Alt + Shift + Z"),TEXT("Redo view in Solve")},{TEXT("F"),TEXT("Fit flow domain")},
            {TEXT("Right drag"),TEXT("Look and enter free flight")},{TEXT("W / A / S / D"),TEXT("Fly forward / left / back / right")},
            {TEXT("Q / E"),TEXT("Fly down / up")},{TEXT("Shift"),TEXT("Faster flight")},
            {TEXT("Middle drag"),TEXT("Pan camera")},{TEXT("Mouse wheel"),TEXT("Zoom camera")},
            {TEXT("Escape"),TEXT("Close a menu or cancel placement")}};
        for(const auto& Key:Keys)Rows->AddSlot().AutoHeight().Padding(0,0,0,10)
            [SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(.56f)[Label(Key.Key,10,Cyan)]
                +SHorizontalBox::Slot().FillWidth(.44f)[Paragraph(NAME_None,Key.Value)]];
    }
    else if(Page==EStudioHelpPage::Diagnostics)
    {
        // Refresh is explicit: the selectable snapshot and its UTC identity
        // remain stable while the user changes the camera or playback.
        return SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[Paragraph(TEXT("HelpDiagnosticHint"),
                FString(TEXT("A snapshot of this application, project, recording and renderer. Copy retains exactly the displayed snapshot.")))]
            +SVerticalBox::Slot().FillHeight(1).Padding(0,0,0,10)
                [SNew(SMultiLineEditableTextBox).Tag(TEXT("HelpDiagnostics")).Style(&DiagnosticStyle()).Font(Font(10)).IsReadOnly(true)
                    .ReadOnlyForegroundColor(Text)
                    .AutoWrapText(true).Text_Lambda([this]{return FText::FromString(DiagnosticSnapshot);})]
            +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)
                    [Action(TEXT("HelpRefresh"),TEXT("Refresh"),[this]{RefreshDiagnostics();})]
                +SHorizontalBox::Slot().AutoWidth()
                    [Action(TEXT("HelpCopy"),TEXT("Copy diagnostics"),[this]{Copy(DiagnosticSnapshot);CopyNotice=TEXT("Copied this snapshot.");})]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,8,0,0)
                [Paragraph(TEXT("HelpCopyStatus"),TAttribute<FString>::CreateLambda([this]{return CopyNotice;}))];
    }
    else
    {
        Rows->AddSlot().AutoHeight().Padding(0,0,0,12)[Label(TEXT("LBM Solver Studio"),16,Text,true)];
        Rows->AddSlot().AutoHeight().Padding(0,0,0,16)[Paragraph(TEXT("HelpAbout"),
            FString(TEXT("A UI and CFD visualization interface for a custom lattice Boltzmann solver. Built with Unreal Engine and native Slate controls.")))];
        Rows->AddSlot().AutoHeight().Padding(0,0,0,10)[Paragraph(TEXT("HelpVersion"),TEXT("Application version ")+StudioHelp::ApplicationVersion())];
        Rows->AddSlot().AutoHeight().Padding(0,0,0,16)[Paragraph(TEXT("HelpEngineVersion"),TEXT("Unreal Engine ")+FEngineVersion::Current().ToString())];
        Rows->AddSlot().AutoHeight()[Paragraph(TEXT("HelpAboutData"),
            FString(TEXT("Displayed CFD comes from original recordings. The control harness tests command behavior without computing fields. Results retains each recording's source attribution.")))];
    }
    return SNew(SScrollBox).Style(&ScrollStyle()).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)+SScrollBox::Slot()[Rows];
}
