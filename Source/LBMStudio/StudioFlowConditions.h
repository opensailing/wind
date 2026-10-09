#pragma once
#include "StudioCase.h"

/** Retained SI case requests, independent of recorded fields and boundary assignments.
 * Reynolds uses reference inlet speed, reference length and the assigned fluid's
 * kinematic viscosity. This calculator makes no LBM conversion or stability claim. */
struct FStudioFlowConditionsEdit
{
    enum EField : int32 { VelocityX, VelocityY, VelocityZ, Pressure, Length, Density, Reynolds, FieldCount };
    FGuid CaseId, FluidId;
    FStudioCaseSetup Saved;
    TOptional<double> Viscosity;
    FString Values[FieldCount];
    int32 Units[FieldCount]{};
    FString Error;
    int32 ErrorField = INDEX_NONE;

    void Reset(const FStudioCaseDraft& Case, bool bKeepUnits = true);
    bool Matches(const FStudioCaseDraft& Case) const;
    bool IsDirty() const;
    bool ChangeUnit(EField Field, int32 Unit);
    /** Leaves output unchanged on failure. Blank values remain unset. */
    bool Build(FStudioCaseSetup& Out);
    /** Explicit draft-only calculator actions. No mutation of the applied case. */
    bool UseCalculatedReynolds();
    bool UseTargetSpeed();
    TOptional<double> CalculatedReynolds() const;
    static void CopySettings(const FStudioCaseSetup& From, FStudioCaseSetup& To);
    static const FStudioMaterial* Fluid(const FStudioCaseDraft& Case);
    static const TCHAR* UnitLabel(EField Field, int32 Unit);
    static int32 UnitCount(EField Field);
private:
    FString SavedText(int32 Field) const;
    bool Parse(int32 Field, TOptional<double>& Out, FString& Reason) const;
    bool ParseSetup(FStudioCaseSetup& Out, bool bIgnoreReynolds = false);
    static double UnitScale(EField Field, int32 Unit);
};
