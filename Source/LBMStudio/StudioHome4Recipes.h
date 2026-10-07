#pragma once
#include "StudioHome4Config.h"

/** Published recipe descriptions are templates, never evidence that this app ran a gate. */
struct FStudioHome4Recipe
{
    FString Id, Name, Anchor, Driver, Reference, Gate, Notes;
    FStudioHome4Spec Template;
};
struct FStudioHome4LadderRung
{
    int32 Refinement = 1;
    FStudioHome4Spec Spec;
    TOptional<double> EstimatedSeconds;
};
struct FStudioHome4GateResult
{
    bool bEvaluated = false, bPassed = false;
    FString Reason;
    TOptional<double> MaximumAbsoluteError, RelativeL2Error;
};
namespace StudioHome4Recipes
{
    const TArray<FStudioHome4Recipe>& All();
    const FStudioHome4Recipe* Find(const FString& Id);
    /** Every changed supplied recipe value is reported; unspecified values have no asserted envelope. */
    TArray<FString> Departures(const FStudioHome4Spec& Spec);
    bool Ladder(const FStudioHome4Spec& Base, const TArray<int32>& Refinements,
        TArray<FStudioHome4LadderRung>& Out, FString& Error);
    /** Explicitly paired values only: reference alignment and tolerance belong to the caller. */
    FStudioHome4GateResult Compare(const TArray<double>& Actual, const TArray<double>& Reference,
        double AbsoluteTolerance, double RelativeTolerance, const FString& ReferenceIdentity);
    TOptional<double> ObservedOrder(double Coarse, double Medium, double Fine, double Refinement);
}
