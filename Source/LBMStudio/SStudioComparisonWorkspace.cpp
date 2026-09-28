/*
THESIS: Compare two original CFD frames under an explicit time interpretation.
OWN-WORLD: Existing blue-black Slate workspace, cyan selection, compact CoreStyle type.
STORY: Select output, align its original times, then inspect each flow from any camera.
FIRST VIEWPORT: Two equal flow views beneath source and alignment controls; each retains its own identity and camera.
FORM: Local Results extension; the sidebar remains the only workspace navigator.
FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, and DESIGN.md
*/
#include "SStudioComparisonWorkspace.h"
#include "StudioSnapshotSource.h"
#include "StudioScene.h"
#include "StudioFlowViewport.h"
#include "StudioMenuButton.h"
#include "StudioTheme.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/World.h"
#include "Async/Async.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"

namespace
{
TSharedRef<STextBlock> ComparisonText(TFunction<FString()> Value,int32 Size=10,FLinearColor Color=StudioUI::Text)
{return SNew(STextBlock).Text_Lambda([Value]{return FText::FromString(Value());}).Font(StudioUI::Font(Size)).ColorAndOpacity(Color).AutoWrapText(true);}
TSharedRef<SButton> ComparisonButton(FName Tag,const FString& Caption,TFunction<void()> Action)
{return SNew(SButton).Tag(Tag).ButtonStyle(&StudioUI::ButtonStyle()).ContentPadding(FMargin(10,6))
    .OnClicked_Lambda([Action]{Action();return FReply::Handled();})[StudioUI::Label(Caption)];}
TSharedRef<SWidget> ComparisonMenu(FName Tag,TFunction<FString()> Caption,TFunction<TSharedRef<SWidget>()> Build)
{return SNew(SStudioMenuButton).Tag(Tag).ButtonStyle(&StudioUI::ButtonStyle()).ContentPadding(FMargin(10,6))
    .OnGetMenuContent_Lambda([Build]{return Build();}).ButtonContent()[ComparisonText(Caption)];}
TSharedRef<SWidget> ComparisonChoices(const TSharedRef<SVerticalBox>& Rows)
{return SNew(SBorder).BorderImage(&StudioUI::PanelBrush).Padding(8)
    [SNew(SBox).WidthOverride(410).MaxDesiredHeight(360)[SNew(SScrollBox)
        .ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)+SScrollBox::Slot()[Rows]]];}
void ComparisonChoice(const TSharedRef<SVerticalBox>& Rows,FName Tag,const FString& Caption,TFunction<void()> Action)
{auto Button=ComparisonButton(Tag,Caption,[Action]{FSlateApplication::Get().DismissAllMenus();Action();});
    Button->SetContent(ComparisonText([Caption]{return Caption;}));Rows->AddSlot().AutoHeight().Padding(0,2)[Button];}
FString AlignmentLabel(EStudioTimeAlignment Mode)
{
    switch(Mode){case EStudioTimeAlignment::RecordedTime:return TEXT("Recorded timestamps");
    case EStudioTimeAlignment::ElapsedFromStart:return TEXT("Elapsed from each start");
    case EStudioTimeAlignment::ManualOffset:return TEXT("Manual B offset");default:return TEXT("Choose time alignment…");}
}
bool KnownComparisonUnit(const FString& Unit)
{return !Unit.TrimStartAndEnd().IsEmpty()&&!Unit.Equals(TEXT("unknown"),ESearchCase::IgnoreCase)&&!Unit.Equals(TEXT("unspecified"),ESearchCase::IgnoreCase);}

class SComparisonScale final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SComparisonScale){} SLATE_ARGUMENT(AStudioScene*,Scene) SLATE_END_ARGS()
    void Construct(const FArguments& A){Scene=A._Scene;}
    FVector2D ComputeDesiredSize(float) const override{return FVector2D(180,8);}
    int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle&,bool) const override
    {
        if(!Scene.IsValid())return Layer;
        const auto& Map=Scene->PresentedColorMapping();const auto Size=G.GetLocalSize();
        for(int32 I=0;I<64;++I)FSlateDrawElement::MakeBox(Out,Layer,G.ToPaintGeometry(FVector2D(Size.X/64.+1,Size.Y),
            FSlateLayoutTransform(FVector2D(Size.X*I/64.,0))),FCoreStyle::Get().GetBrush("WhiteBrush"),ESlateDrawEffect::None,
            StudioColor::Map(FMath::Lerp(Map.Minimum,Map.Maximum,double(I)/63),Map));
        return Layer;
    }
private:TWeakObjectPtr<AStudioScene> Scene;
};
}

bool SStudioComparisonWorkspace::Busy() const{return PendingSource.IsValid()||Task.IsBusy()||RestoreTask.IsBusy();}
bool SStudioComparisonWorkspace::Visible() const{return M&&M->Project.Id==ProjectId&&M->Workspace==EStudioWorkspace::Results;}
bool SStudioComparisonWorkspace::Current() const
{return Pair.IsSet()&&Pair->Matches(Request())&&Scenes[0].IsValid()&&Scenes[1].IsValid()&&Scenes[0]->HasCurrentFrame()&&Scenes[1]->HasCurrentFrame();}
FStudioComparisonRequest SStudioComparisonWorkspace::Request() const
{return {M->Project.Id,Sources[0],Sources[1],Ordinal,Scalar,Alignment};}

