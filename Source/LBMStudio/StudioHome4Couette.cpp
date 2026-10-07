#include "StudioHome4Couette.h"
#include "Dom/JsonObject.h"

bool StudioHome4Couette::Parse(const TSharedPtr<FJsonObject>& O,FStudioHome4CouetteInputs& Out,FString& Error)
{
    if(!O){Error=TEXT("Explicit original Couette inputs must be an object.");return false;}
    FStudioHome4CouetteInputs C;
    auto N=[&](const TCHAR* K,double& V){const auto F=O->TryGetField(K);return F&&F->Type==EJson::Number&&F->TryGetNumber(V)&&FMath::IsFinite(V);};
    const auto Conditions=O->TryGetField(TEXT("steady_newtonian_no_end_effects"));
    if(!O->TryGetStringField(TEXT("domain"),C.Domain)||!O->TryGetStringField(TEXT("units"),C.Units)||
        !O->TryGetStringField(TEXT("torque_convention"),C.TorqueConvention)||!N(TEXT("dynamic_viscosity"),C.DynamicViscosity)||
        !N(TEXT("inner_radius"),C.InnerRadius)||!N(TEXT("inner_omega"),C.InnerOmega)||!N(TEXT("outer_omega"),C.OuterOmega)||
        !Conditions||Conditions->Type!=EJson::Boolean||!Conditions->TryGetBool(C.bSteadyNewtonianNoEndEffects))
    {Error=TEXT("Couette inputs need explicit domain, units, torque convention, mu, radius, inner/outer omega and the steady Newtonian/no-end-effects condition.");return false;}
    for(const TCHAR* Key:{TEXT("outer_radius"),TEXT("axial_span")})
    {
        const auto V=O->TryGetField(Key);if(!V||V->Type==EJson::Null)continue;double D=0;
        if(!N(Key,D)){Error=TEXT("Couette radii/span must be finite explicit numbers.");return false;}
        if(FString(Key)==TEXT("outer_radius"))C.OuterRadius=D;else C.AxialSpan=D;
    }
    double Value=0;FString Unit;if(!Torque(C,Value,Unit,Error))return false;Out=MoveTemp(C);return true;
}
bool StudioHome4Couette::Torque(const FStudioHome4CouetteInputs& C,double& Out,FString& Unit,FString& Error)
{
    const bool PerLength=C.TorqueConvention==TEXT("fluid_on_inner_per_length"),Total=C.TorqueConvention==TEXT("fluid_on_inner_total");
    if((C.Domain!=TEXT("concentric_cylinders")&&C.Domain!=TEXT("unbounded_stationary_far_field"))||
        (C.Units!=TEXT("SI")&&C.Units!=TEXT("lattice"))||(!PerLength&&!Total)||!C.bSteadyNewtonianNoEndEffects||
        !FMath::IsFinite(C.DynamicViscosity)||C.DynamicViscosity<=0||!FMath::IsFinite(C.InnerRadius)||C.InnerRadius<=0||
        !FMath::IsFinite(C.InnerOmega)||!FMath::IsFinite(C.OuterOmega)||
        (Total&&(!C.AxialSpan||!FMath::IsFinite(*C.AxialSpan)||*C.AxialSpan<=0))||
        (PerLength&&C.AxialSpan.IsSet()))
    {Error=TEXT("Analytic Couette requires a declared steady Newtonian circular/unbounded domain, SI or lattice units, positive original mu/radius and an explicit total span or per-length convention.");return false;}
    double Factor=1;
    if(C.Domain==TEXT("concentric_cylinders"))
    {
        if(!C.OuterRadius||!FMath::IsFinite(*C.OuterRadius)||*C.OuterRadius<=C.InnerRadius)
        {Error=TEXT("Concentric Couette requires an explicit outer radius larger than the inner radius.");return false;}
        const double Ratio=C.InnerRadius/ *C.OuterRadius,Denominator=(1-Ratio)*(1+Ratio);
        if(Denominator<=1.e-12){Error=TEXT("Supplied Couette gap is numerically unresolved.");return false;}Factor=1/Denominator;
    }
    else if(C.OuterRadius.IsSet()||C.OuterOmega!=0)
    {Error=TEXT("Unbounded stationary-far-field Couette requires no outer radius and exactly zero far-field omega.");return false;}
    const double Value=-4.*UE_DOUBLE_PI*C.DynamicViscosity*C.InnerRadius*C.InnerRadius*(C.InnerOmega-C.OuterOmega)*Factor*(Total?*C.AxialSpan:1.);
    if(!FMath::IsFinite(Value)){Error=TEXT("Analytic Couette torque exceeds finite arithmetic range.");return false;}
    Unit=C.Units==TEXT("SI")?(Total?TEXT("N m"):TEXT("N")):(Total?TEXT("lattice torque"):TEXT("lattice torque per cell"));
    Out=Value;Error.Empty();return true;
}
FString StudioHome4Couette::FormulaDescription()
{return TEXT("Steady Newtonian side torque on inner cylinder: -4*pi*mu*Ri^2*(Omega_i-Omega_o)/(1-(Ri/Ro)^2), multiplied by declared axial span for total torque. Unbounded stationary far-field uses factor 1. No finite-box or end correction, stability or CFD result is inferred.");}
FString StudioHome4Couette::CitationURL()
{return TEXT("https://farside.ph.utexas.edu/teaching/336L/Fluidhtml/node137.html");}
