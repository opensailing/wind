#include "SStudioHome4Validation.h"
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
            const auto* Prepared=Cache.Get(State->Evidence,State->SelectedSeries,bConvergence);
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
    Model = A._Model; State = A._State ? A._State : MakeShared<FStudioHome4ValidationState>();ScopeState();
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
                FString::Printf(TEXT("\nSupplied tolerances: absolute %.6g, relative %.6g\nActual source: %s\nReference source: %s\nSHA256 %s"), S.AbsoluteTolerance, S.RelativeTolerance, *E.ActualSource, *E.ReferenceSource, *E.SourceSHA256);
            if (E.ObservedOrder) Description += TEXT("\nObserved order ") + OptionalNumber(E.ObservedOrder) + TEXT(" · ") + E.OrderMetric;
            else if (!E.OrderRuns.IsEmpty()) Description += TEXT("\nObserved order unavailable: scalar sequence does not show monotone convergence.");
            return FText::FromString(Description);
        })];
    Rows->AddSlot().AutoHeight()[SNew(StudioHome4ValidationUIPrivate::SReferenceOverlay).Tag(TEXT("Home4ConvergencePlot")).State(State).Convergence(true)
        .ToolTipText(FText::FromString(StudioHome4ReportPlots::SelectionDescription()))];
    Rows->AddSlot().AutoHeight()[SNew(STextBlock).Tag(TEXT("Home4ConvergenceIdentity")).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text_Lambda([this]
        {
            if(!State->Evidence || State->Evidence->OrderRuns.IsEmpty())return FText::FromString(TEXT("No three-run scalar evidence supplied. The development queue is not measured convergence."));
            const auto& E=*State->Evidence;FString V=E.OrderMetric+TEXT(" [")+E.OrderUnit+TEXT("] · optional original extraction metadata; no cross-run window equivalence inferred.");
            for(const auto& R:E.OrderRuns)V+=TEXT("\n")+StudioHome4Validation::ScalarRunDescription(R);
            return FText::FromString(V);
        })];
    Rows->AddSlot().AutoHeight().Padding(0, 10, 0, 3)[Label(TEXT("Development refinement queue"), 11, Text, true)];
    Input(TEXT("Increasing factors (comma separated)"), TEXT("Home4RefinementFactors"), &RefinementDraft);
    Rows->AddSlot().AutoHeight().Padding(0, 5)[Action(TEXT("Build refinement specifications"), TEXT("Home4ValidationBuildLadder"), [this] { BuildLadder(); })];
    Rows->AddSlot().AutoHeight()[SAssignNew(RungRows, SVerticalBox)];
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
    State->Evidence.Reset();State->Ladder.Reset();State->SelectedSeries=0;
    State->Status=TEXT("not_evaluated · project, case or recipe changed; import evidence for the selected scope.");
    ShownRungs=INDEX_NONE;
}
bool SStudioHome4Validation::ImportPath(const FString& Path, const FStudioHome4ReferenceExpectation& Expected)
{
    if (Pending.IsValid()) { State->Status = TEXT("A reference import is already running."); return false; }
    if (Path.IsEmpty()) { State->Status = TEXT("Reference import cancelled; previous evidence retained."); return false; }
    ScopeState();const auto M=Model.Pin();ImportProjectId=M?M->Project.Id:FGuid();ImportCaseId=M?M->Project.Draft.Id:FGuid();
    State->Status = TEXT("Reading and checking original reference evidence; previous evidence retained.");
    Pending = Async(EAsyncExecution::ThreadPool, [Path, Expected]
    { FImportResult R; StudioHome4Validation::Load(Path, Expected, R.Evidence, R.Error); return R; }); return true;
}
void SStudioHome4Validation::PollImport()
{
    ScopeState();
    if (!Pending.IsValid() || !Pending.IsReady()) return;
    auto R = MoveTemp(Pending.GetMutable()); Pending = {};
    if (!R.Error.IsEmpty()) { State->Status = R.Error + TEXT(" Previous evidence retained."); return; }
    const auto M = Model.Pin();
    if (M && (M->Project.Id!=ImportProjectId||M->Project.Draft.Id!=ImportCaseId||!M->Project.Draft.Home4 || M->Project.Draft.Home4->RecipeId != R.Evidence.RecipeId))
    {ScopeState();State->Status = TEXT("Selected project/case/recipe changed while importing; the imported gate was not attached."); return; }
    R.Evidence.AttachedProjectId=State->ProjectId;R.Evidence.AttachedCaseId=State->CaseId;
    State->Evidence = MakeShared<FStudioHome4ReferenceEvidence>(MoveTemp(R.Evidence)); State->SelectedSeries = 0;
    State->Status = TEXT("Imported identified reference evidence · ") + State->Evidence->GateStatus(); Refresh();
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
    State->Ladder = MoveTemp(Result); State->Status = TEXT("Development queue specifications built. No solver runs were launched."); ShownRungs = INDEX_NONE; Refresh(); return true;
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
                .Text(FText::FromString(Description)).ToolTipText(FText::FromString(StudioHome4Config::Serialize(R.Spec)))];
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
