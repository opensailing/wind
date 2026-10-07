#include "StudioHome4Telemetry.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace StudioHome4TelemetryPrivate
{
    constexpr double MaxExactInteger = 9007199254740991.;
    constexpr double MaxFinite = 1.7976931348623157e308;
    struct FReader
    {
        bool bValid = true, bRecognized = false;
        TSharedPtr<FJsonValue> Field(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name)
        {
            if (!Object) return nullptr;
            const auto* Found = Object->Values.Find(Name);
            if (!Found) return nullptr;
            bRecognized = true;
            return *Found;
        }
        void Number(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, TOptional<double>& Out,
            double Min = -MaxFinite, double Max = MaxFinite, bool bInteger = false)
        {
            const auto Value = Field(Object, Name);
            if (!Value || Value->Type == EJson::Null) return;
            double N = 0;
            if (Value->Type != EJson::Number || !Value->TryGetNumber(N) || !FMath::IsFinite(N) ||
                N < Min || N > Max || (bInteger && FMath::FloorToDouble(N) != N))
            { bValid = false; return; }
            Out = N;
        }
        void Integer(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, TOptional<int64>& Out)
        {
            TOptional<double> N;
            Number(Object, Name, N, 0, MaxExactInteger, true);
            if (N) Out = int64(*N);
        }
        void Boolean(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, TOptional<bool>& Out)
        {
            const auto Value = Field(Object, Name);
            if (!Value || Value->Type == EJson::Null) return;
            if (Value->Type != EJson::Boolean) { bValid = false; return; }
            Out = Value->AsBool();
        }
        void String(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, FString& Out, int32 Max = 256)
        {
            const auto Value = Field(Object, Name);
            if (!Value || Value->Type == EJson::Null) return;
            if (Value->Type != EJson::String) { bValid = false; return; }
            Out = Value->AsString();
            if (Out.Len() > Max || Out.TrimStartAndEnd().IsEmpty() || Out.Contains(TEXT("\n")) ||
                Out.Contains(TEXT("\r"))) bValid = false;
            for (const TCHAR C : Out) if (C == 0) bValid = false;
        }
        TSharedPtr<FJsonObject> Object(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name)
        {
            const auto Value = Field(Parent, Name);
            if (!Value || Value->Type == EJson::Null) return nullptr;
            if (Value->Type != EJson::Object) { bValid = false; return nullptr; }
            return Value->AsObject();
        }
        void Numbers(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, TArray<TOptional<double>>& Out)
        {
            const auto Value = Field(Object, Name);
            if (!Value || Value->Type == EJson::Null) return;
            if (Value->Type != EJson::Array || Value->AsArray().Num() > 64) { bValid = false; return; }
            for (const auto& Item : Value->AsArray())
            {
                if (Item->Type == EJson::Null) { Out.Add(TOptional<double>()); continue; }
                double N = 0;
                if (Item->Type != EJson::Number || !Item->TryGetNumber(N) || !FMath::IsFinite(N))
                { bValid = false; return; }
                Out.Add(N);
            }
        }
        void Cell(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, TOptional<FIntVector>& Out)
        {
            const auto Value = Field(Object, Name);
            if (!Value || Value->Type == EJson::Null) return;
            if (Value->Type != EJson::Array || Value->AsArray().Num() != 3) { bValid = false; return; }
            FIntVector Cell(0, 0, 0);
            for (int32 I = 0; I < 3; ++I)
            {
                const auto& Item = Value->AsArray()[I];
                double N = 0;
                if (Item->Type != EJson::Number || !Item->TryGetNumber(N) || !FMath::IsFinite(N) ||
                    N < 0 || N > MAX_int32 || FMath::FloorToDouble(N) != N)
                { bValid = false; return; }
                Cell[I] = int32(N);
            }
            Out = Cell;
        }
    };

    void ReadBudget(FReader& R, const TSharedPtr<FJsonObject>& O, FStudioHome4EnergyBudget& B)
    {
        R.Number(O, TEXT("W"), B.Work);
        R.Number(O, TEXT("D_near"), B.DissipationNear, 0);
        R.Number(O, TEXT("D_far"), B.DissipationFar, 0);
        R.Number(O, TEXT("D_air"), B.DissipationAir, 0);
        R.Number(O, TEXT("Z_beach"), B.BeachLoss, 0);
        R.Number(O, TEXT("Z_floor"), B.FloorLoss, 0);
        R.Number(O, TEXT("dKE"), B.DeltaKE);
        R.Number(O, TEXT("dPE"), B.DeltaPE);
        R.Number(O, TEXT("res"), B.Residual);
    }

    void ReadSample(FReader& R, const TSharedPtr<FJsonObject>& O, FStudioHome4Sample& S)
    {
        R.Integer(O, TEXT("step"), S.Step);
        R.Number(O, TEXT("t_lat"), S.LatticeTime, 0);
        R.Number(O, TEXT("t_phys"), S.PhysicalTime, 0);
        R.Number(O, TEXT("t_star"), S.DimensionlessTime, 0);
        R.String(O, TEXT("backend"), S.Backend);
        R.Number(O, TEXT("mlups_inst"), S.ReportedMLUPSInstant, 0);
        R.Number(O, TEXT("mlups_cum"), S.ReportedMLUPSCumulative, 0);
        R.Number(O, TEXT("ma_inst"), S.Mach, 0);
        R.Number(O, TEXT("tau_min"), S.TauMinimum, 0);
        R.Number(O, TEXT("umax"), S.MaximumSpeed, 0);
        R.Cell(O, TEXT("umax_loc"), S.MaximumSpeedCell);
        R.Number(O, TEXT("div_u_norm"), S.DivergenceNorm, 0);
        R.Number(O, TEXT("pe_surface"), S.SurfaceEnergy, 0);
        const auto Mass = R.Object(O, TEXT("mass_ledger"));
        R.Number(Mass, TEXT("phi"), S.Mass.PhiDrift);
        R.Number(Mass, TEXT("injected"), S.Mass.Injected);
        R.Numbers(Mass, TEXT("levels"), S.Mass.LevelDrifts);
        R.Numbers(Mass, TEXT("level_injected"), S.Mass.LevelInjections);
        const auto KE = R.Object(O, TEXT("ke"));
        R.Number(KE, TEXT("water"), S.WaterKE, 0);
        R.Number(KE, TEXT("air"), S.AirKE, 0);
        const auto F = R.Object(O, TEXT("forces"));
        R.Number(F, TEXT("Fx"), S.Forces.Fx);
        R.Number(F, TEXT("Fy"), S.Forces.Fy);
        R.Number(F, TEXT("Fz"), S.Forces.Fz);
        R.Number(F, TEXT("My"), S.Forces.My);
        R.Number(F, TEXT("Fx_p"), S.Forces.PressureFx);
        R.Number(F, TEXT("Fy_p"), S.Forces.PressureFy);
        R.Number(F, TEXT("Fz_p"), S.Forces.PressureFz);
        R.Number(F, TEXT("Fx_nu"), S.Forces.ViscousFx);
        R.Number(F, TEXT("Fy_nu"), S.Forces.ViscousFy);
        R.Number(F, TEXT("Fz_nu"), S.Forces.ViscousFz);
        R.Number(F, TEXT("mea_Fx"), S.Forces.MomentumFx);
        R.Number(F, TEXT("mea_Fy"), S.Forces.MomentumFy);
        R.Number(F, TEXT("mea_Fz"), S.Forces.MomentumFz);
        R.Number(F, TEXT("mea_My"), S.Forces.MomentumMy);
        const auto W = R.Object(O, TEXT("window"));
        R.Number(W, TEXT("Fx"), S.Window.Fx);
        R.Number(W, TEXT("Fy"), S.Window.Fy);
        R.Number(W, TEXT("Fz"), S.Window.Fz);
        R.Number(W, TEXT("My"), S.Window.My);
        R.Number(W, TEXT("Fx_prev"), S.Window.PreviousFx);
        R.Number(W, TEXT("Fy_prev"), S.Window.PreviousFy);
        R.Number(W, TEXT("Fz_prev"), S.Window.PreviousFz);
        R.Number(W, TEXT("My_prev"), S.Window.PreviousMy);
        R.Number(W, TEXT("start"), S.Window.Start, 0);
        R.Number(W, TEXT("end"), S.Window.End, 0);
        R.Number(W, TEXT("prev_start"), S.Window.PreviousStart, 0);
        R.Number(W, TEXT("prev_end"), S.Window.PreviousEnd, 0);
        if ((S.Window.Start && S.Window.End && *S.Window.Start >= *S.Window.End) ||
            (S.Window.PreviousStart && S.Window.PreviousEnd && *S.Window.PreviousStart >= *S.Window.PreviousEnd) ||
            (S.Window.PreviousEnd && S.Window.Start && *S.Window.PreviousEnd > *S.Window.Start)) R.bValid = false;
        const auto G = R.Object(O, TEXT("safeguards"));
        R.Integer(G, TEXT("limiter_cells"), S.LimiterCells);
        R.Integer(G, TEXT("threshold_cells"), S.ThresholdCells);
        const auto B = R.Object(O, TEXT("budget"));
        ReadBudget(R, B, S.Budget);
        const auto Phases = R.Object(B, TEXT("phases"));
        if (Phases)
        {
            if (Phases->Values.Num() > 16) R.bValid = false;
            else for (const auto& Entry : Phases->Values)
            {
                if (Entry.Key.IsEmpty() || Entry.Key.Len() > 64 || Entry.Value->Type != EJson::Object)
                { R.bValid = false; break; }
                FStudioHome4EnergyBudget Phase;
                ReadBudget(R, Entry.Value->AsObject(), Phase);
                S.PhaseBudgets.Add(FString(*Entry.Key), MoveTemp(Phase));
            }
        }
        const auto Work = R.Object(O, TEXT("performance"));
        R.Number(Work, TEXT("elapsed_seconds"), S.Work.ElapsedSeconds, 0);
        R.Number(Work, TEXT("node_updates"), S.Work.NodeUpdates, 0, MaxExactInteger, true);
        R.Number(Work, TEXT("transferred_bytes"), S.Work.TransferredBytes, 0, MaxExactInteger, true);
        R.Number(Work, TEXT("cumulative_elapsed_seconds"), S.Work.CumulativeElapsedSeconds, 0);
        R.Number(Work, TEXT("cumulative_node_updates"), S.Work.CumulativeNodeUpdates, 0, MaxExactInteger, true);
        const auto Rest = R.Object(O, TEXT("wb_rest"));
        R.Boolean(Rest, TEXT("at_rest"), S.RestCondition);
        R.Number(Rest, TEXT("pd_max"), S.RestMaxDynamicPressure, 0);
        const auto Trouble = R.Object(O, TEXT("locator"));
        R.Cell(Trouble, TEXT("cell"), S.Trouble.Cell);
        TOptional<int64> Level;
        R.Integer(Trouble, TEXT("level"), Level);
        if (Level) { if (*Level > MAX_int32) R.bValid = false; else S.Trouble.Level = int32(*Level); }
        R.Number(Trouble, TEXT("phi"), S.Trouble.Phi);
        R.Number(Trouble, TEXT("tau"), S.Trouble.Tau, 0);
        R.Boolean(Trouble, TEXT("limiter"), S.Trouble.Limiter);
        R.Boolean(Trouble, TEXT("threshold"), S.Trouble.ForceThreshold);
        R.Boolean(Trouble, TEXT("band"), S.Trouble.InBand);
        R.Boolean(Trouble, TEXT("sponge"), S.Trouble.InSponge);
        R.Boolean(Trouble, TEXT("beach"), S.Trouble.InBeach);
        R.Boolean(Trouble, TEXT("cut_link_shell"), S.Trouble.InCutLinkShell);
        R.String(Trouble, TEXT("zone"), S.Trouble.Zone);
        TOptional<bool> Nonfinite;
        R.Boolean(O, TEXT("nonfinite"), Nonfinite);
        S.bNonfinite = Nonfinite.Get(false);
    }

    bool IsUTF8(const TArray<uint8>& Bytes)
    {
        for (int32 I = 0; I < Bytes.Num();)
        {
            const uint8 C = Bytes[I++];
            if (C < 0x80) { if (C == 0) return false; continue; }
            int32 N = 0;
            uint32 Code = 0, Min = 0;
            if (C >= 0xc2 && C <= 0xdf) { N = 1; Code = C & 0x1f; Min = 0x80; }
            else if (C >= 0xe0 && C <= 0xef) { N = 2; Code = C & 0x0f; Min = 0x800; }
            else if (C >= 0xf0 && C <= 0xf4) { N = 3; Code = C & 7; Min = 0x10000; }
            else return false;
            if (Bytes.Num() - I < N) return false;
            for (int32 J = 0; J < N; ++J)
            {
                const uint8 Next = Bytes[I++];
                if ((Next & 0xc0) != 0x80) return false;
                Code = (Code << 6) | (Next & 0x3f);
            }
            if (Code < Min || Code > 0x10ffff || (Code >= 0xd800 && Code <= 0xdfff)) return false;
        }
        return true;
    }

    // Bound JSON nesting before the recursive object deserializer sees a line.
    // Its streaming reader also lets us reject duplicate decoded property names.
    bool IsBoundedJSON(const FString& Line)
    {
        const auto Reader = TJsonReaderFactory<>::Create(Line);
        EJsonNotation Notation;
        TArray<TSet<FString>> ObjectKeys;
        TArray<bool> Containers;
        while (Reader->ReadNext(Notation))
        {
            if (Notation == EJsonNotation::Error) return false;
            if (Notation != EJsonNotation::ObjectEnd && Notation != EJsonNotation::ArrayEnd &&
                !Containers.IsEmpty() && Containers.Last())
            {
                const FString Key = Reader->GetIdentifier();
                if (ObjectKeys.Last().Contains(Key)) return false;
                ObjectKeys.Last().Add(Key);
            }
            if (Notation == EJsonNotation::ObjectStart || Notation == EJsonNotation::ArrayStart)
            {
                if (Containers.Num() >= 64) return false;
                const bool bObject = Notation == EJsonNotation::ObjectStart;
                Containers.Add(bObject);
                if (bObject) ObjectKeys.Add(TSet<FString>());
            }
            else if (Notation == EJsonNotation::ObjectEnd || Notation == EJsonNotation::ArrayEnd)
            {
                if (Containers.IsEmpty()) return false;
                if (Containers.Last()) ObjectKeys.Pop(EAllowShrinking::No);
                Containers.Pop(EAllowShrinking::No);
            }
        }
        return Containers.IsEmpty();
    }

    template<typename T> bool Regresses(const TOptional<T>& Value, const TOptional<T>& Previous)
    { return Value && Previous && *Value < *Previous; }
    template<typename T> void RetainKnown(const TOptional<T>& Value, TOptional<T>& Previous)
    { if (Value) Previous = Value; }
    template<typename T> void AddBounded(TArray<T>& Items, T Item, int32 Maximum)
    {
        if (Items.Num() >= Maximum) Items.RemoveAt(0, Items.Num() - Maximum + 1, EAllowShrinking::No);
        Items.Add(MoveTemp(Item));
    }
    TOptional<double> Ratio(const TOptional<double>& Count, const TOptional<double>& Seconds, double Scale)
    {
        if (!Count || !Seconds || !FMath::IsFinite(*Count) || !FMath::IsFinite(*Seconds) ||
            *Count < 0 || *Seconds <= 0) return {};
        const double Result = (*Count / *Seconds) / Scale;
        return FMath::IsFinite(Result) ? TOptional<double>(Result) : TOptional<double>();
    }
    bool Nonnegative(const TOptional<double>& Value)
    { return Value && FMath::IsFinite(*Value) && *Value >= 0; }
    FStudioHome4HealthSignal Signal(const TCHAR* Id, const TCHAR* Label, const TCHAR* Remedy)
    {
        FStudioHome4HealthSignal S;
        S.Id = Id; S.Label = Label; S.Remedy = Remedy; S.Reason = TEXT("Measurement unavailable.");
        return S;
    }
    void Agreement(FStudioHome4HealthSignal& Signal, const TArray<TPair<TOptional<double>, TOptional<double>>>& Pairs,
        const TOptional<double>& Relative, const TOptional<double>& Absolute, const TOptional<double>& Reference)
    {
        if (!Nonnegative(Relative) || !Nonnegative(Absolute) || !Nonnegative(Reference))
        { Signal.Reason = TEXT("Explicit reference magnitude and absolute/relative tolerances are required."); return; }
        const double Threshold = *Absolute + *Relative * *Reference;
        if (!FMath::IsFinite(Threshold)) { Signal.Reason = TEXT("Tolerance calculation is unavailable."); return; }
        TOptional<double> Difference;
        for (const auto& Pair : Pairs)
        {
            if (Pair.Key && Pair.Value)
            {
                const double D = FMath::Abs(*Pair.Key - *Pair.Value);
                if (!FMath::IsFinite(D)) { Signal.Reason = TEXT("Difference exceeds the finite diagnostic range."); return; }
                Difference = FMath::Max(Difference.Get(0), D);
            }
            else if (Pair.Key || Pair.Value)
            { Signal.Reason = TEXT("A reported component is missing its comparison partner."); return; }
        }
        if (!Difference) { Signal.Reason = TEXT("No complete component pair is measured."); return; }
        Signal.Value = Difference; Signal.Threshold = Threshold;
        Signal.Status = *Difference <= Threshold ? EStudioHome4Health::Healthy : EStudioHome4Health::Warning;
        Signal.Reason = *Reference == 0 ? TEXT("Absolute difference compared with the near-zero absolute tolerance.") :
            TEXT("Maximum component difference compared with the supplied reference and tolerances.");
    }
}

