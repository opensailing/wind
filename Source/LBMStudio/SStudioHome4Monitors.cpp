#include "SStudioHome4Monitors.h"
#include "StudioModel.h"
#include "StudioFileDialog.h"
#include "StudioTheme.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SLeafWidget.h"
#include "Rendering/DrawElements.h"
#include "InputCoreTypes.h"
#include <cerrno>
#include <cstdlib>

namespace StudioHome4MonitorPrivate
{
    FString Number(const TOptional<double>& V, const TCHAR* Unit = TEXT(""))
    { return V ? FString::Printf(TEXT("%.6g%s%s"), *V, *Unit ? TEXT(" ") : TEXT(""), Unit) : TEXT("Unavailable"); }
    FString Count(const TOptional<int64>& V)
    { return V ? FString::Printf(TEXT("%lld"), *V) : TEXT("Unavailable"); }
    FString Fact(const TOptional<bool>& V)
    { return V ? (*V ? TEXT("yes") : TEXT("no")) : TEXT("unavailable"); }
    FString Cell(const TOptional<FIntVector>& V)
    { return V ? FString::Printf(TEXT("[%d, %d, %d]"), V->X, V->Y, V->Z) : TEXT("Unavailable"); }
    const TCHAR* KindName(EStudioHome4OutputKind Kind)
    {
        switch (Kind)
        {
        case EStudioHome4OutputKind::Trace: return TEXT("Trace");
        case EStudioHome4OutputKind::Slice: return TEXT("Slice");
        case EStudioHome4OutputKind::Visualization: return TEXT("Visualization snapshot");
        case EStudioHome4OutputKind::Restart: return TEXT("Restart state");
        }
        return TEXT("Unknown");
    }
    FString BudgetText(const FStudioHome4EnergyBudget& B)
    {
        return TEXT("W ") + Number(B.Work) + TEXT(" · D near/far/air ") + Number(B.DissipationNear) + TEXT(" / ") +
            Number(B.DissipationFar) + TEXT(" / ") + Number(B.DissipationAir) + TEXT("\nBeach/floor ") +
            Number(B.BeachLoss) + TEXT(" / ") + Number(B.FloorLoss) + TEXT(" · ΔKE/ΔPE ") + Number(B.DeltaKE) +
            TEXT(" / ") + Number(B.DeltaPE) + TEXT(" · reported residual ") + Number(B.Residual);
    }
    bool PolicyNumber(const FString& Text, TOptional<double>& Out)
    {
        const FString Trimmed = Text.TrimStartAndEnd();
        if (Trimmed.IsEmpty()) { Out.Reset(); return true; }
        if (Trimmed.Len() > 64) return false;
        int32 I = 0, Digits = 0;
        auto Digit = [](TCHAR C) { return C >= '0' && C <= '9'; };
        if (Trimmed[I] == '+') ++I;
        while (I < Trimmed.Len() && Digit(Trimmed[I])) { ++I; ++Digits; }
        if (I < Trimmed.Len() && Trimmed[I] == '.')
        { ++I; while (I < Trimmed.Len() && Digit(Trimmed[I])) { ++I; ++Digits; } }
        if (!Digits) return false;
        if (I < Trimmed.Len() && (Trimmed[I] == 'e' || Trimmed[I] == 'E'))
        {
            ++I;
            if (I < Trimmed.Len() && (Trimmed[I] == '+' || Trimmed[I] == '-')) ++I;
            int32 Exponents = 0;
            while (I < Trimmed.Len() && Digit(Trimmed[I])) { ++I; ++Exponents; }
            if (!Exponents) return false;
        }
        if (I != Trimmed.Len()) return false;
        const FTCHARToUTF8 UTF8(*Trimmed);
        char* End = nullptr; errno = 0;
        const double N = std::strtod(UTF8.Get(), &End);
        if (errno == ERANGE || End != UTF8.Get() + UTF8.Length() || !FMath::IsFinite(N) || N < 0) return false;
        Out = N; return true;
    }
    class SHome4HistoryPlot final : public SLeafWidget
    {
    public:
        SLATE_BEGIN_ARGS(SHome4HistoryPlot) : _Metric(0) {}
            SLATE_ARGUMENT(TFunction<const FStudioHome4TelemetryStream*()>, Stream)
            SLATE_ARGUMENT(int32, Metric)
        SLATE_END_ARGS()
        void Construct(const FArguments& A) { Read = A._Stream; Metric = A._Metric; }
        FVector2D ComputeDesiredSize(float) const override { return FVector2D(300, 90); }
        int32 OnPaint(const FPaintArgs&, const FGeometry& G, const FSlateRect&, FSlateWindowElementList& Out,
            int32 Layer, const FWidgetStyle&, bool) const override
        {
            using namespace StudioUI;
            auto TextAt = [&](const FString& Value, FVector2D At)
            { FSlateDrawElement::MakeText(Out, Layer + 1, G.ToPaintGeometry(FVector2D(1, 1), FSlateLayoutTransform(At)), Value, Font(8), ESlateDrawEffect::None, Muted); };
            const auto* Stream = Read();
            if (!Stream || Stream->History().Num() < 2)
            { TextAt(TEXT("History unavailable · at least two original samples required"), FVector2D(0, 20)); return Layer + 2; }
            const auto& H = Stream->History();
            auto Value = [&](const FStudioHome4Sample& S) -> TOptional<double>
            { if (S.bNonfinite) return {}; return Metric == 0 ? S.Mass.PhiDrift : Metric == 1 ? S.Budget.Residual : S.Forces.Fx; };
            bool bSteps = true;
            double Low = TNumericLimits<double>::Max(), High = -TNumericLimits<double>::Max();
            for (const auto& S : H)
            {
                bSteps &= S.Step.IsSet();
                if (const auto V = Value(S)) { Low = FMath::Min(Low, *V); High = FMath::Max(High, *V); }
            }
            if (Low == TNumericLimits<double>::Max())
            { TextAt(TEXT("Measured history unavailable"), FVector2D(0, 20)); return Layer + 2; }
            const double Range = High - Low;
            if (!FMath::IsFinite(Range))
            { TextAt(TEXT("Source range exceeds finite chart scale"), FVector2D(0, 20)); return Layer + 2; }
            const double Pad = FMath::Max(FMath::Max(FMath::Abs(Low), FMath::Abs(High)) * .05, 1e-12);
            Low -= Pad; High += Pad;
            auto X = [&](const FStudioHome4Sample& S) { return bSteps ? double(*S.Step) : double(S.RecordIndex); };
            const double Start = X(H[0]), End = X(H.Last());
            const auto Size = G.GetLocalSize();
            const double Left = 52, Right = FMath::Max(Left + 1, Size.X - 3), Top = 4, Bottom = Size.Y - 19;
            for (int32 I = 0; I < 2; ++I)
            {
                const double Y = I ? Bottom : Top;
                TArray<FVector2D> Points{{Left, Y}, {Right, Y}};
                FSlateDrawElement::MakeLines(Out, Layer, G.ToPaintGeometry(), Points, ESlateDrawEffect::None, Muted.CopyWithNewOpacity(.2), true, 1);
                TextAt(FString::Printf(TEXT("%.3g"), I ? Low : High), FVector2D(0, FMath::Max(0., Y - 5)));
            }
            TextAt(FString::Printf(TEXT("%.0f"), Start), FVector2D(Left, Bottom + 3));
            TextAt(bSteps ? TEXT("solver step") : TEXT("original record order"), FVector2D(FMath::Max(Left, Right - 110), Bottom + 3));
            TArray<FVector2D> Points;
            auto Flush = [&]
            { if (Points.Num() > 1) FSlateDrawElement::MakeLines(Out, Layer + 1, G.ToPaintGeometry(), Points, ESlateDrawEffect::None, Cyan, true, 1.5); Points.Reset(); };
            for (const auto& S : H)
            {
                const auto V = Value(S);
                if (!V) { Flush(); continue; }
                Points.Add(FVector2D(Left + (Right - Left) * (X(S) - Start) / FMath::Max(1., End - Start),
                    Bottom - (Bottom - Top) * (*V - Low) / (High - Low)));
            }
            Flush(); return Layer + 2;
        }
    private:
        TFunction<const FStudioHome4TelemetryStream*()> Read;
        int32 Metric = 0;
    };
}

