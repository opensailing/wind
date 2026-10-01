/*
THESIS: One Snapshot action exports one view or selected original frames.
OWN-WORLD: Inherit the dense navy Slate shell, compact rows and cyan focus.
STORY: Choose PNG or sequence, review original times and framing, export while
camera/playback stay live, then cancel or reveal the completed output.
FIRST VIEWPORT: Source and mode lead a scrollable form; status, save, cancel
and reveal stay in a fixed footer. No new workspace or duplicate action.
FORM: Local Operate extension of the approved Solve workspace.
FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, and DESIGN.md
*/
#include "StudioSnapshotUI.h"
#include "StudioScene.h"
#include "StudioModel.h"
#include "StudioTheme.h"
#include "StudioFileDialog.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
namespace { TFunction<void()> NextImageSequenceBarrier; }
void FStudioSnapshotUI::SetBeforeNextPublishForAutomation(TFunction<void()> Barrier)
{check(IsInGameThread());if(FParse::Param(FCommandLine::Get(),TEXT("StudioAutomation")))NextImageSequenceBarrier=MoveTemp(Barrier);}
#endif

bool StudioSnapshotUI::Selection(const FString& First,const FString& Last,const FString& Stride,int32 Count,
    int32& A,int32& B,int32& Step,FString& Error)
{
    auto Parse=[](FString Text,int32& Out)
    {
        Text.TrimStartAndEndInline();if(Text.IsEmpty()||Text.Len()>10)return false;
        for(const TCHAR C:Text)if(C<TEXT('0')||C>TEXT('9'))return false;
        int64 N=0;if(!LexTryParseString(N,*Text)||N<1||N>MAX_int32)return false;Out=int32(N);return true;
    };
    int32 From=0,To=0,Every=0;
    if(!Parse(First,From)||!Parse(Last,To)||!Parse(Stride,Every))
    {Error=TEXT("Enter positive whole numbers for first frame, last frame and stride.");return false;}
    if(From>To||To>Count)
    {Error=FString::Printf(TEXT("Choose first ≤ last within frames 1–%d."),Count);return false;}
    if((int64(To)-From)/Every+1>StudioImageSequence::MaximumFrames)
    {Error=TEXT("Export at most 100,000 images. Narrow the range or increase the stride.");return false;}
    A=From-1;B=To-1;Step=Every;Error.Empty();return true;
}
bool FStudioSnapshotUI::SameContext() const
{
    const auto M=Model.Pin();return M&&M->Project.Id==DraftProject&&M->Solver==Source.Pin();
}
FIntPoint FStudioSnapshotUI::OutputSize() const
{
    const auto Size=Scene.IsValid()?Scene->PresentedViewportSize():FIntPoint(16,9);
    const double Ratio=Aspect==1?16./9:Aspect==2?4./3:Aspect==3?1.:Size.Y>0?double(Size.X)/Size.Y:16./9;
    return FIntPoint(Width,FMath::RoundToInt(Width/Ratio));
}
FString FStudioSnapshotUI::Validation() const
{
    if(!SameContext())return TEXT("Source or project changed. Reopen Snapshot to export its view.");
    const auto M=Model.Pin();
    if(!Scene.IsValid()||M->IsProjectOpenPending()||M->IsRecordingLoadPending()||!Scene->HasCurrentFrame()||!Scene->HasPresentedFrame())
        return TEXT("Wait for the displayed frame, or pause replay.");
    if(Guard){const auto Error=Guard();if(!Error.IsEmpty())return Error;}
    if(!StudioSnapshot::ValidSize(OutputSize()))return TEXT("Reduce the width; either dimension must be at most 4096 pixels.");
    if(bSequence)
    {
        int32 A,B,S;FString Error;
        if(!StudioSnapshotUI::Selection(bAll?TEXT("1"):First,bAll?FString::FromInt(M->Solver->FrameCount()):Last,Stride,M->Solver->FrameCount(),A,B,S,Error))return Error;
        bool Valid=!Folder.IsEmpty()&&Folder.Len()<=128&&Folder.TrimStartAndEnd()==Folder&&!Folder.StartsWith(TEXT("."));
        for(const TCHAR C:Folder)Valid&=C>=32&&C!=127&&C!=TEXT('/')&&C!=TEXT('\\')&&C!=TEXT(':');
        if(!Valid)return TEXT("Enter a new folder name without outer spaces, slashes, colons or a leading dot.");
    }
    return {};
}
FString FStudioSnapshotUI::SelectionLabel() const
{
    if(IsBusy())return FrozenSummary;
    if(!SameContext())return TEXT("Source changed. Reopen Snapshot.");
    const auto S=Source.Pin();const auto& D=S->Descriptor();
    if(!bSequence)return FString::Printf(TEXT("%s · displayed original frame"),*D.Title);
    int32 A,B,Step;FString Error;
    if(!StudioSnapshotUI::Selection(bAll?TEXT("1"):First,bAll?FString::FromInt(D.Frames.Num()):Last,Stride,D.Frames.Num(),A,B,Step,Error))return D.Title;
    const int32 N=(B-A)/Step+1,End=A+(N-1)*Step;
    return FString::Printf(TEXT("%s\n%d PNGs · frames %d–%d · %.9g–%.9g s"),*D.Title,N,A+1,End+1,D.Frames[A].Time,D.Frames[End].Time);
}
FString FStudioSnapshotUI::Status() const
{
    if(Sequence.IsBusy())
    {
        const auto P=Sequence.Progress();
        if(P.State==EStudioFieldExportState::Cancelled)return TEXT("Cancelling image sequence…");
        if(P.State==EStudioFieldExportState::Publishing||P.Phase==EStudioImageSequencePhase::Complete)return TEXT("Publishing completed image sequence…");
        const TCHAR* Phase=P.Phase==EStudioImageSequencePhase::Encoding?TEXT("Encoding"):P.Phase==EStudioImageSequencePhase::AwaitingImage?TEXT("Rendering"):TEXT("Preparing");
        return FString::Printf(TEXT("%s · %d of %d images saved"),Phase,P.CompletedFrames,P.TotalFrames);
    }
    if(PNG.IsBusy())return PNG.State()==EStudioSnapshotExportState::Cancelled?TEXT("Cancelling snapshot export…"):
        PNG.State()==EStudioSnapshotExportState::Writing?TEXT("Writing PNG…"):TEXT("Encoding PNG…");
    const auto Error=Validation();return Error.IsEmpty()?Notice:Error;
}
FString FStudioSnapshotUI::ButtonLabel() const
{
    if(Sequence.IsBusy()){const auto P=Sequence.Progress();return FString::Printf(TEXT("Snapshot · %d/%d"),P.CompletedFrames,P.TotalFrames);}
    return PNG.IsBusy()?TEXT("Snapshot · saving…"):TEXT("Snapshot");
}
void FStudioSnapshotUI::Report(const FString& Message,bool Error)
{Notice=Message;bError=Error;if(const auto M=Model.Pin();M&&M->Project.Id==DraftProject)M->Notice=Message;}
void FStudioSnapshotUI::Save()
{
    if(bShutdown||IsBusy())return;
    const FString Error=Validation();if(!Error.IsEmpty()){Report(Error,true);return;}
    const auto M=Model.Pin();const auto S=Source.Pin();
    FStudioSnapshot Image;Image.Options=Options;Image.Options.Size=OutputSize();FString CaptureError;
    if(!Capture||!Capture(Image,CaptureError)){Report(CaptureError.IsEmpty()?TEXT("Could not capture this view. Reopen Snapshot and try again."):CaptureError,true);return;}
    const bool Multiple=bSequence;const FGuid Owner=Image.Project;const FString Name=Folder;
    FStudioImageSequenceRequest Request;
    if(Multiple)
    {
        Request.Source=S;
        if(!StudioSnapshotUI::Selection(bAll?TEXT("1"):First,bAll?FString::FromInt(S->FrameCount()):Last,Stride,S->FrameCount(),Request.FirstOrdinal,Request.LastOrdinal,Request.Stride,CaptureError))
        {Report(CaptureError,true);return;}
        Image.Pixels.Reset();Request.View=MoveTemp(Image);
    }
    // Everything that defines the export is pinned before the native panel.
    FrozenSummary=SelectionLabel();const auto Size=OutputSize();FrozenSummary+=FString::Printf(TEXT("\nFrozen view · %d × %d px"),Size.X,Size.Y);
    FSlateApplication::Get().DismissAllMenus();FString Destination;
    const bool Accepted=Multiple?StudioFileDialog::ExportFolder(Destination):
        StudioFileDialog::SnapshotPNG(FString::Printf(TEXT("flow-frame-%d"),Image.Identity.Frame.Index),Destination);
    if(!Accepted){Report(Multiple?TEXT("Image sequence folder selection cancelled."):TEXT("Snapshot export cancelled."));return;}
    JobProject=Owner;bool Started=false;
#if WITH_DEV_AUTOMATION_TESTS
    if(Multiple)Sequence.BeforePublishForAutomation=MoveTemp(NextImageSequenceBarrier);
#endif
    if(Multiple)Started=Sequence.Start(Scene.IsValid()?Scene->GetWorld():nullptr,MoveTemp(Request),Destination,Name,CaptureError);
    else Started=PNG.Start(MoveTemp(Image),Destination);
    if(Started){Path=Multiple?Destination/Name:Destination;bSaved=false;Report(Multiple?TEXT("Preparing image sequence…"):TEXT("Encoding PNG…"));}
    else Report(CaptureError.IsEmpty()?TEXT("Could not start snapshot export. Reopen Snapshot and try again."):CaptureError,true);
}
void FStudioSnapshotUI::Cancel(){if(Sequence.IsBusy())Sequence.Cancel();else PNG.Cancel();}
void FStudioSnapshotUI::Tick(FStudioModel& M)
{
    Sequence.Tick();bool Finished=false;
    if(const auto R=Sequence.Poll())
    {
        Finished=true;bError=!R->bSuccess&&!R->bCancelled;bSaved=R->bSuccess;Path=R->Path;
        Notice=R->bSuccess?FString::Printf(TEXT("Saved %d PNGs · %s"),R->CompletedFrames,*FPaths::GetCleanFilename(Path)):
            R->bCancelled?TEXT("Image sequence cancelled. Destination unchanged."):TEXT("Image sequence failed: ")+R->Error+
            (R->FailedOrdinal==INDEX_NONE?FString():FString::Printf(TEXT(" (frame %d)"),R->FailedOrdinal+1));
    }
    if(const auto R=PNG.Poll())
    {
        Finished=true;bError=!R->bSuccess&&!R->bCancelled;bSaved=R->bSuccess;Path=R->Path;
        Notice=R->bSuccess?FString::Printf(TEXT("Saved PNG · frame %d · %.9g s"),R->Frame.Index,R->Frame.Time):
            R->bCancelled?TEXT("Snapshot export cancelled."):TEXT("Snapshot export failed: ")+R->Error;
    }
    if(Finished&&M.Project.Id==JobProject)
    {M.Notice=Notice;M.AddLog(Notice+(bSaved?TEXT(" · ")+Path:FString()),bError?EStudioLogSeverity::Error:EStudioLogSeverity::Info);}
}
void FStudioSnapshotUI::Shutdown(){if(bShutdown)return;bShutdown=true;PNG.Cancel();Sequence.Shutdown();Guard={};Capture={};}
TSharedRef<SWidget> FStudioSnapshotUI::Menu(const TSharedRef<FStudioModel>& InModel,AStudioScene* InScene,
    TFunction<FString()> InGuard,TFunction<bool(FStudioSnapshot&,FString&)> InCapture)
{
    using namespace StudioUI;const auto Self=AsShared();
    if(!IsBusy())
    {
        if(DraftProject!=InModel->Project.Id){Notice.Empty();Path.Empty();bSaved=bError=false;}
        if(DraftProject!=InModel->Project.Id||Source.Pin()!=InModel->Solver)
        {First=FString::FromInt(InModel->SelectedFrame+1);Last=FString::FromInt(InModel->Solver->FrameCount());Stride=TEXT("1");bAll=false;}
        DraftProject=InModel->Project.Id;Source=InModel->Solver;
    }
    Model=InModel;Scene=InScene;Guard=MoveTemp(InGuard);Capture=MoveTemp(InCapture);
    auto Items=SNew(SVerticalBox);
    auto Row=[&](const TCHAR* Caption,TSharedRef<SWidget> Control)
    {Items->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(.42).VAlign(VAlign_Center)[Label(Caption)]
        +SHorizontalBox::Slot().FillWidth(.58)[Control]];};
    auto TextRow=[&](const TCHAR* Caption,const TCHAR* Tag,FString FStudioSnapshotUI::* Member)
    {Row(Caption,SNew(SEditableTextBox).Tag(Tag).Style(&InputStyle()).Font(Font(10)).SelectAllTextWhenFocused(true)
        .Text(FText::FromString(this->*Member)).OnTextChanged_Lambda([Self,Member](const FText& T){Self.Get().*Member=T.ToString();}));};
    auto Choices=[&](const TArray<FString>& Names,const TArray<FName>& Tags,TFunction<int32()> Read,TFunction<void(int32)> Write)
    {
        auto Box=SNew(SHorizontalBox);for(int32 I=0;I<Names.Num();++I)Box->AddSlot().FillWidth(1).Padding(0,0,I+1<Names.Num()?4:0,0)
            [SNew(SButton).Tag(Tags[I]).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(6,5))
             .OnClicked_Lambda([Write,I]{Write(I);return FReply::Handled();})
             [SNew(STextBlock).Text(FText::FromString(Names[I])).Font(Font(10)).ColorAndOpacity_Lambda([Read,I]{return FSlateColor(Read()==I?Cyan:Text);})]];
        return Box;
    };
    Row(TEXT("Export"),Choices({TEXT("Single PNG"),TEXT("Sequence")},{TEXT("SnapshotSingle"),TEXT("SnapshotSequence")},
        [Self]{return Self->bSequence?1:0;},[Self](int32 I){Self->bSequence=I==1;}));
    Row(TEXT("Width (px)"),SNew(SBox).Tag(TEXT("SnapshotWidth"))[SNew(SNumericEntryBox<int32>).Font(Font(10)).EditableTextBoxStyle(&InputStyle()).AllowSpin(false).MinValue(640).MaxValue(4096)
        .Value_Lambda([Self]{return TOptional<int32>(Self->Width);}).OnValueCommitted_Lambda([Self](int32 W,ETextCommit::Type){Self->Width=W;})]);
    Row(TEXT("Frame"),Choices({TEXT("View"),TEXT("16:9"),TEXT("4:3"),TEXT("1:1")},
        {TEXT("SnapshotAspect0"),TEXT("SnapshotAspect1"),TEXT("SnapshotAspect2"),TEXT("SnapshotAspect3")},
        [Self]{return Self->Aspect;},[Self](int32 I){Self->Aspect=I;}));
    Items->AddSlot().AutoHeight().Padding(0,0,0,10)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text_Lambda([Self]{const auto S=Self->OutputSize();return FText::FromString(FString::Printf(TEXT("%d × %d px · centered crop of this view"),S.X,S.Y));})];
    auto SequenceItems=SNew(SVerticalBox);auto Common=Items;Items=SequenceItems;
    Row(TEXT("Frames"),Choices({TEXT("Range"),TEXT("All")},{TEXT("SnapshotRange"),TEXT("SnapshotAll")},
        [Self]{return Self->bAll?1:0;},[Self](int32 I){Self->bAll=I==1;}));
    auto Range=SNew(SVerticalBox);Items=Range;
    TextRow(TEXT("First frame (1-based)"),TEXT("SnapshotFirst"),&FStudioSnapshotUI::First);
    TextRow(TEXT("Last frame (inclusive)"),TEXT("SnapshotLast"),&FStudioSnapshotUI::Last);
    Items=SequenceItems;Items->AddSlot().AutoHeight()[SNew(SBox).Visibility_Lambda([Self]{return Self->bAll?EVisibility::Collapsed:EVisibility::Visible;})[Range]];
    TextRow(TEXT("Stride (every Nth frame)"),TEXT("SnapshotStride"),&FStudioSnapshotUI::Stride);
    TextRow(TEXT("New folder name"),TEXT("SnapshotFolder"),&FStudioSnapshotUI::Folder);
    Items->AddSlot().AutoHeight().Padding(0,0,0,10)[Label(TEXT("Original frames only. No interpolation or movie rate."),9,Muted)];
    Items=Common;Items->AddSlot().AutoHeight()[SNew(SBox).Visibility_Lambda([Self]{return Self->bSequence?EVisibility::Visible:EVisibility::Collapsed;})[SequenceItems]];
    for(const auto Entry:TArray<TPair<FString,bool FStudioSnapshotOptions::*>>{
        {TEXT("Inspection annotations"),&FStudioSnapshotOptions::bAnnotations},{TEXT("Scalar legend"),&FStudioSnapshotOptions::bLegend},{TEXT("Source and physical time"),&FStudioSnapshotOptions::bFrameInfo}})
    {
        const FName Tag=Entry.Value==&FStudioSnapshotOptions::bAnnotations?TEXT("SnapshotAnnotations"):Entry.Value==&FStudioSnapshotOptions::bLegend?TEXT("SnapshotLegend"):TEXT("SnapshotFrameInfo");
        Items->AddSlot().AutoHeight().Padding(0,0,0,7)[SNew(SCheckBox).Tag(Tag)
            .IsChecked_Lambda([Self,Member=Entry.Value]{return Self->Options.*Member?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
            .OnCheckStateChanged_Lambda([Self,Member=Entry.Value](ECheckBoxState S){Self->Options.*Member=S==ECheckBoxState::Checked;})[Label(Entry.Key)]];
    }
    Items->AddSlot().AutoHeight().Padding(0,3,0,6)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text(FText::FromString(TEXT("The outline previews the exported area. Camera, display and original-frame metadata stay embedded in each PNG.")))];
    auto Actions=SNew(SVerticalBox);
    Actions->AddSlot().AutoHeight().Padding(0,6,0,8)[SNew(STextBlock).Tag(TEXT("SnapshotNotice")).Font(Font(10)).AutoWrapText(true)
        .Text_Lambda([Self]{return FText::FromString(Self->Status());}).ColorAndOpacity_Lambda([Self]{return FSlateColor(Self->bError||(!Self->IsBusy()&&!Self->Validation().IsEmpty())?Amber:Muted);})];
    Actions->AddSlot().AutoHeight().Padding(0,0,0,7)[SNew(SProgressBar).Tag(TEXT("SnapshotProgress"))
        .Visibility_Lambda([Self]{return Self->Sequence.IsBusy()?EVisibility::Visible:EVisibility::Collapsed;})
        .Percent_Lambda([Self]{const auto P=Self->Sequence.Progress();return TOptional<float>(P.TotalFrames?float(P.CompletedFrames)/P.TotalFrames:0);})];
    Actions->AddSlot().AutoHeight()[SNew(SButton).Tag(TEXT("SaveSnapshotPNG")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,6))
        .IsEnabled_Lambda([Self]{return !Self->IsBusy()&&Self->Validation().IsEmpty();})
        .OnClicked_Lambda([Self]{Self->Save();return FReply::Handled();})
        [SNew(STextBlock).Font(Font(10)).ColorAndOpacity(Cyan).Text_Lambda([Self]{return FText::FromString(Self->bSequence?TEXT("Choose folder and export…"):TEXT("Choose destination and save…"));})]];
    Actions->AddSlot().AutoHeight().Padding(0,6,0,0)[SNew(SButton).Tag(TEXT("CancelSnapshotExport")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,6))
        .Visibility_Lambda([Self]{return Self->IsBusy()?EVisibility::Visible:EVisibility::Collapsed;})
        .IsEnabled_Lambda([Self]{return Self->Sequence.IsBusy()?Self->Sequence.Progress().State==EStudioFieldExportState::Writing:Self->PNG.State()==EStudioSnapshotExportState::Encoding;})
        .OnClicked_Lambda([Self]{Self->Cancel();return FReply::Handled();})[Label(TEXT("Cancel export"))]];
    Actions->AddSlot().AutoHeight().Padding(0,6,0,0)[SNew(SButton).Tag(TEXT("RevealSnapshot")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,6))
        .Visibility_Lambda([Self]{return Self->bSaved&&!Self->IsBusy()?EVisibility::Visible:EVisibility::Collapsed;})
        .OnClicked_Lambda([Self]{FPlatformProcess::ExploreFolder(*Self->Path);return FReply::Handled();})[Label(TEXT("Show in Finder"))]];
    return SNew(SBox).WidthOverride(430).MaxDesiredHeight(520)[SNew(SBorder).Tag(TEXT("SnapshotPanel")).BorderImage(&PanelBrush).Padding(12)
        [SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight().Padding(0,0,0,7)[Label(TEXT("Save flow images"),12,Text,true)]
         +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[SNew(STextBlock).Tag(TEXT("SnapshotSource")).Font(Font(10)).ColorAndOpacity(Cyan).AutoWrapText(true)
            .Text_Lambda([Self]{return FText::FromString(Self->SelectionLabel());})]
         +SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)
            +SScrollBox::Slot()[SNew(SBox).Tag(TEXT("SnapshotDraft")).IsEnabled_Lambda([Self]{return !Self->IsBusy()&&Self->SameContext();})[Items]]]
         +SVerticalBox::Slot().AutoHeight()[Actions]]];
}
