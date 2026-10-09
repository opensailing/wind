#include "StudioHome4SciencePresentation.h"
#include "StudioTheme.h"
#include "StudioHome4BodyDiagnostics.h"

namespace StudioHome4SciencePresentation
{
    const TCHAR* Name(EMetric M)
    {
        static const TCHAR* Names[]={TEXT("Mass drift"),TEXT("Budget residual"),TEXT("Force channels"),TEXT("Water KE"),TEXT("Air KE"),TEXT("Surface energy"),
            TEXT("Selected phase KE"),TEXT("Selected phase PE"),TEXT("Mach number"),TEXT("Minimum tau"),TEXT("Maximum speed"),TEXT("Divergence norm"),TEXT("Spurious speed"),
            TEXT("Limiter cells"),TEXT("Threshold cells"),TEXT("Measured MLUPS"),TEXT("Cumulative MLUPS"),TEXT("Achieved GB/s"),TEXT("Selected level mass drift"),
            TEXT("Selected level injection"),TEXT("Selected level measured MLUPS"),TEXT("Body position Z"),TEXT("Body velocity Z"),TEXT("Body roll"),TEXT("Body pitch"),TEXT("Body yaw"),TEXT("Fitted added mass"),TEXT("Fitted damping"),TEXT("Driver reported MLUPS"),TEXT("Driver reported cumulative MLUPS"),TEXT("Measured tau margin to 1/2"),TEXT("Budget terms"),TEXT("Held/running/quasi-static/reference heave"),TEXT("Held/running/quasi-static/reference pitch")};
        return int32(M)<UE_ARRAY_COUNT(Names)?Names[int32(M)]:TEXT("Unavailable metric");
    }
    const FStudioHome4Normalization* Normalization(const FStudioHome4Sample* S,const FString& Body)
    {
        if(!S||!S->Metadata)return nullptr;
        return Body.IsEmpty()?&S->Metadata->Normalization:S->Metadata->BodyNormalizations.Find(Body);
    }
    bool CanNormalize(const FStudioHome4Sample* S,int32 Component,const FString& Body)
    {
        const auto* N=Normalization(S,Body);
        const auto Divisor=N?(Component==3?N->MomentDivisor:N->ForceDivisor):TOptional<double>();
        return Divisor&&FMath::IsFinite(*Divisor)&&*Divisor>0;
    }
    FValue Quantity(const TOptional<double>& V,const FStudioHome4Sample& S,EStudioHome4Quantity Q,EStudioHome4UnitDisplay Display,bool Normalize,const FString& Body)
    {
        FValue Out; if(!V||!FMath::IsFinite(*V))return Out;
        if(Normalize&&(Q==EStudioHome4Quantity::Force||Q==EStudioHome4Quantity::Moment))
        {
            const auto* N=Normalization(&S,Body);
            const auto D=N?(Q==EStudioHome4Quantity::Force?N->ForceDivisor:N->MomentDivisor):TOptional<double>();
            if(D&&FMath::IsFinite(*D)&&*D>0){const double Value=*V/ *D;if(FMath::IsFinite(Value)){Out.Number=Value;Out.Unit=Q==EStudioHome4Quantity::Force?N->ForceLabel:N->MomentLabel;return Out;}}
            Out.Number=V;Out.Unit=TEXT("raw source units · normalization unavailable");Out.bRawFallback=true;return Out;
        }
        const auto M=S.Metadata;
        TOptional<EStudioHome4UnitDisplay> From;
        if(M)
        {
            if(Q==EStudioHome4Quantity::Force||Q==EStudioHome4Quantity::Moment)From=M->ForceUnits;
            else if(Q==EStudioHome4Quantity::Energy)From=M->EnergyUnits;
            else if(Q==EStudioHome4Quantity::Velocity)From=M->VelocityUnits;
            else if(Q==EStudioHome4Quantity::Length)From=M->LengthUnits;
        }
        if(Q==EStudioHome4Quantity::Dimensionless){Out.Number=V;Out.Unit=TEXT("1");return Out;}
        if(From&&M)
        {
            const auto Converted=StudioHome4Config::ConvertUnits(*V,Q,*From,Display,M->UnitMap);
            if(Converted)
            {
                Out.Number=Converted;
                if(Display==EStudioHome4UnitDisplay::Nondimensional)Out.Unit=TEXT("1");
                else if(Display==EStudioHome4UnitDisplay::Physical)
                    Out.Unit=Q==EStudioHome4Quantity::Force?TEXT("N"):Q==EStudioHome4Quantity::Moment?TEXT("N m"):Q==EStudioHome4Quantity::Energy?TEXT("J"):Q==EStudioHome4Quantity::Velocity?TEXT("m/s"):TEXT("m");
                else Out.Unit=Q==EStudioHome4Quantity::Velocity?TEXT("cells/step"):Q==EStudioHome4Quantity::Length?TEXT("cells"):TEXT("lattice units");
                return Out;
            }
        }
        Out.Number=V;Out.Unit=TEXT("raw source units");Out.bRawFallback=true;return Out;
    }
    FString Text(const FValue& V)
    {return V.Number?FString::Printf(TEXT("%.6g %s"),*V.Number,*V.Unit):TEXT("Unavailable");}
    TOptional<double> BudgetImbalance(const FStudioHome4EnergyBudget& B)
    {
        if(!B.Work||!B.DissipationNear||!B.DissipationFar||!B.DissipationAir||!B.BeachLoss||!B.FloorLoss||!B.DeltaKE||!B.DeltaPE)return {};
        const double V=*B.Work-(*B.DissipationNear+*B.DissipationFar+*B.DissipationAir+*B.BeachLoss+*B.FloorLoss+*B.DeltaKE+*B.DeltaPE);
        return FMath::IsFinite(V)?TOptional<double>(V):TOptional<double>();
    }
    TArray<FBar> Budget(const FStudioHome4Sample* S,EStudioHome4UnitDisplay Display,const FString& Phase)
    {
        TArray<FBar> Out; if(!S||S->bNonfinite)return Out;
        const auto* B=Phase.IsEmpty()?&S->Budget:S->PhaseBudgets.Find(Phase);if(!B)return Out;
        const TCHAR* Names[]={TEXT("Work"),TEXT("D near"),TEXT("D far"),TEXT("D air"),TEXT("Beach"),TEXT("Floor"),TEXT("Delta KE"),TEXT("Delta PE"),TEXT("Residual")};
        const TOptional<double> Values[]={B->Work,B->DissipationNear,B->DissipationFar,B->DissipationAir,B->BeachLoss,B->FloorLoss,B->DeltaKE,B->DeltaPE,B->Residual};
        for(int32 I=0;I<9;++I){FBar Bar;Bar.Label=Names[I];Bar.Value=Quantity(Values[I],*S,EStudioHome4Quantity::Energy,Display);Bar.Color=I==8?StudioUI::Amber:StudioUI::Cyan;Out.Add(MoveTemp(Bar));}
        return Out;
    }
    const FStudioHome4BodyMeasurement* Body(const FStudioHome4Sample& S,const FString& Id)
    {return Id.IsEmpty()?nullptr:S.Bodies.FindByPredicate([&](const auto& B){return B.Id==Id;});}
    FHistory History(const FStudioHome4TelemetryStream* Stream,EMetric M,EStudioHome4UnitDisplay Display,int32 Component,bool Normalize,const FString& BodyId,int32 Level,const FString& Phase)
    {
        FHistory Out; if(!Stream||Stream->History().IsEmpty()){Out.Note=TEXT("Original history unavailable");return Out;}
        const auto& H=Stream->History(); bool Star=true,Step=true;
        for(const auto& S:H){Star&=S.DimensionlessTime.IsSet();Step&=S.Step.IsSet();}
        Out.Axis=Star?TEXT("original t*"):Step?TEXT("solver step"):TEXT("original record order");
        const TCHAR* Channels[]={TEXT("Stress"),TEXT("Momentum"),TEXT("Pressure"),TEXT("Viscous"),TEXT("Stress - momentum")};
        const FLinearColor Colors[]={StudioUI::Cyan,StudioUI::Amber,FLinearColor(.63,.54,.95),FLinearColor(.4,.8,.52),StudioUI::Text};
        const TCHAR* BudgetNames[]={TEXT("Work"),TEXT("D near"),TEXT("D far"),TEXT("D air"),TEXT("Beach"),TEXT("Floor"),TEXT("Delta KE"),TEXT("Delta PE"),TEXT("Residual")};
        const bool BodyComparison=M==EMetric::BodyHeaveComparison||M==EMetric::BodyPitchComparison;
        const bool FitComparison=M==EMetric::AddedMass||M==EMetric::Damping;
        const TCHAR* BodyNames[]={TEXT("Held"),TEXT("Running"),TEXT("Source quasi-static"),TEXT("Identified reference"),TEXT("Calculated K inverse")};
        const int32 ChannelCount=M==EMetric::Forces?5:M==EMetric::BudgetTerms?9:BodyComparison?5:FitComparison?2:1;
        for(int32 I=0;I<ChannelCount;++I){FSeries Series;Series.Label=M==EMetric::Forces?Channels[I]:M==EMetric::BudgetTerms?BudgetNames[I]:BodyComparison?BodyNames[I]:FitComparison?(I?TEXT("Identified fit reference"):Name(M)):Name(M);Series.Color=Colors[I%5];Out.Series.Add(MoveTemp(Series));}
        bool Raw=false;FString ExpectedUnit;
        for(const auto& S:H)
        {
            Out.X.Add(Star?*S.DimensionlessTime:Step?double(*S.Step):double(S.RecordIndex));
            const auto* B=Body(S,BodyId);const auto* L=S.Levels.FindByPredicate([&](const auto& V){return V.Level==Level;});
            const auto* P=S.PhaseEnergies.Find(Phase);EStudioHome4Quantity Q=EStudioHome4Quantity::Dimensionless;
            TArray<TOptional<double>> Values;FString ExplicitUnit;
            TOptional<double> CalculatedSI;
            if(BodyComparison)
            {
                Values.SetNum(5);
                if(B)
                {
                    const auto Quasi=StudioHome4BodyDiagnostics::QuasiStatic(S,*B);
                    if(M==EMetric::BodyHeaveComparison)
                    {Values={B->EquilibriumHeave,B->RunningHeave,B->QuasiStaticHeave,B->ReferenceSource.IsEmpty()?TOptional<double>():B->ReferenceHeave,TOptional<double>()};Q=EStudioHome4Quantity::Length;CalculatedSI=Quasi.HeaveMeters;}
                    else
                    {Values={B->EquilibriumAttitudeDegrees?TOptional<double>(B->EquilibriumAttitudeDegrees->Y):TOptional<double>(),B->RunningAttitudeDegrees?TOptional<double>(B->RunningAttitudeDegrees->Y):TOptional<double>(),B->QuasiStaticPitchDegrees,B->ReferenceSource.IsEmpty()?TOptional<double>():B->ReferenceAttitudeDegrees?TOptional<double>(B->ReferenceAttitudeDegrees->Y):TOptional<double>(),Quasi.PitchDegrees};ExplicitUnit=TEXT("degrees");}
                }
                Out.Note=TEXT("Original supplied body comparisons; reference identity and missing conventions remain explicit. No tank gate is inferred. Calculated K inverse requires declared compatible SI components/loads.");
            }
            else if(FitComparison)
            {
                Values.SetNum(2);
                if(B)
                {Values={M==EMetric::AddedMass?B->AddedMass:B->Damping,B->FitReferenceSource.IsEmpty()?TOptional<double>():M==EMetric::AddedMass?B->ReferenceAddedMass:B->ReferenceDamping};ExplicitUnit=M==EMetric::AddedMass?B->AddedMassUnit:B->DampingUnit;}
                if(ExplicitUnit.IsEmpty())ExplicitUnit=TEXT("raw source units");
                Out.Note=TEXT("Original fitted/reference coefficients on original log time; supplied fit frequency/window/method retained in the source. Use Validation's separate original reference selector for frequency curves and explicit tolerance gates.");
            }
            else if(M==EMetric::BudgetTerms)
            {
                const auto* Budget=Phase.IsEmpty()?&S.Budget:S.PhaseBudgets.Find(Phase);
                if(Budget)Values={Budget->Work,Budget->DissipationNear,Budget->DissipationFar,Budget->DissipationAir,Budget->BeachLoss,Budget->FloorLoss,Budget->DeltaKE,Budget->DeltaPE,Budget->Residual};
                else Values.SetNum(9);
                Q=EStudioHome4Quantity::Energy;
            }
            else if(M==EMetric::Forces)
            {
                const auto* F=BodyId.IsEmpty()?&S.Forces:B?&B->Forces:nullptr;
                if(F&&Component>=0&&Component<4)
                {
                    const TOptional<double> Stress[]={F->Fx,F->Fy,F->Fz,F->My},Momentum[]={F->MomentumFx,F->MomentumFy,F->MomentumFz,F->MomentumMy},
                        Pressure[]={F->PressureFx,F->PressureFy,F->PressureFz,F->PressureMy},Viscous[]={F->ViscousFx,F->ViscousFy,F->ViscousFz,F->ViscousMy};
                    TOptional<double> Difference;if(Stress[Component]&&Momentum[Component]){const double D=*Stress[Component]-*Momentum[Component];if(FMath::IsFinite(D))Difference=D;}
                    Values={Stress[Component],Momentum[Component],Pressure[Component],Viscous[Component],Difference};
                }
                else Values.SetNum(5);
                Q=Component==3?EStudioHome4Quantity::Moment:EStudioHome4Quantity::Force;
            }
            else
            {
                TOptional<double> V;
                switch(M)
                {
                case EMetric::Mass:V=S.Mass.PhiDrift;break;case EMetric::Residual:{const auto* Budget=Phase.IsEmpty()?&S.Budget:S.PhaseBudgets.Find(Phase);if(Budget)V=Budget->Residual;Q=EStudioHome4Quantity::Energy;break;}
                case EMetric::WaterKE:V=S.WaterKE;Q=EStudioHome4Quantity::Energy;break;case EMetric::AirKE:V=S.AirKE;Q=EStudioHome4Quantity::Energy;break;
                case EMetric::SurfaceEnergy:V=S.SurfaceEnergy;Q=EStudioHome4Quantity::Energy;break;case EMetric::PhaseKE:V=P?P->KE:TOptional<double>();Q=EStudioHome4Quantity::Energy;break;
                case EMetric::PhasePE:V=P?P->PE:TOptional<double>();Q=EStudioHome4Quantity::Energy;break;case EMetric::Mach:V=S.Mach;break;case EMetric::Tau:V=S.TauMinimum;break;
                case EMetric::Speed:V=S.MaximumSpeed;Q=EStudioHome4Quantity::Velocity;break;case EMetric::Divergence:V=S.DivergenceNorm;ExplicitUnit=TEXT("raw source norm");break;
                case EMetric::Spurious:V=S.SpuriousSpeed;Q=EStudioHome4Quantity::Velocity;break;
                case EMetric::Limiter:if(S.LimiterCells)V=double(*S.LimiterCells);ExplicitUnit=TEXT("cells");break;
                case EMetric::Threshold:if(S.ThresholdCells)V=double(*S.ThresholdCells);ExplicitUnit=TEXT("cells");break;
                case EMetric::MLUPS:V=FStudioHome4Diagnostics::Performance(S).MLUPSInstant;ExplicitUnit=TEXT("MLUPS");break;
                case EMetric::CumulativeMLUPS:V=FStudioHome4Diagnostics::Performance(S).MLUPSCumulative;ExplicitUnit=TEXT("MLUPS");break;
                case EMetric::Bandwidth:V=FStudioHome4Diagnostics::Performance(S).GigabytesPerSecond;ExplicitUnit=TEXT("GB/s");break;
                case EMetric::ReportedMLUPS:V=S.ReportedMLUPSInstant;ExplicitUnit=TEXT("reported MLUPS");break;
                case EMetric::ReportedCumulativeMLUPS:V=S.ReportedMLUPSCumulative;ExplicitUnit=TEXT("reported MLUPS");break;
                case EMetric::TauMargin:if(S.TauMinimum)V=*S.TauMinimum-.5;ExplicitUnit=TEXT("tau - 1/2");break;
                case EMetric::LevelMass:V=L?L->MassDrift:S.Mass.LevelDrifts.IsValidIndex(Level)?S.Mass.LevelDrifts[Level]:TOptional<double>();break;
                case EMetric::LevelInjection:V=L?L->Injection:S.Mass.LevelInjections.IsValidIndex(Level)?S.Mass.LevelInjections[Level]:TOptional<double>();ExplicitUnit=TEXT("raw source mass");break;
                case EMetric::LevelMLUPS:if(L){FStudioHome4Sample WorkSample;WorkSample.Work=L->Work;V=FStudioHome4Diagnostics::Performance(WorkSample).MLUPSInstant;}ExplicitUnit=TEXT("MLUPS");break;
                case EMetric::BodyPositionZ:if(B&&B->Position)V=B->Position->Z;Q=EStudioHome4Quantity::Length;break;
                case EMetric::BodyVelocityZ:if(B&&B->Velocity)V=B->Velocity->Z;Q=EStudioHome4Quantity::Velocity;break;
                case EMetric::BodyRoll:case EMetric::BodyPitch:case EMetric::BodyYaw:if(B&&B->AttitudeDegrees)V=(*B->AttitudeDegrees)[int32(M)-int32(EMetric::BodyRoll)];ExplicitUnit=TEXT("degrees");break;
                case EMetric::AddedMass:if(B){V=B->AddedMass;ExplicitUnit=B->AddedMassUnit.IsEmpty()?TEXT("raw source units"):B->AddedMassUnit;}break;
                case EMetric::Damping:if(B){V=B->Damping;ExplicitUnit=B->DampingUnit.IsEmpty()?TEXT("raw source units"):B->DampingUnit;}break;
                default:break;
                }
                Values.Add(V);
            }
            for(int32 I=0;I<Values.Num();++I)
            {
                auto V=Quantity(Values[I],S,Q,Display,M==EMetric::Forces&&Normalize,BodyId);
                if(M==EMetric::BodyHeaveComparison&&I==4&&CalculatedSI)
                {
                    if(Display==EStudioHome4UnitDisplay::Physical){V.Number=CalculatedSI;V.Unit=TEXT("m");}
                    else if(S.Metadata){V.Number=StudioHome4Config::ConvertUnits(*CalculatedSI,EStudioHome4Quantity::Length,EStudioHome4UnitDisplay::Physical,Display,S.Metadata->UnitMap);V.Unit=Display==EStudioHome4UnitDisplay::Lattice?TEXT("cells"):TEXT("1");}
                }
                if(!ExplicitUnit.IsEmpty())V.Unit=ExplicitUnit;
                if(S.bNonfinite)V.Number.Reset();
                if(V.Number)
                {
                    if(ExpectedUnit.IsEmpty())ExpectedUnit=V.Unit;
                    if(ExpectedUnit!=V.Unit){V.Number.Reset();Out.Note=TEXT("Samples with different source units are omitted; gaps remain visible.");}
                    Raw|=V.bRawFallback;
                }
                Out.Series[I].Values.Add(V.Number);
            }
        }
        Out.Unit=ExpectedUnit.IsEmpty()?TEXT("Unavailable"):ExpectedUnit;
        if(Raw&&Out.Note.IsEmpty())Out.Note=TEXT("Original source units retained; conversion or normalization metadata unavailable.");
        return Out;
    }
}
