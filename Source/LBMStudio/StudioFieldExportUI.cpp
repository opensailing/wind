/*
THESIS: One Export owner for original fields, frame ranges and evaluated pipelines.
OWN-WORLD: Dense native blue-black Slate menus and cyan selection.
FIRST VIEWPORT: Explicit source or pipeline identity first; matching format,
scope and coordinates, then destination/save with progress and cancellation.
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
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
namespace { TFunction<void()> NextFieldExportBarrier; }
void FStudioFieldExportUI::SetBeforeNextPublishForAutomation(TFunction<void()> Barrier)
{check(IsInGameThread());if(FParse::Param(FCommandLine::Get(),TEXT("StudioAutomation")))NextFieldExportBarrier=MoveTemp(Barrier);}
#endif

FStudioFieldSequenceRequest FStudioFieldExportUI::SequenceRequest() const
{
    FStudioFieldSequenceRequest R{Source,0,0,Draft.Scalars,Draft.Coordinates,Draft.Format};
    if(Scope==EScope::All)R.LastOrdinal=Source?Source->FrameCount()-1:INDEX_NONE;
    else
    {
        auto Parse=[](FString Text)
        {
            Text.TrimStartAndEndInline();if(Text.IsEmpty()||Text.Len()>10)return 0;
            for(const TCHAR C:Text)if(C<TEXT('0')||C>TEXT('9'))return 0;
            int64 Value=0;return LexTryParseString(Value,*Text)&&Value>0&&Value<=MAX_int32?int32(Value):0;
        };
        const int32 First=Parse(FirstText),Last=Parse(LastText);
        R.FirstOrdinal=First-1;R.LastOrdinal=Last-1;
    }
    return R;
}
FString FStudioFieldExportUI::Validation() const
{
    if(bPipeline)
    {
        if(!Pipeline)return PipelineRecovery.IsEmpty()?TEXT("Evaluate the pipeline, then reopen Export."):PipelineRecovery;
        if(bProbe&&Draft.Format!=EStudioFieldExportFormat::CSV)return TEXT("Choose CSV to preserve every probe row and missing-value status.");
        return {};
    }
    if(!Draft.Field)return TEXT("Wait for the displayed frame, then reopen Export.");
    if(Draft.Scalars.IsEmpty())return TEXT("Select at least one scalar array.");
    if(Scope!=EScope::Current)
    {
        FString Error;if(!StudioFieldSequence::Validate(SequenceRequest(),Error))return Error;
        bool Valid=!FolderName.IsEmpty()&&FolderName.Len()<=128&&!FolderName.StartsWith(TEXT("."));
        for(const TCHAR C:FolderName)Valid&=C>=32&&C!=TEXT('/')&&C!=TEXT('\\')&&C!=TEXT(':');
        if(!Valid)return TEXT("Enter a new folder name without slashes, colons or a leading dot.");
    }
    return {};
}
FString FStudioFieldExportUI::FrameDescription() const
{
    if(Scope==EScope::Current)return FrameLabel;
    const auto R=SequenceRequest();
    if(!Source||!Source->Descriptor().Frames.IsValidIndex(R.FirstOrdinal)||!Source->Descriptor().Frames.IsValidIndex(R.LastOrdinal)||R.FirstOrdinal>R.LastOrdinal)
        return TEXT("Choose first and last frames inside this recording.");
    const auto& A=Source->Descriptor().Frames[R.FirstOrdinal];const auto& B=Source->Descriptor().Frames[R.LastOrdinal];
    return FString::Printf(TEXT("%d original frames · steps %d–%d · %.9g–%.9g s"),R.LastOrdinal-R.FirstOrdinal+1,A.Index,B.Index,A.Time,B.Time);
}
FStudioFieldExportProgress FStudioFieldExportUI::Progress() const
{
    if(!Sequence.IsBusy())return Task.Progress();
    const auto P=Sequence.Progress();FStudioFieldExportProgress R;R.State=P.State;
    R.Total=int64(P.TotalFrames)*1000000;R.Completed=int64(P.CompletedFrames)*1000000;
    if(P.FrameTotal)R.Completed+=int64(1000000.*P.FrameCompleted/P.FrameTotal);return R;
}
void FStudioFieldExportUI::Cancel(){if(Sequence.IsBusy())Sequence.Cancel();else Task.Cancel();}
FString FStudioFieldExportUI::Status() const
{
    if(IsBusy())
    {
        const auto P=Progress();
        if(P.State==EStudioFieldExportState::Cancelled)return TEXT("Cancelling field export…");
        if(P.State==EStudioFieldExportState::Publishing||P.State==EStudioFieldExportState::Complete)return TEXT("Publishing completed export…");
        if(Sequence.IsBusy())
        {const auto F=Sequence.Progress();return FString::Printf(TEXT("Writing frame %d of %d · %d%%"),FMath::Min(F.CompletedFrames+1,F.TotalFrames),F.TotalFrames,P.Total?int32(100.*P.Completed/P.Total):0);}
        return P.Total?FString::Printf(TEXT("%s · %d%%"),bPipeline?TEXT("Writing evaluated output"):TEXT("Writing original values"),int32(100.*P.Completed/P.Total)):TEXT("Preparing field export…");
    }
    return Notice;
}
void FStudioFieldExportUI::MenuOpenChanged(bool bOpen)
{
    bMenuOpen=bOpen;
    if(!bOpen&&!IsBusy())ReleaseSnapshots();
}
void FStudioFieldExportUI::ReleaseSnapshots(){Draft.Field.Reset();Source.Reset();Pipeline.Reset();}
void FStudioFieldExportUI::Tick(FStudioModel& Model)
{
    bool Finished=false;
    if(const auto Result=Task.Poll())
    {
        Finished=true;bError=!Result->bSuccess&&!Result->bCancelled;bSaved=Result->bSuccess;Path=Result->Path;
        Notice=Result->bSuccess?FString::Printf(TEXT("Saved %s · %s · step %d · %.9g s"),*FPaths::GetCleanFilename(Path),
            Result->PipelineId.IsValid()?*Result->PipelineName:*Result->Identity.Dataset,Result->Identity.Frame.Index,Result->Identity.Frame.Time):
            Result->bCancelled?TEXT("Field export cancelled. Destination unchanged."):TEXT("Field export failed: ")+Result->Error;
    }
    if(const auto Result=Sequence.Poll())
    {
        Finished=true;bError=!Result->bSuccess&&!Result->bCancelled;bSaved=Result->bSuccess;Path=Result->Path;
        Notice=Result->bSuccess&&Result->FirstIdentity&&Result->LastIdentity?
            FString::Printf(TEXT("Saved %s · %d original frames · %s · steps %d–%d"),*FPaths::GetCleanFilename(Path),Result->CompletedFrames,
                *Result->FirstIdentity->Dataset,Result->FirstIdentity->Frame.Index,Result->LastIdentity->Frame.Index):
            Result->bCancelled?TEXT("Field export cancelled. Destination unchanged."):TEXT("Field export failed: ")+Result->Error+
                (Result->FailedOrdinal==INDEX_NONE?FString():FString::Printf(TEXT(" (frame %d)"),Result->FailedOrdinal+1));
    }
    if(Finished)
    {
        if(Project==Model.Project.Id)
        {Model.Notice=Notice;Model.AddLog(Notice+(bSaved?TEXT(" · ")+Path:FString()),bError?EStudioLogSeverity::Error:EStudioLogSeverity::Info);}
        if(!bMenuOpen)ReleaseSnapshots();
    }
    if(Project!=Model.Project.Id&&!IsBusy())
    {
        ReleaseSnapshots();SourceKey.Empty();Project=Model.Project.Id;Dataset.Empty();Path.Empty();bSaved=bError=false;
        PipelineRecovery=TEXT("Project changed. Reopen Export to choose its evaluated output.");
        Notice=bMenuOpen?TEXT("Project changed. Reopen Export to choose its displayed source."):TEXT("");
    }
}
void FStudioFieldExportUI::Save(const TSharedRef<FStudioModel>& Model)
{
    if(IsBusy()||!Validation().IsEmpty()||Project!=Model->Project.Id)return;
    // Pin both requests before menu dismissal/native selection can advance replay.
    auto Frozen=Draft;auto PipelineFrozen=Pipeline;auto Frames=SequenceRequest();
    const auto Identity=bPipeline?TOptional<FStudioFieldIdentity>(PipelineFrozen->Evaluation.Prepared.Recipe.Source.Identity):Frozen.Field->Identity();if(!Identity)return;
    const bool PipelineOutput=bPipeline,Multiple=!bPipeline&&Scope!=EScope::Current;const FString Name=FolderName;const FGuid Owner=Project;
    if(PipelineFrozen){PipelineFrozen->Format=Frozen.Format;PipelineFrozen->Coordinates=Frozen.Coordinates;}
    FSlateApplication::Get().DismissAllMenus();FString Destination;
    const FString Suggested=FString::Printf(TEXT("%s-step-%d"),PipelineOutput?TEXT("pipeline"):TEXT("field"),Identity->Frame.Index);
    const bool Accepted=Multiple?StudioFileDialog::ExportFolder(Destination):Frozen.Format==EStudioFieldExportFormat::VTK?
        StudioFileDialog::FieldVTK(Suggested,Destination,PipelineOutput):StudioFileDialog::CSV(Suggested,Destination,
            PipelineOutput?TEXT("Export Pipeline Output"):TEXT("Export Original Field"),PipelineOutput?
            TEXT("Saves evaluated vertices or all probe rows, with missing values, source identity, units and the complete recipe."):
            TEXT("Saves original point rows and selected arrays. The first comment contains source identity, units and coordinate meaning."));
    if(!Accepted){Notice=Multiple?TEXT("Export folder selection cancelled."):TEXT("Export file selection cancelled.");if(Owner==Model->Project.Id)Model->Notice=Notice;bError=false;return;}
    Draft=Frozen;Source=Frames.Source;Pipeline=PipelineFrozen;Project=Owner;bool Started=false;
#if WITH_DEV_AUTOMATION_TESTS
    if(Multiple)Sequence.BeforePublishForAutomation=MoveTemp(NextFieldExportBarrier);else Task.BeforePublishForAutomation=MoveTemp(NextFieldExportBarrier);
#endif
    FString Error;
    if(Multiple)Started=Sequence.Start(MoveTemp(Frames),Destination,Name,Error);
    else if(PipelineOutput)Started=Task.Start(MoveTemp(*PipelineFrozen),Destination);
    else Started=Task.Start(MoveTemp(Frozen),Destination);
    if(Started){Path=Multiple?Destination/Name:Destination;bSaved=false;bError=false;Notice=PipelineOutput?TEXT("Writing evaluated pipeline output…"):TEXT("Writing original field data…");}
    else {Notice=Error.IsEmpty()?TEXT("Could not start export. Reopen the menu and try again."):Error;bError=true;if(!bMenuOpen)ReleaseSnapshots();}
    if(Project==Model->Project.Id)Model->Notice=Notice;
}
TSharedRef<SWidget> FStudioFieldExportUI::Menu(const TSharedRef<FStudioModel>& Model,AStudioScene* Scene,
    bool bPipelineContext,TOptional<FStudioPipelineEvaluationResult> Evaluation,const FString& Recovery)
{
    using namespace StudioUI;
    bMenuOpen=true;
    if(!IsBusy())
    {
        if(Project!=Model->Project.Id)SourceKey.Empty();
        ReleaseSnapshots();Scalars.Empty();Title.Empty();FrameLabel.Empty();Topology.Empty();Project=Model->Project.Id;
        bPipeline=bPipelineContext;bProbe=false;PipelineRecovery=Recovery;Method.Empty();ScalarMeaning.Empty();
        if(bPipeline)
        {
            Scope=EScope::Current;Title=TEXT("Pipeline output");FrameLabel=TEXT("No exportable evaluation");
            if(Evaluation)
            {
                const auto& P=Evaluation->Prepared.Recipe;const auto& I=P.Source.Identity;const auto& O=*Evaluation->Output;const auto& S=Evaluation->Prepared.Field->SelectedScalar();
                const FString Key=TEXT("pipeline:")+P.Id.ToString()+I.Dataset+I.MetadataSHA256+I.PayloadSHA256+I.ReconstructionSHA256;
                if(Key!=SourceKey){Notice.Empty();Path.Empty();bSaved=bError=false;Draft.Format=EStudioFieldExportFormat::VTK;Draft.Coordinates=EStudioExportCoordinates::Scene;}
                SourceKey=Key;Dataset=I.Dataset;Title=P.Name+TEXT("\n")+P.Source.Title;bProbe=O.Probe.IsSet();
                if(bProbe)Draft.Format=EStudioFieldExportFormat::CSV;
                FrameLabel=FString::Printf(TEXT("Frozen evaluation · frame %d · step %d · %.9g s"),I.Ordinal+1,I.Frame.Index,I.Frame.Time);
                Topology=bProbe?FString::Printf(TEXT("%d probe rows · unavailable values remain blank"),O.Probe->Samples.Num()):
                    FString::Printf(TEXT("%d evaluated vertices · %d triangles · %d segments"),O.Vertices.Num(),O.Triangles.Num(),O.Lines.Num());
                Method=O.Method;ScalarMeaning=S.Label+TEXT(" · ")+S.Id+TEXT(" (")+S.Unit+TEXT(") · ")+S.Origin;
                const auto Expression=Evaluation->Prepared.Field->ScalarExpression(S.Id);if(!Expression.IsEmpty())ScalarMeaning+=TEXT("\n")+Expression;
                Pipeline=FStudioPipelineExportRequest{MoveTemp(*Evaluation),Draft.Coordinates,Draft.Format};
                if(Path.IsEmpty()){Notice.Empty();bError=false;}
            }
        }
        else
        {
            if(Scene&&Scene->HasCurrentFrame()&&Scene->HasPresentedFrame())Draft.Field=Scene->PresentedField();
            const auto Identity=Draft.Field?Draft.Field->Identity():TOptional<FStudioFieldIdentity>();
            if(Identity&&Draft.Field->OriginalPointCount()>0)
            {
                const FString Key=Identity->Dataset+Identity->MetadataSHA256+Identity->PayloadSHA256+Identity->ReconstructionSHA256;
                const bool NewSource=Key!=SourceKey;SourceKey=Key;Source=Model->Solver;Dataset=Identity->Dataset;
                if(NewSource)
                {
                    Notice.Empty();Path.Empty();bSaved=bError=false;Draft.Scalars={Scene->PresentedScalar().Id};Draft.Coordinates=EStudioExportCoordinates::Source;
                    Scope=EScope::Current;FirstText=FString::FromInt(Identity->Ordinal+1);LastText=FString::FromInt(Source->FrameCount());
                    FolderName=TEXT("flow-")+FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
                }
                Title=Source->Descriptor().Title;
                FrameLabel=FString::Printf(TEXT("Frozen frame %d · step %d · %.9g s"),Identity->Ordinal+1,Identity->Frame.Index,Identity->Frame.Time);
                for(const auto& S:Source->Descriptor().Scalars)if(Draft.Field->Scalar(S.Id))Scalars.Add(S);
                Topology=Draft.Field->OriginalTriangleCount()>0?
                    FText::AsNumber(Draft.Field->OriginalTriangleCount()).ToString()+TEXT(" original triangles"):
                    FText::AsNumber(Draft.Field->OriginalPointCount()).ToString()+TEXT(" original points · no source mesh");
                if(Path.IsEmpty()){Notice.Empty();bError=false;}
            }
            else {Draft.Field.Reset();Notice=TEXT("Wait for the displayed frame, then reopen Export.");bError=false;}
        }
    }
    const auto State=AsShared();
    auto Editable=[State,Weak=TWeakPtr<FStudioModel>(Model)]
    {const auto M=Weak.Pin();return M&&M->Project.Id==State->Project&&!M->IsProjectOpenPending()&&State->HasSnapshot()&&!State->IsBusy();};
    auto Header=SNew(SVerticalBox);
    Header->AddSlot().AutoHeight().Padding(0,0,0,10)[Label(bPipeline?TEXT("Export pipeline output"):TEXT("Export original field data"),12,Text,true)];
    Header->AddSlot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(Title)).Font(Font(10,true)).ColorAndOpacity(Text).AutoWrapText(true)];
    Header->AddSlot().AutoHeight().Padding(0,4,0,0)[SNew(STextBlock).Tag(TEXT("VTKFrozenFrame"))
        .Text_Lambda([State]{return FText::FromString(State->FrameDescription());}).Font(Font(10)).ColorAndOpacity(Cyan).AutoWrapText(true)];
    Header->AddSlot().AutoHeight().Padding(0,4,0,12)[SNew(STextBlock).Text(FText::FromString(Topology)).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)];
    auto Items=SNew(SVerticalBox);
    auto Formats=SNew(SHorizontalBox);
    for(bool CSV:{false,true})Formats->AddSlot().FillWidth(1).Padding(0,0,CSV?0:5,0)
        [SNew(SButton).Tag(CSV?TEXT("ExportFormatCSV"):TEXT("ExportFormatVTK")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,6))
            .IsEnabled_Lambda([Editable,State,CSV]{return Editable()&&(CSV||!State->bProbe);}).OnClicked_Lambda([State,CSV]{State->Draft.Format=CSV?EStudioFieldExportFormat::CSV:EStudioFieldExportFormat::VTK;return FReply::Handled();})
            [SNew(STextBlock).Font(Font(10)).Text(FText::FromString(CSV?TEXT("CSV table"):TEXT("VTK XML")))
                .ColorAndOpacity_Lambda([State,CSV]{return FSlateColor((State->Draft.Format==EStudioFieldExportFormat::CSV)==CSV?Cyan:Text);})]];
    Items->AddSlot().AutoHeight()[Formats];
    Items->AddSlot().AutoHeight().Padding(0,5,0,12)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text_Lambda([State]{if(State->bPipeline)return FText::FromString(State->bProbe?TEXT("Probe tables require CSV to preserve every probe row and missing-value status."):
            State->Draft.Format==EStudioFieldExportFormat::CSV?TEXT("Evaluated vertex rows; connectivity is omitted. Metadata identifies derived positions and the complete recipe."):
            TEXT("Evaluated points, lines or triangles. Metadata identifies derived positions and the complete recipe."));
            return FText::FromString(State->Draft.Format==EStudioFieldExportFormat::CSV?
            TEXT("Original point rows. Skip the first metadata comment when reading the CSV table."):
            TEXT("Original points and supplied mesh. No display reconstruction or extrusion."));})];
    auto Scopes=SNew(SHorizontalBox).Visibility(bPipeline?EVisibility::Collapsed:EVisibility::Visible);
    for(const auto Choice:{EScope::Current,EScope::Range,EScope::All})Scopes->AddSlot().FillWidth(1).Padding(0,0,Choice==EScope::All?0:5,0)
        [SNew(SButton).Tag(Choice==EScope::Current?TEXT("ExportScopeCurrent"):Choice==EScope::Range?TEXT("ExportScopeRange"):TEXT("ExportScopeAll"))
            .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(7,6)).IsEnabled_Lambda(Editable)
            .OnClicked_Lambda([State,Choice]{State->Scope=Choice;return FReply::Handled();})
            [SNew(STextBlock).Font(Font(10)).Text(FText::FromString(Choice==EScope::Current?TEXT("Displayed frame"):Choice==EScope::Range?TEXT("Frame range"):TEXT("All frames")))
                .ColorAndOpacity_Lambda([State,Choice]{return FSlateColor(State->Scope==Choice?Cyan:Text);})]];
    Items->AddSlot().AutoHeight().Padding(0,0,0,8)[Scopes];
    auto Range=SNew(SHorizontalBox).Visibility_Lambda([State]{return State->Scope==EScope::Range?EVisibility::Visible:EVisibility::Collapsed;});
    for(bool Last:{false,true})Range->AddSlot().FillWidth(1).Padding(0,0,Last?0:10,0)
        [SNew(SHorizontalBox)+SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,7,0)[Label(Last?TEXT("Last frame"):TEXT("First frame"),9)]
            +SHorizontalBox::Slot().FillWidth(1)[SNew(SEditableTextBox).Tag(Last?TEXT("ExportLastFrame"):TEXT("ExportFirstFrame"))
                .Style(&InputStyle()).Font(Font(10)).IsEnabled_Lambda(Editable)
                .Text_Lambda([State,Last]{return FText::FromString(Last?State->LastText:State->FirstText);})
                .OnTextChanged_Lambda([State,Last](const FText& Value){(Last?State->LastText:State->FirstText)=Value.ToString();})]];
    Items->AddSlot().AutoHeight().Padding(0,0,0,6)[Range];
    Items->AddSlot().AutoHeight().Padding(0,0,0,10)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Visibility_Lambda([State]{return State->Scope!=EScope::Current?EVisibility::Visible:EVisibility::Collapsed;})
        .Text_Lambda([State]{return FText::FromString(FString::Printf(TEXT("Frames 1–%d · inclusive originals, no interpolation. Files may require substantial disk space."),State->Source?State->Source->FrameCount():0));})];
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
    Selection->SetVisibility(bPipeline?EVisibility::Collapsed:EVisibility::Visible);
    Items->AddSlot().AutoHeight()[Selection];
    Items->AddSlot().AutoHeight().Padding(0,4,0,12)[SNew(SBox).Visibility(bPipeline?EVisibility::Collapsed:EVisibility::Visible).MaxDesiredHeight(125)[SNew(SScrollBox)
        .ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)+SScrollBox::Slot()[Choices]]];
    if(bPipeline&&Pipeline)
    {
        Items->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(STextBlock).Tag(TEXT("PipelineExportScalar")).Font(Font(10)).ColorAndOpacity(Text).AutoWrapText(true).Text(FText::FromString(ScalarMeaning))];
        Items->AddSlot().AutoHeight().Padding(0,0,0,12)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true).Text(FText::FromString(Method))];
        Items->AddSlot().AutoHeight().Padding(0,0,0,12)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)
            .Text(FText::FromString(TEXT("This evaluated result is frozen while Export is open. To export another result, change the recipe or frame and Evaluate again.")))];
    }
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
    Items->AddSlot().AutoHeight().Padding(0,0,0,10)[SNew(SVerticalBox)
        .Visibility_Lambda([State]{return State->Scope!=EScope::Current?EVisibility::Visible:EVisibility::Collapsed;})
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[Label(TEXT("New export folder name"),10,Text,true)]
        +SVerticalBox::Slot().AutoHeight()[SNew(SEditableTextBox).Tag(TEXT("ExportFolderName")).Style(&InputStyle()).Font(Font(10))
            .IsEnabled_Lambda(Editable).Text_Lambda([State]{return FText::FromString(State->FolderName);})
            .OnTextChanged_Lambda([State](const FText& Value){State->FolderName=Value.ToString();})]
        +SVerticalBox::Slot().AutoHeight().Padding(0,5,0,0)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)
            .Text(FText::FromString(TEXT("The complete sequence is published together. Existing folders are never replaced.")))]];
    auto Footer=SNew(SVerticalBox);
    Footer->AddSlot().AutoHeight()[SNew(SButton).Tag(TEXT("SaveFieldVTK")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,7))
        .IsEnabled_Lambda([State,Editable]{return Editable()&&State->Validation().IsEmpty();})
        .OnClicked_Lambda([State,Weak=TWeakPtr<FStudioModel>(Model)]{if(const auto M=Weak.Pin())State->Save(M.ToSharedRef());return FReply::Handled();})[SNew(STextBlock).Font(Font(10)).ColorAndOpacity(Cyan).Text_Lambda([State]{return FText::FromString(State->Scope==EScope::Current?TEXT("Choose destination and save…"):TEXT("Choose parent folder and export…"));})]];
    Footer->AddSlot().AutoHeight().Padding(0,8,0,0)[SNew(SProgressBar).Tag(TEXT("VTKProgress"))
        .FillColorAndOpacity(Cyan)
        .Visibility_Lambda([State]{return State->IsBusy()?EVisibility::Visible:EVisibility::Collapsed;})
        .Percent_Lambda([State]()->TOptional<float>{const auto P=State->Progress();return P.Total?TOptional<float>(float(double(P.Completed)/P.Total)):TOptional<float>();})];
    Footer->AddSlot().AutoHeight().Padding(0,6,0,0)[SNew(SButton).Tag(TEXT("CancelFieldVTK")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,6))
        .Visibility_Lambda([State]{return State->IsBusy()?EVisibility::Visible:EVisibility::Collapsed;})
        .IsEnabled_Lambda([State]{return State->Progress().State==EStudioFieldExportState::Writing;})
        .OnClicked_Lambda([State]{State->Cancel();return FReply::Handled();})[Label(TEXT("Cancel export"),10)]];
    Footer->AddSlot().AutoHeight().Padding(0,8,0,0)[SNew(STextBlock).Tag(TEXT("VTKNotice")).Font(Font(9)).AutoWrapText(true)
        .ColorAndOpacity_Lambda([State]{return FSlateColor(State->bError?Amber:Muted);})
        .Text_Lambda([State]{return FText::FromString(!State->IsBusy()&&!State->Validation().IsEmpty()?State->Validation():State->Status());})];
    Footer->AddSlot().AutoHeight().Padding(0,6,0,0)[SNew(SButton).Tag(TEXT("RevealFieldVTK")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(9,5))
        .Visibility_Lambda([State]{return State->bSaved&&!State->IsBusy()?EVisibility::Visible:EVisibility::Collapsed;})
        .ToolTipText_Lambda([State]{return FText::FromString(State->Path);})
        .OnClicked_Lambda([State]{FPlatformProcess::ExploreFolder(*State->Path);return FReply::Handled();})[Label(TEXT("Show saved export in Finder"),9)]];
    return SNew(SBox).WidthOverride(430).MaxDesiredHeight(610)[SNew(SBorder).Tag(TEXT("FieldExportPanel")).BorderImage(&PanelBrush).Padding(14)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[Header]
            +SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)
                +SScrollBox::Slot()[Items]]
            +SVerticalBox::Slot().AutoHeight().Padding(0,10,0,0)[Footer]]];
}