bool FStudioHome4EnergyBudget::IsComplete() const
{
    return Work && DissipationNear && DissipationFar && DissipationAir && BeachLoss && FloorLoss &&
        DeltaKE && DeltaPE && Residual;
}

TArray<FStudioHome4HealthSignal> FStudioHome4Diagnostics::Evaluate(const FStudioHome4Sample& S,
    const FStudioHome4DiagnosticPolicy& P)
{
    using namespace StudioHome4TelemetryPrivate;
    TArray<FStudioHome4HealthSignal> Out;
    auto Mass = Signal(TEXT("mass"), TEXT("Phase mass ledger"),
        TEXT("Inspect per-level ledgers, correction injections, boundary fluxes and MD restriction."));
    Mass.Threshold = 1e-4;
    if (S.Mass.PhiDrift)
    {
        double Worst = FMath::Abs(*S.Mass.PhiDrift);
        bool bComplete = true;
        for (const auto& Level : S.Mass.LevelDrifts)
        { if (Level) Worst = FMath::Max(Worst, FMath::Abs(*Level)); else bComplete = false; }
        Mass.Value = Worst;
        Mass.Status = Worst >= 1e-4 ? EStudioHome4Health::Warning :
            (bComplete ? EStudioHome4Health::Healthy : EStudioHome4Health::Unavailable);
        Mass.Reason = bComplete ? TEXT("Maximum absolute root/per-level ledger drift; healthy requires strictly below 1e-4.") :
            TEXT("A per-level ledger is unavailable; measured drifts are retained.");
        bool bGrowing = S.Mass.InjectionMagnitudeChange && *S.Mass.InjectionMagnitudeChange > 0;
        for (const auto& Change : S.Mass.LevelInjectionMagnitudeChanges) if (Change && *Change > 0) bGrowing = true;
        if (bGrowing)
        {
            Mass.Status = EStudioHome4Health::Warning;
            Mass.Reason = TEXT("Correction injection magnitude increased between adjacent measurements; inspect positive feedback.");
        }
    }
    Out.Add(MoveTemp(Mass));
    auto Budget = Signal(TEXT("budget"), TEXT("Energy budget closure"),
        TEXT("Inspect body work, dissipation, beach/floor losses and non-conservative operations."));
    if (!S.Budget.IsComplete()) Budget.Reason = TEXT("All nine budget terms are required for closure.");
    else if (!Nonnegative(P.BudgetAbsoluteTolerance)) Budget.Reason = TEXT("An explicit absolute budget tolerance is required.");
    else
    {
        const auto& B = S.Budget;
        const double Losses = *B.DissipationNear + *B.DissipationFar + *B.DissipationAir + *B.BeachLoss +
            *B.FloorLoss + *B.DeltaKE + *B.DeltaPE;
        const double Imbalance = *B.Work - Losses;
        const double IdentityError = Imbalance - *B.Residual;
        const double Worst = FMath::Max(FMath::Abs(Imbalance), FMath::Max(FMath::Abs(*B.Residual), FMath::Abs(IdentityError)));
        if (FMath::IsFinite(Losses) && FMath::IsFinite(Imbalance) && FMath::IsFinite(IdentityError) && FMath::IsFinite(Worst))
        {
            Budget.Value = Worst; Budget.Threshold = P.BudgetAbsoluteTolerance;
            Budget.Status = Worst <= *P.BudgetAbsoluteTolerance ? EStudioHome4Health::Healthy : EStudioHome4Health::Warning;
            Budget.Reason = TEXT("Maximum of computed imbalance, reported residual and their consistency error.");
        }
        else Budget.Reason = TEXT("Budget arithmetic exceeds the finite diagnostic range.");
    }
    Out.Add(MoveTemp(Budget));
    auto Forces = Signal(TEXT("forces"), TEXT("Force channel agreement"),
        TEXT("Inspect cut links, body retabulation and stress/momentum channel normalization."));
    const TArray<TPair<TOptional<double>, TOptional<double>>> ForcePairs = {
        {S.Forces.Fx, S.Forces.MomentumFx}, {S.Forces.Fy, S.Forces.MomentumFy},
        {S.Forces.Fz, S.Forces.MomentumFz}, {S.Forces.My, S.Forces.MomentumMy}};
    const int32 ForceIndex = int32(P.ForceComponent);
    if (ForcePairs.IsValidIndex(ForceIndex)) Agreement(Forces, {ForcePairs[ForceIndex]},
        P.ForceRelativeTolerance, P.ForceAbsoluteTolerance, P.ForceReferenceMagnitude);
    const TCHAR* ComponentNames[] = {TEXT("Fx"), TEXT("Fy"), TEXT("Fz"), TEXT("My")};
    if (ForcePairs.IsValidIndex(ForceIndex)) Forces.Label += FString::Printf(TEXT(" (%s)"), ComponentNames[ForceIndex]);
    Out.Add(MoveTemp(Forces));
    auto Window = Signal(TEXT("window"), TEXT("Steady-window bracket"),
        TEXT("Extend the run or averaging window and inspect transient forcing."));
    const TArray<TPair<TOptional<double>, TOptional<double>>> WindowPairs = {
        {S.Window.Fx, S.Window.PreviousFx}, {S.Window.Fy, S.Window.PreviousFy},
        {S.Window.Fz, S.Window.PreviousFz}, {S.Window.My, S.Window.PreviousMy}};
    const int32 WindowIndex = int32(P.WindowComponent);
    if (WindowPairs.IsValidIndex(WindowIndex)) Agreement(Window, {WindowPairs[WindowIndex]},
        P.WindowRelativeTolerance, P.WindowAbsoluteTolerance, P.WindowReferenceMagnitude);
    if (WindowPairs.IsValidIndex(WindowIndex)) Window.Label += FString::Printf(TEXT(" (%s)"), ComponentNames[WindowIndex]);
    Out.Add(MoveTemp(Window));
    auto Rest = Signal(TEXT("wb-rest"), TEXT("WB rest test"), TEXT("Inspect boundary conditions, full gravity and MD transfer."));
    if (!S.RestCondition || !*S.RestCondition) Rest.Reason = TEXT("An explicit well-balanced rest condition is required.");
    else if (!S.RestMaxDynamicPressure) Rest.Reason = TEXT("Maximum absolute dynamic pressure is unavailable.");
    else if (!Nonnegative(P.RestPressureTolerance)) Rest.Reason = TEXT("An explicit rest-pressure tolerance is required.");
    else
    {
        Rest.Value = S.RestMaxDynamicPressure; Rest.Threshold = P.RestPressureTolerance;
        Rest.Status = *Rest.Value <= *Rest.Threshold ? EStudioHome4Health::Healthy : EStudioHome4Health::Warning;
        Rest.Reason = TEXT("Maximum absolute dynamic pressure under the explicitly reported rest condition.");
    }
    Out.Add(MoveTemp(Rest));
    if (S.bNonfinite) for (auto& Signal : Out)
    { Signal.Status = EStudioHome4Health::Warning; Signal.Reason = TEXT("Driver reported a non-finite guard event; inspect the last good sample."); }
    return Out;
}

