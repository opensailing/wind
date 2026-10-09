/*
THESIS: Build a reproducible analysis from one authentic CFD frame and inspect its output.
OWN-WORLD: Existing blue-black Slate panels, cyan selection, compact CoreStyle controls.
STORY: Choose a saved recipe, edit its ordered operations, evaluate, then place its camera.
FIRST VIEWPORT: A 300-unit operation editor on the left; the dominant flow view on the right, with source identity and numerical method adjacent. Evaluate stays in the header.
FORM: Local Operate extension. Sidebar-only workspace navigation; the existing project Save persists recipes and cameras.
FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, and DESIGN.md
*/
#include "SStudioPipelineWorkspace.h"
#include "StudioScene.h"
#include "StudioFlowViewport.h"
#include "StudioSavedFieldView.h"
#include "StudioSurfaceReconstruction.h"
#include "StudioVolume.h"
#include "StudioMenuButton.h"
#include "StudioTheme.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/World.h"
#include "Async/Async.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"

namespace StudioPipelineUI
{
TSharedRef<STextBlock> Text(TFunction<FString()> Value,int32 Size=10,FLinearColor Color=StudioUI::Text)
{return SNew(STextBlock).Text_Lambda([Value]{return FText::FromString(Value());}).Font(StudioUI::Font(Size)).ColorAndOpacity(Color).AutoWrapText(true);}
TSharedRef<STextBlock> Note(const FString& Value)
{return Text([Value]{return Value;},9,StudioUI::Muted);}
TSharedRef<SButton> Button(FName Tag,const FString& Caption,TFunction<void()> Action)
{return SNew(SButton).Tag(Tag).ButtonStyle(&StudioUI::ButtonStyle()).ContentPadding(FMargin(9,6))
    .OnClicked_Lambda([Action]{Action();return FReply::Handled();})[StudioUI::Label(Caption)];}
TSharedRef<SWidget> Menu(FName Tag,TFunction<FString()> Caption,TFunction<TSharedRef<SWidget>()> Build)
{return SNew(SStudioMenuButton).Tag(Tag).Method(EPopupMethod::UseCurrentWindow).ButtonStyle(&StudioUI::ButtonStyle()).ContentPadding(FMargin(9,6))
    .OnGetMenuContent_Lambda([Build]{return Build();}).ButtonContent()[Text(Caption)];}
TSharedRef<SWidget> Choices(const TSharedRef<SVerticalBox>& Rows,double Width=350)
{return SNew(SBorder).BorderImage(&StudioUI::PanelBrush).Padding(12)
    [SNew(SBox).WidthOverride(Width).MaxDesiredHeight(420)[SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)+SScrollBox::Slot()[Rows]]];}
void Choice(const TSharedRef<SVerticalBox>& Rows,FName Tag,const FString& Caption,TFunction<void()> Action)
{auto B=Button(Tag,Caption,[Action]{FSlateApplication::Get().DismissAllMenus();Action();});B->SetContent(Text([Caption]{return Caption;}));Rows->AddSlot().AutoHeight().Padding(0,2)[B];}
FString Kind(EStudioPipelineOperation K)
{switch(K){case EStudioPipelineOperation::Field:return TEXT("Scalar field");case EStudioPipelineOperation::Magnitude:return TEXT("Vector magnitude");
    case EStudioPipelineOperation::ClipBox:return TEXT("Box clip");case EStudioPipelineOperation::Slice:return TEXT("Plane slice");
    case EStudioPipelineOperation::Contour:return TEXT("Contour");case EStudioPipelineOperation::Probe:return TEXT("Probe");}return {};}
FString SampleStatus(EStudioProbeSampleStatus S)
{
    switch(S)
    {
    case EStudioProbeSampleStatus::Value:return TEXT("Value");
    case EStudioProbeSampleStatus::OutsideCoverage:return TEXT("Outside coverage");
    case EStudioProbeSampleStatus::OffPlane:return TEXT("Off source plane");
    case EStudioProbeSampleStatus::MissingPoint:return TEXT("Point unavailable");
    case EStudioProbeSampleStatus::FieldUnavailable:return TEXT("Field unavailable");
    case EStudioProbeSampleStatus::NoInterpolation:return TEXT("No interpolation");
    }
    return TEXT("Unavailable");
}
bool SameEvaluation(FStudioSavedPipeline A,FStudioSavedPipeline B)
{A.Name=B.Name;A.Source.Camera=B.Source.Camera;return StudioPipelines::Equals(A,B);}
FString Unique(const FString& Base,TFunction<bool(const FString&)> Exists)
{FString Name=Base;for(int32 I=2;Exists(Name);++I)Name=Base+FString::Printf(TEXT(" %d"),I);return Name;}
FStudioFieldIdentity Identity(const IStudioSolver& Source,int32 Ordinal)
{
    const auto& D=Source.Descriptor();FStudioFieldIdentity I;I.Dataset=D.Id;I.MetadataSHA256=D.MetadataSHA256;I.PayloadSHA256=D.PayloadSHA256;
    I.Ordinal=Ordinal;I.Frame=Source.EvaluateFrame(Ordinal);I.SpatialDimensions=D.SpatialDimensions;I.SourceOffset=D.SourceOffset;
    I.Interpolation=D.bSourcePoints?EStudioFieldInterpolation::None:EStudioFieldInterpolation::SourceTriangles;
    if(const auto R=Source.Reconstruction()){I.ReconstructionSHA256=R->MetadataSHA256;I.Interpolation=EStudioFieldInterpolation::ReconstructedTriangles;}
    if(const auto R=Source.VolumeReconstruction()){I.ReconstructionSHA256=R->ReconstructionIdentity();I.Interpolation=R->Interpolation();}return I;
}
class SScale final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SScale){} SLATE_ARGUMENT(AStudioScene*,Scene) SLATE_END_ARGS()
    void Construct(const FArguments& A){Scene=A._Scene;}
    FVector2D ComputeDesiredSize(float) const override{return FVector2D(180,8);}
    int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 L,const FWidgetStyle&,bool) const override
    {
        if(!Scene.IsValid())return L;const auto& M=Scene->PresentedColorMapping();const auto Size=G.GetLocalSize();
        for(int32 I=0;I<64;++I)
        {FSlateDrawElement::MakeBox(Out,L,G.ToPaintGeometry(FVector2D(Size.X/64.+1,Size.Y),FSlateLayoutTransform(FVector2D(Size.X*I/64.,0))),
            FCoreStyle::Get().GetBrush("WhiteBrush"),ESlateDrawEffect::None,StudioColor::Map(FMath::Lerp(M.Minimum,M.Maximum,double(I)/63),M));}
        return L;
    }
private:TWeakObjectPtr<AStudioScene> Scene;
};
}

const FStudioSavedPipeline* SStudioPipelineWorkspace::Selected() const {return M&&M->Project.Id==ProjectId?M->FindPipeline(SelectedId):nullptr;}
bool SStudioPipelineWorkspace::Visible() const {return M&&M->Project.Id==ProjectId&&M->Workspace==EStudioWorkspace::PostProcessing;}
bool SStudioPipelineWorkspace::Busy() const {return Pending.IsValid()||PendingSource.IsValid();}
bool SStudioPipelineWorkspace::CanEdit() const {return Visible()&&!Busy()&&!M->IsProjectOpenPending();}
bool SStudioPipelineWorkspace::Current() const
{return Selected()&&PresentedRecipe.IsSet()&&Evaluation.IsSet()&&StudioPipelineUI::SameEvaluation(*Selected(),*PresentedRecipe)&&
    (Evaluation->Output->Kind==EStudioPipelineOutputKind::ProbeTable||(Scene.IsValid()&&Scene->HasCurrentFrame()));}
void SStudioPipelineWorkspace::Error(const FString& Message){Notice=Message;bError=true;}