void SStudioComparisonWorkspace::Construct(const FArguments& Args)
{
    using namespace StudioUI;
    M=Args._Model;World=Args._World;Recordings=Args._Recordings;Back=Args._OnBack;ProjectId=M->Project.Id;
    Sources[0]=M->Solver;Ordinal=M->SelectedFrame;Scalar=M->ActiveScalar().Id;
    Notice=TEXT("Choose a second recording and a time alignment.");
    auto Controls=SNew(SVerticalBox).IsEnabled_Lambda([this]{return !Busy()&&!M->IsProjectOpenPending();});
    auto SourcesRow=SNew(SHorizontalBox);
    for(int32 Side=0;Side<2;++Side)SourcesRow->AddSlot().FillWidth(1).Padding(Side?8:0,0,Side?0:8,0)
        [SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[Label(Side?TEXT("Recording B"):TEXT("Recording A"),9,Muted)]
            +SVerticalBox::Slot().AutoHeight()[ComparisonMenu(Side?TEXT("CompareSourceB"):TEXT("CompareSourceA"),
                [this,Side]{return Sources[Side]?Sources[Side]->Descriptor().Title:TEXT("Choose recording…");},[this,Side]{return SourceMenu(Side);})]];
    Controls->AddSlot().AutoHeight().Padding(0,0,0,12)[SourcesRow];
    auto AlignmentRow=SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,12,0)[SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[Label(TEXT("Time alignment"),9,Muted)]
            +SVerticalBox::Slot().AutoHeight()[ComparisonMenu(TEXT("CompareAlignment"),[this]{return AlignmentLabel(Alignment.Mode);},[this]{return AlignmentMenu();})]]
        +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,12,0)[SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[Label(TEXT("Frame matching"),9,Muted)]
            +SVerticalBox::Slot().AutoHeight()[ComparisonMenu(TEXT("CompareMatch"),[this]{return Alignment.Match==EStudioTimeMatch::Exact?TEXT("Exact timestamp"):TEXT("Nearest within tolerance");},[this]{return MatchMenu();})]]
        +SHorizontalBox::Slot().FillWidth(1)[SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[Label(TEXT("Supplied scalar"),9,Muted)]
            +SVerticalBox::Slot().AutoHeight()[ComparisonMenu(TEXT("CompareScalar"),[this]
                {if(Sources[0])if(const auto* S=Sources[0]->Descriptor().Scalars.FindByPredicate([this](const auto& F){return F.Id==Scalar;}))return S->Label+TEXT(" (")+S->Unit+TEXT(")");
                    return FString(TEXT("Choose common scalar…"));},[this]{return ScalarMenu();})]];
    Controls->AddSlot().AutoHeight().Padding(0,0,0,10)[AlignmentRow];
    Controls->AddSlot().AutoHeight().Padding(0,0,0,10)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)[Label(TEXT("Frame A"),10)]
        +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(96)[SNew(SNumericEntryBox<int32>).Tag(TEXT("CompareFrame"))
            .EditableTextBoxStyle(&InputStyle()).Font(Font(10)).MinValue(1).MaxValue_Lambda([this]{return Sources[0]?Sources[0]->FrameCount():1;})
            .Value_Lambda([this]{return TOptional<int32>(Ordinal+1);}).OnValueCommitted_Lambda([this](int32 V,ETextCommit::Type)
                {if(Sources[0]&&V>=1&&V<=Sources[0]->FrameCount()&&Ordinal!=V-1){InvalidatePair();Ordinal=V-1;}})]]
        +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(8,0)[ComparisonText([this]{return Sources[0]?FString::Printf(TEXT("of %d · step %d · %.9g s"),Sources[0]->FrameCount(),Sources[0]->EvaluateFrame(Ordinal).Index,Sources[0]->EvaluateFrame(Ordinal).Time):TEXT("Select recording A");},9,Muted)]
        +SHorizontalBox::Slot().AutoWidth().Padding(10,0)[SNew(SHorizontalBox).Visibility_Lambda([this]{return Alignment.Mode==EStudioTimeAlignment::ManualOffset?EVisibility::Visible:EVisibility::Collapsed;})
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)[Label(TEXT("B offset (s)"),9)]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(116)[SNew(SNumericEntryBox<double>).Tag(TEXT("CompareOffset"))
                .EditableTextBoxStyle(&InputStyle()).Font(Font(10)).MaxFractionalDigits(12).Value_Lambda([this]{return TOptional<double>(Alignment.SecondaryOffsetSeconds);})
                .OnValueCommitted_Lambda([this](double V,ETextCommit::Type){if(V!=Alignment.SecondaryOffsetSeconds){InvalidatePair();Alignment.SecondaryOffsetSeconds=V;}})]]]
        +SHorizontalBox::Slot().AutoWidth()[SNew(SHorizontalBox).Visibility_Lambda([this]{return Alignment.Match==EStudioTimeMatch::Nearest?EVisibility::Visible:EVisibility::Collapsed;})
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)[Label(TEXT("Tolerance (s)"),9)]
            +SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(116)[SNew(SNumericEntryBox<double>).Tag(TEXT("CompareTolerance"))
                .EditableTextBoxStyle(&InputStyle()).Font(Font(10)).MaxFractionalDigits(12).MinValue(0).Value_Lambda([this]{return TOptional<double>(Alignment.MaximumMismatchSeconds);})
                .OnValueCommitted_Lambda([this](double V,ETextCommit::Type){if(V!=Alignment.MaximumMismatchSeconds){InvalidatePair();Alignment.MaximumMismatchSeconds=V;}})]]]];
    auto Apply=ComparisonButton(TEXT("CompareApply"),TEXT("Compare frames"),[this]{Compare();});
    Apply->SetEnabled(TAttribute<bool>::CreateLambda([this]{return !Busy()&&Sources[0]&&Sources[1]&&!Scalar.IsEmpty()&&Alignment.Mode!=EStudioTimeAlignment::Unset&&!M->IsProjectOpenPending();}));
    auto CancelButton=ComparisonButton(TEXT("CompareCancel"),TEXT("Cancel read"),[this]{Cancel();});
    CancelButton->SetVisibility(TAttribute<EVisibility>::CreateLambda([this]{return Busy()?EVisibility::Visible:EVisibility::Collapsed;}));
    auto Save=ComparisonMenu(TEXT("CompareSaveMenu"),[]{return TEXT("Save comparison…");},[this]{return SaveMenu();});
    Save->SetEnabled(TAttribute<bool>::CreateLambda([this]{return Current()&&!Busy()&&!M->IsProjectOpenPending();}));
    auto Saved=ComparisonMenu(TEXT("CompareSavedMenu"),[this]{return FString::Printf(TEXT("Saved comparisons (%d)…"),M->Project.Comparisons.Num());},[this]{return SavedMenu();});
    Saved->SetEnabled(TAttribute<bool>::CreateLambda([this]{return !Busy()&&!M->IsProjectOpenPending();}));
    ChildSlot[SNew(SBorder).BorderImage(&PanelBrush).Padding(20)[SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,14)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1)[Label(TEXT("Compare recordings"),18,Text,true)]
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)[Save]
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)[Saved]
            +SHorizontalBox::Slot().AutoWidth()[ComparisonButton(TEXT("CompareBack"),TEXT("Back to Results"),[Action=Back]{Action.ExecuteIfBound();})]]
        +SVerticalBox::Slot().AutoHeight()[Controls]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth()[Apply]
            +SHorizontalBox::Slot().AutoWidth().Padding(8,0)[CancelButton]
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12,0)[SNew(SCheckBox).Tag(TEXT("CompareCommonRange"))
                .IsEnabled_Lambda([this]{return !Busy();})
                .IsChecked_Lambda([this]{return bCommonRange?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                .OnCheckStateChanged_Lambda([this](ECheckBoxState S){bCommonRange=S==ECheckBoxState::Checked;SetRanges();})[Label(TEXT("Shared color range"),10)]]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[SNew(STextBlock).Tag(TEXT("CompareNotice")).Font(Font(10)).AutoWrapText(true)
            .ColorAndOpacity_Lambda([this]{return FSlateColor(bError?Amber:Muted);}).Text_Lambda([this]
                {if(Pair.IsSet()&&!Current())
                    {for(int32 I=0;I<2;++I)if(Scenes[I].IsValid()&&Scenes[I]->Model&&!Scenes[I]->Model->Notice.IsEmpty())
                        return FText::FromString(FString(I?TEXT("B: "):TEXT("A: "))+Scenes[I]->Model->Notice);
                    return FText::FromString(TEXT("Rendering both original frames…"));}
                return FText::FromString(Notice);})]
        +SVerticalBox::Slot().FillHeight(1)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,8,0)[SAssignNew(Views[0],SBox)]
            +SHorizontalBox::Slot().FillWidth(1).Padding(8,0,0,0)[SAssignNew(Views[1],SBox)]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,10,0,0)[ComparisonText([this]
            {return TEXT("Drag to orbit · middle drag to pan · right drag + WASDQE to fly · wheel to zoom · F to fit. Toolbar playback and export use ")+M->Solver->Descriptor().Title+TEXT(" in Solve.");},9,Muted)]]];
}