void SStudioHome4Monitors::Construct(const FArguments& A)
{
    using namespace StudioUI;
    using namespace StudioHome4MonitorPrivate;
    Model = A._Model; SessionStream = A._Stream; OnLocate = A._OnLocateCell;
    if (const auto M = Model.Pin()) ScopedProjectId = M->Project.Id;
    PolicyDraft.SetNum(8);
    Status = TEXT("Choose an original HOME4 JSONL log, or connect a session science stream.");
    auto Rows = SNew(SVerticalBox);
    Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 8)[SNew(STextBlock).Tag(TEXT("Home4MonitorSource")).Font(Font(9)).ColorAndOpacity(Muted).AutoWrapText(true)
        .Text_Lambda([this] { return FText::FromString(SourceText()); })];
    for (int32 I = 0; I < 5; ++I)
    {
        const FName Tag(*FString::Printf(TEXT("Home4Health%d"), I));
        Rows->AddSlot().AutoHeight().Padding(0, 5, 0, 5)[SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Tag(Tag).Font(Font(11, true))
                .ColorAndOpacity_Lambda([this, I] { const auto H = Health(I); return FSlateColor(H.Status == EStudioHome4Health::Healthy ? Cyan : H.Status == EStudioHome4Health::Warning ? Amber : Muted); })
                .Text_Lambda([this, I] { const auto H = Health(I); const TCHAR* State = H.Status == EStudioHome4Health::Healthy ? TEXT("Healthy") : H.Status == EStudioHome4Health::Warning ? TEXT("Warning") : TEXT("Unavailable");
                    return FText::FromString(H.Label + TEXT(" · ") + State + (H.Value ? TEXT(" · ") + Number(H.Value) : TEXT(""))); })]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 2)[SNew(STextBlock).Font(Font(9)).ColorAndOpacity(Text).AutoWrapText(true)
                .Text_Lambda([this, I] { const auto H = Health(I); return FText::FromString(H.Reason + (H.Threshold ? TEXT(" Threshold: ") + Number(H.Threshold) : TEXT(""))); })]
            + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true)
                .Text_Lambda([this, I] { return FText::FromString(Health(I).Remedy); })]];
    }
    auto Heading = [&](const TCHAR* TextValue) { Rows->AddSlot().AutoHeight().Padding(0, 14, 0, 5)[Label(TextValue, 11, Text, true)]; };
    auto Detail = [&](const TCHAR* Title, FName Key)
    {
        Heading(Title);
        Rows->AddSlot().AutoHeight()[SNew(STextBlock).Tag(FName(*(TEXT("Home4Detail_") + Key.ToString()))).Font(Font(9)).ColorAndOpacity(Text).AutoWrapText(true)
            .Text_Lambda([this, Key] { return FText::FromString(DetailText(Key)); })];
    };
    Detail(TEXT("Mass ledgers and correction injections"), TEXT("Mass"));
    Rows->AddSlot().AutoHeight().Padding(0, 5)[SNew(SHome4HistoryPlot).Stream([this] { return DisplayStream(); }).Metric(0)];
    Detail(TEXT("Energy terms · reported units and interval"), TEXT("Budget"));
    Rows->AddSlot().AutoHeight().Padding(0, 5)[SNew(SHome4HistoryPlot).Stream([this] { return DisplayStream(); }).Metric(1)];
    Detail(TEXT("Independent force channels"), TEXT("Forces"));
    Rows->AddSlot().AutoHeight().Padding(0, 5)[SNew(SHome4HistoryPlot).Stream([this] { return DisplayStream(); }).Metric(2)];
    Detail(TEXT("Current [previous] averaging window"), TEXT("Window"));
    Detail(TEXT("Measured work and bandwidth"), TEXT("Performance"));
    Detail(TEXT("Safeguards and extrema"), TEXT("Safeguards"));
    Detail(TEXT("Trouble locator · original cell facts"), TEXT("Trouble"));
    Rows->AddSlot().AutoHeight().Padding(0, 6)[SNew(SButton).Tag(TEXT("Home4LocateCell")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8, 5))
        .IsEnabled_Lambda([this] { return CanLocate(); }).OnClicked_Lambda([this] { Locate(); return FReply::Handled(); })[Label(TEXT("Locate reported cell"), 9)]];
    Heading(TEXT("Four output kinds · driver reported paths"));
    Rows->AddSlot().AutoHeight()[SAssignNew(OutputRows, SVerticalBox).Tag(TEXT("Home4OutputTimeline"))];
    Heading(TEXT("Health thresholds · enter explicit reference and tolerance"));
    Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 5)[SNew(STextBlock).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true).Text(FText::FromString(TEXT("Mass: |drift| < 1e-4. Other gates remain unavailable until you supply their limits.")))];
    const TCHAR* Captions[] = {TEXT("Budget absolute tolerance"), TEXT("Force relative tolerance"), TEXT("Force absolute tolerance"),
        TEXT("Force reference magnitude"), TEXT("Window relative tolerance"), TEXT("Window absolute tolerance"), TEXT("Window reference magnitude"), TEXT("WB rest pressure tolerance")};
    for (int32 I = 0; I < UE_ARRAY_COUNT(Captions); ++I)
        Rows->AddSlot().AutoHeight().Padding(0, 3)[SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(Captions[I], 9, Muted)]
            + SHorizontalBox::Slot().AutoWidth()[SNew(SBox).WidthOverride(120)[SNew(SEditableTextBox).Tag(FName(*FString::Printf(TEXT("Home4Policy%d"), I)))
                .Style(&InputStyle()).Font(Font(9)).HintText(FText::FromString(TEXT("Unavailable")))
                .Text_Lambda([this, I] { return FText::FromString(PolicyDraft[I]); }).OnTextChanged_Lambda([this, I](const FText& T) { PolicyDraft[I] = T.ToString(); })]]];
    for (bool bForce : {true, false})
    {
        Rows->AddSlot().AutoHeight().Padding(0, 7, 0, 3)[Label(bForce ? TEXT("Force comparison component") : TEXT("Window comparison component"), 9, Muted)];
        auto Components = SNew(SHorizontalBox);
        const TCHAR* Names[] = {TEXT("Fx"), TEXT("Fy"), TEXT("Fz"), TEXT("My")};
        for (int32 I = 0; I < 4; ++I)
            Components->AddSlot().FillWidth(1).Padding(0, 0, 5, 0)[SNew(SButton).Tag(FName(*FString::Printf(TEXT("Home4%sComponent%d"), bForce ? TEXT("Force") : TEXT("Window"), I)))
                .ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(6, 4)).OnClicked_Lambda([this, bForce, I] {
                    (bForce ? Policy.ForceComponent : Policy.WindowComponent) = FStudioHome4DiagnosticPolicy::EComponent(I); return FReply::Handled(); })
                [SNew(STextBlock).Font(Font(9)).Text(FText::FromString(Names[I])).ColorAndOpacity_Lambda([this, bForce, I] {
                    return FSlateColor(int32(bForce ? Policy.ForceComponent : Policy.WindowComponent) == I ? Cyan : Muted); })]];
        Rows->AddSlot().AutoHeight()[Components];
    }
    Rows->AddSlot().AutoHeight().Padding(0, 7, 0, 12)[SNew(SButton).Tag(TEXT("Home4ApplyPolicy")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8, 5))
        .OnClicked_Lambda([this] { ApplyPolicy(); return FReply::Handled(); })[Label(TEXT("Apply health thresholds"), 9)]];
    ChildSlot[SNew(SBorder).BorderImage(&PanelBrush).Padding(14)[SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()[Label(TEXT("HOME4 Monitors"), 17, Text, true)]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)[SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[Label(TEXT("Original run ID (optional GUID)"), 9, Muted)]
            + SHorizontalBox::Slot().FillWidth(1)[SNew(SEditableTextBox).Tag(TEXT("Home4OriginalRunId")).Style(&InputStyle()).Font(Font(9))
                .HintText(FText::FromString(TEXT("Replay stays unbound when omitted")))
                .Text_Lambda([this] { return FText::FromString(OriginalRunIdDraft); })
                .OnTextChanged_Lambda([this](const FText& T) { OriginalRunIdDraft = T.ToString(); })]]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 8)[SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Tag(TEXT("Home4ImportTelemetry")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8, 5))
                .IsEnabled_Lambda([this] { return !IsImporting(); }).OnClicked_Lambda([this] { ImportDialog(); return FReply::Handled(); })[Label(TEXT("Import JSONL"), 9)]]
            + SHorizontalBox::Slot().AutoWidth().Padding(6, 0)[SNew(SButton).Tag(TEXT("Home4CancelTelemetryImport")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8, 5))
                .IsEnabled_Lambda([this] { return IsImporting(); }).OnClicked_Lambda([this] { CancelImport(); return FReply::Handled(); })[Label(TEXT("Cancel read"), 9)]]
            + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Tag(TEXT("Home4SessionTelemetry")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8, 5))
                .IsEnabled_Lambda([this] { return SessionStream.IsValid(); }).OnClicked_Lambda([this] { bShowImported = false; RefreshOutputs(); return FReply::Handled(); })[Label(TEXT("Session stream"), 9)]]
            + SHorizontalBox::Slot().AutoWidth().Padding(6, 0)[SNew(SButton).Tag(TEXT("Home4ReplayTelemetry")).ButtonStyle(&ButtonStyle()).ContentPadding(FMargin(8, 5))
                .IsEnabled_Lambda([this] { return ImportedStream.IsValid(); }).OnClicked_Lambda([this] { bShowImported = true; RefreshOutputs(); return FReply::Handled(); })[Label(TEXT("Imported replay"), 9)]]]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)[SNew(STextBlock).Tag(TEXT("Home4MonitorStatus")).Font(Font(8)).ColorAndOpacity(Muted).AutoWrapText(true)
            .Text_Lambda([this] { return FText::FromString(Status); })]
        + SVerticalBox::Slot().FillHeight(1)[SAssignNew(Scroll, SScrollBox).Tag(TEXT("Home4MonitorScroll"))
            .ScrollWhenFocusChanges(EScrollWhenFocusChanges::InstantScroll) + SScrollBox::Slot()[Rows]]]];
    RefreshOutputs();
}

