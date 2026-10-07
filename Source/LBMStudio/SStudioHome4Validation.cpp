#include "SStudioHome4Validation.h"
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
        SLATE_BEGIN_ARGS(SReferenceOverlay) {} SLATE_ARGUMENT(TSharedPtr<FStudioHome4ValidationState>, State) SLATE_END_ARGS()
        void Construct(const FArguments& A) { State = A._State; }
        FVector2D ComputeDesiredSize(float) const override { return FVector2D(380, 240); }
        FReply OnMouseButtonDown(const FGeometry&,const FPointerEvent&) override {return FReply::Handled();}
        FReply OnMouseMove(const FGeometry&,const FPointerEvent&) override {return FReply::Handled();}
        int32 OnPaint(const FPaintArgs&, const FGeometry& G, const FSlateRect&, FSlateWindowElementList& Out,
            int32 Layer, const FWidgetStyle&, bool) const override
        {
            using namespace StudioUI;
            auto TextAt = [&](const FString& Value, FVector2D At, FLinearColor Color)
            { FSlateDrawElement::MakeText(Out, Layer + 2, G.ToPaintGeometry(FVector2D(1, 1), FSlateLayoutTransform(At)), Value, Font(8), ESlateDrawEffect::None, Color); };
            if (!State->Evidence || !State->Evidence->Series.IsValidIndex(State->SelectedSeries))
            { TextAt(TEXT("not_evaluated · import actual and reference measurements"), FVector2D(0, 30), Muted); return Layer + 3; }
            const auto& S = State->Evidence->Series[State->SelectedSeries];
            const auto Size = G.GetLocalSize(); const double Left = 55, Right = FMath::Max(Left + 1, Size.X - 10), Top = 30, Bottom = Size.Y - 30;
            double Low = TNumericLimits<double>::Max(), High = -TNumericLimits<double>::Max();
            for (double V : S.Actual) { Low = FMath::Min(Low, V); High = FMath::Max(High, V); }
            for (double V : S.Reference) { Low = FMath::Min(Low, V); High = FMath::Max(High, V); }
            const double Span = High - Low, XSpan = S.Abscissae.Last() - S.Abscissae[0];
            if (!FMath::IsFinite(Span) || !FMath::IsFinite(XSpan))
            { TextAt(TEXT("Source range exceeds finite chart scaling"), FVector2D(0, 30), Muted); return Layer + 3; }
            const double Pad = FMath::Max(Span * .05, FMath::Max(FMath::Abs(Low), FMath::Abs(High)) * .001 + 1e-12);
            Low -= Pad; High += Pad;
            if (!FMath::IsFinite(Low) || !FMath::IsFinite(High) || !FMath::IsFinite(High - Low) || High <= Low)
            { TextAt(TEXT("Source range exceeds finite chart scaling"), FVector2D(0, 30), Muted); return Layer + 3; }
            TextAt(TEXT("Actual · ") + S.Unit, FVector2D(Left, 3), Cyan);
            TextAt(TEXT("Reference · ") + S.Unit, FVector2D(Left + 130, 3), Amber);
            TextAt(FString::Printf(TEXT("%.4g"), High), FVector2D(0, Top), Muted);
            TextAt(FString::Printf(TEXT("%.4g"), Low), FVector2D(0, Bottom - 10), Muted);
            TextAt(S.AbscissaName + TEXT(" [") + S.AbscissaUnit + TEXT("]"), FVector2D(Left, Bottom + 8), Muted);
            for (int32 Channel = 0; Channel < 2; ++Channel)
            {
                const auto& Values = Channel ? S.Reference : S.Actual;
                TArray<FVector2D> Points; Points.Reserve(FMath::Min(Values.Num(),2001));
                const int32 Stride=FMath::Max(1,(Values.Num()+1999)/2000);
                for (int32 I = 0; I < Values.Num(); I+=Stride)
                    Points.Add(FVector2D(Left + (Right - Left) * (S.Abscissae[I] - S.Abscissae[0]) / FMath::Max(1e-12, XSpan),
                        Bottom - (Bottom - Top) * (Values[I] - Low) / (High - Low)));
                if(Values.Num()>2000)TextAt(TEXT("Plot preview strided; gate uses every original sample"),FVector2D(Left,Bottom-13),Muted);
                if (Points.Num() > 1) FSlateDrawElement::MakeLines(Out, Layer + Channel, G.ToPaintGeometry(), Points, ESlateDrawEffect::None, Channel ? Amber : Cyan, true, Channel ? 1.f : 2.f);
                else if (Points.Num() == 1)
                { Points.Add(Points[0] + FVector2D(3, 0)); FSlateDrawElement::MakeLines(Out, Layer + Channel, G.ToPaintGeometry(), Points, ESlateDrawEffect::None, Channel ? Amber : Cyan, true, 3); }
            }
            return Layer + 3;
        }
    private:
        TSharedPtr<FStudioHome4ValidationState> State;
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
    Rows->AddSlot().AutoHeight()[SNew(StudioHome4ValidationUIPrivate::SReferenceOverlay).Tag(TEXT("Home4ReferenceOverlay")).State(State)];
    Rows->AddSlot().AutoHeight().Padding(0, 4)[SNew(STextBlock).Tag(TEXT("Home4ReferenceGate")).Font(Font(9)).ColorAndOpacity(Text).AutoWrapText(true)
        .Text_Lambda([this]
        {
            using namespace StudioHome4ValidationUIPrivate;
            if (!State->Evidence || !State->Evidence->Series.IsValidIndex(State->SelectedSeries)) return FText::FromString(TEXT("not_evaluated · no reference evidence supplied."));
            const auto& E = *State->Evidence; const auto& S = E.Series[State->SelectedSeries];
            FString Description = TEXT("Evidence run ")+E.RunId.ToString(EGuidFormats::Short)+TEXT(" · ")+E.GateStatus() + TEXT(" · ") + S.Name + TEXT(" [") + S.Unit + TEXT("]\nImported evidence evaluates its identified run; the current draft has no transferred gate.\n") + S.Gate.Reason +
                TEXT("\nMax absolute error ") + OptionalNumber(S.Gate.MaximumAbsoluteError) + TEXT(" · relative L2 error ") + OptionalNumber(S.Gate.RelativeL2Error) +
                FString::Printf(TEXT("\nSupplied tolerances: absolute %.6g, relative %.6g\nActual source: %s\nReference source: %s\nSHA256 %s"), S.AbsoluteTolerance, S.RelativeTolerance, *E.ActualSource, *E.ReferenceSource, *E.SourceSHA256);
            if (E.ObservedOrder) Description += TEXT("\nObserved order ") + OptionalNumber(E.ObservedOrder) + TEXT(" · ") + E.OrderMetric;
            else if (!E.OrderRuns.IsEmpty()) Description += TEXT("\nObserved order unavailable: scalar sequence does not show monotone convergence.");
            return FText::FromString(Description);
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