SStudioComparisonWorkspace::~SStudioComparisonWorkspace()
{Cancel();Task.Shutdown();RestoreTask.Shutdown();if(PendingSource.IsValid()){PendingSource.Wait();PendingSource={};}CloseViews();}
void SStudioComparisonWorkspace::CloseViews()
{
    for(int32 I=0;I<2;++I)
    {
        if(Views[I])Views[I]->SetContent(SNullWidget::NullWidget);
        if(Scenes[I].IsValid()){if(Scenes[I]->Model)Cameras[I]=Scenes[I]->SavedCameraState();Scenes[I]->Destroy();}
        Scenes[I].Reset();
    }
    Pair.Reset();
}
void SStudioComparisonWorkspace::InvalidatePair()
{CloseViews();bError=false;Notice=TEXT("Settings changed. Compare frames to inspect the new pair.");}
void SStudioComparisonWorkspace::Cancel()
{Task.Cancel();RestoreTask.Cancel();if(SourceCancellation)SourceCancellation->store(true);}
void SStudioComparisonWorkspace::OpenSource(int32 Side,const FString& Id)
{
    if(Busy()||!Visible())return;InvalidatePair();LoadingSide=Side;
    SourceCancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    Notice=TEXT("Verifying original recording…");bError=false;
    PendingSource=Async(EAsyncExecution::ThreadPool,[Id,Refs=M->Project.Recordings,Cancel=SourceCancellation]{return StudioRecordings::Open(Id,Refs,0,Cancel);});
}
void SStudioComparisonWorkspace::RefreshScalar()
{
    if(!Sources[0]||!Sources[1])return;
    const auto* A=Sources[0]->Descriptor().Scalars.FindByPredicate([this](const auto& S){return S.Id==Scalar;});
    const auto* B=Sources[1]->Descriptor().Scalars.FindByPredicate([this](const auto& S){return S.Id==Scalar;});
    if(!A||!B||A->Unit!=B->Unit||!KnownComparisonUnit(A->Unit))Scalar.Empty();
}
void SStudioComparisonWorkspace::Tick(const FGeometry& G,double Time,float Delta)
{
    SCompoundWidget::Tick(G,Time,Delta);
    if(ProjectId!=M->Project.Id)
    {
        Cancel();CloseViews();Sources[0].Reset();Sources[1].Reset();Cameras[0].Reset();Cameras[1].Reset();
        Notice=TEXT("The project changed. Return to recordings to start a comparison.");bError=true;return;
    }
    if(PendingSource.IsValid()&&PendingSource.IsReady())
    {
        auto R=PendingSource.Consume();const bool Cancelled=SourceCancellation->load();SourceCancellation.Reset();
        if(Cancelled){Notice=TEXT("Recording read cancelled. Previous selection retained.");bError=false;}
        else if(!R.Source){Notice=R.Error;bError=true;}
        else{Sources[LoadingSide]=MoveTemp(R.Source);SourceReferences[LoadingSide]=MoveTemp(R.Reference);Cameras[LoadingSide].Reset();if(!LoadingSide)Ordinal=0;RefreshScalar();Notice=TEXT("Choose an alignment and compare the original frames.");bError=false;}
        LoadingSide=INDEX_NONE;
    }
    if(auto Result=Task.Poll())
    {
        if(Result->Matches(Request()))Present(MoveTemp(*Result));
        else
        {
            CloseViews();Notice=Result->Frames.Error.IsEmpty()?TEXT("Comparison selection changed. Compare again."):Result->Frames.Error;
            if(Result->Frames.Status==EStudioComparisonStatus::NoMatch)
                Notice+=TEXT("\nChoose a compatible Frame A, recording or time alignment, then select Compare frames again.");
            bError=Result->Frames.Status!=EStudioComparisonStatus::Cancelled;
        }
    }
    if(auto Restored=RestoreTask.Poll())
    {
        const auto* Saved=M->FindComparison(Restored->Saved.Id);
        if(!M->IsProjectOpenPending()&&Saved&&Restored->Matches(M->Project.Id,*Saved))
        {
            CloseViews();Sources[0]=MoveTemp(Restored->PrimarySource);Sources[1]=MoveTemp(Restored->SecondarySource);
            SourceReferences[0]=Restored->Saved.Primary.Reference;SourceReferences[1]=Restored->Saved.Secondary.Reference;
            Cameras[0]=Restored->Saved.Primary.Camera;Cameras[1]=Restored->Saved.Secondary.Camera;
            Ordinal=Restored->Saved.Primary.Identity.Ordinal;Scalar=Restored->Saved.Scalar;Alignment=Restored->Saved.Alignment;bCommonRange=Restored->Saved.bSharedRange;
            Present(MoveTemp(*Restored->Pair));Notice=TEXT("Opened ")+Restored->Saved.Name+TEXT(". ")+Notice;
        }
        else
        {Notice=Restored->Error.IsEmpty()?TEXT("The project or saved comparison changed while opening. Current comparison kept; open the saved comparison again."):Restored->Error;bError=!Restored->bCancelled;}
    }
}
void SStudioComparisonWorkspace::Compare()
{
    if(Busy()||!Visible())return;CloseViews();FString Error;
    if(!Task.Start(Request(),Error)){Notice=Error;bError=true;return;}
    Notice=TEXT("Reading both original scalar snapshots…");bError=false;
}
void SStudioComparisonWorkspace::Present(FStudioComparisonResult Result)
{
    CloseViews();if(!World.IsValid()||!Result.Matches(Request()))return;
    Pair=MoveTemp(Result);const TWeakPtr<SStudioComparisonWorkspace> Weak=SharedThis(this);
    for(int32 I=0;I<2;++I)
    {
        const auto Source=I?Pair->Secondary.Snapshot:Pair->Primary.Snapshot;
        auto Model=MakeShared<FStudioModel>(Source.ToSharedRef());if(Cameras[I].IsSet())Model->EditCamera(TEXT("Retain comparison camera"),*Cameras[I]);
        auto* Scene=World->SpawnActor<AStudioScene>();
        if(!Scene){CloseViews();Notice=TEXT("The comparison views could not be created. Compare again.");bError=true;return;}
        Scenes[I]=Scene;Scene->Tags.Add(I?TEXT("StudioComparisonB"):TEXT("StudioComparisonA"));
        Scene->SetViewVisibility([Weak]{const auto W=Weak.Pin();return W&&W->Visible();});Scene->Initialize(Model);
    }
    SetRanges();for(int32 I=0;I<2;++I)Views[I]->SetContent(View(I));
    const auto& F=Pair->Frames;
    Notice=FString::Printf(TEXT("%s · aligned A %.9g s / B %.9g s · B − A %+.9g s. Original frames; no temporal interpolation."),
        *AlignmentLabel(Alignment.Mode),F.PrimaryAlignedTime,F.SecondaryAlignedTime,F.MismatchSeconds);bError=false;
}
void SStudioComparisonWorkspace::SetRanges()
{
    if(!Pair.IsSet())return;
    const auto& A=Pair->Primary.Scalar;const auto& B=Pair->Secondary.Scalar;
    double Min=FMath::Min(A.Minimum,B.Minimum),Max=FMath::Max(A.Maximum,B.Maximum);
    if(Min==Max){const auto Range=StudioColor::Resolve(TEXT(""),A,{});Min=Range.Minimum;Max=Range.Maximum;}
    for(const auto& Scene:Scenes)if(Scene.IsValid())Scene->Model->SetScalarStyle(0,bCommonRange,Min,Max);
}

