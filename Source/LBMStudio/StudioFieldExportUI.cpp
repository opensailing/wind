/*
THESIS: Export explicitly chosen original values from the named frozen frame.
OWN-WORLD: Dense native blue-black Slate menus and cyan selection.
FIRST VIEWPORT: Source/frame first, supplied arrays, source/scene coordinates,
then one save action; progress and cancellation occupy the same task panel.
FORM: Local Operate extension; preserve the reference shell and one Export route.
FINISH: Native two-size review and documentation follow the verified build.
*/
#include "StudioFieldExportUI.h"
#include "StudioModel.h"
#include "StudioScene.h"
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
#include "Widgets/Notifications/SProgressBar.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
namespace { TFunction<void()> NextFieldExportBarrier; }
void FStudioFieldExportUI::SetBeforeNextPublishForAutomation(TFunction<void()> Barrier)
{check(IsInGameThread());if(FParse::Param(FCommandLine::Get(),TEXT("StudioAutomation")))NextFieldExportBarrier=MoveTemp(Barrier);}
#endif

FString FStudioFieldExportUI::Status() const
{
    if(Task.IsBusy())
    {
        const auto P=Task.Progress();
        if(P.State==EStudioFieldExportState::Cancelled)return TEXT("Cancelling VTK export…");
        if(P.State==EStudioFieldExportState::Publishing||P.State==EStudioFieldExportState::Complete)return TEXT("Publishing VTK file…");
        return P.Total?FString::Printf(TEXT("Writing original values · %d%%"),int32(100.*P.Completed/P.Total)):TEXT("Preparing VTK export…");
    }
    return Notice;
}
void FStudioFieldExportUI::MenuOpenChanged(bool bOpen)
{
    bMenuOpen=bOpen;
    if(!bOpen&&!Task.IsBusy())Draft.Field.Reset();
}
void FStudioFieldExportUI::Tick(FStudioModel& Model)
{
    if(const auto Result=Task.Poll())
    {
        bError=!Result->bSuccess&&!Result->bCancelled;bSaved=Result->bSuccess;Path=Result->Path;
        Notice=Result->bSuccess?FString::Printf(TEXT("Saved %s · %s · step %d · %.9g s"),*FPaths::GetCleanFilename(Path),*Result->Identity.Dataset,Result->Identity.Frame.Index,Result->Identity.Frame.Time):
            Result->bCancelled?TEXT("VTK export cancelled. Destination unchanged."):TEXT("VTK export failed: ")+Result->Error;
        if(Project==Model.Project.Id)
        {Model.Notice=Notice;Model.AddLog(Notice+(Result->bSuccess?TEXT(" · ")+Path:FString()),bError?EStudioLogSeverity::Error:EStudioLogSeverity::Info);}
        if(!bMenuOpen)Draft.Field.Reset();
    }
    if(Project!=Model.Project.Id&&!Task.IsBusy())
    {
        Draft.Field.Reset();Project=Model.Project.Id;Dataset.Empty();Path.Empty();bSaved=bError=false;
        Notice=bMenuOpen?TEXT("Project changed. Reopen VTK export to choose its displayed frame."):TEXT("");
    }
}
void FStudioFieldExportUI::Save(const TSharedRef<FStudioModel>& Model)
{
    if(Task.IsBusy()||!Draft.Field||Draft.Scalars.IsEmpty()||Project!=Model->Project.Id)return;
    // Native file selection may advance replay and dismiss/release the draft.
    // Pin the complete request before either occurs.
    auto Frozen=Draft;const auto Identity=Frozen.Field->Identity();if(!Identity)return;
    FSlateApplication::Get().DismissAllMenus();FString Destination;
    if(!StudioFileDialog::FieldVTK(FString::Printf(TEXT("field-step-%d"),Identity->Frame.Index),Destination))
    {Notice=TEXT("VTK file selection cancelled.");Model->Notice=Notice;bError=false;return;}
    Draft=Frozen;
#if WITH_DEV_AUTOMATION_TESTS
    Task.BeforePublishForAutomation=MoveTemp(NextFieldExportBarrier);
#endif
    if(Task.Start(MoveTemp(Frozen),Destination))
    {Path=Destination;bSaved=false;bError=false;Notice=TEXT("Writing original field…");}
    else {Notice=TEXT("Could not start VTK export. Reopen the menu and try again.");bError=true;}
    Model->Notice=Notice;
}
TSharedRef<SWidget> FStudioFieldExportUI::Menu(const TSharedRef<FStudioModel>& Model,AStudioScene* Scene)
{
    using namespace StudioUI;
    bMenuOpen=true;
    if(!Task.IsBusy())
    {
        Draft={};Scalars.Empty();Title.Empty();FrameLabel.Empty();Topology.Empty();Project=Model->Project.Id;
        if(Scene&&Scene->HasCurrentFrame()&&Scene->HasPresentedFrame())Draft.Field=Scene->PresentedField();
        const auto Identity=Draft.Field?Draft.Field->Identity():TOptional<FStudioFieldIdentity>();
        if(Identity&&Draft.Field->OriginalPointCount()>0)
        {
            if(Dataset!=Identity->Dataset){Notice.Empty();Path.Empty();bSaved=bError=false;}
            Dataset=Identity->Dataset;
            Title=Model->Solver->Descriptor().Title;
            FrameLabel=FString::Printf(TEXT("Frozen frame %d · step %d · %.9g s"),Identity->Ordinal+1,Identity->Frame.Index,Identity->Frame.Time);
            for(const auto& S:Model->Solver->Descriptor().Scalars)if(Draft.Field->Scalar(S.Id))Scalars.Add(S);
            Draft.Scalars.Add(Scene->PresentedScalar().Id);
            Topology=Draft.Field->OriginalTriangleCount()>0?
                FText::AsNumber(Draft.Field->OriginalTriangleCount()).ToString()+TEXT(" original triangles"):
                FText::AsNumber(Draft.Field->OriginalPointCount()).ToString()+TEXT(" original points · no source mesh");
            if(Path.IsEmpty()){Notice.Empty();bError=false;}
        }
        else {Draft.Field.Reset();Notice=TEXT("Wait for the displayed frame, then reopen VTK export.");bError=false;}
    }
    const auto State=AsShared();
    auto Editable=[State,Weak=TWeakPtr<FStudioModel>(Model)]
    {const auto M=Weak.Pin();return M&&M->Project.Id==State->Project&&State->Draft.Field&&!State->Task.IsBusy();};
    auto Items=SNew(SVerticalBox);
    Items->AddSlot().AutoHeight().Padding(0,0,0,10)[Label(TEXT("Export original field"),12,Text,true)];
    Items->AddSlot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(Title)).Font(Font(10,true)).ColorAndOpacity(Text).AutoWrapText(true)];
    Items->AddSlot().AutoHeight().Padding(0,4,0,0)[SNew(STextBlock).Tag(TEXT("VTKFrozenFrame"))
        .Text(FText::FromString(FrameLabel)).Font(Font(10)).ColorAndOpacity(Cyan).AutoWrapText(true)];
    Items->AddSlot().AutoHeight().Padding(0,4,0,12)[SNew(STextBlock).Text(FText::FromString(Topology)).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)];
    auto Choices=SNew(SVerticalBox);
    for(const auto& Scalar:Scalars)
    {
        const FString Id=Scalar.Id;
        Choices->AddSlot().AutoHeight().Padding(0,3)[SNew(SCheckBox).Tag(FName(*(TEXT("VTKField_")+Id)))
            .IsEnabled_Lambda(Editable).IsChecked_Lambda([State,Id]{return State->Draft.Scalars.Contains(Id)?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
            .OnCheckStateChanged_Lambda([State,Id](ECheckBoxState V){if(V==ECheckBoxState::Checked)State->Draft.Scalars.AddUnique(Id);else State->Draft.Scalars.Remove(Id);})
            [SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1).Padding(5,0)[Label(Scalar.Label,10)]
                +SHorizontalBox::Slot().AutoWidth()[Label(Scalar.Unit+(Scalar.Origin==TEXT("derived")?TEXT(" · derived"):TEXT("")),9,Muted)]]];
    }
    auto Selection=SNew(SHorizontalBox);
    Selection->AddSlot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Scalar arrays"),10,Text,true)];
    for(bool All:{true,false})Selection->AddSlot().AutoWidth().Padding(4,0)[SNew(SButton).Tag(All?TEXT("VTKSelectAll"):TEXT("VTKSelectNone"))
        .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7,4)).IsEnabled_Lambda(Editable)
        .OnClicked_Lambda([State,All]{State->Draft.Scalars.Empty();if(All)for(const auto& S:State->Scalars)State->Draft.Scalars.Add(S.Id);return FReply::Handled();})[Label(All?TEXT("All"):TEXT("None"),9)]];
    Items->AddSlot().AutoHeight()[Selection];
    Items->AddSlot().AutoHeight().Padding(0,4,0,12)[SNew(SBox).MaxDesiredHeight(190)[SNew(SScrollBox)
        .ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)+SScrollBox::Slot()[Choices]]];
    Items->AddSlot().AutoHeight().Padding(0,0,0,6)[Label(TEXT("Point coordinates"),10,Text,true)];
    auto Coordinates=SNew(SHorizontalBox);
    for(bool SceneCoordinates:{false,true})Coordinates->AddSlot().FillWidth(1).Padding(0,0,SceneCoordinates?0:5,0)
        [SNew(SButton).Tag(SceneCoordinates?TEXT("VTKCoordinatesScene"):TEXT("VTKCoordinatesSource"))
            .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7,6)).IsEnabled_Lambda(Editable)
            .OnClicked_Lambda([State,SceneCoordinates]{State->Draft.Coordinates=SceneCoordinates?EStudioExportCoordinates::Scene:EStudioExportCoordinates::Source;return FReply::Handled();})
            [SNew(STextBlock).Font(Font(10)).Text(FText::FromString(SceneCoordinates?TEXT("Scene XZY"):TEXT("Source XYZ")))
                .ColorAndOpacity_Lambda([State,SceneCoordinates]{return FSlateColor((State->Draft.Coordinates==EStudioExportCoordinates::Scene)==SceneCoordinates?Cyan:Text);})]];
    Items->AddSlot().AutoHeight()[Coordinates];
    Items->AddSlot().AutoHeight().Padding(0,6,0,12)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text_Lambda([State]{return FText::FromString(State->Draft.Coordinates==EStudioExportCoordinates::Source?
            TEXT("Meters, before display transforms. Includes source identity and units."):
            TEXT("Meters, with the display axis mapping and offset. Scalar components keep their source basis."));})];
    Items->AddSlot().AutoHeight()[SNew(SButton).Tag(TEXT("SaveFieldVTK")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,7))
        .IsEnabled_Lambda([State,Editable]{return Editable()&&!State->Draft.Scalars.IsEmpty();})
        .OnClicked_Lambda([State,Weak=TWeakPtr<FStudioModel>(Model)]{if(const auto M=Weak.Pin())State->Save(M.ToSharedRef());return FReply::Handled();})[Label(TEXT("Choose destination and save VTK…"),10,Cyan)]];
    Items->AddSlot().AutoHeight().Padding(0,8,0,0)[SNew(SProgressBar).Tag(TEXT("VTKProgress"))
        .FillColorAndOpacity(Cyan)
        .Visibility_Lambda([State]{return State->Task.IsBusy()?EVisibility::Visible:EVisibility::Collapsed;})
        .Percent_Lambda([State]()->TOptional<float>{const auto P=State->Task.Progress();return P.Total?TOptional<float>(float(double(P.Completed)/P.Total)):TOptional<float>();})];
    Items->AddSlot().AutoHeight().Padding(0,6,0,0)[SNew(SButton).Tag(TEXT("CancelFieldVTK")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,6))
        .Visibility_Lambda([State]{return State->Task.IsBusy()?EVisibility::Visible:EVisibility::Collapsed;})
        .IsEnabled_Lambda([State]{return State->Task.Progress().State==EStudioFieldExportState::Writing;})
        .OnClicked_Lambda([State]{State->Task.Cancel();return FReply::Handled();})[Label(TEXT("Cancel VTK export"),10)]];
    Items->AddSlot().AutoHeight().Padding(0,8,0,0)[SNew(STextBlock).Tag(TEXT("VTKNotice")).Font(Font(9)).AutoWrapText(true)
        .ColorAndOpacity_Lambda([State]{return FSlateColor(State->bError?Amber:Muted);})
        .Text_Lambda([State]{return FText::FromString(State->Draft.Field&&State->Draft.Scalars.IsEmpty()&&!State->Task.IsBusy()?TEXT("Select at least one scalar array."):State->Status());})];
    Items->AddSlot().AutoHeight().Padding(0,6,0,0)[SNew(SButton).Tag(TEXT("RevealFieldVTK")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,5))
        .Visibility_Lambda([State]{return State->bSaved&&!State->Task.IsBusy()?EVisibility::Visible:EVisibility::Collapsed;})
        .ToolTipText_Lambda([State]{return FText::FromString(State->Path);})
        .OnClicked_Lambda([State]{FPlatformProcess::ExploreFolder(*State->Path);return FReply::Handled();})[Label(TEXT("Show saved file in Finder"),9)]];
    return SNew(SBox).WidthOverride(370).MaxDesiredHeight(550)[SNew(SBorder).BorderImage(&PanelBrush).Padding(14)
        [SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)+SScrollBox::Slot()[Items]]];
}