SStudioHome4Monitors::~SStudioHome4Monitors() { CancelImport(); }
void SStudioHome4Monitors::Tick(const FGeometry& G, double At, float Delta)
{ SCompoundWidget::Tick(G, At, Delta); ScopeProject(); PollImport(); RefreshOutputs(); }
FReply SStudioHome4Monitors::OnKeyDown(const FGeometry& G, const FKeyEvent& E)
{
    if (E.GetKey() == EKeys::Home) { Scroll->ScrollToStart(); return FReply::Handled(); }
    if (E.GetKey() == EKeys::End) { Scroll->ScrollToEnd(); return FReply::Handled(); }
    if (E.GetKey() == EKeys::PageUp || E.GetKey() == EKeys::PageDown)
    { Scroll->SetScrollOffset(FMath::Max(0.f, Scroll->GetScrollOffset() + (E.GetKey() == EKeys::PageDown ? 1.f : -1.f) * Scroll->GetCachedGeometry().GetLocalSize().Y * .8f)); return FReply::Handled(); }
    return SCompoundWidget::OnKeyDown(G, E);
}
FStudioHome4TelemetryStream* SStudioHome4Monitors::DisplayStream() const
{ return IsImportedReplay() ? ImportedStream.Get() : SessionStream.Get(); }
TOptional<FGuid> SStudioHome4Monitors::OriginalRunIdentity() const
{
    if (IsImportedReplay() && !bImportedOriginalRunIdentity) return {};
    const auto* Stream = DisplayStream();
    if (Stream && Stream->Latest()) return Stream->Latest()->Source.RunId;
    if (Stream && !Stream->OutputEvents().IsEmpty()) return Stream->OutputEvents().Last().Source.RunId;
    return {};
}
const FStudioHome4Sample* SStudioHome4Monitors::Sample() const
{ const auto* Stream = DisplayStream(); return Stream && Stream->Latest() ? &*Stream->Latest() : nullptr; }
FStudioHome4HealthSignal SStudioHome4Monitors::Health(int32 Index) const
{ const auto* S = Sample(); const auto H = FStudioHome4Diagnostics::Evaluate(S ? *S : FStudioHome4Sample(), Policy); return H[Index]; }
FString SStudioHome4Monitors::SourceText() const
{
    using namespace StudioHome4MonitorPrivate;
    const auto* S = Sample();
    if (!S)
    {
        const auto M = Model.Pin();
        return M && M->Job().Capabilities().bControlHarness ? TEXT("Science telemetry unavailable. The control harness supplies no solver measurements. Import an original JSONL log to inspect replay data.") : TEXT("Science telemetry unavailable. No HOME4 numerical measurements have been supplied.");
    }
    return FString::Printf(TEXT("%s · %s\nRun %s · source %s\nStep %s · t lattice %s · t physical %s · t* %s%s"),
        IsImportedReplay() ? TEXT("Imported replay") : TEXT("Session measurements"), S->Backend.IsEmpty() ? TEXT("Backend unavailable") : *S->Backend,
        *S->Source.RunId.ToString(), *S->Source.SourceId, *Count(S->Step), *Number(S->LatticeTime), *Number(S->PhysicalTime, TEXT("s")),
        *Number(S->DimensionlessTime), IsImportedReplay() ? *FString(TEXT("\nOriginal log: ") + ImportPath +
            (bImportedOriginalRunIdentity ? TEXT("\nOriginal run ID supplied by owner; spatial binding requires an exact original-grid match.") : TEXT("\nIndependent replay identity; original run ID unavailable, spatial binding disabled.")) +
            TEXT("\nRecovery records are historical; importing never stops or checkpoints a job.")) : TEXT(""));
}
FString SStudioHome4Monitors::DetailText(FName Key) const
{
    using namespace StudioHome4MonitorPrivate;
    const auto* S = Sample();
    if (!S) return TEXT("Unavailable · requires an original science measurement.");
    if (Key == TEXT("Mass"))
    {
        FString Text = TEXT("Root drift ") + Number(S->Mass.PhiDrift) + TEXT(" · injected ") + Number(S->Mass.Injected) +
            TEXT(" · change in injection magnitude ") + Number(S->Mass.InjectionMagnitudeChange);
        const int32 N = FMath::Max(S->Mass.LevelDrifts.Num(), S->Mass.LevelInjections.Num());
        for (int32 I = 0; I < N; ++I)
            Text += FString::Printf(TEXT("\nLevel %d · drift %s · injected %s"), I,
                *Number(S->Mass.LevelDrifts.IsValidIndex(I) ? S->Mass.LevelDrifts[I] : TOptional<double>()),
                *Number(S->Mass.LevelInjections.IsValidIndex(I) ? S->Mass.LevelInjections[I] : TOptional<double>()));
        return Text;
    }
    if (Key == TEXT("Budget"))
    {
        FString Text = BudgetText(S->Budget);
        TArray<FString> Phases; S->PhaseBudgets.GetKeys(Phases); Phases.Sort();
        for (const auto& Phase : Phases) Text += TEXT("\n") + Phase + TEXT("\n") + BudgetText(S->PhaseBudgets[Phase]);
        return Text + TEXT("\nKE water/air ") + Number(S->WaterKE) + TEXT(" / ") + Number(S->AirKE) + TEXT(" · surface energy ") + Number(S->SurfaceEnergy);
    }
    if (Key == TEXT("Forces"))
        return TEXT("Stress Fx/Fy/Fz: ") + Number(S->Forces.Fx) + TEXT(" / ") + Number(S->Forces.Fy) + TEXT(" / ") + Number(S->Forces.Fz) +
            TEXT(" · My ") + Number(S->Forces.My) + TEXT("\nMomentum Fx/Fy/Fz: ") + Number(S->Forces.MomentumFx) + TEXT(" / ") +
            Number(S->Forces.MomentumFy) + TEXT(" / ") + Number(S->Forces.MomentumFz) + TEXT(" · My ") + Number(S->Forces.MomentumMy) +
            TEXT("\nPressure Fx/Fy/Fz: ") + Number(S->Forces.PressureFx) + TEXT(" / ") + Number(S->Forces.PressureFy) + TEXT(" / ") + Number(S->Forces.PressureFz) +
            TEXT("\nViscous Fx/Fy/Fz: ") + Number(S->Forces.ViscousFx) + TEXT(" / ") + Number(S->Forces.ViscousFy) + TEXT(" / ") + Number(S->Forces.ViscousFz);
    if (Key == TEXT("Window"))
        return TEXT("Fx ") + Number(S->Window.Fx) + TEXT(" [") + Number(S->Window.PreviousFx) + TEXT("] · Fy ") + Number(S->Window.Fy) + TEXT(" [") + Number(S->Window.PreviousFy) +
            TEXT("]\nFz ") + Number(S->Window.Fz) + TEXT(" [") + Number(S->Window.PreviousFz) + TEXT("] · My ") + Number(S->Window.My) + TEXT(" [") + Number(S->Window.PreviousMy) +
            TEXT("]\nInterval ") + Number(S->Window.Start) + TEXT("–") + Number(S->Window.End) + TEXT(" [") + Number(S->Window.PreviousStart) + TEXT("–") + Number(S->Window.PreviousEnd) + TEXT("]");
    if (Key == TEXT("Performance"))
    {
        const auto P = FStudioHome4Diagnostics::Performance(*S);
        return TEXT("Measured instant/cumulative MLUPS: ") + Number(P.MLUPSInstant) + TEXT(" / ") + Number(P.MLUPSCumulative) + TEXT("\nAchieved bandwidth: ") + Number(P.GigabytesPerSecond, TEXT("GB/s")) +
            TEXT("\nDriver reported instant/cumulative MLUPS: ") + Number(S->ReportedMLUPSInstant) + TEXT(" / ") + Number(S->ReportedMLUPSCumulative) +
            TEXT("\nElapsed window ") + Number(S->Work.ElapsedSeconds, TEXT("s")) + TEXT(" · node updates ") + Number(S->Work.NodeUpdates) + TEXT(" · transfer bytes ") + Number(S->Work.TransferredBytes);
    }
    if (Key == TEXT("Safeguards"))
        return TEXT("Limiter/threshold cells: ") + Count(S->LimiterCells) + TEXT(" / ") + Count(S->ThresholdCells) + TEXT("\nMax speed ") + Number(S->MaximumSpeed) +
            TEXT(" at ") + Cell(S->MaximumSpeedCell) + TEXT(" · Mach ") + Number(S->Mach) + TEXT(" · minimum τ ") + Number(S->TauMinimum) + TEXT("\nDivergence norm ") + Number(S->DivergenceNorm);
    if (Key == TEXT("Trouble"))
    {
        const auto* Stream = DisplayStream();
        const auto& F = !Stream->ActionRequests().IsEmpty() ? Stream->ActionRequests().Last().Facts : S->Trouble;
        FString Text = TEXT("Cell ") + Cell(F.Cell ? F.Cell : S->MaximumSpeedCell) + TEXT(" · level ") + (F.Level ? FString::FromInt(*F.Level) : TEXT("Unavailable")) + TEXT(" · φ ") + Number(F.Phi) + TEXT(" · τ ") + Number(F.Tau) +
            TEXT("\nLimiter ") + Fact(F.Limiter) + TEXT(" · force threshold ") + Fact(F.ForceThreshold) + TEXT("\nBand ") + Fact(F.InBand) + TEXT(" · sponge ") + Fact(F.InSponge) + TEXT(" · beach ") + Fact(F.InBeach) +
            TEXT(" · cut-link shell ") + Fact(F.InCutLinkShell) + TEXT("\nZone: ") + (F.Zone.IsEmpty() ? TEXT("Unavailable") : F.Zone);
        if (!Stream->ActionRequests().IsEmpty())
        {
            const auto& A = Stream->ActionRequests().Last();
            Text += TEXT("\n") + A.Reason + TEXT(" Last good step: ") + Count(A.LastGoodStep) + TEXT(". Reported restart: ") + A.RestartPath.Get(TEXT("Unavailable"));
        }
        return Text;
    }
    return TEXT("Unavailable");
}
void SStudioHome4Monitors::ApplyPolicy()
{
    using namespace StudioHome4MonitorPrivate;
    FStudioHome4DiagnosticPolicy Candidate = Policy;
    TOptional<double>* Fields[] = {&Candidate.BudgetAbsoluteTolerance, &Candidate.ForceRelativeTolerance, &Candidate.ForceAbsoluteTolerance,
        &Candidate.ForceReferenceMagnitude, &Candidate.WindowRelativeTolerance, &Candidate.WindowAbsoluteTolerance, &Candidate.WindowReferenceMagnitude, &Candidate.RestPressureTolerance};
    for (int32 I = 0; I < UE_ARRAY_COUNT(Fields); ++I)
        if (!PolicyNumber(PolicyDraft[I], *Fields[I]))
        { Status = FString::Printf(TEXT("Threshold field %d must be a finite nonnegative number or empty. Previous limits retained."), I + 1); return; }
    Policy = Candidate;
    if (SessionStream) SessionStream->SetDiagnosticPolicy(Policy);
    if (ImportedStream) ImportedStream->SetDiagnosticPolicy(Policy);
    Status = TEXT("Explicit health thresholds applied. Empty limits keep their gate unavailable.");
}
bool SStudioHome4Monitors::CanLocate() const
{
    const auto* S = Sample(); if (!S || !OnLocate.IsBound() || !OriginalRunIdentity()) return false;
    const auto* Stream = DisplayStream();
    if (!Stream->ActionRequests().IsEmpty()) return Stream->ActionRequests().Last().Facts.Cell.IsSet();
    return S->Trouble.Cell.IsSet() || S->MaximumSpeedCell.IsSet();
}
void SStudioHome4Monitors::Locate()
{
    if (!CanLocate()) return;
    const auto* Stream = DisplayStream();
    FStudioHome4CellFacts Facts = Stream->ActionRequests().IsEmpty() ? Sample()->Trouble : Stream->ActionRequests().Last().Facts;
    if (!Facts.Cell) Facts.Cell = Sample()->MaximumSpeedCell;
    OnLocate.ExecuteIfBound(Facts);
}
void SStudioHome4Monitors::RefreshOutputs()
{
    using namespace StudioHome4MonitorPrivate;
    const auto* Stream = DisplayStream();
    const uint64 Last = Stream && !Stream->OutputEvents().IsEmpty() ? Stream->OutputEvents().Last().RecordIndex : 0;
    if (DisplayedStream == Stream && DisplayedOutputIndex == Last) return;
    DisplayedStream = Stream; DisplayedOutputIndex = Last; OutputRows->ClearChildren();
    for (int32 I = 0; I < 4; ++I)
    {
        const auto Kind = EStudioHome4OutputKind(I);
        FString Latest = TEXT("Unavailable");
        if (Stream) for (const auto& E : Stream->OutputEvents()) if (E.Kind == Kind) Latest = TEXT("step ") + Count(E.Step) + TEXT(" · ") + E.Path;
        OutputRows->AddSlot().AutoHeight().Padding(0, 3)[SNew(STextBlock).Tag(FName(*FString::Printf(TEXT("Home4OutputKind%d"), I)))
            .Font(StudioUI::Font(9)).ColorAndOpacity(Kind == EStudioHome4OutputKind::Restart ? StudioUI::Amber : StudioUI::Text).AutoWrapText(true)
            .Text(FText::FromString(FString(KindName(Kind)) + TEXT(" · ") + Latest))];
    }
    if (Stream)
    {
        const auto& Events = Stream->OutputEvents();
        for (int32 I = FMath::Max(0, Events.Num() - 12); I < Events.Num(); ++I)
        {
            const auto& E = Events[I];
            OutputRows->AddSlot().AutoHeight().Padding(0, 2)[StudioUI::Label(FString::Printf(TEXT("%s · step %s · %s"), KindName(E.Kind), *Count(E.Step), *E.Path), 8, StudioUI::Muted)];
        }
    }
}
void SStudioHome4Monitors::ImportDialog()
{
    const FString Identity = OriginalRunIdDraft.TrimStartAndEnd(); FGuid Original;
    if (!Identity.IsEmpty() && (!FGuid::Parse(Identity, Original) || !Original.IsValid()))
    { Status = TEXT("Original run ID must be a valid GUID or empty. Previous science source retained."); return; }
    FString Path;
    if (!StudioFileDialog::DataFile(false, TEXT("Import original HOME4 science JSONL log"), TEXT(""), TEXT("jsonl"), Path))
    { Status = TEXT("Import cancelled. Previous science source retained."); return; }
    BeginImportPath(Path, OriginalRunIdDraft);
}
bool SStudioHome4Monitors::BeginImportPath(const FString& Path, const FString& OriginalRunId)
{
    ScopeProject();
    if (Pending.IsValid()) { Status = TEXT("A science log is already being read."); return false; }
    if (Path.IsEmpty()) { Status = TEXT("Import cancelled. Previous science source retained."); return false; }
    const FString Identity = OriginalRunId.TrimStartAndEnd(); FGuid Run = FGuid::NewGuid();
    if (!Identity.IsEmpty() && (!FGuid::Parse(Identity, Run) || !Run.IsValid()))
    { Status = TEXT("Original run ID must be a valid GUID or empty. Previous science source retained."); return false; }
    const FStudioHome4Source Source{Run, FPaths::ConvertRelativePathToFull(Path)};
    const bool bOriginalIdentity = !Identity.IsEmpty();
    const auto M = Model.Pin(); ImportProjectId = M ? M->Project.Id : FGuid();
    Cancellation = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    Status = TEXT("Reading original JSONL in the background. Previous science source remains visible.");
    Pending = Async(EAsyncExecution::ThreadPool, [Path, Source, bOriginalIdentity, Cancel = Cancellation]
        { return ReadImport(Path, Source, bOriginalIdentity, Cancel); });
    return true;
}
void SStudioHome4Monitors::CancelImport()
{ if (Cancellation) Cancellation->store(true, std::memory_order_relaxed); }
void SStudioHome4Monitors::PollImport()
{
    ScopeProject();
    if (!Pending.IsValid() || !Pending.IsReady()) return;
    const bool bCancelled = Cancellation && Cancellation->load(std::memory_order_relaxed);
    auto Result = MoveTemp(Pending.GetMutable()); Pending = {}; Cancellation.Reset();
    const auto M = Model.Pin();
    if (M && M->Project.Id != ImportProjectId)
    { Status = TEXT("Project changed while importing; the previous replay was cleared and the new replay was not attached."); return; }
    if (bCancelled) { Status = TEXT("Science import cancelled. Previous science source retained."); return; }
    if (!Result.Error.IsEmpty()) { Status = Result.Error + TEXT(" Previous science source retained."); return; }
    ImportedStream = MakeShared<FStudioHome4TelemetryStream>(MoveTemp(*Result.Stream)); ImportedStream->SetDiagnosticPolicy(Policy);
    bImportedOriginalRunIdentity = Result.bOriginalRunIdentity;
    ImportPath = Result.Path; bShowImported = true;
    Status = FString::Printf(TEXT("Imported replay · %lld original bytes, %lld lines; %lld unknown records skipped. Last %d measurements retained."), Result.Bytes, Result.Lines, Result.Unknown, ImportedStream->History().Num());
    RefreshOutputs();
}
void SStudioHome4Monitors::ScopeProject()
{
    const auto M = Model.Pin(); if (!M || M->Project.Id == ScopedProjectId) return;
    ScopedProjectId = M->Project.Id; SessionStream.Reset(); ImportedStream.Reset(); ImportPath.Empty(); bImportedOriginalRunIdentity = false;
    OriginalRunIdDraft.Empty(); bShowImported = false; CancelImport();
    Status = TEXT("Project changed; imported replay cleared. Supply this project's explicitly identified original log.");
    RefreshOutputs();
}
SStudioHome4Monitors::FImportResult SStudioHome4Monitors::ReadImport(const FString& Path,
    const FStudioHome4Source& Source, bool bOriginalRunIdentity, const TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe>& Cancel)
{
    FImportResult R;
    auto Cancelled = [&] { return Cancel->load(std::memory_order_relaxed); };
    auto Fail = [&](const TCHAR* Error) { R.Error = Error; R.Stream.Reset(); return MoveTemp(R); };
    if (Cancelled()) return Fail(TEXT("Science import cancelled."));
    FStudioFileAccess Access(Path);
    const auto Before = IFileManager::Get().GetTimeStamp(*Path);
    TUniquePtr<FArchive> File(IFileManager::Get().CreateFileReader(*Path, FILEREAD_Silent));
    if (!File) return Fail(TEXT("Original science log is missing or inaccessible."));
    const int64 Size = File->TotalSize();
    if (Size <= 0 || Size > 64LL * 1024 * 1024) return Fail(TEXT("Science log must contain between 1 byte and 64 MiB."));
    R.Stream = MakeUnique<FStudioHome4TelemetryStream>();
    R.Stream->BeginRun(Source); R.bOriginalRunIdentity = bOriginalRunIdentity;
    TArray<uint8> Chunk; Chunk.SetNumUninitialized(65536);
    uint8 LastByte = '\n';
    auto Consume = [&](const uint8* Bytes, int32 N)
    {
        int32 Offset = 0;
        while (Offset < N)
        {
            if (Cancelled()) return false;
            const auto Batch = R.Stream->AppendBytes(Bytes + Offset, N - Offset);
            if (Batch.ConsumedBytes <= 0) { R.Error = TEXT("Science parser could not advance."); return false; }
            Offset += Batch.ConsumedBytes; R.Lines += Batch.CompleteLines; R.Malformed += Batch.Malformed;
            R.Unknown += Batch.Unknown; R.Oversized += Batch.Oversized; R.Regressing += Batch.Regressing;
            if (R.Malformed || R.Oversized || R.Regressing) { R.Error = FString::Printf(TEXT("Science import rejected invalid or regressing data near original line %lld."), R.Lines); return false; }
            if (R.Lines > 1000000) { R.Error = TEXT("Science log exceeds the one-million-line import budget."); return false; }
        }
        return true;
    };
    while (R.Bytes < Size)
    {
        if (Cancelled()) return Fail(TEXT("Science import cancelled."));
        const int32 N = int32(FMath::Min<int64>(Chunk.Num(), Size - R.Bytes));
        File->Serialize(Chunk.GetData(), N);
        if (File->IsError()) return Fail(TEXT("Original science log could not be read completely."));
        R.Bytes += N; LastByte = Chunk[N - 1];
        if (!Consume(Chunk.GetData(), N))
        { if (Cancelled()) return Fail(TEXT("Science import cancelled.")); R.Stream.Reset(); return R; }
    }
    // A completed import may end after a whole JSON value without a final LF.
    // Tailing keeps this fragment pending; import explicitly closes it at EOF.
    if (LastByte != '\n')
    {
        const uint8 LF = '\n';
        if (!Consume(&LF, 1)) { R.Stream.Reset(); return R; }
    }
    if (Cancelled()) return Fail(TEXT("Science import cancelled."));
    if (IFileManager::Get().GetTimeStamp(*Path) != Before || IFileManager::Get().FileSize(*Path) != Size)
        return Fail(TEXT("Original science log changed during import. Select a completed log."));
    if (R.Stream->History().IsEmpty() && R.Stream->OutputEvents().IsEmpty()) return Fail(TEXT("No recognized HOME4 science measurements or outputs were found."));
    R.Path = FPaths::ConvertRelativePathToFull(Path); return R;
}