TSharedRef<SWidget> SStudioComparisonWorkspace::SourceMenu(int32 Side)
{
    auto Rows=SNew(SVerticalBox);
    for(const auto& R:Recordings)ComparisonChoice(Rows,FName(*(FString(Side?TEXT("CompareB_"):TEXT("CompareA_"))+R.Id)),R.Title+TEXT(" · ")+R.Id,[this,Side,Id=R.Id]{OpenSource(Side,Id);});
    return ComparisonChoices(Rows);
}
TSharedRef<SWidget> SStudioComparisonWorkspace::ScalarMenu()
{
    auto Rows=SNew(SVerticalBox);int32 Count=0;
    if(Sources[0]&&Sources[1])for(const auto& A:Sources[0]->Descriptor().Scalars)
        if(KnownComparisonUnit(A.Unit)&&Sources[1]->Descriptor().Scalars.ContainsByPredicate([&](const auto& B){return A.Id==B.Id&&A.Unit==B.Unit;}))
        {++Count;ComparisonChoice(Rows,FName(*(TEXT("CompareScalar_")+A.Id)),A.Label+TEXT(" (")+A.Unit+TEXT(")"),[this,Id=A.Id]{InvalidatePair();Scalar=Id;});}
    if(!Count)Rows->AddSlot().AutoHeight()[StudioUI::Label(TEXT("Choose two recordings with a common scalar and known units."),10)];
    return ComparisonChoices(Rows);
}
TSharedRef<SWidget> SStudioComparisonWorkspace::AlignmentMenu()
{
    auto Rows=SNew(SVerticalBox);
    for(const auto Mode:{EStudioTimeAlignment::RecordedTime,EStudioTimeAlignment::ElapsedFromStart,EStudioTimeAlignment::ManualOffset})
        ComparisonChoice(Rows,FName(*FString::Printf(TEXT("CompareAlignment%d"),int32(Mode))),AlignmentLabel(Mode),[this,Mode]
            {InvalidatePair();Alignment.Mode=Mode;if(Mode!=EStudioTimeAlignment::ManualOffset)Alignment.SecondaryOffsetSeconds=0;});
    Rows->AddSlot().AutoHeight().Padding(0,8)[ComparisonText([]{return TEXT("Recorded timestamps compares the supplied numbers; it does not establish a shared physical clock. Manual offset: B recorded time + offset = A recorded time.");},9,StudioUI::Muted)];
    return ComparisonChoices(Rows);
}
TSharedRef<SWidget> SStudioComparisonWorkspace::MatchMenu()
{
    auto Rows=SNew(SVerticalBox);
    for(const bool Nearest:{false,true})ComparisonChoice(Rows,Nearest?TEXT("CompareNearest"):TEXT("CompareExact"),Nearest?TEXT("Nearest within tolerance"):TEXT("Exact timestamp"),[this,Nearest]
        {InvalidatePair();Alignment.Match=Nearest?EStudioTimeMatch::Nearest:EStudioTimeMatch::Exact;if(!Nearest)Alignment.MaximumMismatchSeconds=0;});
    Rows->AddSlot().AutoHeight().Padding(0,8)[ComparisonText([]{return TEXT("Nearest chooses an original frame within the tolerance. No extrapolation.");},9,StudioUI::Muted)];
    return ComparisonChoices(Rows);
}