FStudioHome4MeasuredPerformance FStudioHome4Diagnostics::Performance(const FStudioHome4Sample& S)
{
    using namespace StudioHome4TelemetryPrivate;
    FStudioHome4MeasuredPerformance P;
    if (S.bNonfinite) return P;
    P.MLUPSInstant = Ratio(S.Work.NodeUpdates, S.Work.ElapsedSeconds, 1e6);
    P.MLUPSCumulative = Ratio(S.Work.CumulativeNodeUpdates, S.Work.CumulativeElapsedSeconds, 1e6);
    P.GigabytesPerSecond = Ratio(S.Work.TransferredBytes, S.Work.ElapsedSeconds, 1e9);
    return P;
}

TOptional<FStudioHome4ActionRequest> FStudioHome4Diagnostics::TroubleAction(const FStudioHome4Sample& S,
    const FStudioHome4DiagnosticPolicy& P)
{
    using namespace StudioHome4TelemetryPrivate;
    const bool bHot = Nonnegative(P.MaximumSpeedTrigger) && S.MaximumSpeed && *S.MaximumSpeed > *P.MaximumSpeedTrigger;
    if (!S.bNonfinite && !bHot) return {};
    FStudioHome4ActionRequest A;
    A.Source = S.Source; A.RecordIndex = S.RecordIndex; A.Facts = S.Trouble;
    if (!A.Facts.Cell) A.Facts.Cell = S.MaximumSpeedCell;
    A.bLocateCell = A.Facts.Cell.IsSet();
    A.Reason = S.bNonfinite ? TEXT("Driver non-finite guard requested stop and last-good-state recovery.") :
        TEXT("Measured maximum speed exceeded the explicitly supplied trouble threshold.");
    return A;
}

