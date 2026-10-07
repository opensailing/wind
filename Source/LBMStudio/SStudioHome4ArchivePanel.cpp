#include "SStudioHome4ArchivePanel.h"
#include "StudioHome4ArchivePrivate.h"
#include "StudioModel.h"
#include "StudioFileDialog.h"
#include "StudioTheme.h"
#include "Async/Async.h"
#include "Misc/Paths.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBorder.h"

TSharedRef<SWidget> SStudioHome4ArchivePanel::Text(const FString& Label,FName Tag,FString& Value,const FString& Help)
{
    using namespace StudioUI;auto* Target=&Value;
    return SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(Label)).Font(Font(10)).ColorAndOpacity(StudioUI::Text)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,3,0,8)[SNew(SEditableTextBox).Tag(Tag).Style(&InputStyle()).Font(Font(10)).ToolTipText(FText::FromString(Help))
            .Text_Lambda([Target]{return FText::FromString(*Target);}).OnTextChanged_Lambda([Target](const FText& T){*Target=T.ToString();})];
}
TSharedRef<SWidget> SStudioHome4ArchivePanel::Choice(const FString& Label,FName Tag,FString& Value,const TArray<FString>& Options,const FString& Help)
{
    using namespace StudioUI;auto* Target=&Value;auto Row=SNew(SHorizontalBox);
    for(int32 I=0;I<Options.Num();++I){const FString Option=Options[I];Row->AddSlot().FillWidth(1).Padding(0,0,I+1<Options.Num()?3:0,0)
        [SNew(SButton).Tag(FName(Tag.ToString()+TEXT("_")+Option)).ButtonStyle(&ButtonStyle()).ToolTipText(FText::FromString(Help)).ContentPadding(FMargin(3,5))
         .OnClicked_Lambda([Target,Option]{*Target=Option;return FReply::Handled();})[SNew(STextBlock).Font(Font(9)).Text(FText::FromString(Option)).ColorAndOpacity_Lambda([Target,Option]{return FSlateColor(*Target==Option?Cyan:StudioUI::Text);})]];}
    return SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(Label)).Font(Font(10)).ColorAndOpacity(StudioUI::Text)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,3,0,8)[Row];
}
void SStudioHome4ArchivePanel::Construct(const FArguments& A)
{
    Model=A._Model;using namespace StudioUI;auto Form=SNew(SVerticalBox);
    Form->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(STextBlock).Font(Font(10)).AutoWrapText(true).ColorAndOpacity(Muted)
        .Text(FText::FromString(TEXT("Import original HOME4 NPZ snapshots. Confirm source axes and units below. Original masks, steps and provenance are retained.")))];
    Form->AddSlot().AutoHeight().Padding(0,0,0,6)[SNew(SButton).Tag(TEXT("Home4ArchiveAddSource")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,5))
        .IsEnabled_Lambda([this]{return !Task.IsBusy()&&!PendingInspection.IsValid();}).OnClicked_Lambda([this]{AddSource();return FReply::Handled();})[Label(TEXT("Add original NPZ snapshot"))]];
    Form->AddSlot().AutoHeight()[SAssignNew(SourceList,SVerticalBox)];
    Form->AddSlot().AutoHeight()[Choice(TEXT("Array axis order"),TEXT("Home4ArchiveAxes"),AxisOrder,{TEXT("xyz"),TEXT("xzy"),TEXT("yxz"),TEXT("yzx"),TEXT("zxy"),TEXT("zyx")},TEXT("Meaning of each source array dimension. Storage order C/F is read from NPY; physical axes require this declaration."))];
    Form->AddSlot().AutoHeight()[Choice(TEXT("Origin/spacing component order"),TEXT("Home4ArchiveMetadata"),MetadataOrder,{TEXT("xyz"),TEXT("array")},TEXT("Whether original origin and spacing triples are XYZ or follow the declared array-axis order."))];
    Form->AddSlot().AutoHeight()[Choice(TEXT("Original coordinates"),TEXT("Home4ArchiveCoordinateUnits"),CoordinateUnits,{TEXT("lattice"),TEXT("physical")},TEXT("Units of source origin and spacing: lattice lengths or metres."))];
    Form->AddSlot().AutoHeight()[Choice(TEXT("Original velocity"),TEXT("Home4ArchiveVelocityUnits"),VelocityUnits,{TEXT("lattice"),TEXT("physical")},TEXT("Units of ux/uy/uz: lattice cells per step or metres per second."))];
    Form->AddSlot().AutoHeight()[Text(TEXT("Original metres per lattice cell (dx)"),TEXT("Home4ArchiveDx"),Dx,TEXT("Required for lattice coordinate/velocity conversion. Never copied from the next-run case."))];
    Form->AddSlot().AutoHeight()[Text(TEXT("Original seconds per solver step (dt)"),TEXT("Home4ArchiveDt"),Dt,TEXT("Required for physical original times and velocity conversion."))];
    Form->AddSlot().AutoHeight()[Text(TEXT("Original reference density (kg/m³, optional)"),TEXT("Home4ArchiveDensity"),Density,TEXT("Stored density and WB pressure conversions use this original reference; blank keeps unavailable conversions explicit."))];
    Form->AddSlot().AutoHeight()[Text(TEXT("Original physical time origin (s)"),TEXT("Home4ArchiveTimeOrigin"),TimeOrigin,TEXT("Physical timestamp = original iteration × dt + this explicitly confirmed time origin."))];
    Form->AddSlot().AutoHeight()[Text(TEXT("Liquid threshold in original phi"),TEXT("Home4ArchiveLiquid"),Liquid,TEXT("Display/velocity support and central-difference halos use this threshold. Phi overshoots are preserved."))];
    Form->AddSlot().AutoHeight()[Text(TEXT("Original XYZ crop (optional)"),TEXT("Home4ArchiveCrop"),Crop,TEXT("XSTART:XSTOP,YSTART:YSTOP,ZSTART:ZSTOP; half-open original integer bounds. Blank retains all nodes."))];
    Form->AddSlot().AutoHeight()[Text(TEXT("Preview stride"),TEXT("Home4ArchiveStride"),Stride,TEXT("Retain every Nth original node. Original support masks protect hidden air/obstacles. The preview is identified in provenance."))];
    Form->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SCheckBox).Tag(TEXT("Home4ArchiveDerivatives")).IsChecked_Lambda([this]{return bDerivatives?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
        .OnCheckStateChanged_Lambda([this](ECheckBoxState S){bDerivatives=S==ECheckBoxState::Checked;})[Label(TEXT("Original-grid Q, vorticity, divergence and helicity"))]];
    Form->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SCheckBox).Tag(TEXT("Home4ArchiveWBPressure")).IsChecked_Lambda([this]{return bWBPressure?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
        .ToolTipText(FText::FromString(TEXT("Explicitly declare WB lattice convention. Requires original rho, p_star, Pi_h and Pi_h0 plus dx/dt/reference density for Pa.")))
        .OnCheckStateChanged_Lambda([this](ECheckBoxState S){bWBPressure=S==ECheckBoxState::Checked;})[Label(TEXT("Confirm WB lattice pressure convention"))]];
    Form->AddSlot().AutoHeight()[Text(TEXT("Recording title"),TEXT("Home4ArchiveTitle"),Title)];
    Form->AddSlot().AutoHeight()[Text(TEXT("Original source URL / URI"),TEXT("Home4ArchiveURI"),URI,TEXT("Identify the actual supplied source; local file URIs are accepted. No fabricated CFD sample is installed."))];
    Form->AddSlot().AutoHeight()[Text(TEXT("Source attribution"),TEXT("Home4ArchiveAttribution"),Attribution)];
    Form->AddSlot().AutoHeight()[Text(TEXT("New output folder name"),TEXT("Home4ArchiveFolder"),Folder,TEXT("Published beside the private stage. Existing destinations are never overwritten."))];
    auto Actions=SNew(SHorizontalBox);for(int32 I=0;I<2;++I)Actions->AddSlot().FillWidth(1).Padding(0,0,I?0:5,0)
        [SNew(SButton).Tag(I?TEXT("Home4ArchiveVTI"):TEXT("Home4ArchiveImport")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8,6))
         .IsEnabled_Lambda([this]{return !Task.IsBusy()&&!PendingInspection.IsValid()&&!bImportPending;}).OnClicked_Lambda([this,I]{Start(I==1);return FReply::Handled();})[Label(I?TEXT("Export original VTI"):TEXT("Convert and open"))]];
    Form->AddSlot().AutoHeight()[Actions];Form->AddSlot().AutoHeight().Padding(0,6,0,0)[SNew(SButton).Tag(TEXT("Home4ArchiveCancel")).ButtonStyle(&ButtonStyle())
        .IsEnabled_Lambda([this]{return Task.IsBusy()||PendingInspection.IsValid();}).OnClicked_Lambda([this]{Task.Cancel();if(InspectionCancel)InspectionCancel->store(true);return FReply::Handled();})[Label(TEXT("Cancel archive operation"))]];
    Form->AddSlot().AutoHeight().Padding(0,6,0,0)[SNew(SButton).Tag(TEXT("Home4ArchiveOpenCompleted")).ButtonStyle(&ButtonStyle())
        .IsEnabled_Lambda([this]{return !CompletedRecording.IsEmpty()&&!Task.IsBusy()&&!bImportPending;}).OnClicked_Lambda([this]{OpenCompleted();return FReply::Handled();})[Label(TEXT("Open completed recording"))]];
    Form->AddSlot().AutoHeight().Padding(0,8,0,0)[SNew(STextBlock).Tag(TEXT("Home4ArchiveStatus")).Font(Font(9)).AutoWrapText(true).ColorAndOpacity(Muted).Text_Lambda([this]{return FText::FromString(Status());})];
    ChildSlot[Form];
}
SStudioHome4ArchivePanel::~SStudioHome4ArchivePanel(){Task.Cancel();if(InspectionCancel)InspectionCancel->store(true);}
void SStudioHome4ArchivePanel::AddSource()
{
    if(Task.IsBusy()||PendingInspection.IsValid())return;FString Path;
    bool Selected=false;
#if WITH_DEV_AUTOMATION_TESTS
    if(bAutomationFileDialogs){if(NextSourcePath.IsSet()){Path=MoveTemp(*NextSourcePath);NextSourcePath.Reset();Selected=!Path.IsEmpty();}}
    else
#endif
    Selected=StudioFileDialog::DataFile(false,TEXT("Choose original HOME4 NPZ snapshot"),{},TEXT("npz"),Path);
    if(!Selected){Notice=TEXT("Source selection cancelled.");return;}
    if(Inspections.Num()>=StudioHome4Archives::MaximumFrames){Notice=TEXT("Snapshot list exceeds the bounded frame count.");return;}
    InspectionCancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);PendingInspection=Async(EAsyncExecution::ThreadPool,[Path,C=InspectionCancel]{return StudioHome4Archives::Inspect(Path,C);});Notice=TEXT("Verifying original archive headers, values and checksums…");
}
void SStudioHome4ArchivePanel::RebuildSources()
{
    using namespace StudioUI;SourceList->ClearChildren();for(int32 I=0;I<Inspections.Num();++I)
    {
        auto& Source=Inspections[I];SourceList->AddSlot().AutoHeight().Padding(0,0,0,4)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1)[SNew(STextBlock).Font(Font(9)).AutoWrapText(true).Text(FText::FromString(FString::Printf(TEXT("%d. %s · %d original arrays"),I+1,*FPaths::GetCleanFilename(Source.Source.Path),Source.Members.Num())))]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SButton).ButtonStyle(&ButtonStyle()).IsEnabled_Lambda([this]{return !Task.IsBusy()&&!PendingInspection.IsValid();})
                .OnClicked_Lambda([this,I]{Inspections.RemoveAt(I);RebuildSources();return FReply::Handled();})[Label(TEXT("Remove"),8)]]];
    }
}
bool SStudioHome4ArchivePanel::Request(FStudioHome4ArchiveRequest& R,FString& E)const
{
    using namespace StudioHome4ArchivePrivate;if(Inspections.IsEmpty()){E=TEXT("Add original NPZ snapshots in increasing solver-step order.");return false;}
    auto Number=[&](const FString& T,double& V){const auto Clean=T.TrimStartAndEnd();return !Clean.IsEmpty()&&Clean.Len()<=128&&LexTryParseString(V,*Clean)&&FMath::IsFinite(V);};
    auto OptionalNumber=[&](const FString& T,TOptional<double>& V){if(T.TrimStartAndEnd().IsEmpty())return true;double D;if(!Number(T,D))return false;V=D;return true;};
    R.Mapping.AxisOrder=AxisOrder;R.Mapping.MetadataOrder=MetadataOrder;R.Mapping.CoordinateUnits=CoordinateUnits;R.Mapping.VelocityUnits=VelocityUnits;
    if(!OptionalNumber(Dx,R.Mapping.DxMeters)||!OptionalNumber(Dt,R.Mapping.DtSeconds)||!OptionalNumber(Density,R.Mapping.DensityReferenceKgM3)||!Number(Liquid,R.Mapping.LiquidMinimum)||!Number(TimeOrigin,R.Mapping.TimeOriginSeconds))
    {E=TEXT("Enter finite original unit anchors, liquid threshold and time origin.");return false;}
    int64 N=0;const FString Step=Stride.TrimStartAndEnd();for(TCHAR C:Step)if(C<'0'||C>'9'){E=TEXT("Preview stride must be a positive integer.");return false;}
    if(!LexTryParseString(N,*Step)||N<1||N>1024){E=TEXT("Preview stride must be 1–1024.");return false;}R.PreviewStride=int32(N);
    if(!Crop.TrimStartAndEnd().IsEmpty())
    {
        TArray<FString> Axes;Crop.ParseIntoArray(Axes,TEXT(","),false);FIntVector Lo,Hi;if(Axes.Num()!=3){E=TEXT("Crop uses XSTART:XSTOP,YSTART:YSTOP,ZSTART:ZSTOP.");return false;}
        for(int32 I=0;I<3;++I){TArray<FString> Ends;Axes[I].ParseIntoArray(Ends,TEXT(":"),false);if(Ends.Num()!=2){E=TEXT("Crop uses half-open original XYZ integer pairs.");return false;}
            for(int32 K=0;K<2;++K){if(Ends[K].IsEmpty()||Ends[K].Len()>10){E=TEXT("Crop indices must be nonnegative integers.");return false;}for(TCHAR C:Ends[K])if(C<'0'||C>'9'){E=TEXT("Crop indices must be nonnegative integers.");return false;}int64 V;if(!LexTryParseString(V,*Ends[K])||V>MAX_int32){E=TEXT("Crop index exceeds bounds.");return false;}(K?Hi:Lo)[I]=int32(V);}}
        R.CropMinimum=Lo;R.CropMaximum=Hi;
    }
    for(const auto& Inspection:Inspections)R.Sources.Add(Inspection.Source);R.Title=Title;R.SourceURI=URI;R.Attribution=Attribution;R.FolderName=Folder;R.bDerivatives=bDerivatives;R.PressureConvention=bWBPressure?TEXT("wb_lattice"):TEXT("");
    return R.Mapping.Validate(E);
}
void SStudioHome4ArchivePanel::Start(bool VTI)
{
    if(Task.IsBusy()||PendingInspection.IsValid())return;auto M=Model.Pin();if(!M)return;
    if(M->IsProjectOpenPending()||M->IsRecordingLoadPending()){Notice=TEXT("Wait for the pending project/recording change before starting an archive operation.");return;}
    FStudioHome4ArchiveRequest R;FString E;if(!Request(R,E)){Notice=E;return;}R.Output=VTI?EStudioHome4ArchiveOutput::VTI:EStudioHome4ArchiveOutput::Recording;
    bool Selected=false;
#if WITH_DEV_AUTOMATION_TESTS
    if(bAutomationFileDialogs){if(NextOutputParent.IsSet()){R.OutputParent=MoveTemp(*NextOutputParent);NextOutputParent.Reset();Selected=!R.OutputParent.IsEmpty();}}
    else
#endif
    Selected=StudioFileDialog::ExportFolder(R.OutputParent);
    if(!Selected){Notice=TEXT("Output folder selection cancelled.");return;}
    ScopeProject=M->Project.Id;ScopeSource=M->Solver;CompletedRecording.Empty();if(!Task.Start(MoveTemp(R),E))Notice=E;else Notice=TEXT("Converting verified original source snapshots…");
}
void SStudioHome4ArchivePanel::OpenCompleted()
{auto M=Model.Pin();if(M&&!CompletedRecording.IsEmpty()&&!M->IsProjectOpenPending()&&!M->IsRecordingLoadPending()){bImportPending=M->RequestExternalRecording(CompletedRecording);Notice=M->Notice;}}
FString SStudioHome4ArchivePanel::Status()const
{
    if(Task.IsBusy()){const auto P=Task.Progress();const TCHAR* Phase=P.State==EStudioHome4ArchiveState::Publishing?TEXT("Publishing complete output"):P.State==EStudioHome4ArchiveState::Cancelled?TEXT("Cancelling"):TEXT("Converting original snapshots");return FString::Printf(TEXT("%s · %d/%d frames · %.1f MiB staged"),Phase,P.CompletedFrames,P.TotalFrames,P.Bytes/(1024.*1024.));}
    if(bImportPending){auto M=Model.Pin();return M?M->Notice:Notice;}return Notice;
}
void SStudioHome4ArchivePanel::Tick(const FGeometry& G,double T,float D)
{
    SCompoundWidget::Tick(G,T,D);
    if(PendingInspection.IsValid()&&PendingInspection.IsReady())
    {
        auto R=PendingInspection.Get();PendingInspection={};const bool Cancelled=InspectionCancel&&InspectionCancel->load();InspectionCancel.Reset();
        if(Cancelled||R.bCancelled)Notice=TEXT("Archive inspection cancelled.");else if(!R.Error.IsEmpty())Notice=R.Error;
        else
        {
            if(Inspections.IsEmpty()&&R.OriginalRunSpec)
            {FStudioHome4ArchiveMapping M;FString E;if(StudioHome4Archives::MappingFromMetadata(*R.OriginalRunSpec,M,E)){AxisOrder=M.AxisOrder;MetadataOrder=M.MetadataOrder;CoordinateUnits=M.CoordinateUnits;VelocityUnits=M.VelocityUnits;Dx=M.DxMeters?FString::Printf(TEXT("%.17g"),M.DxMeters.GetValue()):TEXT("");Dt=M.DtSeconds?FString::Printf(TEXT("%.17g"),M.DtSeconds.GetValue()):TEXT("");Density=M.DensityReferenceKgM3?FString::Printf(TEXT("%.17g"),M.DensityReferenceKgM3.GetValue()):TEXT("");Liquid=FString::Printf(TEXT("%.17g"),M.LiquidMinimum);TimeOrigin=FString::Printf(TEXT("%.17g"),M.TimeOriginSeconds);}}
            Inspections.Add(MoveTemp(R));RebuildSources();Notice=TEXT("Archive verified. Confirm original conventions; add snapshots in increasing solver-step order.");
        }
    }
    if(const auto R=Task.Poll())
    {
        if(!R->bSuccess)Notice=R->Error;
        else
        {CompletedRecording=R->RecordingJSON;Notice=FString::Printf(TEXT("Saved %d original frames: %s"),R->Frames,*R->Path);auto M=Model.Pin();if(M&&M->Project.Id==ScopeProject&&M->Solver==ScopeSource.Pin()&&!M->IsProjectOpenPending()&&!M->IsRecordingLoadPending()&&!CompletedRecording.IsEmpty())OpenCompleted();else if(!CompletedRecording.IsEmpty())Notice+=TEXT(" · Project/source changed. Use Open completed recording when ready.");}
    }
    if(bImportPending){auto M=Model.Pin();if(!M||!M->IsRecordingLoadPending()){bImportPending=false;if(M)Notice=M->Notice;}}
}