TSharedRef<SWidget> SStudioComparisonWorkspace::View(int32 Side)
{
    using namespace StudioUI;auto* Scene=Scenes[Side].Get();const auto Data=Side?Pair->Secondary:Pair->Primary;
    const TCHAR* Letter=Side?TEXT("B"):TEXT("A");const int32 Index=Data.Identity.Ordinal;
    return SNew(SVerticalBox).IsEnabled_Lambda([this]{return !RestoreTask.IsBusy();})
        .Visibility_Lambda([this]{return Current()?EVisibility::Visible:EVisibility::Hidden;})
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,5)[SNew(SBox).MinDesiredHeight(36)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(0,0,8,0)[SNew(STextBlock).Font(Font(11,true)).ColorAndOpacity(Text).AutoWrapText(true).Text(FText::FromString(FString(Letter)+TEXT(" · ")+Data.Title))]
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ComparisonButton(Side?TEXT("CompareFitB"):TEXT("CompareFitA"),TEXT("Fit"),[this,Side]{if(Scenes[Side].IsValid())Scenes[Side]->FitCamera();})]
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(5,0,0,0)[ComparisonMenu(Side?TEXT("CompareCameraB"):TEXT("CompareCameraA"),[]{return TEXT("Camera…");},[this,Side]{return CameraMenu(Side);})]]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,6)[Label(FString::Printf(TEXT("Frame %d · source step %d · %.9g s · %dD"),Index+1,Data.Identity.Frame.Index,Data.Identity.Frame.Time,Data.Identity.SpatialDimensions),9,Muted)]
        +SVerticalBox::Slot().FillHeight(1)[MakeStudioFlowViewport(Scene,Side?TEXT("ComparisonViewportB"):TEXT("ComparisonViewportA"))]
        +SVerticalBox::Slot().AutoHeight().Padding(0,7,0,5)[SNew(SComparisonScale).Scene(Scene)]
        +SVerticalBox::Slot().AutoHeight()[ComparisonText([this,Side,Unit=Data.Scalar.Unit,Label=Data.Scalar.Label]
            {if(!Scenes[Side].IsValid())return FString();const auto& C=Scenes[Side]->PresentedColorMapping();return FString::Printf(TEXT("%s · %.6g to %.6g %s"),*Label,C.Minimum,C.Maximum,*Unit);},9)]
        +SVerticalBox::Slot().AutoHeight().Padding(0,4,0,0)[Label(FString::Printf(TEXT("Source range %.6g to %.6g %s · %s"),Data.Scalar.Minimum,Data.Scalar.Maximum,*Data.Scalar.Unit,
            Data.Identity.Interpolation==EStudioFieldInterpolation::None?TEXT("Original points"):
            Data.Identity.Interpolation==EStudioFieldInterpolation::SourceTriangles?TEXT("Source triangles"):TEXT("Explicit display reconstruction")),9,Muted)];
}
TSharedRef<SWidget> SStudioComparisonWorkspace::CameraMenu(int32 Side)
{
    using namespace StudioUI;auto Rows=SNew(SVerticalBox);
    for(int32 Axis=0;Axis<6;++Axis)
    {
        const TCHAR* Names[]={TEXT("X (m)"),TEXT("Y (m)"),TEXT("Z (m)"),TEXT("Pitch (°)"),TEXT("Yaw (°)"),TEXT("Roll (°)")};
        Rows->AddSlot().AutoHeight().Padding(0,4)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SBox).WidthOverride(90)[Label(Names[Axis],10)]]
            +SHorizontalBox::Slot().FillWidth(1)[SNew(SNumericEntryBox<double>).Tag(FName(*FString::Printf(TEXT("CompareCamera%dAxis%d"),Side,Axis)))
                .EditableTextBoxStyle(&InputStyle()).Font(Font(10)).MaxFractionalDigits(12).Value_Lambda([this,Side,Axis]() -> TOptional<double>
                {if(!Scenes[Side].IsValid())return {};const auto* S=Scenes[Side].Get();if(Axis<3)return S->CameraPosition()[Axis];const auto R=S->CameraRotation();return Axis==3?R.Pitch:Axis==4?R.Yaw:R.Roll;})
                .OnValueCommitted_Lambda([this,Side,Axis](double V,ETextCommit::Type)
                {if(!Scenes[Side].IsValid()||!FMath::IsFinite(V))return;auto* S=Scenes[Side].Get();if(Axis<3){auto P=S->CameraPosition();P[Axis]=V;S->SetCameraPosition(P);}
                    else{auto R=S->CameraRotation();if(Axis==3)R.Pitch=V;else if(Axis==4)R.Yaw=V;else R.Roll=V;S->SetCameraRotation(R);}})]];
    }
    Rows->AddSlot().AutoHeight().Padding(0,8)[SNew(SCheckBox).Tag(Side?TEXT("CompareOrthoB"):TEXT("CompareOrthoA"))
        .IsChecked_Lambda([this,Side]{return Scenes[Side].IsValid()&&Scenes[Side]->SavedCameraState().bOrthographic?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
        .OnCheckStateChanged_Lambda([this,Side](ECheckBoxState State){if(Scenes[Side].IsValid())
            {auto C=Scenes[Side]->SavedCameraState();C.bOrthographic=State==ECheckBoxState::Checked;
                if(C.bOrthographic)C.OrthoWidth=2*C.OrbitDistance*FMath::Tan(FMath::DegreesToRadians(C.FieldOfView*.5));
                Scenes[Side]->RestoreCamera(C,TEXT("Comparison projection"));}})
        [Label(TEXT("Orthographic projection"),10)]];
    return ComparisonChoices(Rows);
}

bool SStudioComparisonWorkspace::CaptureSaved(const FString& Name,FStudioSavedComparison& Out)
{
    if(!Current()||Busy()||M->IsProjectOpenPending())
    {EditNotice=TEXT("Wait for both original frames to appear before saving this comparison.");bEditError=true;return false;}
    auto References=M->Project.Recordings;for(const auto& Reference:SourceReferences)if(Reference.IsSet())References.Add(*Reference);
    const bool Good=StudioSavedComparisons::Create(Name,*Pair,Scenes[0]->SavedCameraState(),Scenes[1]->SavedCameraState(),bCommonRange,References,Out,EditNotice);
    bEditError=!Good;return Good;
}
void SStudioComparisonWorkspace::CollectionResult(bool Success)
{
    EditNotice=M->ComparisonNotice;bEditError=!Success;Notice=EditNotice;bError=bEditError;
    if(Success){FSlateApplication::Get().DismissAllMenus();FSlateApplication::Get().SetKeyboardFocus(SharedThis(this),EFocusCause::SetDirectly);}
}
void SStudioComparisonWorkspace::SaveCurrent()
{
    FStudioSavedComparison Saved;if(!CaptureSaved(SaveName,Saved))return;
    const bool Success=M->AddComparison(MoveTemp(Saved));if(Success)bSaveDraftStarted=false;CollectionResult(Success);
}
void SStudioComparisonWorkspace::UpdateSaved(const FGuid& Id)
{
    const auto* Existing=M->FindComparison(Id);
    if(!Existing){EditNotice=Notice=TEXT("This comparison is no longer saved. Save it as a new comparison.");bEditError=bError=true;return;}
    FStudioSavedComparison Saved;if(!CaptureSaved(Existing->Name,Saved))return;CollectionResult(M->UpdateComparison(Id,MoveTemp(Saved)));
}
void SStudioComparisonWorkspace::RenameSaved(const FGuid& Id)
{
    const bool Success=M->RenameComparison(Id,RenameDrafts.FindRef(Id));if(Success)RenameDrafts.Remove(Id);CollectionResult(Success);
}
void SStudioComparisonWorkspace::OpenSaved(const FGuid& Id)
{
    if(Busy()||!Visible()||M->IsProjectOpenPending())return;const auto* Saved=M->FindComparison(Id);
    if(!Saved){Notice=TEXT("This comparison is no longer saved. Choose another saved comparison.");bError=true;return;}
    if(!RestoreTask.Start(M->Project.Id,*Saved,M->Project.Recordings,Notice)){bError=true;return;}
    Notice=TEXT("Opening ")+Saved->Name+TEXT(". Current comparison stays visible until both original frames are verified.");bError=false;
}
TSharedRef<SWidget> SStudioComparisonWorkspace::SaveMenu()
{
    using namespace StudioUI;auto Rows=SNew(SVerticalBox);EditNotice.Empty();bEditError=false;
    if(!bSaveDraftStarted)
    {
        for(int32 I=1;I<=StudioSavedComparisons::MaxEntries+1;++I)
        {SaveName=FString::Printf(TEXT("Comparison %d"),I);if(!M->Project.Comparisons.ContainsByPredicate([this](const auto& S){return S.Name.Equals(SaveName,ESearchCase::IgnoreCase);}))break;}
        bSaveDraftStarted=true;
    }
    Rows->AddSlot().AutoHeight().Padding(0,0,0,6)[Label(TEXT("Comparison name"),10)];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SEditableTextBox).Tag(TEXT("CompareSaveName")).Style(&InputStyle()).Font(Font(10)).ClearKeyboardFocusOnCommit(false)
        .Text(FText::FromString(SaveName)).OnTextChanged_Lambda([this](const FText& T){SaveName=T.ToString();})
        .OnTextCommitted_Lambda([this](const FText&,ETextCommit::Type Commit){if(Commit==ETextCommit::OnEnter)SaveCurrent();})];
    Rows->AddSlot().AutoHeight()[ComparisonButton(TEXT("CompareSaveSubmit"),TEXT("Save comparison"),[this]{SaveCurrent();})];
    Rows->AddSlot().AutoHeight().Padding(0,8)[ComparisonText([]{return TEXT("Keeps these original frames, alignment, scalar, ranges and both cameras in the project. Use Save to write the project file.");},9,Muted)];
    Rows->AddSlot().AutoHeight()[ComparisonText([this]{return EditNotice;},10,Amber)];return ComparisonChoices(Rows);
}
TSharedRef<SWidget> SStudioComparisonWorkspace::RenameMenu(const FGuid& Id)
{
    using namespace StudioUI;auto Rows=SNew(SVerticalBox);EditNotice.Empty();bEditError=false;
    const auto* Saved=M->FindComparison(Id);if(!Saved)return ComparisonChoices(Rows);
    if(!RenameDrafts.Contains(Id))RenameDrafts.Add(Id,Saved->Name);
    Rows->AddSlot().AutoHeight().Padding(0,0,0,6)[Label(TEXT("Comparison name"),10)];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SEditableTextBox).Tag(TEXT("CompareRenameName")).Style(&InputStyle()).Font(Font(10)).ClearKeyboardFocusOnCommit(false)
        .Text(FText::FromString(RenameDrafts[Id])).OnTextChanged_Lambda([this,Id](const FText& T){RenameDrafts.Add(Id,T.ToString());})
        .OnTextCommitted_Lambda([this,Id](const FText&,ETextCommit::Type Commit){if(Commit==ETextCommit::OnEnter)RenameSaved(Id);})];
    Rows->AddSlot().AutoHeight()[ComparisonButton(TEXT("CompareRenameSubmit"),TEXT("Rename comparison"),[this,Id]{RenameSaved(Id);})];
    Rows->AddSlot().AutoHeight().Padding(0,8)[ComparisonText([this]{return EditNotice;},10,Amber)];return ComparisonChoices(Rows);
}
TSharedRef<SWidget> SStudioComparisonWorkspace::SavedMenu()
{
    using namespace StudioUI;auto Rows=SNew(SVerticalBox);EditNotice.Empty();bEditError=false;
    if(M->Project.Comparisons.IsEmpty())Rows->AddSlot().AutoHeight().Padding(0,0,0,10)
        [ComparisonText([]{return TEXT("No saved comparisons. Compare two original frames, then choose Save comparison.");},10,Muted)];
    for(const auto& Saved:M->Project.Comparisons)
    {
        const auto Id=Saved.Id;const FString Suffix=Id.ToString();
        Rows->AddSlot().AutoHeight().Padding(0,4,0,5)[SNew(STextBlock).Font(Font(11,true)).ColorAndOpacity(Text).AutoWrapText(true).Text(FText::FromString(Saved.Name))];
        Rows->AddSlot().AutoHeight()[ComparisonText([Saved]{return FString::Printf(TEXT("A · %s · frame %d\nB · %s · frame %d"),*Saved.Primary.Title,Saved.Primary.Identity.Ordinal+1,*Saved.Secondary.Title,Saved.Secondary.Identity.Ordinal+1);},9,Muted)];
        auto Update=ComparisonButton(FName(TEXT("CompareUpdate_")+Suffix),TEXT("Update"),[this,Id]{UpdateSaved(Id);});
        Update->SetToolTipText(FText::FromString(TEXT("Replace this saved comparison with the current original frames, alignment, ranges and cameras. Undo comparisons restores the previous setup.")));
        Update->SetEnabled(TAttribute<bool>::CreateLambda([this]{return Current()&&!Busy();}));
        Rows->AddSlot().AutoHeight().Padding(0,7,0,14)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[ComparisonButton(FName(TEXT("CompareOpen_")+Suffix),TEXT("Open"),[this,Id]{FSlateApplication::Get().DismissAllMenus();OpenSaved(Id);})]
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[Update]
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[ComparisonMenu(FName(TEXT("CompareRename_")+Suffix),[]{return TEXT("Rename…");},[this,Id]{return RenameMenu(Id);})]
            +SHorizontalBox::Slot().AutoWidth()[ComparisonButton(FName(TEXT("CompareDelete_")+Suffix),TEXT("Delete"),[this,Id]{CollectionResult(M->DeleteComparison(Id));})]];
    }
    auto Undo=ComparisonButton(TEXT("CompareUndo"),TEXT("Undo comparisons"),[this]{CollectionResult(M->UndoComparisons());});
    Undo->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->CanUndoComparisons();}));
    auto Redo=ComparisonButton(TEXT("CompareRedo"),TEXT("Redo"),[this]{CollectionResult(M->RedoComparisons());});
    Redo->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->CanRedoComparisons();}));
    Rows->AddSlot().AutoHeight()[SNew(SHorizontalBox)+SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[Undo]+SHorizontalBox::Slot().AutoWidth()[Redo]];
    Rows->AddSlot().AutoHeight().Padding(0,8)[ComparisonText([this]{return EditNotice;},10,Amber)];return ComparisonChoices(Rows);
}