void SStudioPipelineWorkspace::Construct(const FArguments& Args)
{
    using namespace StudioPipelineUI;using StudioUI::Label;
    M=Args._Model;World=Args._World;Results=Args._OnResults;ProjectId=M->Project.Id;
    auto Controls=SNew(SHorizontalBox).IsEnabled_Lambda([this]{return CanEdit();});
    Controls->AddSlot().FillWidth(1).Padding(0,0,8,0)[Menu(TEXT("PipelineSaved"),[this]{return Selected()?Selected()->Name:TEXT("Choose saved pipeline…");},[this]{return SavedMenu();})];
    Controls->AddSlot().AutoWidth().Padding(0,0,8,0)[Menu(TEXT("PipelineNew"),[]{return TEXT("New pipeline…");},[this]{return NewMenu();})];
    auto Manage=Menu(TEXT("PipelineManage"),[]{return TEXT("Manage…");},[this]{return ManageMenu();});Manage->SetEnabled(TAttribute<bool>::CreateLambda([this]{return Selected()!=nullptr;}));
    Controls->AddSlot().AutoWidth().Padding(0,0,8,0)[Manage];
    auto Undo=Button(TEXT("PipelineUndo"),TEXT("Undo"),[this]{History(false);});Undo->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->CanUndoPipelines();}));
    auto Redo=Button(TEXT("PipelineRedo"),TEXT("Redo"),[this]{History(true);});Redo->SetEnabled(TAttribute<bool>::CreateLambda([this]{return M->CanRedoPipelines();}));
    Controls->AddSlot().AutoWidth().Padding(0,0,5,0)[Undo];Controls->AddSlot().AutoWidth()[Redo];
    auto Apply=Button(TEXT("PipelineEvaluate"),TEXT("Evaluate"),[this]{Evaluate();});
    Apply->SetEnabled(TAttribute<bool>::CreateLambda([this]{return CanEdit()&&Selected();}));Apply->SetContent(Label(TEXT("Evaluate"),10,StudioUI::Cyan,true));
    auto Stop=Button(TEXT("PipelineCancel"),TEXT("Cancel"),[this]{Cancel();});Stop->SetVisibility(TAttribute<EVisibility>::CreateLambda([this]{return Busy()?EVisibility::Visible:EVisibility::Collapsed;}));
    auto Left=SNew(SVerticalBox).IsEnabled_Lambda([this]{return CanEdit()&&Selected();});
    Left->AddSlot().AutoHeight().Padding(0,0,0,7)[Text([this]{return Selected()?Selected()->Source.Title:TEXT("Original recording");},11)];
    Left->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)[Label(TEXT("Frame"),9)]
        +SHorizontalBox::Slot().FillWidth(1)[SNew(SNumericEntryBox<int32>).Tag(TEXT("PipelineFrame")).Font(StudioUI::Font()).EditableTextBoxStyle(&StudioUI::InputStyle())
            .MinValue(1).MaxValue_Lambda([this]{return Source?Source->FrameCount():1;}).IsEnabled_Lambda([this]{return Source.IsValid();})
            .Value_Lambda([this]()->TOptional<int32>{return Selected()?Selected()->Source.Identity.Ordinal+1:1;})
            .OnValueCommitted_Lambda([this](int32 V,ETextCommit::Type){if(!CanEdit()||!Selected()||!Source||V<1||V>Source->FrameCount()||!ApplyOperation())return;
                SaveCamera();auto P=*Selected();P.Source.Identity=StudioPipelineUI::Identity(*Source,V-1);Update(MoveTemp(P));})]
        +SHorizontalBox::Slot().AutoWidth().Padding(7,0,0,0)[Menu(TEXT("PipelineSourceInfo"),[]{return TEXT("Source…");},[this]{return SourceMenu();})]];
    Left->AddSlot().AutoHeight().Padding(0,0,0,14)[Text([this]{if(!Selected())return FString();const auto& I=Selected()->Source.Identity;
        return FString::Printf(TEXT("%dD · source step %d · %.9g s"),I.SpatialDimensions,I.Frame.Index,I.Frame.Time);},9,StudioUI::Muted)];
    Left->AddSlot().AutoHeight().Padding(0,0,0,7)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Ordered operations"),10,StudioUI::Text,true)]
        +SHorizontalBox::Slot().AutoWidth()[Menu(TEXT("PipelineAdd"),[]{return TEXT("Add…");},[this]{return AddMenu();})]];
    Left->AddSlot().AutoHeight()[SAssignNew(Operations,SVerticalBox)];
    Left->AddSlot().AutoHeight().Padding(0,14,0,0)[SAssignNew(Editor,SBox)];
    ChildSlot[SNew(SBorder).BorderImage(&StudioUI::PanelBrush).Padding(16)[SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Post-Processing"),18,StudioUI::Text,true)]
            +SHorizontalBox::Slot().AutoWidth().Padding(7,0)[Stop]+SHorizontalBox::Slot().AutoWidth()[Apply]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Controls]
        +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)[SNew(STextBlock).Tag(TEXT("PipelineNotice")).Font(StudioUI::Font()).AutoWrapText(true)
            .ColorAndOpacity_Lambda([this]{return bError?StudioUI::Amber:StudioUI::Muted;}).Text_Lambda([this]{return FText::FromString(Notice);})]
        +SVerticalBox::Slot().FillHeight(1)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,14,0)[SNew(SBox).WidthOverride(300)[SNew(SScrollBox).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)+SScrollBox::Slot()[Left]]]
            +SHorizontalBox::Slot().FillWidth(1)[SAssignNew(Output,SBox)]]
        +SVerticalBox::Slot().AutoHeight().Padding(0,10,0,0)[Text([this]{return TEXT("Project Save keeps pipelines and cameras. Export saves the evaluated output here. Toolbar playback uses ")+M->Solver->Descriptor().Title+TEXT(" in Solve.");},9,StudioUI::Muted)]]];
    Notice=TEXT("Create a pipeline from an original recording, or choose a saved pipeline.");RefreshOutput();
}
SStudioPipelineWorkspace::~SStudioPipelineWorkspace()
{Cancel();if(Pending.IsValid())Pending.Wait();if(PendingSource.IsValid())PendingSource.Wait();Pending={};PendingSource={};CloseView();}
void SStudioPipelineWorkspace::Cancel(){if(Cancellation)Cancellation->store(true);}
void SStudioPipelineWorkspace::CloseView()
{
    if(Output)Output->SetContent(SNullWidget::NullWidget);
    if(Scene.IsValid()){WorkingCamera=Scene->SavedCameraState();Scene->Destroy();}Scene.Reset();Evaluation.Reset();PresentedRecipe.Reset();bRefreshOutput=true;
}
bool SStudioPipelineWorkspace::EnsureResolved()
{
    if(ProjectId!=M->Project.Id||!bFormDirty)return true;
    Error(TEXT("Apply or Revert pipeline parameters before saving, closing or replacing the project."));
    M->Notice=Notice;M->Navigate(EStudioWorkspace::PostProcessing);return false;
}
TOptional<FStudioPipelineEvaluationResult> SStudioPipelineWorkspace::ExportSnapshot(FString& Why)
{
    Why.Empty();
    if(!Visible()||M->IsProjectOpenPending()){Why=TEXT("Wait for this project to open, then reopen Export.");return {};}
    if(!Selected()){Why=TEXT("Choose or create a pipeline, then Evaluate before exporting.");return {};}
    if(Busy()){Why=TEXT("Wait for evaluation to finish, then reopen Export.");return {};}
    if(bFormDirty){Why=TEXT("Apply or Revert pipeline parameters, then Evaluate before exporting.");return {};}
    if(!Evaluation||!PresentedRecipe||!StudioPipelineUI::SameEvaluation(*Selected(),*PresentedRecipe))
    {Why=TEXT("Evaluate the current pipeline, then reopen Export.");return {};}
    SaveCamera();auto Frozen=*Evaluation;
    Frozen.Prepared.Field=Evaluation->Prepared.Field->WithPresentation(*Selected());Frozen.Prepared.Recipe=*Selected();
    if(!Frozen.Prepared.Field){Why=TEXT("The pipeline changed. Evaluate again before exporting.");return {};}
    return Frozen;
}
void SStudioPipelineWorkspace::SaveCamera()
{
    if(!Selected()||!Scene.IsValid()||Busy()||M->IsProjectOpenPending()||!PresentedRecipe.IsSet()||!StudioPipelineUI::SameEvaluation(*Selected(),*PresentedRecipe))return;
    const auto C=Scene->SavedCameraState();if(StudioView::CameraEquals(Selected()->Source.Camera,C))return;
    auto P=*Selected();P.Source.Camera=C;if(M->UpdatePipeline(P.Id,P)){ObservedRecipe=P;ObservedRevision=M->PipelineRevision;WorkingCamera=C;}
}
void SStudioPipelineWorkspace::Observe()
{
    if(ObservedRevision==M->PipelineRevision)return;
    const auto* P=Selected();
    if(!P)
    {
        CloseView();Source.Reset();ObservedRecipe.Reset();OperationId.Invalidate();bFormDirty=false;
        if(!M->Project.Pipelines.IsEmpty()){SelectedId=M->Project.Pipelines.Last().Id;P=Selected();}
    }
    if(P)
    {
        if(!ObservedRecipe.IsSet()||!StudioPipelineUI::SameEvaluation(*ObservedRecipe,*P))
        {
            Cancel();CloseView();WorkingCamera=P->Source.Camera;
            if(Source&&!StudioSavedFieldViews::SameIdentity(StudioPipelineUI::Identity(*Source,P->Source.Identity.Ordinal),P->Source.Identity))Source.Reset();
            Notice=TEXT("Pipeline changed. Evaluate to update its output.");bError=false;
        }
        else if(Scene.IsValid()&&!StudioView::CameraEquals(ObservedRecipe->Source.Camera,P->Source.Camera))
        {Scene->RestoreCamera(P->Source.Camera,TEXT("Restore saved pipeline camera"));WorkingCamera=P->Source.Camera;}
        ObservedRecipe=*P;
        if(!P->Operations.ContainsByPredicate([this](const auto& O){return O.Id==OperationId;}))OperationId=P->Operations[0].Id;
    }
    ObservedRevision=M->PipelineRevision;bFormDirty=false;FormNotice.Empty();RefreshOperations();RefreshEditor();RefreshOutput();
}
void SStudioPipelineWorkspace::Synchronize()
{
    if(ProjectId!=M->Project.Id)
    {Cancel();CloseView();Source.Reset();ProjectId=M->Project.Id;SelectedId.Invalidate();OperationId.Invalidate();ObservedRecipe.Reset();WorkingCamera.Reset();ObservedRevision=MAX_uint64;bInitialized=false;bFormDirty=false;}
    if(PendingSource.IsValid()&&PendingSource.IsReady())
    {
        auto R=PendingSource.Consume();const bool Cancelled=Cancellation->load();Cancellation.Reset();
        if(Cancelled||RequestProject!=ProjectId||M->IsProjectOpenPending()){Notice=TEXT("Recording read cancelled. Pipelines kept.");bError=false;}
        else if(!R.Source)Error(R.Error+TEXT(" Open Results to locate or import this recording."));
        else
        {
            Source=R.Source;FStudioSavedPipeline P;P.Source.Title=Source->Descriptor().Title;P.Source.Reference=R.Reference;
            P.Source.Identity=StudioPipelineUI::Identity(*Source,NewOrdinal);
            P.Source.Camera=StudioView::FitBounds(P.Source.Camera,Source->Descriptor().DisplayBounds,4./3.,.001);
            const auto& Scalar=Source->Descriptor().Scalars[0];FStudioPipelineOperation O;O.Name=Scalar.Label;O.Field=Scalar.Id;O.Unit=Scalar.Unit;P.Operations={O};
            P.Name=StudioPipelineUI::Unique(TEXT("Field analysis"),[this](const FString& N){return M->Project.Pipelines.ContainsByPredicate([&](const auto& V){return V.Name.Equals(N,ESearchCase::IgnoreCase);});});
            if(M->AddPipeline(P))Select(M->Project.Pipelines.Last().Id);else Error(M->PipelineNotice);
        }
    }
    if(Pending.IsValid()&&Pending.IsReady())
    {
        auto V=Pending.Consume();const bool Cancelled=Cancellation->load();Cancellation.Reset();
        if(Cancelled){Notice=TEXT("Evaluation cancelled. Evaluate again when ready.");bError=false;}
        else if(!Selected()||RequestProject!=ProjectId||RequestPipeline!=SelectedId||RequestRevision!=M->PipelineRevision||M->IsProjectOpenPending())
        {Notice=TEXT("The project or pipeline changed. Evaluate the current recipe.");bError=false;}
        else if(!V.Error.IsEmpty())Error(V.Error+TEXT(" Check the operation parameters or locate the source in Results, then Evaluate again."));
        else Present(MoveTemp(V));
    }
    Observe();
    if(Visible()&&!bInitialized)
    {bInitialized=true;if(Selected())Evaluate();}
    if(Visible()&&Scene.IsValid()&&!Scene->Model->IsViewEditActive())SaveCamera();
    if(bRefreshOutput){bRefreshOutput=false;RefreshOutput();}
}
void SStudioPipelineWorkspace::Select(const FGuid& Id,bool Run)
{
    if(!CanEdit()||!ApplyOperation())return;SaveCamera();SelectedId=Id;ObservedRevision=MAX_uint64;ObservedRecipe.Reset();WorkingCamera.Reset();OperationId.Invalidate();Observe();if(Run)Evaluate();
}
bool SStudioPipelineWorkspace::Update(FStudioSavedPipeline P)
{
    if(!CanEdit()||!Selected())return false;FString Why;TArray<FStudioPipelineStage> Stages;
    if(!(Source?StudioPipelines::Compile(P,Source->Descriptor(),Stages,Why):StudioPipelines::IsValid(P,Why))){Error(Why);return false;}
    if(!M->UpdatePipeline(P.Id,P)){Error(M->PipelineNotice);return false;}Observe();return true;
}
void SStudioPipelineWorkspace::NewPipeline(const FString& Id)
{
    if(!CanEdit()||!ApplyOperation())return;SaveCamera();RequestProject=ProjectId;NewSourceId=Id;Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    const int32 Frame=Id==M->Project.Dataset?M->SelectedFrame:0;NewOrdinal=Frame;
    PendingSource=Async(EAsyncExecution::ThreadPool,[Id,Frame,Refs=M->Project.Recordings,C=Cancellation]{return StudioRecordings::Open(Id,Refs,Frame,C);});
    Notice=TEXT("Verifying the original recording…");bError=false;
}
void SStudioPipelineWorkspace::Evaluate()
{
    if(!CanEdit()||!Selected()||!ApplyOperation())return;SaveCamera();
    FStudioPipelinePrepareRequest R;R.ProjectId=ProjectId;R.Revision=M->PipelineRevision;R.Recipe=*Selected();R.Source=Source;R.References=M->Project.Recordings;
    RequestProject=ProjectId;RequestPipeline=SelectedId;RequestRevision=M->PipelineRevision;CloseView();
    Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    Pending=Async(EAsyncExecution::ThreadPool,[R=MoveTemp(R),C=Cancellation]
    {
        FEvaluatedView V;V.Result=StudioPipelineEvaluation::Evaluate(R,C);V.Error=V.Result.Error;
        if(V.Error.IsEmpty()&&!C->load())V.Snapshot=FStudioSnapshotSource::CreatePipeline(V.Result,V.Error);return V;
    });Notice=TEXT("Evaluating ordered operations on the original frame…");bError=false;RefreshOutput();
}
void SStudioPipelineWorkspace::Present(FEvaluatedView V)
{
    if(!World.IsValid()||!V.Snapshot||!Selected())return;Source=V.Result.Prepared.Source;Evaluation=MoveTemp(V.Result);PresentedRecipe=*Selected();
    if(Evaluation->Output->Kind!=EStudioPipelineOutputKind::ProbeTable)
    {
        auto* S=World->SpawnActor<AStudioScene>();if(!S){Error(TEXT("The analysis view could not be created. Evaluate again."));return;}
        Scene=S;S->Tags.Add(TEXT("StudioPipelineView"));const TWeakPtr<SStudioPipelineWorkspace> Weak=SharedThis(this);
        S->SetViewVisibility([Weak]{const auto W=Weak.Pin();return W&&W->Visible();});
        S->InitializePipeline(V.Snapshot.ToSharedRef());S->RestoreCamera(WorkingCamera.IsSet()?*WorkingCamera:Selected()->Source.Camera,TEXT("Pipeline camera"));
    }
    Notice=Evaluation->Output->IsEmpty()?TEXT("Evaluation complete. No values intersect these operations. Adjust the clip, contour or probe."):TEXT("Evaluation complete. Original frame retained; camera is independent of Solve.");bError=false;
    RefreshEditor();RefreshOutput();
}