FStudioHome4TelemetryStream::FStudioHome4TelemetryStream(const FStudioHome4TailLimits& InLimits) : Limits(InLimits)
{
    Limits.MaxBytesPerAppend = FMath::Clamp(Limits.MaxBytesPerAppend, 1, 1048576);
    Limits.MaxLinesPerAppend = FMath::Clamp(Limits.MaxLinesPerAppend, 1, 1024);
    Limits.MaxLineBytes = FMath::Clamp(Limits.MaxLineBytes, 1, 1048576);
    Limits.MaxHistory = FMath::Clamp(Limits.MaxHistory, 1, 4096);
    Limits.MaxOutputEvents = FMath::Clamp(Limits.MaxOutputEvents, 1, 4096);
    Limits.MaxActionRequests = FMath::Clamp(Limits.MaxActionRequests, 1, 1024);
}

bool FStudioHome4TelemetryStream::BeginRun(const FStudioHome4Source& InSource)
{
    if (!InSource.RunId.IsValid() || InSource.SourceId.TrimStartAndEnd().IsEmpty() || InSource.SourceId.Len() > 256 ||
        InSource.SourceId.Contains(TEXT("\n")) || InSource.SourceId.Contains(TEXT("\r"))) return false;
    Source = InSource; bActive = true; RecordIndex = 0;
    ResetTail(); Samples.Reset(); Outputs.Reset(); Actions.Reset(); LatestSample.Reset(); GoodSample.Reset(); Restart.Reset();
    LastStep.Reset(); LastLatticeTime.Reset(); LastPhysicalTime.Reset(); LastDimensionlessTime.Reset();
    LastCumulativeElapsed.Reset(); LastCumulativeUpdates.Reset();
    return true;
}

