#include "StudioHome4Readouts.h"

FString StudioHome4Readouts::Unit(EStudioHome4Quantity Q,EStudioHome4UnitDisplay Display)
{
    if(Q==EStudioHome4Quantity::Dimensionless||Display==EStudioHome4UnitDisplay::Nondimensional)return TEXT("1");
    const bool SI=Display==EStudioHome4UnitDisplay::Physical;
    switch(Q)
    {
    case EStudioHome4Quantity::Length:return SI?TEXT("m"):TEXT("cells");
    case EStudioHome4Quantity::Time:return SI?TEXT("s"):TEXT("steps");
    case EStudioHome4Quantity::Velocity:return SI?TEXT("m/s"):TEXT("cells/step");
    case EStudioHome4Quantity::KinematicViscosity:case EStudioHome4Quantity::Mobility:return SI?TEXT("m²/s"):TEXT("cells²/step");
    case EStudioHome4Quantity::Acceleration:return SI?TEXT("m/s²"):TEXT("cells/step²");
    case EStudioHome4Quantity::Density:return SI?TEXT("kg/m³"):TEXT("lu");
    case EStudioHome4Quantity::Pressure:return SI?TEXT("Pa"):TEXT("lu");
    case EStudioHome4Quantity::SurfaceTension:return SI?TEXT("N/m"):TEXT("lu");
    case EStudioHome4Quantity::Force:return SI?TEXT("N"):TEXT("lu");
    case EStudioHome4Quantity::Moment:case EStudioHome4Quantity::Energy:return SI?TEXT("N m"):TEXT("lu");
    case EStudioHome4Quantity::StrainRate:return SI?TEXT("1/s"):TEXT("1/step");
    case EStudioHome4Quantity::SquaredRate:return SI?TEXT("1/s²"):TEXT("1/step²");
    case EStudioHome4Quantity::SpecificDissipation:return SI?TEXT("m²/s³"):TEXT("cells²/step³");
    default:return TEXT("1");
    }
}
FString StudioHome4Readouts::Value(double V,EStudioHome4Quantity Q,EStudioHome4UnitDisplay From,EStudioHome4UnitDisplay To,const FStudioHome4Spec* Map)
{
    if(!FMath::IsFinite(V))return TEXT("Nonfinite");
    TOptional<double> Result;
    if(From==To||Q==EStudioHome4Quantity::Dimensionless)Result=V;
    else if(Map)Result=StudioHome4Config::ConvertUnits(V,Q,From,To,*Map);
    return Result.IsSet()?FString::Printf(TEXT("%.6g %s"),Result.GetValue(),*Unit(Q,To)):TEXT("Not supplied · source map required");
}
FString StudioHome4Readouts::Tooltip(double V,EStudioHome4Quantity Q,EStudioHome4UnitDisplay From,const FStudioHome4Spec* Map)
{
    return TEXT("Lattice: ")+Value(V,Q,From,EStudioHome4UnitDisplay::Lattice,Map)+TEXT("\nPhysical: ")+Value(V,Q,From,EStudioHome4UnitDisplay::Physical,Map)+TEXT("\nNondimensional: ")+Value(V,Q,From,EStudioHome4UnitDisplay::Nondimensional,Map);
}
FString StudioHome4Readouts::Time(int64 Step,const FStudioHome4Spec* Map,TOptional<double> PhysicalTime)
{
    return FString::Printf(TEXT("Step %lld · "),Step)+(PhysicalTime.IsSet()?Value(PhysicalTime.GetValue(),EStudioHome4Quantity::Time,EStudioHome4UnitDisplay::Physical,EStudioHome4UnitDisplay::Physical,Map):Value(double(Step),EStudioHome4Quantity::Time,EStudioHome4UnitDisplay::Lattice,EStudioHome4UnitDisplay::Physical,Map))+TEXT(" · t* ")+Value(double(Step),EStudioHome4Quantity::Time,EStudioHome4UnitDisplay::Lattice,EStudioHome4UnitDisplay::Nondimensional,Map);
}

FString StudioHome4Readouts::Scalar(double V,const FString& SourceUnit,EStudioHome4UnitDisplay To,const FStudioHome4Spec* Map,bool IsTooltip)
{
    struct FUnit {const TCHAR* Name;EStudioHome4Quantity Quantity;EStudioHome4UnitDisplay From;};
    using Q=EStudioHome4Quantity;using D=EStudioHome4UnitDisplay;
    static const FUnit Units[]={
        {TEXT("1"),Q::Dimensionless,D::Lattice},{TEXT(""),Q::Dimensionless,D::Lattice},
        {TEXT("m/s"),Q::Velocity,D::Physical},{TEXT("lu_velocity"),Q::Velocity,D::Lattice},
        {TEXT("m"),Q::Length,D::Physical},{TEXT("lu_length"),Q::Length,D::Lattice},{TEXT("cells"),Q::Length,D::Lattice},
        {TEXT("Pa"),Q::Pressure,D::Physical},{TEXT("lu_pressure"),Q::Pressure,D::Lattice},
        {TEXT("kg/m3"),Q::Density,D::Physical},{TEXT("kg/m³"),Q::Density,D::Physical},{TEXT("lu_density"),Q::Density,D::Lattice},
        {TEXT("m2/s"),Q::KinematicViscosity,D::Physical},{TEXT("m²/s"),Q::KinematicViscosity,D::Physical},{TEXT("cells2/step"),Q::KinematicViscosity,D::Lattice},
        {TEXT("1/s"),Q::StrainRate,D::Physical},{TEXT("1/step"),Q::StrainRate,D::Lattice},
        {TEXT("1/s2"),Q::SquaredRate,D::Physical},{TEXT("1/s²"),Q::SquaredRate,D::Physical},{TEXT("1/step2"),Q::SquaredRate,D::Lattice},
        {TEXT("m/s2"),Q::Acceleration,D::Physical},{TEXT("m/s²"),Q::Acceleration,D::Physical},
        {TEXT("m2/s3"),Q::SpecificDissipation,D::Physical},{TEXT("m²/s³"),Q::SpecificDissipation,D::Physical},{TEXT("cells2/step3"),Q::SpecificDissipation,D::Lattice}};
    if(Map)for(const auto& U:Units)if(SourceUnit==U.Name)return IsTooltip?Tooltip(V,U.Quantity,U.From,Map):Value(V,U.Quantity,U.From,To,Map);
    return FString::Printf(TEXT("%.6g %s%s"),V,*SourceUnit,IsTooltip?TEXT(" · original source units; conversion map or quantity convention not supplied"):TEXT(""));
}