void SStudioPipelineWorkspace::SelectOperation(const FGuid& Id)
{if(!CanEdit()||!ApplyOperation())return;OperationId=Id;RefreshOperations();RefreshEditor();}
void SStudioPipelineWorkspace::RefreshOperations()
{
    using namespace StudioPipelineUI;
    Operations->ClearChildren();if(!Selected())return;
    for(int32 I=0;I<Selected()->Operations.Num();++I)
    {
        const auto O=Selected()->Operations[I];
        auto B=Button(FName(*FString::Printf(TEXT("PipelineOperation%d"),I)),O.Name,[this,Id=O.Id]{SelectOperation(Id);});
        B->SetContent(SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight()[Text([O,I]{return FString::Printf(TEXT("%d. %s"),I+1,*O.Name);},10,O.Id==OperationId?StudioUI::Cyan:StudioUI::Text)]
            +SVerticalBox::Slot().AutoHeight().Padding(0,3,0,0)[Note(Kind(O.Kind)+(O.Kind==EStudioPipelineOperation::Field?TEXT(" · ")+O.Field+TEXT(" (")+O.Unit+TEXT(")"):FString())+(O.bEnabled?TEXT(""):TEXT(" · disabled")))]);
        Operations->AddSlot().AutoHeight().Padding(0,2)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,8,0)[SNew(SCheckBox).Tag(FName(*FString::Printf(TEXT("PipelineEnabled%d"),I)))
                .ToolTipText(FText::FromString(TEXT("Enable ")+O.Name)).IsChecked(O.bEnabled?ECheckBoxState::Checked:ECheckBoxState::Unchecked)
                .OnCheckStateChanged_Lambda([this,Id=O.Id](ECheckBoxState S){ToggleOperation(Id,S==ECheckBoxState::Checked);})]
            +SHorizontalBox::Slot().FillWidth(1)[B]];
    }
}
TSharedRef<SWidget> SStudioPipelineWorkspace::Number(const FString& Key,const FString& Caption,double Value)
{
    Numbers.Add(Key,FString::Printf(TEXT("%.17g"),Value));
    return SNew(SHorizontalBox)
        +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SBox).WidthOverride(104)[StudioUI::Label(Caption,9)]]
        +SHorizontalBox::Slot().FillWidth(1)[SNew(SEditableTextBox).Tag(FName(*(TEXT("PipelineNumber")+Key))).Style(&StudioUI::InputStyle()).Font(StudioUI::Font()).ClearKeyboardFocusOnCommit(false)
            .Text(FText::FromString(Numbers[Key])).SelectAllTextWhenFocused(true)
            .OnTextChanged_Lambda([this,Key](const FText& T){Numbers.FindOrAdd(Key)=T.ToString();bFormDirty=true;})
            .OnTextCommitted_Lambda([this](const FText&,ETextCommit::Type T){if(T==ETextCommit::OnEnter)ApplyOperation();})];
}
void SStudioPipelineWorkspace::RefreshEditor()
{
    using namespace StudioPipelineUI;using StudioUI::Label;
    if(!Selected()){Editor->SetContent(SNullWidget::NullWidget);return;}
    const auto* O=Selected()->Operations.FindByPredicate([this](const auto& V){return V.Id==OperationId;});
    if(!O){Editor->SetContent(SNullWidget::NullWidget);return;}EditOperation=*O;EditName=O->Name;Numbers.Reset();bFormDirty=false;
    auto Rows=SNew(SVerticalBox);auto Actions=SNew(SHorizontalBox);
    auto Up=Button(TEXT("PipelineMoveUp"),TEXT("Move up"),[this]{MoveOperation(-1);});
    auto Down=Button(TEXT("PipelineMoveDown"),TEXT("Move down"),[this]{MoveOperation(1);});
    Up->SetEnabled(Selected()->Operations[0].Id!=OperationId);Down->SetEnabled(Selected()->Operations.Last().Id!=OperationId);
    Actions->AddSlot().AutoWidth().Padding(0,0,6,0)[Up];Actions->AddSlot().AutoWidth().Padding(0,0,6,0)[Down];
    Actions->AddSlot().AutoWidth()[Button(TEXT("PipelineRemove"),TEXT("Remove"),[this]{DeleteOperation();})];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,12)[Actions];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,6)[Label(Kind(O->Kind),11,StudioUI::Text,true)];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,10)[SNew(SEditableTextBox).Tag(TEXT("PipelineOperationName")).Style(&StudioUI::InputStyle()).Font(StudioUI::Font()).ClearKeyboardFocusOnCommit(false)
        .Text_Lambda([this]{return FText::FromString(EditName);}).HintText(FText::FromString(TEXT("Operation name"))).OnTextChanged_Lambda([this](const FText& T){EditName=T.ToString();bFormDirty=true;})];
    if(O->Kind==EStudioPipelineOperation::Field)
    {
        Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[Menu(TEXT("PipelineScalar"),[this]{return EditOperation.Field+TEXT(" (")+EditOperation.Unit+TEXT(")");},[this]{return ScalarMenu();})];
        Rows->AddSlot().AutoHeight()[Note(TEXT("Select an original array or an enabled earlier magnitude."))];
    }
    if(O->Kind==EStudioPipelineOperation::Magnitude)
    {
        Rows->AddSlot().AutoHeight().Padding(0,0,0,6)[Label(TEXT("Components with identical units"),9,StudioUI::Muted)];
        for(int32 I=0;I<3;++I)Rows->AddSlot().AutoHeight().Padding(0,0,0,6)[Menu(FName(*FString::Printf(TEXT("PipelineComponent%d"),I)),[this,I]
            {return EditOperation.Components.IsValidIndex(I)?EditOperation.Components[I]:FString(TEXT("No third component"));},[this,I]{return ScalarMenu(I);})];
        Rows->AddSlot().AutoHeight()[Text([this]{return TEXT("Output: ")+EditOperation.Field+TEXT(" (")+EditOperation.Unit+TEXT(")\nComponents are interpolated before their Euclidean magnitude is calculated.");},9,StudioUI::Muted)];
    }
    if(O->Kind==EStudioPipelineOperation::ClipBox||O->Kind==EStudioPipelineOperation::Slice||O->Kind==EStudioPipelineOperation::Probe)
    {
        Rows->AddSlot().AutoHeight().Padding(0,0,0,6)[Label(TEXT("Scene coordinates in meters (X, Y, Z)"),9,StudioUI::Muted)];
        if(O->Kind==EStudioPipelineOperation::Probe)
        {
            Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SCheckBox).Tag(TEXT("PipelineProbeLine"))
                .IsChecked_Lambda([this]{return EditOperation.bLine?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                .OnCheckStateChanged_Lambda([this](ECheckBoxState S){EditOperation.bLine=S==ECheckBoxState::Checked;bFormDirty=true;})[Label(TEXT("Sample along a line"))]];
        }
        for(int32 V=0;V<2;++V)for(int32 Axis=0;Axis<3;++Axis)
        {
            const FString Key=FString::Printf(TEXT("%c%d"),V?'B':'A',Axis);const TCHAR* Axes[]={TEXT("X"),TEXT("Y"),TEXT("Z")};
            FString LabelText;
            if(O->Kind==EStudioPipelineOperation::ClipBox)LabelText=V?TEXT("Maximum "):TEXT("Minimum ");
            if(O->Kind==EStudioPipelineOperation::Slice)LabelText=V?TEXT("Normal "):TEXT("Origin ");
            if(O->Kind==EStudioPipelineOperation::Probe)LabelText=V?TEXT("End "):TEXT("Point / start ");
            auto Input=Number(Key,LabelText+Axes[Axis],(V?O->B:O->A)[Axis]);
            if(O->Kind==EStudioPipelineOperation::Probe&&V)Input->SetEnabled(TAttribute<bool>::CreateLambda([this]{return EditOperation.bLine;}));
            if(O->Kind==EStudioPipelineOperation::Slice&&Selected()->Source.Identity.SpatialDimensions==2&&(V||Axis==1))Input->SetEnabled(false);
            Rows->AddSlot().AutoHeight().Padding(0,0,0,6)[Input];
        }
        if(O->Kind==EStudioPipelineOperation::Slice)Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[Label(
            Selected()->Source.Identity.SpatialDimensions==2?TEXT("This 2D recording stays on its original X/Z plane."):TEXT("The nonzero normal is normalized when applied."),9,StudioUI::Muted)];
        if(O->Kind==EStudioPipelineOperation::Probe)
        {
            auto Samples=Number(TEXT("Samples"),TEXT("Line samples"),O->Samples);Samples->SetEnabled(TAttribute<bool>::CreateLambda([this]{return EditOperation.bLine;}));
            Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[Samples];
            Rows->AddSlot().AutoHeight()[Note(TEXT("2–1,024 line samples. Missing values stay unavailable; a point returns one row."))];
        }
    }
    if(O->Kind==EStudioPipelineOperation::Contour)
    {
        const auto Fields=AvailableScalars(O->Id);FString Unit=TEXT("scalar units");
        if(Evaluation.IsSet())Unit=Evaluation->Prepared.Field->SelectedScalar().Unit;
        else if(Selected())for(const auto& Stage:Selected()->Operations){if(Stage.Id==O->Id)break;if(Stage.bEnabled&&(Stage.Kind==EStudioPipelineOperation::Field||Stage.Kind==EStudioPipelineOperation::Magnitude))Unit=Stage.Unit;}
        Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[Number(TEXT("Value"),TEXT("Isovalue (")+Unit+TEXT(")"),O->Value)];
        Rows->AddSlot().AutoHeight()[Note(TEXT("Extracts lines from a 2D field or slice, and a surface from a verified 3D volume."))];
    }
    Rows->AddSlot().AutoHeight().Padding(0,12,0,0)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().AutoWidth().Padding(0,0,8,0)[Button(TEXT("PipelineApplyOperation"),TEXT("Apply parameters"),[this]{ApplyOperation();})]
        +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("PipelineRevertOperation"),TEXT("Revert"),[this]{FormNotice.Empty();RefreshEditor();})]];
    Rows->AddSlot().AutoHeight().Padding(0,8,0,0)[Text([this]{return !FormNotice.IsEmpty()?FormNotice:bFormDirty?TEXT("Parameters changed. Apply to the saved recipe; Evaluate updates the output."):TEXT("Parameters match the saved recipe.");},9,StudioUI::Muted)];
    Editor->SetContent(Rows);
}
bool SStudioPipelineWorkspace::ApplyOperation()
{
    if(!bFormDirty)return true;if(!CanEdit()||!Selected())return false;
    auto P=*Selected();auto* O=P.Operations.FindByPredicate([this](const auto& V){return V.Id==OperationId;});if(!O){Error(TEXT("This operation was removed. Select another operation."));return false;}
    auto Edited=EditOperation;Edited.Name=EditName.TrimStartAndEnd();
    for(const auto& Pair:Numbers)
    {
        if(Edited.Kind==EStudioPipelineOperation::Probe&&!Edited.bLine&&(Pair.Key.StartsWith(TEXT("B"))||Pair.Key==TEXT("Samples")))continue;
        double V;if(!StudioColor::ParseNumber(Pair.Value,V)){FormNotice=TEXT("Enter a finite number for ")+Pair.Key+TEXT(". Parameters kept for correction.");Error(FormNotice);return false;}
        if(Pair.Key==TEXT("Value"))Edited.Value=V;
        else if(Pair.Key==TEXT("Samples"))
        {if(V<2||V>1024||V!=FMath::FloorToDouble(V)){FormNotice=TEXT("Line samples must be a whole number from 2 to 1,024.");Error(FormNotice);return false;}Edited.Samples=int32(V);}
        else if(Pair.Key.Len()==2)(Pair.Key[0]=='A'?Edited.A:Edited.B)[Pair.Key[1]-'0']=V;
    }
    if(Edited.Kind==EStudioPipelineOperation::Slice)
    {
        const double Length=Edited.B.Size();if(!FMath::IsFinite(Length)||Length<1.e-12){FormNotice=TEXT("Enter a nonzero slice normal.");Error(FormNotice);return false;}Edited.B/=Length;
    }
    *O=MoveTemp(Edited);const bool Success=Update(MoveTemp(P));
    if(Success){bFormDirty=false;FormNotice.Empty();}else FormNotice=Notice;return Success;
}
TArray<FStudioScalarDescriptor> SStudioPipelineWorkspace::AvailableScalars(const FGuid& Before) const
{
    TArray<FStudioScalarDescriptor> Out;if(Source)Out=Source->Descriptor().Scalars;if(!Selected())return Out;
    for(const auto& O:Selected()->Operations)
    {
        if(O.Id==Before)break;if(!O.bEnabled||O.Kind!=EStudioPipelineOperation::Magnitude)continue;
        FStudioScalarDescriptor S;S.Id=O.Field;S.Label=O.Name;S.Unit=O.Unit;S.Origin=TEXT("pipeline-derived");Out.Add(S);
    }
    return Out;
}
TSharedRef<SWidget> SStudioPipelineWorkspace::ScalarMenu(int32 Component)
{
    using namespace StudioPipelineUI;auto Rows=SNew(SVerticalBox);
    if(Component==2)Choice(Rows,TEXT("PipelineComponentNone"),TEXT("No third component"),[this]{EditOperation.Components.SetNum(2);bFormDirty=true;});
    for(const auto& S:AvailableScalars(OperationId))
    {
        if(Component!=INDEX_NONE&&(S.Unit.TrimStartAndEnd().IsEmpty()||S.Unit.Equals(TEXT("unknown"),ESearchCase::IgnoreCase)||S.Unit.Equals(TEXT("unspecified"),ESearchCase::IgnoreCase)))continue;
        Choice(Rows,FName(*(TEXT("PipelineScalar_")+S.Id)),S.Label+TEXT(" · ")+S.Id+TEXT(" (")+S.Unit+TEXT(")"),[this,S,Component]
        {
            if(Component==INDEX_NONE)
            {
                const auto Fields=AvailableScalars(OperationId);
                const auto* Previous=Fields.FindByPredicate([this](const auto& F){return F.Id==EditOperation.Field;});
                if(Previous&&EditName==Previous->Label)EditName=S.Label;
                EditOperation.Field=S.Id;EditOperation.Unit=S.Unit;
            }
            else{if(EditOperation.Components.Num()<=Component)EditOperation.Components.SetNum(Component+1);EditOperation.Components[Component]=S.Id;if(Component==0)EditOperation.Unit=S.Unit;}
            bFormDirty=true;
        });
    }
    if(!Source)Rows->AddSlot().AutoHeight()[StudioUI::Label(TEXT("Evaluate to verify the source before choosing an array."),10)];
    return Choices(Rows,380);
}
void SStudioPipelineWorkspace::AddOperation(EStudioPipelineOperation K)
{
    if(!CanEdit()||!Selected()||!ApplyOperation())return;SaveCamera();auto P=*Selected();FStudioPipelineOperation O;O.Kind=K;
    O.Name=StudioPipelineUI::Unique(StudioPipelineUI::Kind(K),[&](const FString& N){return P.Operations.ContainsByPredicate([&](const auto& V){return V.Name.Equals(N,ESearchCase::IgnoreCase);});});
    const auto Scalars=AvailableScalars({});const FBox Bounds=Source?Source->Descriptor().DisplayBounds:FBox(FVector(-1,-1,-1),FVector(1,1,1));
    O.A=Bounds.GetCenter();O.B=O.A+FVector(FMath::Max(.001,Bounds.GetSize().X)*.2,0,0);
    if(K==EStudioPipelineOperation::Field)
    {if(Scalars.IsEmpty()){Error(TEXT("Evaluate to verify the available source arrays."));return;}O.Field=Scalars[0].Id;O.Unit=Scalars[0].Unit;}
    if(K==EStudioPipelineOperation::Magnitude)
    {
        for(const auto& S:Scalars)
        {
            TArray<FString> Group;for(const auto& C:Scalars)if(C.Unit==S.Unit&&C.Id!=S.Id)Group.Add(C.Id);
            if(Group.Num()>0&&!S.Unit.IsEmpty()&&!S.Unit.Equals(TEXT("unknown"),ESearchCase::IgnoreCase)&&!S.Unit.Equals(TEXT("unspecified"),ESearchCase::IgnoreCase))
            {O.Components={S.Id,Group[0]};O.Unit=S.Unit;break;}
        }
        if(O.Components.Num()!=2){Error(TEXT("A magnitude needs two source components with the same known unit."));return;}
        O.Field=TEXT("derived.")+O.Id.ToString(EGuidFormats::Digits);
    }
    if(K==EStudioPipelineOperation::ClipBox)
    {O.A=Bounds.Min;O.B=Bounds.Max;for(int32 A=0;A<3;++A)if(O.A[A]==O.B[A]){O.A[A]-=.001;O.B[A]+=.001;}}
    if(K==EStudioPipelineOperation::Slice){O.B=FVector::RightVector;if(P.Source.Identity.SpatialDimensions==2)O.A.Y=P.Source.Identity.SourceOffset.Y;}
    if(K==EStudioPipelineOperation::Contour)
    {if(Evaluation.IsSet())O.Value=Evaluation->Output->Range.IsSet()?Evaluation->Output->Range->GetMax():Evaluation->Prepared.Field->SelectedScalar().Minimum;}
    // Insert scalar definitions before geometry, and geometry before a terminal probe.
    int32 At=P.Operations.Num();
    if(K==EStudioPipelineOperation::Field||K==EStudioPipelineOperation::Magnitude)
    {for(int32 I=0;I<P.Operations.Num();++I)if(P.Operations[I].Kind!=EStudioPipelineOperation::Field&&P.Operations[I].Kind!=EStudioPipelineOperation::Magnitude){At=I;break;}}
    else if(K!=EStudioPipelineOperation::Probe)
    {for(int32 I=0;I<P.Operations.Num();++I)if(P.Operations[I].Kind==EStudioPipelineOperation::Probe){At=I;break;}}
    P.Operations.Insert(O,At);if(Update(MoveTemp(P))){OperationId=O.Id;RefreshOperations();RefreshEditor();}
}
void SStudioPipelineWorkspace::MoveOperation(int32 Offset)
{
    if(!CanEdit()||!Selected()||!ApplyOperation())return;SaveCamera();auto P=*Selected();const int32 At=P.Operations.IndexOfByPredicate([this](const auto& O){return O.Id==OperationId;});
    FString Why;if(!StudioPipelines::Move(P,OperationId,At+Offset,Why)){Error(Why);return;}Update(MoveTemp(P));
}
void SStudioPipelineWorkspace::DeleteOperation()
{
    if(!CanEdit()||!Selected())return;auto P=*Selected();P.Operations.RemoveAll([this](const auto& O){return O.Id==OperationId;});Update(MoveTemp(P));
}
void SStudioPipelineWorkspace::ToggleOperation(const FGuid& Id,bool Enabled)
{
    if(!CanEdit()||!Selected()||!ApplyOperation()){RefreshOperations();return;}SaveCamera();auto P=*Selected();FString Why;
    if(!StudioPipelines::SetEnabled(P,Id,Enabled,Why)){Error(Why);RefreshOperations();return;}Update(MoveTemp(P));
}
void SStudioPipelineWorkspace::History(bool Redo)
{
    if(!CanEdit())return;if(bFormDirty){Error(TEXT("Apply or Revert the edited parameters before using pipeline history."));return;}SaveCamera();
    if(Redo?M->RedoPipelines():M->UndoPipelines()){Observe();Notice=M->PipelineNotice;bError=false;}else Error(M->PipelineNotice);
}
TSharedRef<SWidget> SStudioPipelineWorkspace::NewMenu()
{
    using namespace StudioPipelineUI;auto Rows=SNew(SVerticalBox);
    Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[StudioUI::Label(TEXT("Create from an original recording"),11,StudioUI::Text,true)];
    TSet<FString> Seen;
    auto Add=[&](const FString& Id,const FString& Title)
    {if(Seen.Contains(Id))return;Seen.Add(Id);Choice(Rows,FName(*(TEXT("PipelineNew_")+Id)),Title+(Id==M->Project.Dataset?TEXT(" · current Solve frame"):TEXT(" · first frame")),[this,Id]{NewPipeline(Id);});};
    Add(M->Project.Dataset,M->Solver->Descriptor().Title);
    for(const auto& R:M->Project.Recordings)Add(R.Id,R.Title);
    for(const auto& R:StudioRecordings::Installed())Add(R.Id,R.Title);
    Rows->AddSlot().AutoHeight().Padding(0,10,0,0)[StudioUI::Label(TEXT("Import and locate recordings in Results. New pipelines do not change the Solve recording."),9,StudioUI::Muted)];
    Choice(Rows,TEXT("PipelineResults"),TEXT("Open Results"),[this]{Results.ExecuteIfBound();});return Choices(Rows,410);
}
TSharedRef<SWidget> SStudioPipelineWorkspace::SavedMenu()
{
    using namespace StudioPipelineUI;auto Rows=SNew(SVerticalBox);
    if(M->Project.Pipelines.IsEmpty())Rows->AddSlot().AutoHeight()[StudioUI::Label(TEXT("No saved pipelines. Choose New pipeline to create one."),10)];
    for(int32 I=0;I<M->Project.Pipelines.Num();++I)
    {const auto& P=M->Project.Pipelines[I];Choice(Rows,FName(*FString::Printf(TEXT("PipelineSaved%d"),I)),P.Name+TEXT(" · ")+P.Source.Title,[this,Id=P.Id]{Select(Id);});}
    return Choices(Rows,410);
}
TSharedRef<SWidget> SStudioPipelineWorkspace::ManageMenu()
{
    using namespace StudioPipelineUI;auto Rows=SNew(SVerticalBox);if(!Selected())return Choices(Rows);
    const FGuid Id=SelectedId;Rename=Selected()->Name;
    Rows->AddSlot().AutoHeight().Padding(0,0,0,6)[StudioUI::Label(TEXT("Pipeline name"),10,StudioUI::Text,true)];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SEditableTextBox).Tag(TEXT("PipelineRename")).Style(&StudioUI::InputStyle()).Font(StudioUI::Font()).ClearKeyboardFocusOnCommit(false)
        .Text(FText::FromString(Rename)).OnTextChanged_Lambda([this](const FText& T){Rename=T.ToString();})];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,10)[Button(TEXT("PipelineRenameApply"),TEXT("Rename"),[this,Id]
    {if(!CanEdit()||!ApplyOperation())return;if(M->RenamePipeline(Id,Rename)){Observe();Notice=M->PipelineNotice;bError=false;FSlateApplication::Get().DismissAllMenus();}else Error(M->PipelineNotice);})];
    Rows->AddSlot().AutoHeight()[Button(TEXT("PipelineDelete"),TEXT("Delete pipeline"),[this,Id]
    {if(!CanEdit())return;FSlateApplication::Get().DismissAllMenus();if(M->DeletePipeline(Id)){Observe();Notice=M->PipelineNotice;bError=false;}else Error(M->PipelineNotice);})];
    Rows->AddSlot().AutoHeight().Padding(0,8,0,0)[StudioUI::Label(TEXT("Undo restores a deleted pipeline. Project Save writes the updated collection."),9,StudioUI::Muted)];return Choices(Rows);
}
TSharedRef<SWidget> SStudioPipelineWorkspace::AddMenu()
{
    using namespace StudioPipelineUI;auto Rows=SNew(SVerticalBox);
    for(const auto K:{EStudioPipelineOperation::Field,EStudioPipelineOperation::Magnitude,EStudioPipelineOperation::ClipBox,EStudioPipelineOperation::Slice,EStudioPipelineOperation::Contour,EStudioPipelineOperation::Probe})
    {
        auto B=Button(FName(*FString::Printf(TEXT("PipelineAdd%d"),int32(K))),Kind(K),[this,K]{FSlateApplication::Get().DismissAllMenus();AddOperation(K);});
        const bool NeedsInterpolation=K==EStudioPipelineOperation::Slice||K==EStudioPipelineOperation::Contour||K==EStudioPipelineOperation::Probe;
        const bool Available=Selected()&&(!NeedsInterpolation||Selected()->Source.Identity.Interpolation!=EStudioFieldInterpolation::None);
        B->SetEnabled(Available);if(!Available)B->SetToolTipText(FText::FromString(TEXT("This point source needs a verified reconstruction. Attach it in Results and create a pipeline from that source.")));
        Rows->AddSlot().AutoHeight().Padding(0,2)[B];
    }
    Rows->AddSlot().AutoHeight().Padding(0,10,0,0)[StudioUI::Label(TEXT("Scalar definitions precede geometry. A probe ends the sequence. One slice and one contour are supported."),9,StudioUI::Muted)];return Choices(Rows);
}
TSharedRef<SWidget> SStudioPipelineWorkspace::SourceMenu()
{
    using namespace StudioPipelineUI;auto Rows=SNew(SVerticalBox);if(!Selected())return Choices(Rows);
    const auto P=*Selected();const auto& I=P.Source.Identity;const FString Method=I.Interpolation==EStudioFieldInterpolation::None?TEXT("Original point samples"):
        I.Interpolation==EStudioFieldInterpolation::SourceTriangles?TEXT("Original source triangles"):I.Interpolation==EStudioFieldInterpolation::ReconstructedTriangles?TEXT("Verified display triangles"):I.Interpolation==EStudioFieldInterpolation::SourceGrid?TEXT("Original structured grid"):I.Interpolation==EStudioFieldInterpolation::SourceSlice?TEXT("Original structured slice"):TEXT("Verified reconstructed volume");
    const FString Details=FString::Printf(TEXT("%s\n%s\nFrame %d · source step %d · %.17g s\n%dD · %s\nScene offset (m): %.9g, %.9g, %.9g\n\nSource SHA-256\n%s\nPayload SHA-256\n%s\nReconstruction SHA-256\n%s\n\nLocation\n%s"),
        *P.Source.Title,*I.Dataset,I.Ordinal+1,I.Frame.Index,I.Frame.Time,I.SpatialDimensions,*Method,I.SourceOffset.X,I.SourceOffset.Y,I.SourceOffset.Z,
        *I.MetadataSHA256,*I.PayloadSHA256,*I.ReconstructionSHA256,P.Source.Reference.IsSet()?*P.Source.Reference->Path:TEXT("Installed recording"));
    Rows->AddSlot().AutoHeight()[SNew(SMultiLineEditableText).Tag(TEXT("PipelineProvenance")).IsReadOnly(true).AutoWrapText(true).Font(StudioUI::Font(9)).Text(FText::FromString(Details))];
    Choice(Rows,TEXT("PipelineLocateResults"),TEXT("Locate or import in Results"),[this]{SaveCamera();Results.ExecuteIfBound();});return Choices(Rows,440);
}
TSharedRef<SWidget> SStudioPipelineWorkspace::CameraMenu()
{
    using namespace StudioPipelineUI;using StudioUI::Label;auto Rows=SNew(SVerticalBox);
    Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[Label(TEXT("Independent pipeline camera"),11,StudioUI::Text,true)];
    for(int32 Axis=0;Axis<6;++Axis)
    {
        const TCHAR* Names[]={TEXT("X (m)"),TEXT("Y (m)"),TEXT("Z (m)"),TEXT("Pitch (°)"),TEXT("Yaw (°)"),TEXT("Roll (°)")};
        Rows->AddSlot().AutoHeight().Padding(0,0,0,7)[SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SBox).WidthOverride(95)[Label(Names[Axis],10)]]
            +SHorizontalBox::Slot().FillWidth(1)[SNew(SNumericEntryBox<double>).Tag(FName(*FString::Printf(TEXT("PipelineCameraAxis%d"),Axis)))
                .EditableTextBoxStyle(&StudioUI::InputStyle()).Font(StudioUI::Font()).MaxFractionalDigits(12)
                .Value_Lambda([this,Axis]()->TOptional<double>{if(!Scene.IsValid())return {};if(Axis<3)return Scene->CameraPosition()[Axis];const auto R=Scene->CameraRotation();return Axis==3?R.Pitch:Axis==4?R.Yaw:R.Roll;})
                .OnValueCommitted_Lambda([this,Axis](double V,ETextCommit::Type)
                {if(!Scene.IsValid()||!FMath::IsFinite(V))return;if(Axis<3){auto P=Scene->CameraPosition();P[Axis]=V;Scene->SetCameraPosition(P);}
                    else{auto R=Scene->CameraRotation();if(Axis==3)R.Pitch=V;else if(Axis==4)R.Yaw=V;else R.Roll=V;Scene->SetCameraRotation(R);}SaveCamera();})]];
    }
    Rows->AddSlot().AutoHeight().Padding(0,5)[SNew(SCheckBox).Tag(TEXT("PipelineCameraFree"))
        .IsChecked_Lambda([this]{return Scene.IsValid()&&Scene->SavedCameraState().bFreeCamera?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
        .OnCheckStateChanged_Lambda([this](ECheckBoxState V){if(Scene.IsValid()){Scene->SetCameraMode(V==ECheckBoxState::Checked);SaveCamera();}})[Label(TEXT("Free flight"))]];
    Rows->AddSlot().AutoHeight().Padding(0,5)[SNew(SCheckBox).Tag(TEXT("PipelineCameraOrtho"))
        .IsChecked_Lambda([this]{return Scene.IsValid()&&Scene->SavedCameraState().bOrthographic?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
        .OnCheckStateChanged_Lambda([this](ECheckBoxState V){if(Scene.IsValid()){auto C=Scene->SavedCameraState();C.bOrthographic=V==ECheckBoxState::Checked;
            if(C.bOrthographic)C.OrthoWidth=2*C.OrbitDistance*FMath::Tan(FMath::DegreesToRadians(C.FieldOfView*.5));Scene->RestoreCamera(C,TEXT("Pipeline projection"));SaveCamera();}})[Label(TEXT("Orthographic projection"))]];
    Rows->AddSlot().AutoHeight().Padding(0,8,0,0)[Label(TEXT("Drag to orbit · middle drag to pan · right drag + WASDQE to fly · wheel to zoom · F to fit. Camera edits are saved with this pipeline."),9,StudioUI::Muted)];return Choices(Rows,350);
}
TSharedRef<SWidget> SStudioPipelineWorkspace::RangeMenu()
{
    using namespace StudioPipelineUI;auto Rows=SNew(SVerticalBox);if(!Scene.IsValid())return Choices(Rows);
    const auto Map=Scene->Model->ActiveColorMapping();RangeMinimum=FString::Printf(TEXT("%.17g"),Map.Minimum);RangeMaximum=FString::Printf(TEXT("%.17g"),Map.Maximum);
    Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[StudioUI::Label(TEXT("Output colors · current evaluation"),11,StudioUI::Text,true)];
    for(int32 I=0;I<3;++I)Choice(Rows,FName(*FString::Printf(TEXT("PipelinePalette%d"),I)),StudioColor::PaletteName(I),[this,I]
    {if(Scene.IsValid()){const auto C=Scene->Model->ActiveColorMapping();Scene->Model->SetScalarStyle(I,C.bManualRange,C.Minimum,C.Maximum);}});
    for(int32 I=0;I<2;++I)Rows->AddSlot().AutoHeight().Padding(0,6)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SNew(SBox).WidthOverride(85)[StudioUI::Label(I?TEXT("Maximum"):TEXT("Minimum"))]]
        +SHorizontalBox::Slot().FillWidth(1)[SNew(SEditableTextBox).Tag(I?TEXT("PipelineRangeMaximum"):TEXT("PipelineRangeMinimum"))
            .Style(&StudioUI::InputStyle()).Font(StudioUI::Font()).ClearKeyboardFocusOnCommit(false).Text(FText::FromString(I?RangeMaximum:RangeMinimum))
            .OnTextChanged_Lambda([this,I](const FText& T){(I?RangeMaximum:RangeMinimum)=T.ToString();})]];
    Rows->AddSlot().AutoHeight().Padding(0,6)[Button(TEXT("PipelineRangeApply"),TEXT("Apply display range"),[this]
    {
        if(!Scene.IsValid())return;double A,B;
        if(!StudioColor::ParseNumber(RangeMinimum,A)||!StudioColor::ParseNumber(RangeMaximum,B)||A>=B){Error(TEXT("Display minimum and maximum must be finite, with minimum below maximum."));return;}
        if(!Scene->Model->SetScalarStyle(Scene->Model->ActiveColorMapping().Palette,true,A,B))Error(Scene->Model->Notice);else FSlateApplication::Get().DismissAllMenus();
    })];
    Choice(Rows,TEXT("PipelineRangeSource"),TEXT("Use scalar metadata range"),[this]{if(Scene.IsValid()){const auto C=Scene->Model->ActiveColorMapping();Scene->Model->SetScalarStyle(C.Palette,false,C.Minimum,C.Maximum);}});
    Rows->AddSlot().AutoHeight().Padding(0,7)[StudioUI::Label(TEXT("Colors do not change numerical values. A new evaluation resets display colors."),9,StudioUI::Muted)];return Choices(Rows);
}
void SStudioPipelineWorkspace::RefreshOutput()
{
    using namespace StudioPipelineUI;using StudioUI::Label;
    if(!Evaluation.IsSet())
    {
        Output->SetContent(SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(.003,.009,.016)).Padding(28)
            [SNew(SVerticalBox)
                +SVerticalBox::Slot().FillHeight(1)
                +SVerticalBox::Slot().AutoHeight()[Text([this]{return Busy()?TEXT("Preparing the recorded field…"):Selected()?TEXT("Ready to evaluate"):TEXT("Build an analysis from recorded CFD");},18,StudioUI::Text)]
                +SVerticalBox::Slot().AutoHeight().Padding(0,12,0,0)[Text([this]{return Busy()?TEXT("Source arrays and ordered operations are checked before any output is shown. Cancel remains available."):
                    Selected()?TEXT("Choose an operation to inspect its parameters. Apply changes, then Evaluate to view the result."):
                    TEXT("Choose New pipeline, select an original recording, and add a clip, slice, contour or probe. Saved recipes reopen at their exact source frame.");},11,StudioUI::Muted)]
                +SVerticalBox::Slot().AutoHeight().Padding(0,14,0,0)[Button(TEXT("PipelineOpenResults"),TEXT("Manage recordings in Results"),[this]{Results.ExecuteIfBound();})]
                +SVerticalBox::Slot().FillHeight(1)]);return;
    }
    const auto& O=*Evaluation->Output;const auto& I=Evaluation->Prepared.Recipe.Source.Identity;const auto S=Evaluation->Prepared.Field->SelectedScalar();
    auto Rows=SNew(SVerticalBox);
    Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(S.Label+TEXT(" (")+S.Unit+TEXT(")"),12,StudioUI::Text,true)]
        +SHorizontalBox::Slot().AutoWidth()[SNew(SHorizontalBox).Visibility(Scene.IsValid()?EVisibility::Visible:EVisibility::Collapsed)
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[Button(TEXT("PipelineFit"),TEXT("Fit"),[this]{if(Scene.IsValid()){Scene->FitCamera();SaveCamera();}})]
            +SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)[Menu(TEXT("PipelineCamera"),[]{return TEXT("Camera…");},[this]{return CameraMenu();})]
            +SHorizontalBox::Slot().AutoWidth()[Menu(TEXT("PipelineColors"),[]{return TEXT("Colors…");},[this]{return RangeMenu();})]]];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,4)[Note(Evaluation->Prepared.Recipe.Source.Title)];
    Rows->AddSlot().AutoHeight().Padding(0,0,0,8)[Label(FString::Printf(TEXT("%dD · frame %d · original step %d · %.9g s · %s"),I.SpatialDimensions,I.Ordinal+1,I.Frame.Index,I.Frame.Time,*S.Origin),9,StudioUI::Muted)];
    if(O.Kind==EStudioPipelineOutputKind::ProbeTable&&O.Probe.IsSet())
    {
        const auto& P=*O.Probe;auto Table=SNew(SVerticalBox);
        auto Row=[](const TArray<FString>& Values,bool Header)
        {
            auto H=SNew(SHorizontalBox);const float Widths[]={.08f,.4f,.15f,.18f,.19f};
            for(int32 K=0;K<Values.Num();++K)H->AddSlot().FillWidth(Widths[K]).Padding(4,5)[SNew(STextBlock).Font(StudioUI::Font(9,Header)).ColorAndOpacity(Header?StudioUI::Muted:StudioUI::Text).AutoWrapText(true).Text(FText::FromString(Values[K]))];return H;
        };
        Table->AddSlot().AutoHeight()[Row({TEXT("Row"),TEXT("Scene X, Y, Z (m)"),TEXT("Distance (m)"),S.Unit,TEXT("Status")},true)];
        for(int32 K=0;K<P.Samples.Num();++K)
        {
            const auto& V=P.Samples[K];const FString Position=V.ScenePosition.IsSet()?FString::Printf(TEXT("%.6g, %.6g, %.6g"),V.ScenePosition->X,V.ScenePosition->Y,V.ScenePosition->Z):TEXT("Unavailable");
            Table->AddSlot().AutoHeight()[Row({FString::FromInt(K+1),Position,FString::Printf(TEXT("%.6g"),V.DistanceAlongLineMeters),
                V.Value.IsSet()?FString::Printf(TEXT("%.9g"),*V.Value):TEXT("—"),SampleStatus(V.Status)},false)];
        }
        Rows->AddSlot().FillHeight(1)[SNew(SScrollBox).Tag(TEXT("PipelineProbeTable")).ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll)+SScrollBox::Slot()[Table]];
        Rows->AddSlot().AutoHeight().Padding(0,8)[Label(FString::Printf(TEXT("%d requested samples · %d values · %d unavailable"),P.Samples.Num(),
            P.Samples.FilterByPredicate([](const auto& V){return V.Value.IsSet();}).Num(),P.Samples.FilterByPredicate([](const auto& V){return !V.Value.IsSet();}).Num()),9,StudioUI::Muted)];
    }
    else if(Scene.IsValid())
    {
        Rows->AddSlot().FillHeight(1)[SNew(SOverlay)
            +SOverlay::Slot()[MakeStudioFlowViewport(Scene.Get(),TEXT("PipelineViewport"))]
            +SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)[SNew(SBox).MaxDesiredWidth(400)
                .Visibility_Lambda([this]{return !Current()||Evaluation->Output->IsEmpty()?EVisibility::Visible:EVisibility::Collapsed;})
                [SNew(SBorder).BorderImage(&StudioUI::PanelBrush).Padding(16)[Text([this]()->FString
                {if(Scene.IsValid()&&!Scene->Model->Notice.IsEmpty())return Scene->Model->Notice;
                    return Evaluation->Output->IsEmpty()?TEXT("No values intersect these operations. Adjust the clip, contour or probe and Evaluate again."):TEXT("Preparing the analysis view…");},10,StudioUI::Amber)]]]];
        Rows->AddSlot().AutoHeight().Padding(0,10,0,5)[SNew(SScale).Scene(Scene.Get())];
        Rows->AddSlot().AutoHeight()[Text([this,Unit=S.Unit]{if(!Scene.IsValid())return FString();const auto& C=Scene->PresentedColorMapping();return FString::Printf(TEXT("Display range: %.9g to %.9g %s"),C.Minimum,C.Maximum,*Unit);},9)];
    }
    Rows->AddSlot().AutoHeight().Padding(0,10,0,0)[Note(O.Method)];
    if(S.Origin==TEXT("pipeline-derived"))Rows->AddSlot().AutoHeight().Padding(0,5,0,0)[Note(Evaluation->Prepared.Field->ScalarExpression(S.Id))];
    Rows->AddSlot().AutoHeight().Padding(0,5,0,0)[Label(O.Range.IsSet()?FString::Printf(TEXT("Evaluated values: %.9g to %.9g %s · %d vertices · %d triangles · %d segments"),O.Range->X,O.Range->Y,*S.Unit,O.Vertices.Num(),O.Triangles.Num(),O.Lines.Num()):TEXT("No evaluated scalar range."),9,StudioUI::Muted)];
    Output->SetContent(Rows);
}