void FStudioHome4TelemetryStream::ResetTail()
{ Pending.Reset(); bDiscardLine = false; }

FStudioHome4TailResult FStudioHome4TelemetryStream::AppendBytes(const uint8* Bytes, int32 NumBytes)
{
    FStudioHome4TailResult Result;
    if (!bActive || !Bytes || NumBytes <= 0) return Result;
    const int32 Maximum = FMath::Min(NumBytes, Limits.MaxBytesPerAppend);
    for (; Result.ConsumedBytes < Maximum && Result.CompleteLines < Limits.MaxLinesPerAppend;)
    {
        const uint8 Byte = Bytes[Result.ConsumedBytes++];
        if (Byte == '\n')
        {
            ++Result.CompleteLines;
            if (bDiscardLine) ++Result.Oversized;
            else
            {
                switch (ParseLine())
                {
                case ELineResult::Accepted: ++Result.Accepted; break;
                case ELineResult::Malformed: ++Result.Malformed; break;
                case ELineResult::Unknown: ++Result.Unknown; break;
                case ELineResult::Regressing: ++Result.Regressing; break;
                }
            }
            ResetTail();
        }
        else if (!bDiscardLine)
        {
            if (Pending.Num() >= Limits.MaxLineBytes) { Pending.Reset(); bDiscardLine = true; }
            else Pending.Add(Byte);
        }
    }
    return Result;
}

