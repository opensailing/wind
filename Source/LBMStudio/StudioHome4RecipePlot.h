#pragma once
#include "StudioHome4Recipes.h"
struct FStudioHome4RecipePlotFamily
{
    FString Id,Label,Reason;
    TArray<FVector2D> Points; // L cells, Ma; complete original template or explicitly stated parameter family.
    bool bOriginalPoint=false;
};
namespace StudioHome4RecipePlot
{
    TArray<FStudioHome4RecipePlotFamily> Families(const FStudioHome4Spec& Active);
    /** Numerical requested-envelope shading only; never a benchmark validation badge. */
    bool WithinNumericalEnvelopeDerived(const FStudioHome4Spec&,const FStudioHome4Derived&,double Length,double Mach);
    bool WithinNumericalEnvelope(const FStudioHome4Spec& Active,double Length,double Mach);
}
