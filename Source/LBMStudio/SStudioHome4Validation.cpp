#include "SStudioHome4Validation.h"
#include "StudioHome4Runtime.h"
#include "StudioHome4RecipeGates.h"
#include "StudioHome4Couette.h"
#include "StudioHome4ReportPlots.h"
#include "StudioModel.h"
#include "StudioTheme.h"
#include "StudioFileDialog.h"
#include "Async/Async.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SLeafWidget.h"
#include "Rendering/DrawElements.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"

namespace StudioHome4ValidationUIPrivate
{
    FString OptionalNumber(const TOptional<double>& V)
    { return V ? FString::Printf(TEXT("%.6g"), *V) : TEXT("Unavailable"); }
    class SReferenceOverlay final : public SLeafWidget
    {
    public:
        SLATE_BEGIN_ARGS(SReferenceOverlay) {} SLATE_ARGUMENT(TSharedPtr<FStudioHome4ValidationState>, State) SLATE_ARGUMENT(bool, Convergence) SLATE_END_ARGS()
        void Construct(const FArguments& A) { State = A._State; bConvergence = A._Convergence; }
        FVector2D ComputeDesiredSize(float) const override { return FVector2D(380, 240); }
        FReply OnMouseButtonDown(const FGeometry&,const FPointerEvent&) override {return FReply::Handled();}
        FReply OnMouseMove(const FGeometry&,const FPointerEvent&) override {return FReply::Handled();}
        int32 OnPaint(const FPaintArgs&, const FGeometry& G, const FSlateRect&, FSlateWindowElementList& Out,
            int32 Layer, const FWidgetStyle&, bool) const override
        {
            using namespace StudioUI;
            auto TextAt = [&](const FString& Value, FVector2D At, FLinearColor Color)
            { FSlateDrawElement::MakeText(Out, Layer + 2, G.ToPaintGeometry(FVector2D(1, 1), FSlateLayoutTransform(At)), Value, Font(8), ESlateDrawEffect::None, Color); };
            const auto Source=bConvergence&&State->ConvergenceEvidence?State->ConvergenceEvidence:State->Evidence;
            const auto* Prepared=Cache.Get(Source,State->SelectedSeries,bConvergence);
            if(!Prepared){TextAt(Cache.Error(),FVector2D(0,30),Muted);return Layer+3;}
            const auto& Plot=*Prepared;
            const auto Size=G.GetLocalSize();const double Left=65, Right=FMath::Max(Left+1,Size.X-15),Top=50,Bottom=FMath::Max(Top+1,Size.Y-45);
            TextAt(Plot.YLabel,FVector2D(Left,3),Text);
            TextAt(Plot.XLabel,FVector2D(Left,Bottom+23),Muted);
            TextAt(FString::Printf(TEXT("%.4g"),Plot.YMax),FVector2D(0,Top),Muted);
            TextAt(FString::Printf(TEXT("%.4g"),Plot.YMin),FVector2D(0,Bottom-10),Muted);
            TextAt(FString::Printf(TEXT("%.4g"),Plot.XMin),FVector2D(Left,Bottom+7),Muted);
            TextAt(FString::Printf(TEXT("%.4g"),Plot.XMax),FVector2D(Right-45,Bottom+7),Muted);
            TArray<FVector2D> Axes={FVector2D(Left,Top),FVector2D(Left,Bottom),FVector2D(Right,Bottom)};
            FSlateDrawElement::MakeLines(Out,Layer,G.ToPaintGeometry(),Axes,ESlateDrawEffect::None,Muted,true,1);
            for(int32 C=0;C<Plot.Channels.Num();++C)
            {
                const auto Color=C?Amber:Cyan;TextAt(Plot.Legends[C],FVector2D(Left+C*110,25),Color);
                TArray<FVector2D> Points;Points.Reserve(Plot.PreviewIndices.Num());
                for(int32 I:Plot.PreviewIndices){const auto V=Plot.Normalized(C,I);Points.Add(FVector2D(Left+(Right-Left)*V.X,Bottom-(Bottom-Top)*V.Y));}
                if(Points.Num()>1)FSlateDrawElement::MakeLines(Out,Layer+1,G.ToPaintGeometry(),Points,ESlateDrawEffect::None,Color,true,2);
                if(Points.Num()<=3)for(const auto& V:Points){TArray<FVector2D> Mark={V-FVector2D(3,0),V+FVector2D(3,0)};FSlateDrawElement::MakeLines(Out,Layer+1,G.ToPaintGeometry(),Mark,ESlateDrawEffect::None,Color,true,4);}
            }
            if(Plot.X.Num()>StudioHome4ReportPlots::PreviewLimit)TextAt(TEXT("2,000 original samples shown; full arrays retained"),FVector2D(Left,Bottom-15),Muted);
            return Layer+3;
        }
    private:
        TSharedPtr<FStudioHome4ValidationState> State;
        bool bConvergence = false;
        FStudioHome4ReferencePlotCache Cache;
    };
}
void SStudioHome4Validation::Construct(const FArguments& A)
{
    using namespace StudioUI;
    Model = A._Model; Runtime = A._Runtime; State = A._State ? A._State : MakeShared<FStudioHome4ValidationState>();ScopeState();
    auto Rows = SNew(SVerticalBox);
    auto Action = [&](const TCHAR* LabelText, FName Tag, TFunction<void()> Function)
    { return SNew(SButton).Tag(Tag).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8, 5)).OnClicked_Lambda([Function] { Function(); return FReply::Handled(); })[Label(LabelText, 9)]; };
    Rows->AddSlot().AutoHeight()[Label(TEXT("Reference evidence and convergence"), 12, Text, true)];
    Rows->AddSlot().AutoHeight().Padding(0, 5)[SNew(STextBlock).Tag(TEXT("Home4ValidationStatus")).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text_Lambda([this] { return FText::FromString(State->Status); })];
    auto Input = [&](const TCHAR* Caption, FName Tag, FString* Value)
    { Rows->AddSlot().AutoHeight().Padding(0, 3)[SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(Caption, 8, Muted)]
        + SHorizontalBox::Slot().FillWidth(1)[SNew(SEditableTextBox).Tag(Tag).Style(&InputStyle()).Font(Font(9))
            .Text_Lambda([Value] { return FText::FromString(*Value); }).OnTextChanged_Lambda([Value](const FText& T) { *Value = T.ToString(); })]]; };
    Input(TEXT("Expected run GUID (optional exact match)"), TEXT("Home4ReferenceRun"), &ExpectedRunDraft);
    Input(TEXT("Expected actual source (optional)"), TEXT("Home4ReferenceActualSource"), &ExpectedActualDraft);
    Input(TEXT("Expected reference source (optional)"), TEXT("Home4ReferenceSource"), &ExpectedReferenceDraft);
    Rows->AddSlot().AutoHeight().Padding(0, 5)[Action(TEXT("Import aligned reference JSON"), TEXT("Home4ReferenceImport"), [this] { ImportDialog(); })];
    Rows->AddSlot().AutoHeight()[SNew(STextBlock).Tag(TEXT("Home4RecipeCoverageContract")).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text_Lambda([this]{return FText::FromString(StudioHome4RecipeGates::Description(State->RecipeId));})];
    Rows->AddSlot().AutoHeight().Padding(0,6)[SNew(SHorizontalBox)
        +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,5,0)[Action(TEXT("Select independent original actual series…"),TEXT("Home4SelectActualSource"),[this]{ImportSourceDialog(false);})]
        +SHorizontalBox::Slot().FillWidth(1)[Action(TEXT("Select independent original reference series…"),TEXT("Home4SelectReferenceSource"),[this]{ImportSourceDialog(true);})]];
    Rows->AddSlot().AutoHeight()[SNew(STextBlock).Tag(TEXT("Home4IndependentSources")).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true).Text_Lambda([this]
    {
        FString Description=ActualSource?TEXT("Actual original source: ")+ActualSource->Data.ActualSource+TEXT(" · run ")+ActualSource->Data.RunId.ToString()+TEXT(" · SHA256 ")+ActualSource->Data.SourceSHA256:TEXT("Actual original source not supplied.");
        Description+=ReferenceSource?TEXT("\nReference original source: ")+ReferenceSource->Data.ReferenceSource+TEXT(" · SHA256 ")+ReferenceSource->Data.SourceSHA256:TEXT("\nReference original source not supplied.");return FText::FromString(Description);
    })];
    Rows->AddSlot().AutoHeight()[SNew(STextBlock).Tag(TEXT("Home4CouetteAnalyticContract")).Font(StudioUI::Font(8)).ColorAndOpacity(StudioUI::Muted).AutoWrapText(true).Text_Lambda([this]{return State->RecipeId==TEXT("couette-spin")?FText::FromString(TEXT("The independent reference selector accepts analytic_couette inputs. ")+StudioHome4Couette::FormulaDescription()):FText::GetEmpty();})];
    Input(TEXT("Alignment absolute tolerance (explicit)"),TEXT("Home4AlignmentAbsolute"),&AlignmentAbsoluteDraft);
    Input(TEXT("Alignment relative tolerance (explicit)"),TEXT("Home4AlignmentRelative"),&AlignmentRelativeDraft);
    Input(TEXT("Optional original alignment window start"),TEXT("Home4AlignmentStart"),&AlignmentStartDraft);
    Input(TEXT("Optional original alignment window end"),TEXT("Home4AlignmentEnd"),&AlignmentEndDraft);
    Rows->AddSlot().AutoHeight()[Action(TEXT("Align exact common original samples (no interpolation)"),TEXT("Home4AlignSources"),[this]{AlignSources();})];
    Input(TEXT("Increasing factors (comma separated)"), TEXT("Home4RefinementFactors"), &RefinementDraft);
    Rows->AddSlot().AutoHeight().Padding(0, 5)[Action(TEXT("Build refinement specifications"), TEXT("Home4ValidationBuildLadder"), [this] { BuildLadder(); })];
    Rows->AddSlot().AutoHeight()[Action(TEXT("Queue immutable ladder requests"),TEXT("Home4ValidationQueueLadder"),[this]{QueueLadder();})];
    Rows->AddSlot().AutoHeight()[SAssignNew(SeriesRows, SVerticalBox)];
    Rows->AddSlot().AutoHeight()[SNew(StudioHome4ValidationUIPrivate::SReferenceOverlay).Tag(TEXT("Home4ReferenceOverlay")).State(State).ToolTipText(FText::FromString(StudioHome4ReportPlots::SelectionDescription()))];
    Rows->AddSlot().AutoHeight().Padding(0, 4)[SNew(STextBlock).Tag(TEXT("Home4ReferenceGate")).Font(Font(9)).ColorAndOpacity(Text).AutoWrapText(true)
        .Text_Lambda([this]
        {
            using namespace StudioHome4ValidationUIPrivate;
            if (!State->Evidence || !State->Evidence->Series.IsValidIndex(State->SelectedSeries)) return FText::FromString(TEXT("not_evaluated · no reference evidence supplied."));
            const auto& E = *State->Evidence; const auto& S = E.Series[State->SelectedSeries];
            FString Description = TEXT("Evidence run ")+E.RunId.ToString(EGuidFormats::Short)+TEXT(" · ")+E.GateStatus() + TEXT(" · ") + S.Name + TEXT(" [") + S.Unit + TEXT("]\nImported evidence evaluates its identified run; the current draft has no transferred gate.\n") + S.Gate.Reason +
                FString::Printf(TEXT("\nInclusive source window: %.17g to %.17g %s\nMax absolute error "),S.Abscissae[0],S.Abscissae.Last(),*S.AbscissaUnit) + OptionalNumber(S.Gate.MaximumAbsoluteError) + TEXT(" · relative L2 error ") + OptionalNumber(S.Gate.RelativeL2Error) +
                FString::Printf(TEXT("\nSupplied tolerances: absolute %.6g, relative %.6g\nActual source: %s\nReference source: %s\nSHA256 %s"), S.AbsoluteTolerance, S.RelativeTolerance, *E.ActualSource, *E.ReferenceSource, *E.SourceSHA256)+TEXT("\nReference method ")+(E.ReferenceMethod.IsEmpty()?TEXT("not supplied"):E.ReferenceMethod);
            if (E.ObservedOrder) Description += TEXT("\nObserved order ") + OptionalNumber(E.ObservedOrder) + TEXT(" · ") + E.OrderMetric;
            else if (!E.OrderRuns.IsEmpty()) Description += TEXT("\nObserved order unavailable: scalar sequence does not show monotone convergence.");
            return FText::FromString(Description);
        })];
    Rows->AddSlot().AutoHeight()[SNew(StudioHome4ValidationUIPrivate::SReferenceOverlay).Tag(TEXT("Home4ConvergencePlot")).State(State).Convergence(true)
        .ToolTipText(FText::FromString(StudioHome4ReportPlots::SelectionDescription()))];
    Rows->AddSlot().AutoHeight()[SNew(STextBlock).Tag(TEXT("Home4ConvergenceIdentity")).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text_Lambda([this]
        {
            const auto Source=State->ConvergenceEvidence?State->ConvergenceEvidence:State->Evidence;
            if(!Source || Source->OrderRuns.IsEmpty())return FText::FromString(TEXT("No three-run scalar evidence supplied. The development queue is not measured convergence."));
            const auto& E=*Source;FString V=E.OrderMetric+TEXT(" [")+E.OrderUnit+TEXT("] · optional original extraction metadata; no cross-run window equivalence inferred.");
            for(const auto& R:E.OrderRuns)V+=TEXT("\n")+StudioHome4Validation::ScalarRunDescription(R);
            return FText::FromString(V);
        })];
    Rows->AddSlot().AutoHeight().Padding(0, 10, 0, 3)[Label(TEXT("Development refinement queue"), 11, Text, true)];
    Rows->AddSlot().AutoHeight()[SAssignNew(RungRows, SVerticalBox)];
    Rows->AddSlot().AutoHeight().Padding(0,8)[Label(TEXT("Original scalar extraction and convergence"),11,Text,true)];
    Input(TEXT("Exact metric identifier"),TEXT("Home4ExtractionMetric"),&MetricDraft);
    Input(TEXT("Value unit"),TEXT("Home4ExtractionUnit"),&UnitDraft);
    Input(TEXT("Abscissa unit"),TEXT("Home4ExtractionTimeUnit"),&TimeUnitDraft);
    Input(TEXT("Declared source epoch"),TEXT("Home4ExtractionEpoch"),&EpochDraft);
    Input(TEXT("Method: trapezoid_mean / arithmetic_mean / last"),TEXT("Home4ExtractionMethod"),&MethodDraft);
    Input(TEXT("Original sampled window start"),TEXT("Home4ExtractionStart"),&StartDraft);
    Input(TEXT("Original sampled window end"),TEXT("Home4ExtractionEnd"),&EndDraft);
    Rows->AddSlot().AutoHeight()[Action(TEXT("Apply common original extraction policy"),TEXT("Home4ApplyExtraction"),[this]{ApplyExtraction();})];
    Input(TEXT("First rung index of consecutive triplet (0 based)"),TEXT("Home4ConvergenceFirstRung"),&TripletDraft);
    Rows->AddSlot().AutoHeight()[Action(TEXT("Clear ladder attachment and results"),TEXT("Home4ClearLadder"),[this]{ClearLadder();})];
    Rows->AddSlot().AutoHeight()[Action(TEXT("Assemble original convergence triplet"),TEXT("Home4AssembleConvergence"),[this]{AssembleConvergence();})];
    Input(TEXT("New bundle folder name"), TEXT("Home4EvidenceBundleName"), &BundleName);
    Rows->AddSlot().AutoHeight().Padding(0, 5)[SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 5, 0)[Action(TEXT("Export reference evidence"), TEXT("Home4ReferenceExport"), [this] { ExportReference(); })]
        + SHorizontalBox::Slot().FillWidth(1)[Action(TEXT("Export development queue"), TEXT("Home4LadderExport"), [this] { ExportQueue(); })]];
    ChildSlot[SNew(SBorder).BorderImage(&PanelBrush).Padding(10)[Rows]]; Refresh();
}
void SStudioHome4Validation::Tick(const FGeometry& G, double At, float Delta)
{ SCompoundWidget::Tick(G, At, Delta);ScopeState(); PollImport(); Refresh(); }
void SStudioHome4Validation::ScopeState()
{
    const auto M=Model.Pin();if(!M)return;
    const FString Recipe=M->Project.Draft.Home4?M->Project.Draft.Home4->RecipeId:FString();
    if(State->ProjectId==M->Project.Id&&State->CaseId==M->Project.Draft.Id&&State->RecipeId==Recipe)return;
    State->ProjectId=M->Project.Id;State->CaseId=M->Project.Draft.Id;State->RecipeId=Recipe;
    State->Evidence.Reset();State->Ladder.Reset();State->Results.Reset();State->ExtractionPolicy.Reset();State->ConvergenceEvidence.Reset();State->SelectedSeries=0;
    ActualSource.Reset();ReferenceSource.Reset();
    State->Status=TEXT("not_evaluated · project, case or recipe changed; import evidence for the selected scope.");
    ShownRungs=INDEX_NONE;
}
bool SStudioHome4Validation::ImportPath(const FString& Path, const FStudioHome4ReferenceExpectation& Expected)
{
    if (IsImporting()) { State->Status = TEXT("A reference import is already running."); return false; }
    if (Path.IsEmpty()) { State->Status = TEXT("Reference import cancelled; previous evidence retained."); return false; }
    ScopeState();const auto M=Model.Pin();ImportProjectId=M?M->Project.Id:FGuid();ImportCaseId=M?M->Project.Draft.Id:FGuid();
    State->Status = TEXT("Reading and checking original reference evidence; previous evidence retained.");
    Pending = Async(EAsyncExecution::ThreadPool, [Path, Expected]
    { FImportResult R; StudioHome4Validation::Load(Path, Expected, R.Evidence, R.Error); return R; }); return true;
}
void SStudioHome4Validation::PollImport()
{
    ScopeState();
    if(PendingSource.IsValid()&&PendingSource.IsReady())
    {
        auto Result=MoveTemp(PendingSource.GetMutable());PendingSource={};
        if(Result.ProjectId!=State->ProjectId||Result.CaseId!=State->CaseId||Result.Recipe!=State->RecipeId)
        {State->Status=TEXT("Independent source scope changed during import; original source was not attached.");}
        else if(!Result.Error.IsEmpty())State->Status=Result.Error+TEXT(" Previous source retained.");
        else
        {
            auto Source=MakeShared<FStudioHome4SeriesSource>(MoveTemp(Result.Source));
            if(Result.bReference)ReferenceSource=Source;else ActualSource=Source;
            State->Status=TEXT("Independent original source selected. Choose explicit tolerances/window and exact-sample alignment.");
        }
    }
    if (!Pending.IsValid() || !Pending.IsReady()) return;
    auto R = MoveTemp(Pending.GetMutable()); Pending = {};
    if (!R.Error.IsEmpty()) { State->Status = R.Error + TEXT(" Previous evidence retained."); return; }
    const auto M = Model.Pin();
    if (M && (M->Project.Id!=ImportProjectId||M->Project.Draft.Id!=ImportCaseId||!M->Project.Draft.Home4 || M->Project.Draft.Home4->RecipeId != R.Evidence.RecipeId))
    {ScopeState();State->Status = TEXT("Selected project/case/recipe changed while importing; the imported gate was not attached."); return; }
    R.Evidence.AttachedProjectId=State->ProjectId;R.Evidence.AttachedCaseId=State->CaseId;
    if(State->Ladder.ContainsByPredicate([&](const auto& Rung){return Rung.PlannedRunId==R.Evidence.RunId;}))
    {
        FString AttachmentError;
        if(!StudioHome4Validation::AttachLadderResult(*State,R.Evidence,AttachmentError))
        {State->Status=AttachmentError+TEXT(" Previous evidence retained.");return;}
        if(Runtime)Runtime->AttachOriginalEvidence(R.Evidence,AttachmentError);
        if(State->ExtractionPolicy&&State->Ladder.Num()>=3)StudioHome4Validation::AssembleConvergence(*State,0,AttachmentError);
        ShownRungs=INDEX_NONE;
    }
    State->Evidence = MakeShared<FStudioHome4ReferenceEvidence>(MoveTemp(R.Evidence)); State->SelectedSeries = 0;
    State->Status = TEXT("Imported identified reference evidence · ") + State->Evidence->GateStatus(); Refresh();
}
bool SStudioHome4Validation::ImportSourcePath(const FString& Path,bool Reference)
{
    ScopeState();if(IsImporting()){State->Status=TEXT("Another original source import is running.");return false;}
    if(Path.IsEmpty()){State->Status=TEXT("Independent source selection cancelled; previous source retained.");return false;}
    const auto Project=State->ProjectId,Case=State->CaseId;const FString Recipe=State->RecipeId;
    PendingSource=Async(EAsyncExecution::ThreadPool,[Path,Recipe,Project,Case,Reference]
    {
        FSourceResult R;R.ProjectId=Project;R.CaseId=Case;R.Recipe=Recipe;R.bReference=Reference;
        if(StudioHome4ReferenceSources::Load(Path,Recipe,R.Source,R.Error)&&R.Source.bReference!=Reference)R.Error=TEXT("Selected original source kind does not match the actual/reference selector.");
        return R;
    });State->Status=TEXT("Reading independent original source in the background; previous source retained.");return true;
}
void SStudioHome4Validation::ImportSourceDialog(bool Reference)
{FString Path;if(StudioFileDialog::DataFile(false,Reference?TEXT("Select original reference series"):TEXT("Select original actual run series"),TEXT(""),TEXT("json"),Path))ImportSourcePath(Path,Reference);}
bool SStudioHome4Validation::AlignSources()
{
    ScopeState();if(!ActualSource||!ReferenceSource){State->Status=TEXT("Select both independent original actual and reference sources.");return false;}
    double Absolute=0,Relative=0;
    if(!LexTryParseString(Absolute,*AlignmentAbsoluteDraft)||!LexTryParseString(Relative,*AlignmentRelativeDraft)){State->Status=TEXT("Supply explicit finite original comparison tolerances.");return false;}
    TOptional<double> Start,End;
    if(!AlignmentStartDraft.TrimStartAndEnd().IsEmpty()||!AlignmentEndDraft.TrimStartAndEnd().IsEmpty())
    {
        double A=0,B=0;if(!LexTryParseString(A,*AlignmentStartDraft)||!LexTryParseString(B,*AlignmentEndDraft)){State->Status=TEXT("Supply both finite original alignment window bounds.");return false;}Start=A;End=B;
    }
    FStudioHome4ReferenceEvidence E;
    if(!StudioHome4ReferenceSources::AlignExact(*ActualSource,*ReferenceSource,Absolute,Relative,Start,End,E,State->Status))return false;
    return SetAlignedEvidence(MoveTemp(E));
}
bool SStudioHome4Validation::SetAlignedEvidence(FStudioHome4ReferenceEvidence E)
{
    ScopeState();FString VerifyError;if(!StudioHome4Validation::VerifyOriginalBytes(E,VerifyError)){State->Status=VerifyError;return false;}
    if(E.RecipeId!=State->RecipeId){State->Status=TEXT("Original aligned evidence belongs to another recipe.");return false;}
    E.AttachedProjectId=State->ProjectId;E.AttachedCaseId=State->CaseId;
    if(State->Ladder.ContainsByPredicate([&](const auto& Rung){return Rung.PlannedRunId==E.RunId;}))
    {
        FString Error;
        if(!StudioHome4Validation::AttachLadderResult(*State,E,Error)){State->Status=Error;return false;}
        if(Runtime)Runtime->AttachOriginalEvidence(E,Error);
        if(State->ExtractionPolicy&&State->Ladder.Num()>=3)StudioHome4Validation::AssembleConvergence(*State,0,Error);
        ShownRungs=INDEX_NONE;
    }
    State->Evidence=MakeShared<FStudioHome4ReferenceEvidence>(MoveTemp(E));State->SelectedSeries=0;
    State->Status=TEXT("Exact original-sample alignment prepared · ")+State->Evidence->GateStatus();Refresh();return true;
}
bool SStudioHome4Validation::ClearLadder()
{
    ScopeState();
    if(Runtime)for(const auto& R:State->Ladder)
    {
        FString Error;
        if(Runtime->CanCommand(R.PlannedRunId,EStudioJobCommand::Stop)&&!Runtime->Cancel(R.PlannedRunId,FPlatformTime::Seconds(),Error))
        {State->Status=Error;return false;}
    }
    State->Ladder.Reset();State->Results.Reset();State->ExtractionPolicy.Reset();State->ConvergenceEvidence.Reset();
    State->Status=TEXT("Ladder attachment cleared explicitly. Project run history and independently selected comparison remain available.");ShownRungs=INDEX_NONE;Refresh();return true;
}
bool SStudioHome4Validation::RetryRung(int32 Index)
{
    ScopeState();
    if(!Runtime||!State->Ladder.IsValidIndex(Index)){State->Status=TEXT("Select a valid planned rung and reviewed runtime target.");return false;}
    auto& R=State->Ladder[Index];
    if(State->Results.ContainsByPredicate([&](const auto& V){return V.RunId==R.PlannedRunId;}))
    {State->Status=TEXT("An original result is already retained for this rung; clear its ladder explicitly before replacing it.");return false;}
    const auto* Job=Runtime->QueueJobs().FindByPredicate([&](const auto& V){return V.RunId==R.PlannedRunId;});
    if(!Job||(Job->State!=EStudioHome4QueueState::Failed&&Job->State!=EStudioHome4QueueState::Cancelled&&Job->State!=EStudioHome4QueueState::Stopped))
    {State->Status=TEXT("Retry is available only after an explicit failed, cancelled or stopped request without attached original results.");return false;}
    const FGuid NewRun=FGuid::NewGuid();FString Error;
    if(!Runtime->Submit(R.Spec,NewRun,FPlatformProcess::UserName(),FPlatformTime::Seconds(),Error)){State->Status=Error;return false;}
    R.PlannedRunId=NewRun;State->Status=TEXT("Immutable rung retried with a new explicit run identity. Earlier request remains in project history.");ShownRungs=INDEX_NONE;Refresh();return true;
}
void SStudioHome4Validation::ImportDialog()
{
    FStudioHome4ReferenceExpectation Expected; const auto M = Model.Pin();
    if (!M || !M->Project.Draft.Home4 || M->Project.Draft.Home4->RecipeId.IsEmpty())
    { State->Status = TEXT("Apply a named HOME4 recipe before importing its reference evidence."); return; }
    Expected.RecipeId = M->Project.Draft.Home4->RecipeId; Expected.ActualSource = ExpectedActualDraft; Expected.ReferenceSource = ExpectedReferenceDraft;
    if (!ExpectedRunDraft.TrimStartAndEnd().IsEmpty())
    { FGuid Run; if (!FGuid::Parse(ExpectedRunDraft, Run) || !Run.IsValid()) { State->Status = TEXT("Expected run identity must be a valid GUID or empty."); return; } Expected.RunId = Run; }
    FString Path;
    if (!StudioFileDialog::DataFile(false, TEXT("Import identified HOME4 aligned reference evidence"), TEXT(""), TEXT("json"), Path))
    { State->Status = TEXT("Reference selection cancelled; previous evidence retained."); return; }
    ImportPath(Path, Expected);
}
bool SStudioHome4Validation::BuildLadder()
{
    ScopeState();
    const auto M = Model.Pin(); if (!M || !M->Project.Draft.Home4) { State->Status = TEXT("Apply a HOME4 specification before building a ladder."); return false; }
    TArray<FString> Parts; RefinementDraft.ParseIntoArray(Parts, TEXT(","), false); TArray<int32> Factors;
    if (Parts.IsEmpty() || Parts.Num() > 12) { State->Status = TEXT("Enter 1–12 increasing integer refinement factors."); return false; }
    for (FString Part : Parts)
    {
        Part = Part.TrimStartAndEnd();
        if (Part.IsEmpty() || Part.Len() > 2) { State->Status = TEXT("Refinement factors must be integers from 1 to 64."); return false; }
        for (TCHAR C : Part) if (C < '0' || C > '9') { State->Status = TEXT("Refinement factors must be integers from 1 to 64."); return false; }
        Factors.Add(FCString::Atoi(*Part));
    }
    TArray<FStudioHome4LadderRung> Result; FString Error;
    if (!StudioHome4Recipes::Ladder(*M->Project.Draft.Home4, Factors, Result, Error)) { State->Status = Error; return false; }
    if(!State->Results.IsEmpty()) {State->Status=TEXT("Existing original ladder evidence is retained; clear its ladder explicitly before rebuilding.");return false;}
    State->Ladder = MoveTemp(Result); State->Status = TEXT("Development queue specifications built. No solver runs were launched."); ShownRungs = INDEX_NONE; Refresh(); return true;
}
bool SStudioHome4Validation::QueueLadder()
{
    ScopeState();if(!Runtime||State->Ladder.IsEmpty()){State->Status=TEXT("Build a ladder and select/review a shared runtime target first.");return false;}
    int32 Submitted=0;
    for(const auto& Rung:State->Ladder)
    {
        if(Runtime->QueueJobs().ContainsByPredicate([&](const auto& J){return J.RunId==Rung.PlannedRunId;}))continue;
        FString Error;
        if(!Runtime->Submit(Rung.Spec,Rung.PlannedRunId,FPlatformProcess::UserName(),FPlatformTime::Seconds(),Error))
        {State->Status=Error+FString::Printf(TEXT(" %d prior immutable rung requests remain queued."),Submitted);ShownRungs=INDEX_NONE;Refresh();return false;}
        ++Submitted;
    }
    State->Status=TEXT("Ladder development requests queued. Original numerical evidence/extraction remains pending.");ShownRungs=INDEX_NONE;Refresh();return true;
}
bool SStudioHome4Validation::ApplyExtraction()
{
    ScopeState();FStudioHome4ExtractionPolicy P;P.Metric=MetricDraft;P.Unit=UnitDraft;P.AbscissaUnit=TimeUnitDraft;P.Epoch=EpochDraft;P.Method=MethodDraft;
    if(!LexTryParseString(P.WindowStart,*StartDraft)||!LexTryParseString(P.WindowEnd,*EndDraft))
    {State->Status=TEXT("Enter finite explicit source window endpoints.");return false;}
    if(!StudioHome4Validation::ApplyExtraction(*State,P,State->Status))return false;
    State->Status=TEXT("Common explicit extraction policy applied to retained original results.");ShownRungs=INDEX_NONE;Refresh();return true;
}
bool SStudioHome4Validation::AssembleConvergence()
{
    ScopeState();int32 First=0;const FString V=TripletDraft.TrimStartAndEnd();
    if(V.IsEmpty()||V.Len()>2){State->Status=TEXT("Enter a bounded nonnegative first rung index.");return false;}
    for(TCHAR C:V)if(C<'0'||C>'9'){State->Status=TEXT("First rung index must be a nonnegative integer.");return false;}
    First=FCString::Atoi(*V);
    if(!StudioHome4Validation::AssembleConvergence(*State,First,State->Status))return false;
    State->Status=TEXT("Convergence assembled from three exact original rung results and declared common extraction. Recipe gate remains independent.");Refresh();return true;
}
void SStudioHome4Validation::Refresh()
{
    if (ShownEvidence != State->Evidence.Get())
    {
        ShownEvidence = State->Evidence.Get(); SeriesRows->ClearChildren();
        if (State->Evidence) for (int32 I = 0; I < State->Evidence->Series.Num(); ++I)
        {
            const auto& S = State->Evidence->Series[I];
            SeriesRows->AddSlot().AutoHeight().Padding(0, 2)[SNew(SButton).Tag(FName(*FString::Printf(TEXT("Home4ReferenceSeries%d"), I))).ButtonStyle(&StudioUI::ButtonStyle())
                .OnClicked_Lambda([this, I] { State->SelectedSeries = I; return FReply::Handled(); })[StudioUI::Label(S.Name + TEXT(" [") + S.Unit + TEXT("]"), 9)]];
        }
    }
    if (ShownRungs != State->Ladder.Num())
    {
        ShownRungs = State->Ladder.Num(); RungRows->ClearChildren();
        if (State->Ladder.IsEmpty()) RungRows->AddSlot().AutoHeight()[StudioUI::Label(TEXT("No refinement queue built."), 8, StudioUI::Muted)];
        for (int32 I = 0; I < State->Ladder.Num(); ++I)
        {
            const auto& R = State->Ladder[I];
            const FString Cells = R.Cells ? LexToString(*R.Cells) : TEXT("Unavailable");
            const FString Memory = R.AllocationBytes ? LexToString(*R.AllocationBytes) + TEXT(" bytes") : TEXT("Unavailable");
            FString Description = FString::Printf(TEXT("%d× · %s · cells %s · allocation %s\nRun %s · lineage %s\nEstimated seconds %s\n%s"), R.Refinement, *R.Spec.Run.Tag,
                *Cells, *Memory, *R.PlannedRunId.ToString(EGuidFormats::Short), *R.Spec.LineageId, *StudioHome4ValidationUIPrivate::OptionalNumber(R.EstimatedSeconds), *R.CostBasis);
            RungRows->AddSlot().AutoHeight().Padding(0, 5)[SNew(STextBlock).Tag(FName(*FString::Printf(TEXT("Home4LadderRung%d"), I))).Font(StudioUI::Font(8)).ColorAndOpacity(StudioUI::Muted).AutoWrapText(true)
                .Text_Lambda([this,I,Description]
                {
                    if(!State->Ladder.IsValidIndex(I))return FText::GetEmpty();
                    const auto Id=State->Ladder[I].PlannedRunId;FString D=Description;
                    if(Runtime)
                    {
                        const auto* J=Runtime->QueueJobs().FindByPredicate([&](const auto& V){return V.RunId==Id;});
                        D+=J?TEXT("\nProtocol ")+FStudioHome4RuntimeSession::StateName(J->State)+TEXT(" · ")+J->Notice:TEXT("\nProtocol request not submitted.");
                    }
                    const auto* Result=State->Results.FindByPredicate([&](const auto& V){return V.RunId==Id;});
                    D+=Result?TEXT("\nScience ")+Result->Status:TEXT("\nScience pending original result; development ACK is not numerical evidence.");
                    if(Result&&Result->Scalar)D+=TEXT("\n")+StudioHome4Validation::ScalarRunDescription(*Result->Scalar);
                    return FText::FromString(D);
                }).ToolTipText(FText::FromString(StudioHome4Config::Serialize(R.Spec)))];
            const auto Run=R.PlannedRunId;auto Actions=SNew(SHorizontalBox);
            Actions->AddSlot().AutoWidth().Padding(0,0,5,0)[SNew(SButton).Tag(FName(*FString::Printf(TEXT("Home4LadderImport%d"),I))).ButtonStyle(&StudioUI::ButtonStyle())
                .OnClicked_Lambda([this,Run]{ExpectedRunDraft=Run.ToString();ImportDialog();return FReply::Handled();})[StudioUI::Label(TEXT("Attach exact original result…"),8)]];
            Actions->AddSlot().AutoWidth()[SNew(SButton).Tag(FName(*FString::Printf(TEXT("Home4LadderCancel%d"),I))).ButtonStyle(&StudioUI::ButtonStyle()).IsEnabled_Lambda([this,Run]{return Runtime&&Runtime->CanCommand(Run,EStudioJobCommand::Stop);})
                .OnClicked_Lambda([this,Run]{Runtime->Cancel(Run,FPlatformTime::Seconds(),State->Status);return FReply::Handled();})[StudioUI::Label(TEXT("Cancel protocol"),8)]];
            Actions->AddSlot().AutoWidth().Padding(5,0,0,0)[SNew(SButton).Tag(FName(*FString::Printf(TEXT("Home4LadderRetry%d"),I))).ButtonStyle(&StudioUI::ButtonStyle())
                .OnClicked_Lambda([this,I]{RetryRung(I);return FReply::Handled();})[StudioUI::Label(TEXT("Retry with new run ID"),8)]];
            RungRows->AddSlot().AutoHeight()[Actions];
        }
    }
}
void SStudioHome4Validation::ExportReference()
{
    ScopeState();
    if (!State->Evidence) { State->Status = TEXT("not_evaluated · import original reference evidence before exporting."); return; }
    FString Parent; if (!StudioFileDialog::ExportFolder(Parent)) return;
    FString Path, Error; State->Status = StudioHome4Validation::ExportEvidence(Parent, BundleName, *State->Evidence, Path, Error) ? TEXT("Reference evidence exported to ") + Path : Error;
}
void SStudioHome4Validation::ExportQueue()
{
    ScopeState();
    if (State->Ladder.IsEmpty()) { State->Status = TEXT("Build the development queue before exporting it."); return; }
    FString Parent; if (!StudioFileDialog::ExportFolder(Parent)) return;
    FString Path, Error; State->Status = StudioHome4Validation::ExportLadder(Parent, BundleName, State->Ladder, Path, Error) ? TEXT("Development queue exported to ") + Path : Error;
}