FStudioHome4TelemetryStream::ELineResult FStudioHome4TelemetryStream::ParseLine()
{
    using namespace StudioHome4TelemetryPrivate;
    if (Pending.IsEmpty()) return ELineResult::Unknown;
    if (!IsUTF8(Pending)) return ELineResult::Malformed;
    const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Pending.GetData()), Pending.Num());
    const FString Line(Converted.Length(), Converted.Get());
    if (Line.TrimStartAndEnd().IsEmpty()) return ELineResult::Unknown;
    if (!IsBoundedJSON(Line)) return ELineResult::Malformed;
    TSharedPtr<FJsonObject> Object;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Line), Object) || !Object) return ELineResult::Malformed;
    FReader R;
    FString Kind;
    R.String(Object, TEXT("kind"), Kind, 64);
    if (!R.bValid) return ELineResult::Malformed;
    EStudioHome4OutputKind StandaloneKind = EStudioHome4OutputKind::Trace;
    const bool bStandalone = Kind == TEXT("trace") || Kind == TEXT("slice") || Kind == TEXT("snapshot") ||
        Kind == TEXT("visualization") || Kind == TEXT("checkpoint") || Kind == TEXT("restart");
    if (!Kind.IsEmpty() && Kind != TEXT("measurement") && !bStandalone) return ELineResult::Unknown;
    R.bRecognized = false;
    FStudioHome4Sample S;
    S.Source = Source;
    ReadSample(R, Object, S);
    TArray<FStudioHome4OutputEvent> Events;
    auto Output = [&](const TCHAR* Key, EStudioHome4OutputKind OutputKind)
    {
        FString Path;
        R.String(Object, Key, Path, 2048);
        if (!Path.IsEmpty())
        { FStudioHome4OutputEvent E; E.Source = Source; E.Step = S.Step; E.Kind = OutputKind; E.Path = MoveTemp(Path); Events.Add(MoveTemp(E)); }
    };
    if (bStandalone)
    {
        if (Kind == TEXT("slice")) StandaloneKind = EStudioHome4OutputKind::Slice;
        else if (Kind == TEXT("snapshot") || Kind == TEXT("visualization")) StandaloneKind = EStudioHome4OutputKind::Visualization;
        else if (Kind == TEXT("checkpoint") || Kind == TEXT("restart")) StandaloneKind = EStudioHome4OutputKind::Restart;
        Output(TEXT("path"), StandaloneKind);
        if (Events.IsEmpty()) R.bValid = false;
    }
    else
    {
        Output(TEXT("trace"), EStudioHome4OutputKind::Trace);
        Output(TEXT("slice"), EStudioHome4OutputKind::Slice);
        Output(TEXT("snapshot"), EStudioHome4OutputKind::Visualization);
        Output(TEXT("checkpoint"), EStudioHome4OutputKind::Restart);
    }
    if (!R.bValid) return ELineResult::Malformed;
    if (!R.bRecognized) return ELineResult::Unknown;
    if (Regresses(S.Step, LastStep) || Regresses(S.LatticeTime, LastLatticeTime) ||
        Regresses(S.PhysicalTime, LastPhysicalTime) || Regresses(S.DimensionlessTime, LastDimensionlessTime) ||
        Regresses(S.Work.CumulativeElapsedSeconds, LastCumulativeElapsed) || Regresses(S.Work.CumulativeNodeUpdates, LastCumulativeUpdates))
        return ELineResult::Regressing;
    S.RecordIndex = ++RecordIndex;
    RetainKnown(S.Step, LastStep); RetainKnown(S.LatticeTime, LastLatticeTime);
    RetainKnown(S.PhysicalTime, LastPhysicalTime); RetainKnown(S.DimensionlessTime, LastDimensionlessTime);
    RetainKnown(S.Work.CumulativeElapsedSeconds, LastCumulativeElapsed);
    RetainKnown(S.Work.CumulativeNodeUpdates, LastCumulativeUpdates);
    for (auto& E : Events)
    {
        E.RecordIndex = RecordIndex;
        if (E.Kind == EStudioHome4OutputKind::Restart) Restart = E;
        AddBounded(Outputs, MoveTemp(E), Limits.MaxOutputEvents);
    }
    if (!bStandalone)
    {
        if (LatestSample)
        {
            const auto& Previous = LatestSample->Mass;
            auto Change = [](const TOptional<double>& Now, const TOptional<double>& Before) -> TOptional<double>
            {
                if (!Now || !Before) return {};
                const double Delta = FMath::Abs(*Now) - FMath::Abs(*Before);
                return FMath::IsFinite(Delta) ? TOptional<double>(Delta) : TOptional<double>();
            };
            S.Mass.InjectionMagnitudeChange = Change(S.Mass.Injected, Previous.Injected);
            for (int32 I = 0; I < S.Mass.LevelInjections.Num(); ++I)
                S.Mass.LevelInjectionMagnitudeChanges.Add(Previous.LevelInjections.IsValidIndex(I) ?
                    Change(S.Mass.LevelInjections[I], Previous.LevelInjections[I]) : TOptional<double>());
        }
        auto Action = FStudioHome4Diagnostics::TroubleAction(S, Policy);
        if (Action)
        {
            if (GoodSample) { Action->LastGoodStep = GoodSample->Step; Action->bCheckpointLastGoodState = true; }
            if (Restart) Action->RestartPath = Restart->Path;
            AddBounded(Actions, MoveTemp(*Action), Limits.MaxActionRequests);
        }
        // A guard/metadata-only record cannot replace a numerical recovery sample.
        if (!S.bNonfinite && (S.Step || S.LatticeTime || S.PhysicalTime || S.DimensionlessTime ||
            S.Mass.PhiDrift || S.MaximumSpeed || S.Forces.Fx || S.Forces.Fy || S.Forces.Fz)) GoodSample = S;
        LatestSample = S;
        AddBounded(Samples, MoveTemp(S), Limits.MaxHistory);
    }
    return ELineResult::Accepted;
}
