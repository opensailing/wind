#pragma once
#include "StudioHome4Telemetry.h"

/** Read-only chart models from retained original measurements. No current draft,
 * time interpolation, gap filling or reference gate is used. */
namespace StudioHome4SciencePresentation
{
    enum class EMetric : uint8
    {
        Mass, Residual, Forces, WaterKE, AirKE, SurfaceEnergy, PhaseKE, PhasePE,
        Mach, Tau, Speed, Divergence, Spurious, Limiter, Threshold, MLUPS, CumulativeMLUPS,
        Bandwidth, LevelMass, LevelInjection, LevelMLUPS, BodyPositionZ, BodyVelocityZ,
        BodyRoll, BodyPitch, BodyYaw, AddedMass, Damping, ReportedMLUPS, ReportedCumulativeMLUPS, TauMargin, BudgetTerms, Count
    };
    struct FValue { TOptional<double> Number; FString Unit; bool bRawFallback = false; };
    struct FSeries { FString Label; FLinearColor Color; TArray<TOptional<double>> Values; };
    struct FHistory
    {
        TArray<double> X;
        TArray<FSeries> Series;
        FString Axis, Unit, Note;
    };
    struct FBar { FString Label; FValue Value; FLinearColor Color; };
    const TCHAR* Name(EMetric Metric);
    FValue Quantity(const TOptional<double>& Value, const FStudioHome4Sample& Sample, EStudioHome4Quantity Quantity,
        EStudioHome4UnitDisplay Display, bool Normalize = false, const FString& BodyId = FString());
    FString Text(const FValue& Value);
    FHistory History(const FStudioHome4TelemetryStream* Stream, EMetric Metric, EStudioHome4UnitDisplay Display,
        int32 Component = 0, bool Normalize = false, const FString& BodyId = FString(), int32 Level = 0, const FString& Phase = FString());
    TArray<FBar> Budget(const FStudioHome4Sample* Sample, EStudioHome4UnitDisplay Display, const FString& Phase = FString());
    TOptional<double> BudgetImbalance(const FStudioHome4EnergyBudget& Budget);
    bool CanNormalize(const FStudioHome4Sample* Sample, int32 Component, const FString& BodyId = FString());
}
