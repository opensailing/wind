#include "StudioHome4Telemetry.h"
#include "StudioHome4JSON.h"
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
        void Vector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, TOptional<FVector>& Out)
        {
            TArray<TOptional<double>> V; Numbers(Object, Name, V);
            if (V.IsEmpty()) { const auto Original = Field(Object, Name); if (Original && Original->Type != EJson::Null) bValid = false; return; }
            if (V.Num() != 3 || !V[0] || !V[1] || !V[2]) { bValid = false; return; }
            Out = FVector(*V[0], *V[1], *V[2]);
        }
        const TArray<TSharedPtr<FJsonValue>>* Array(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, int32 Maximum)
        {
            const auto V = Field(Object, Name);
            if (!V || V->Type == EJson::Null) return nullptr;
            if (V->Type != EJson::Array || V->AsArray().Num() > Maximum) { bValid = false; return nullptr; }
            return &V->AsArray();
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

    void ReadForces(FReader& R, const TSharedPtr<FJsonObject>& O, FStudioHome4Forces& F)
    {
        R.Number(O, TEXT("Fx"), F.Fx); R.Number(O, TEXT("Fy"), F.Fy); R.Number(O, TEXT("Fz"), F.Fz); R.Number(O, TEXT("My"), F.My);
        R.Number(O, TEXT("Fx_p"), F.PressureFx); R.Number(O, TEXT("Fy_p"), F.PressureFy); R.Number(O, TEXT("Fz_p"), F.PressureFz); R.Number(O, TEXT("My_p"), F.PressureMy);
        R.Number(O, TEXT("Fx_nu"), F.ViscousFx); R.Number(O, TEXT("Fy_nu"), F.ViscousFy); R.Number(O, TEXT("Fz_nu"), F.ViscousFz); R.Number(O, TEXT("My_nu"), F.ViscousMy);
        R.Number(O, TEXT("mea_Fx"), F.MomentumFx); R.Number(O, TEXT("mea_Fy"), F.MomentumFy); R.Number(O, TEXT("mea_Fz"), F.MomentumFz); R.Number(O, TEXT("mea_My"), F.MomentumMy);
    }
    void ReadWork(FReader& R, const TSharedPtr<FJsonObject>& O, FStudioHome4Work& W)
    {
        R.Number(O, TEXT("elapsed_seconds"), W.ElapsedSeconds, 0);
        R.Number(O, TEXT("node_updates"), W.NodeUpdates, 0, MaxExactInteger, true);
        R.Number(O, TEXT("transferred_bytes"), W.TransferredBytes, 0, MaxExactInteger, true);
        R.Number(O, TEXT("cumulative_elapsed_seconds"), W.CumulativeElapsedSeconds, 0);
        R.Number(O, TEXT("cumulative_node_updates"), W.CumulativeNodeUpdates, 0, MaxExactInteger, true);
    }
    void ReadWindow(FReader& R, const TSharedPtr<FJsonObject>& O, FStudioHome4Window& W)
    {
        R.Number(O, TEXT("Fx"), W.Fx); R.Number(O, TEXT("Fy"), W.Fy); R.Number(O, TEXT("Fz"), W.Fz); R.Number(O, TEXT("My"), W.My);
        R.Number(O, TEXT("Fx_prev"), W.PreviousFx); R.Number(O, TEXT("Fy_prev"), W.PreviousFy); R.Number(O, TEXT("Fz_prev"), W.PreviousFz); R.Number(O, TEXT("My_prev"), W.PreviousMy);
        R.Number(O, TEXT("start"), W.Start, 0); R.Number(O, TEXT("end"), W.End, 0);
        R.String(O,TEXT("abscissa_unit"),W.AbscissaUnit,64);R.String(O,TEXT("epoch"),W.Epoch,256);
        R.Number(O, TEXT("prev_start"), W.PreviousStart, 0); R.Number(O, TEXT("prev_end"), W.PreviousEnd, 0);
        R.Number(O, TEXT("average_length"), W.AverageLength, 0);
        if ((W.Start && W.End && *W.Start >= *W.End) || (W.PreviousStart && W.PreviousEnd && *W.PreviousStart >= *W.PreviousEnd) ||
            (W.PreviousEnd && W.Start && *W.PreviousEnd > *W.Start)) R.bValid = false;
    }
    void ReadNormalization(FReader& R, const TSharedPtr<FJsonObject>& O, FStudioHome4Normalization& N)
    {
        R.Number(O, TEXT("force_divisor"), N.ForceDivisor, 0); R.Number(O, TEXT("moment_divisor"), N.MomentDivisor, 0);
        R.String(O, TEXT("force_label"), N.ForceLabel, 96); R.String(O, TEXT("moment_label"), N.MomentLabel, 96);
        if ((N.ForceDivisor && *N.ForceDivisor <= 0) || (N.MomentDivisor && *N.MomentDivisor <= 0) ||
            N.ForceDivisor.IsSet() != !N.ForceLabel.IsEmpty() || N.MomentDivisor.IsSet() != !N.MomentLabel.IsEmpty()) R.bValid = false;
    }
    void ReadUnitSystem(FReader& R, const TSharedPtr<FJsonObject>& O, const TCHAR* Name, TOptional<EStudioHome4UnitDisplay>& Unit)
    {
        FString Value; R.String(O, Name, Value, 32);
        if (Value.IsEmpty()) return;
        if (Value == TEXT("lattice")) Unit = EStudioHome4UnitDisplay::Lattice;
        else if (Value == TEXT("physical")) Unit = EStudioHome4UnitDisplay::Physical;
        else if (Value == TEXT("nondimensional")) Unit = EStudioHome4UnitDisplay::Nondimensional;
        else R.bValid = false;
    }
    TSharedPtr<FStudioHome4SourceMetadata> ReadMetadata(FReader& R, const TSharedPtr<FJsonObject>& Root)
    {
        const auto O = R.Object(Root, TEXT("source_metadata")); if (!O) return nullptr;
        auto M = MakeShared<FStudioHome4SourceMetadata>();
        ReadUnitSystem(R, O, TEXT("force_units"), M->ForceUnits); ReadUnitSystem(R, O, TEXT("energy_units"), M->EnergyUnits);
        ReadUnitSystem(R, O, TEXT("velocity_units"), M->VelocityUnits); ReadUnitSystem(R, O, TEXT("length_units"), M->LengthUnits);
        const auto U = R.Object(O, TEXT("unit_map"));
        R.Number(U, TEXT("dx_m"), M->UnitMap.Units.DxMeters, 0, 1e12); R.Number(U, TEXT("dt_s"), M->UnitMap.Units.DtSeconds, 0, 1e12);
        R.Number(U, TEXT("rho_kg_m3"), M->UnitMap.Units.DensityReferenceKgM3, 0, 1e12);
        R.Number(U, TEXT("rho_lattice"), M->UnitMap.Fluids.RhoHeavy, 0, 1e12);
        R.Number(U, TEXT("length_cells"), M->UnitMap.Reference.LengthCells, 0, 1e12);
        R.Number(U, TEXT("time_steps"), M->UnitMap.Reference.TimeSteps, 0, 1e12);
        R.Number(U, TEXT("speed_cells_step"), M->UnitMap.Reference.SpeedCellsPerStep, 0, 1e12);
        FString Error; if (!StudioHome4Config::Validate(M->UnitMap, Error)) R.bValid = false;
        ReadNormalization(R, R.Object(O, TEXT("normalization")), M->Normalization);
        R.String(O,TEXT("divergence_convention"),M->DivergenceConvention,256);R.String(O,TEXT("divergence_unit"),M->DivergenceUnit,64);R.String(O,TEXT("divergence_domain"),M->DivergenceDomain,256);
        R.Number(O,TEXT("device_peak_gbps"),M->DevicePeakGBps,0);R.String(O,TEXT("device_peak_source"),M->DevicePeakSource,2048);
        if(M->DevicePeakGBps&&(*M->DevicePeakGBps<=0||M->DevicePeakSource.IsEmpty()))R.bValid=false;
        const auto* Declared=R.Array(O,TEXT("declared_levels"),64);TSet<int32> Seen;
        if(Declared)for(const auto& V:*Declared)
        {
            double D=0;if(V->Type!=EJson::Number||!V->TryGetNumber(D)||!FMath::IsFinite(D)||D<0||D>63||FMath::FloorToDouble(D)!=D||Seen.Contains(int32(D))){R.bValid=false;break;}
            Seen.Add(int32(D));M->DeclaredLevels.Add(int32(D));
        }
        M->DeclaredLevels.Sort();
        const auto Bodies = R.Object(O, TEXT("body_normalizations"));
        if (Bodies)
        {
            if (Bodies->Values.Num() > 16) R.bValid = false;
            else for (const auto& E : Bodies->Values)
            {
                if (E.Key.IsEmpty() || E.Key.Len() > 64 || E.Value->Type != EJson::Object) { R.bValid = false; break; }
                FStudioHome4Normalization N; ReadNormalization(R, E.Value->AsObject(), N); M->BodyNormalizations.Add(FString(*E.Key), MoveTemp(N));
            }
        }
        return M;
    }
    void ReadExtensions(FReader& R, const TSharedPtr<FJsonObject>& O, FStudioHome4Sample& S)
    {
        const auto Interface = R.Object(O, TEXT("interface"));
        R.Number(Interface, TEXT("spurious_speed"), S.SpuriousSpeed, 0); R.String(Interface, TEXT("spurious_mask"), S.SpuriousMask);
        R.String(Interface,TEXT("spurious_unit"),S.SpuriousUnit,64);R.Boolean(Interface,TEXT("forcing_free"),S.SpuriousForcingFree);R.Boolean(Interface,TEXT("at_rest"),S.SpuriousAtRest);
        const auto Spurious=R.Object(Interface,TEXT("spurious_reference"));
        R.Number(Spurious,TEXT("speed"),S.SpuriousReferenceSpeed,0);R.Number(Spurious,TEXT("absolute_tolerance"),S.SpuriousAbsoluteTolerance,0);R.String(Spurious,TEXT("source"),S.SpuriousReferenceSource,2048);
        if((S.SpuriousReferenceSpeed||S.SpuriousAbsoluteTolerance)&&(S.SpuriousReferenceSource.IsEmpty()||S.SpuriousUnit.IsEmpty()))R.bValid=false;
        const auto Histogram = R.Object(Interface, TEXT("thickness_histogram"));
        if (Histogram)
        {
            R.Number(Histogram,TEXT("phi_min"),S.InterfaceThickness.PhiMinimum,0,1);R.Number(Histogram,TEXT("phi_max"),S.InterfaceThickness.PhiMaximum,0,1);
            R.Number(Histogram,TEXT("expected_xi"),S.InterfaceThickness.ExpectedXi,0);R.String(Histogram,TEXT("thickness_unit"),S.InterfaceThickness.ThicknessUnit,64);R.String(Histogram,TEXT("sampling_source"),S.InterfaceThickness.SamplingSource,2048);
            if(S.InterfaceThickness.PhiMinimum.IsSet()!=S.InterfaceThickness.PhiMaximum.IsSet()||
                (S.InterfaceThickness.PhiMinimum&&*S.InterfaceThickness.PhiMinimum>=*S.InterfaceThickness.PhiMaximum)||
                (S.InterfaceThickness.ExpectedXi&&(*S.InterfaceThickness.ExpectedXi<=0||S.InterfaceThickness.ThicknessUnit.IsEmpty())))R.bValid=false;
            const auto* Edges = R.Array(Histogram, TEXT("edges"), 65); const auto* Counts = R.Array(Histogram, TEXT("counts"), 64);
            if (!Edges || !Counts || Counts->IsEmpty() || Edges->Num() != Counts->Num() + 1) R.bValid = false;
            else
            {
                for (const auto& V : *Edges)
                {
                    double N = 0; if (V->Type != EJson::Number || !V->TryGetNumber(N) || !FMath::IsFinite(N) || N < 0 ||
                        (!S.InterfaceThickness.BinEdges.IsEmpty() && N <= S.InterfaceThickness.BinEdges.Last())) { R.bValid = false; break; }
                    S.InterfaceThickness.BinEdges.Add(N);
                }
                for (const auto& V : *Counts)
                {
                    double N = 0; if (V->Type != EJson::Number || !V->TryGetNumber(N) || !FMath::IsFinite(N) || N < 0 || N > MaxExactInteger || FMath::FloorToDouble(N) != N) { R.bValid = false; break; }
                    S.InterfaceThickness.Counts.Add(int64(N));
                }
            }
        }
        const auto Phases = R.Object(O, TEXT("phase_energies"));
        if (Phases)
        {
            if (Phases->Values.Num() > 16) R.bValid = false;
            else for (const auto& E : Phases->Values)
            {
                if (E.Key.IsEmpty() || E.Key.Len() > 64 || E.Value->Type != EJson::Object) { R.bValid = false; break; }
                FStudioHome4PhaseEnergy P; const auto V = E.Value->AsObject();
                R.Number(V, TEXT("ke"), P.KE, 0); R.Number(V, TEXT("pe"), P.PE); R.Number(V, TEXT("surface"), P.Surface, 0);
                S.PhaseEnergies.Add(FString(*E.Key), MoveTemp(P));
            }
        }
        const auto* Levels = R.Array(O, TEXT("levels"), 64); TSet<int32> LevelIds;
        if (Levels) for (const auto& V : *Levels)
        {
            if (V->Type != EJson::Object) { R.bValid = false; break; }
            const auto L = V->AsObject(); TOptional<int64> Id; R.Integer(L, TEXT("level"), Id);
            if (!Id || *Id > 63 || LevelIds.Contains(int32(*Id))) { R.bValid = false; break; }
            FStudioHome4LevelMeasurement M; M.Level = int32(*Id); LevelIds.Add(M.Level);
            R.Number(L, TEXT("mass_drift"), M.MassDrift); R.Number(L, TEXT("injected"), M.Injection);
            R.Number(L, TEXT("mlups"), M.ReportedMLUPS, 0); ReadWork(R, R.Object(L, TEXT("performance")), M.Work);
            S.Levels.Add(MoveTemp(M));
        }
        const auto* Bodies = R.Array(O, TEXT("bodies"), 16); TSet<FString> BodyIds;
        if (Bodies) for (const auto& V : *Bodies)
        {
            if (V->Type != EJson::Object) { R.bValid = false; break; }
            const auto B = V->AsObject(); FStudioHome4BodyMeasurement M;
            R.String(B, TEXT("id"), M.Id, 64); R.String(B, TEXT("name"), M.Name, 96);
            if (M.Id.IsEmpty() || BodyIds.Contains(M.Id)) { R.bValid = false; break; } BodyIds.Add(M.Id);
            ReadForces(R, R.Object(B, TEXT("forces")), M.Forces); ReadWindow(R, R.Object(B, TEXT("window")), M.Window);
            const auto State = R.Object(B, TEXT("state"));
            R.Vector(State, TEXT("position"), M.Position); R.Vector(State, TEXT("velocity"), M.Velocity);
            R.Vector(State, TEXT("attitude_deg"), M.AttitudeDegrees); R.Vector(State, TEXT("angular_velocity"), M.AngularVelocity);
            R.String(State, TEXT("angular_velocity_unit"), M.AngularVelocityUnit, 32);
            R.String(State, TEXT("integrator_status"), M.IntegratorStatus, 96);
            const auto A = R.Object(B, TEXT("attitude"));
            R.Number(A, TEXT("equilibrium_heave"), M.EquilibriumHeave); R.Number(A, TEXT("running_heave"), M.RunningHeave); R.Number(A, TEXT("reference_heave"), M.ReferenceHeave);
            R.Number(A, TEXT("k33"), M.K33); R.Number(A, TEXT("k35"), M.K35); R.Number(A, TEXT("k55"), M.K55);
            R.String(A, TEXT("stiffness_unit"), M.StiffnessUnit, 64);
            R.String(A,TEXT("k33_unit"),M.K33Unit,64);R.String(A,TEXT("k35_unit"),M.K35Unit,64);R.String(A,TEXT("k55_unit"),M.K55Unit,64);R.String(A,TEXT("stiffness_convention"),M.StiffnessConvention,256);
            R.Vector(A, TEXT("equilibrium_deg"), M.EquilibriumAttitudeDegrees); R.Vector(A, TEXT("running_deg"), M.RunningAttitudeDegrees);
            R.Vector(A, TEXT("reference_deg"), M.ReferenceAttitudeDegrees); R.String(A, TEXT("reference_source"), M.ReferenceSource);
            const auto Fit = R.Object(B, TEXT("fit"));
            R.Number(Fit, TEXT("added_mass"), M.AddedMass); R.Number(Fit, TEXT("damping"), M.Damping);
            R.Number(Fit, TEXT("reference_added_mass"), M.ReferenceAddedMass); R.Number(Fit, TEXT("reference_damping"), M.ReferenceDamping);
            R.String(Fit, TEXT("added_mass_unit"), M.AddedMassUnit, 64); R.String(Fit, TEXT("damping_unit"), M.DampingUnit, 64);
            R.String(Fit, TEXT("reference_source"), M.FitReferenceSource);
            R.Number(Fit,TEXT("frequency"),M.FitFrequency,0);R.String(Fit,TEXT("frequency_unit"),M.FitFrequencyUnit,64);R.String(Fit,TEXT("method"),M.FitMethod,256);
            R.Number(Fit,TEXT("window_start"),M.FitWindowStart);R.Number(Fit,TEXT("window_end"),M.FitWindowEnd);R.String(Fit,TEXT("window_unit"),M.FitWindowUnit,64);R.String(Fit,TEXT("epoch"),M.FitEpoch,256);
            if(M.FitWindowStart.IsSet()!=M.FitWindowEnd.IsSet()||(M.FitWindowStart&&(*M.FitWindowEnd<=*M.FitWindowStart||M.FitWindowUnit.IsEmpty()))||
                (M.FitFrequency&&M.FitFrequencyUnit.IsEmpty()))R.bValid=false;
            const auto Quasi=R.Object(B,TEXT("quasi_static"));
            R.Number(Quasi,TEXT("heave"),M.QuasiStaticHeave);R.Number(Quasi,TEXT("pitch_deg"),M.QuasiStaticPitchDegrees);R.String(Quasi,TEXT("method"),M.QuasiStaticMethod,256);R.String(Quasi,TEXT("source"),M.QuasiStaticSource,2048);
            if((M.QuasiStaticHeave||M.QuasiStaticPitchDegrees)&&(M.QuasiStaticMethod.IsEmpty()||M.QuasiStaticSource.IsEmpty()))R.bValid=false;
            if (((M.ReferenceAttitudeDegrees || M.ReferenceHeave) && M.ReferenceSource.IsEmpty()) || ((M.ReferenceAddedMass || M.ReferenceDamping) && M.FitReferenceSource.IsEmpty())) R.bValid = false;
            const auto Retab = R.Object(B, TEXT("retabulation")); R.Integer(Retab, TEXT("every"), M.RetabulationEvery);
            if (M.RetabulationEvery && *M.RetabulationEvery == 0) R.bValid = false;
            ReadWork(R, R.Object(Retab, TEXT("performance")), M.RetabulationWork);
            S.Bodies.Add(MoveTemp(M));
        }
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
        ReadForces(R, R.Object(O, TEXT("forces")), S.Forces);
        ReadWindow(R, R.Object(O, TEXT("window")), S.Window);
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
        ReadWork(R, R.Object(O, TEXT("performance")), S.Work);
        ReadExtensions(R, O, S);
        const auto Rest = R.Object(O, TEXT("wb_rest"));
        R.Boolean(Rest, TEXT("at_rest"), S.RestCondition);R.Boolean(Rest,TEXT("full_gravity"),S.RestFullGravity);
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
        R.String(Trouble, TEXT("patch_id"), S.Trouble.PatchId, 128);
        TOptional<bool> Nonfinite;
        R.Boolean(O, TEXT("nonfinite"), Nonfinite);
        S.bNonfinite = Nonfinite.Get(false);
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
    bool Home4TelemetryNonnegative(const TOptional<double>& Value)
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
        if (!Home4TelemetryNonnegative(Relative) || !Home4TelemetryNonnegative(Absolute) || !Home4TelemetryNonnegative(Reference))
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

bool FStudioHome4SourceMetadata::Equivalent(const FStudioHome4SourceMetadata& O) const
{
    auto Same = [](const FStudioHome4Normalization& A, const FStudioHome4Normalization& B)
    { return A.ForceDivisor == B.ForceDivisor && A.MomentDivisor == B.MomentDivisor && A.ForceLabel == B.ForceLabel && A.MomentLabel == B.MomentLabel; };
    if (ForceUnits != O.ForceUnits || EnergyUnits != O.EnergyUnits || VelocityUnits != O.VelocityUnits || LengthUnits != O.LengthUnits ||
        DivergenceConvention!=O.DivergenceConvention||DivergenceUnit!=O.DivergenceUnit||DivergenceDomain!=O.DivergenceDomain||DevicePeakGBps!=O.DevicePeakGBps||DevicePeakSource!=O.DevicePeakSource||DeclaredLevels!=O.DeclaredLevels||
        StudioHome4Config::Serialize(UnitMap) != StudioHome4Config::Serialize(O.UnitMap) || !Same(Normalization, O.Normalization) || BodyNormalizations.Num() != O.BodyNormalizations.Num()) return false;
    for (const auto& E : BodyNormalizations) { const auto* Other = O.BodyNormalizations.Find(E.Key); if (!Other || !Same(E.Value, *Other)) return false; }
    return true;
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
    auto Mass = Signal(TEXT("mass"), TEXT("Phase ledger drift threshold"),
        TEXT("Inspect per-level ledgers, correction injections, boundary fluxes and MD restriction."));
    Mass.Threshold = 1e-4;
    if (S.Mass.PhiDrift)
    {
        double Worst = FMath::Abs(*S.Mass.PhiDrift);
        bool bComplete = true;
        for (const auto& Level : S.Mass.LevelDrifts)
        { if (Level) Worst = FMath::Max(Worst, FMath::Abs(*Level)); else bComplete = false; }
        for(const auto& Level:S.Levels) {if(Level.MassDrift)Worst=FMath::Max(Worst,FMath::Abs(*Level.MassDrift));else bComplete=false;}
        if(S.Metadata)for(int32 Expected:S.Metadata->DeclaredLevels)
        {
            if(Expected==0)continue;
            const bool Indexed=S.Mass.LevelDrifts.IsValidIndex(Expected)&&S.Mass.LevelDrifts[Expected].IsSet();
            const bool Named=S.Levels.ContainsByPredicate([&](const auto& L){return L.Level==Expected&&L.MassDrift.IsSet();});
            if(!Indexed&&!Named)bComplete=false;
        }
        Mass.Value = Worst;
        Mass.Status = Worst >= 1e-4 ? EStudioHome4Health::Warning :
            (bComplete ? EStudioHome4Health::Healthy : EStudioHome4Health::Unavailable);
        Mass.Reason = bComplete ? TEXT("Maximum absolute root/per-level ledger drift; healthy requires strictly below 1e-4.") :
            TEXT("A per-level ledger is unavailable; measured drifts are retained.");
        bool TrendComplete=S.Mass.InjectionMagnitudeChange.IsSet();
        for(const auto& Change:S.Mass.LevelInjectionMagnitudeChanges)TrendComplete&=Change.IsSet();
        for(const auto& Level:S.Levels)TrendComplete&=Level.InjectionMagnitudeChange.IsSet();
        for(int32 I=1;I<S.Mass.LevelDrifts.Num();++I)TrendComplete&=S.Mass.LevelInjectionMagnitudeChanges.IsValidIndex(I)&&S.Mass.LevelInjectionMagnitudeChanges[I].IsSet();
        if(S.Metadata)for(int32 Expected:S.Metadata->DeclaredLevels)
        {
            if(Expected==0)continue;
            const bool Indexed=S.Mass.LevelInjectionMagnitudeChanges.IsValidIndex(Expected)&&S.Mass.LevelInjectionMagnitudeChanges[Expected].IsSet();
            const bool Named=S.Levels.ContainsByPredicate([&](const auto& L){return L.Level==Expected&&L.InjectionMagnitudeChange.IsSet();});
            TrendComplete&=Indexed||Named;
        }
        Mass.Reason+=TrendComplete?TEXT(" Original adjacent correction-injection trend supplied."):TEXT(" Correction-injection trend coverage is unknown; drift health alone does not establish a complete ledger gate.");
        Mass.Reason+=S.Metadata&&!S.Metadata->DeclaredLevels.IsEmpty()?TEXT(" Original declared-level coverage checked."):TEXT(" Expected MD level inventory was not supplied by the original source.");
        bool bGrowing = S.Mass.InjectionMagnitudeChange && *S.Mass.InjectionMagnitudeChange > 0;
        for (const auto& Change : S.Mass.LevelInjectionMagnitudeChanges) if (Change && *Change > 0) bGrowing = true;
        for (const auto& Level : S.Levels) if (Level.InjectionMagnitudeChange && *Level.InjectionMagnitudeChange > 0) bGrowing = true;
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
    else if (!Home4TelemetryNonnegative(P.BudgetAbsoluteTolerance)) Budget.Reason = TEXT("An explicit absolute budget tolerance is required.");
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
    const auto* Body = P.BodyId.IsEmpty() ? nullptr : S.Bodies.FindByPredicate([&](const auto& B) { return B.Id == P.BodyId; });
    const FStudioHome4Forces EmptyForces;
    const auto& F = P.BodyId.IsEmpty() ? S.Forces : Body ? Body->Forces : EmptyForces;
    const TArray<TPair<TOptional<double>, TOptional<double>>> ForcePairs = {
        {F.Fx, F.MomentumFx}, {F.Fy, F.MomentumFy},
        {F.Fz, F.MomentumFz}, {F.My, F.MomentumMy}};
    const int32 ForceIndex = int32(P.ForceComponent);
    if (ForcePairs.IsValidIndex(ForceIndex)) Agreement(Forces, {ForcePairs[ForceIndex]},
        P.ForceRelativeTolerance, P.ForceAbsoluteTolerance, P.ForceReferenceMagnitude);
    const TCHAR* ComponentNames[] = {TEXT("Fx"), TEXT("Fy"), TEXT("Fz"), TEXT("My")};
    if (ForcePairs.IsValidIndex(ForceIndex)) Forces.Label += FString::Printf(TEXT(" (%s)"), ComponentNames[ForceIndex]);
    if (!P.BodyId.IsEmpty()) Forces.Label += TEXT(" · body ") + P.BodyId;
    Out.Add(MoveTemp(Forces));
    auto Window = Signal(TEXT("window"), TEXT("Steady-window bracket"),
        TEXT("Extend the run or averaging window and inspect transient forcing."));
    const FStudioHome4Window EmptyWindow;
    const auto& W = P.BodyId.IsEmpty() ? S.Window : Body ? Body->Window : EmptyWindow;
    const TArray<TPair<TOptional<double>, TOptional<double>>> WindowPairs = {
        {W.Fx, W.PreviousFx}, {W.Fy, W.PreviousFy},
        {W.Fz, W.PreviousFz}, {W.My, W.PreviousMy}};
    const int32 WindowIndex = int32(P.WindowComponent);
    if (WindowPairs.IsValidIndex(WindowIndex)) Agreement(Window, {WindowPairs[WindowIndex]},
        P.WindowRelativeTolerance, P.WindowAbsoluteTolerance, P.WindowReferenceMagnitude);
    if (WindowPairs.IsValidIndex(WindowIndex)) Window.Label += FString::Printf(TEXT(" (%s)"), ComponentNames[WindowIndex]);
    if (!P.BodyId.IsEmpty()) Window.Label += TEXT(" · body ") + P.BodyId;
    Out.Add(MoveTemp(Window));
    auto Rest = Signal(TEXT("wb-rest"), TEXT("WB rest test"), TEXT("Inspect boundary conditions, full gravity and MD transfer."));
    if (!S.RestCondition || !*S.RestCondition) Rest.Reason = TEXT("An explicit well-balanced rest condition is required.");
    else if(S.RestFullGravity&&!*S.RestFullGravity)Rest.Reason=TEXT("Original rest measurement explicitly reports full gravity disabled; full-gravity WB gate is unavailable.");
    else if (!S.RestMaxDynamicPressure) Rest.Reason = TEXT("Maximum absolute dynamic pressure is unavailable.");
    else if (!Home4TelemetryNonnegative(P.RestPressureTolerance)) Rest.Reason = TEXT("An explicit rest-pressure tolerance is required.");
    else
    {
        Rest.Value = S.RestMaxDynamicPressure; Rest.Threshold = P.RestPressureTolerance;
        Rest.Status = *Rest.Value <= *Rest.Threshold ? EStudioHome4Health::Healthy : EStudioHome4Health::Warning;
        Rest.Label+=*P.RestPressureTolerance==0?TEXT(" · exact zero"):TEXT(" · relaxed explicit pressure tolerance");
        Rest.Reason = TEXT("Maximum absolute dynamic pressure under the explicitly reported rest condition.");
        Rest.Reason+=S.RestFullGravity&&*S.RestFullGravity?TEXT(" Full-gravity original condition supplied."):TEXT(" Original full-gravity condition was not supplied; this is a rest-pressure comparison, not a complete full-gravity WB attestation.");
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
    const bool bHot = Home4TelemetryNonnegative(P.MaximumSpeedTrigger) && S.MaximumSpeed && *S.MaximumSpeed > *P.MaximumSpeedTrigger;
    if (!S.bNonfinite && !bHot) return {};
    FStudioHome4ActionRequest A;
    A.Source = S.Source; A.RecordIndex = S.RecordIndex; A.Step = S.Step; A.Facts = S.Trouble;
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
    if (Limits.SelectedStepStart.IsSet()!=Limits.SelectedStepEnd.IsSet() ||
        (Limits.SelectedStepStart && (*Limits.SelectedStepStart<0 || *Limits.SelectedStepEnd<*Limits.SelectedStepStart))) return false;
    Source = InSource; bActive = true; RecordIndex = 0; SelectedMeasurements=0; PreviousMeasurement.Reset();
    ResetTail(); Samples.Reset(); Outputs.Reset(); Actions.Reset(); LatestSample.Reset(); GoodSample.Reset(); Restart.Reset();
    LastStep.Reset(); LastLatticeTime.Reset(); LastPhysicalTime.Reset(); LastDimensionlessTime.Reset();
    LastCumulativeElapsed.Reset(); LastCumulativeUpdates.Reset(); LastLevelWork.Reset(); SourceMetadata.Reset();
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
    if (!StudioHome4JSON::UTF8(Pending.GetData(), Pending.Num())) return ELineResult::Malformed;
    const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Pending.GetData()), Pending.Num());
    const FString Line(Converted.Length(), Converted.Get());
    if (Line.TrimStartAndEnd().IsEmpty()) return ELineResult::Unknown;
    if (!StudioHome4JSON::Preflight(Line)) return ELineResult::Malformed;
    TSharedPtr<FJsonObject> Object;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Line), Object) || !Object) return ELineResult::Malformed;
    FReader R;
    FString Kind;
    R.String(Object, TEXT("kind"), Kind, 64);
    if (!R.bValid) return ELineResult::Malformed;
    EStudioHome4OutputKind StandaloneKind = EStudioHome4OutputKind::Trace;
    const bool bStandalone = Kind == TEXT("trace") || Kind == TEXT("slice") || Kind == TEXT("snapshot") ||
        Kind == TEXT("visualization") || Kind == TEXT("checkpoint") || Kind == TEXT("restart");
    const bool bMetadataOnly = Kind == TEXT("source_metadata");
    if (!Kind.IsEmpty() && Kind != TEXT("measurement") && !bMetadataOnly && !bStandalone) return ELineResult::Unknown;
    R.bRecognized = false;
    FStudioHome4Sample S;
    S.Source = Source;
    const auto Metadata = ReadMetadata(R, Object);
    if (!bMetadataOnly) ReadSample(R, Object, S);
    if (bMetadataOnly && !Metadata) R.bValid = false;
    if (Metadata && SourceMetadata && !Metadata->Equivalent(*SourceMetadata)) R.bValid = false;
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
    else if (!bMetadataOnly)
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
    for (const auto& L : S.Levels) if (const auto* Previous = LastLevelWork.Find(L.Level))
        if (Regresses(L.Work.CumulativeElapsedSeconds, Previous->CumulativeElapsedSeconds) || Regresses(L.Work.CumulativeNodeUpdates, Previous->CumulativeNodeUpdates))
            return ELineResult::Regressing;
    if (Metadata && !SourceMetadata) SourceMetadata = Metadata;
    S.Metadata = SourceMetadata;
    S.RecordIndex = ++RecordIndex;
    RetainKnown(S.Step, LastStep); RetainKnown(S.LatticeTime, LastLatticeTime);
    RetainKnown(S.PhysicalTime, LastPhysicalTime); RetainKnown(S.DimensionlessTime, LastDimensionlessTime);
    RetainKnown(S.Work.CumulativeElapsedSeconds, LastCumulativeElapsed);
    RetainKnown(S.Work.CumulativeNodeUpdates, LastCumulativeUpdates);
    for (const auto& L : S.Levels)
    {
        auto& Previous = LastLevelWork.FindOrAdd(L.Level);
        RetainKnown(L.Work.CumulativeElapsedSeconds, Previous.CumulativeElapsedSeconds);
        RetainKnown(L.Work.CumulativeNodeUpdates, Previous.CumulativeNodeUpdates);
    }
    auto Selected=[&](const TOptional<int64>& Step){return !Limits.SelectedStepStart || (Step && *Step>=*Limits.SelectedStepStart && *Step<=*Limits.SelectedStepEnd);};
    for (auto& E : Events)
    {
        if(!Selected(E.Step))continue;
        E.RecordIndex = RecordIndex;
        if (E.Kind == EStudioHome4OutputKind::Restart) Restart = E;
        AddBounded(Outputs, MoveTemp(E), Limits.MaxOutputEvents);
    }
    if (!bStandalone && !bMetadataOnly)
    {
        if (PreviousMeasurement)
        {
            const auto& Previous = PreviousMeasurement->Mass;
            auto Change = [](const TOptional<double>& Now, const TOptional<double>& Before) -> TOptional<double>
            {
                if (!Now || !Before) return {};
                const double Delta = FMath::Abs(*Now) - FMath::Abs(*Before);
                return FMath::IsFinite(Delta) ? TOptional<double>(Delta) : TOptional<double>();
            };
            S.Mass.InjectionMagnitudeChange = Change(S.Mass.Injected, Previous.Injected);
            for (auto& Level : S.Levels)
                if (const auto* Before = PreviousMeasurement->Levels.FindByPredicate([&Level](const auto& L) { return L.Level == Level.Level; }))
                    Level.InjectionMagnitudeChange = Change(Level.Injection, Before->Injection);
            for (int32 I = 0; I < S.Mass.LevelInjections.Num(); ++I)
                S.Mass.LevelInjectionMagnitudeChanges.Add(Previous.LevelInjections.IsValidIndex(I) ?
                    Change(S.Mass.LevelInjections[I], Previous.LevelInjections[I]) : TOptional<double>());
        }
        PreviousMeasurement=S;
        if(!Selected(S.Step))return ELineResult::Accepted;
        ++SelectedMeasurements;
        auto Action = FStudioHome4Diagnostics::TroubleAction(S, Policy);
        if (Action)
        {
            if (GoodSample) { Action->LastGoodStep = GoodSample->Step; Action->bCheckpointLastGoodState = true; }
            if (Restart) Action->RestartPath = Restart->Path;
            AddBounded(Actions, MoveTemp(*Action), Limits.MaxActionRequests);
        }
        // A guard/metadata-only record cannot replace a numerical recovery sample.
        if (!Action && !S.bNonfinite && (S.Step || S.LatticeTime || S.PhysicalTime || S.DimensionlessTime ||
            S.Mass.PhiDrift || S.MaximumSpeed || S.Forces.Fx || S.Forces.Fy || S.Forces.Fz)) GoodSample = S;
        LatestSample = S;
        AddBounded(Samples, MoveTemp(S), Limits.MaxHistory);
    }
    return ELineResult::Accepted;
}
