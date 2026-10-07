#pragma once
#include "CoreMinimal.h"
class FJsonObject;
/** Explicit steady Newtonian circular-Couette side torque. No box-wall or end
 * correction is guessed. Inner-cylinder fluid torque follows the supplied axis. */
struct FStudioHome4CouetteInputs
{
    FString Domain, Units, TorqueConvention;
    double DynamicViscosity=0, InnerRadius=0, InnerOmega=0, OuterOmega=0;
    TOptional<double> OuterRadius, AxialSpan;
    bool bSteadyNewtonianNoEndEffects=false;
};
namespace StudioHome4Couette
{
    bool Parse(const TSharedPtr<FJsonObject>& Object,FStudioHome4CouetteInputs& Out,FString& Error);
    bool Torque(const FStudioHome4CouetteInputs& Inputs,double& Out,FString& Unit,FString& Error);
    FString FormulaDescription();
    FString CitationURL();
}
