#include "StudioFlowConditions.h"
#include "StudioColor.h"
#include "StudioMaterials.h"
#include <cmath>
#include <limits>

namespace
{
TOptional<double> Value(const FStudioCaseSetup& S, int32 Field)
{
    if(Field <= FStudioFlowConditionsEdit::VelocityZ)
        return S.InletVelocity ? TOptional<double>((*S.InletVelocity)[Field]) : TOptional<double>();
    switch(Field)
    {
    case FStudioFlowConditionsEdit::Pressure:return S.OutletPressure;
    case FStudioFlowConditionsEdit::Length:return S.ReferenceLength;
    case FStudioFlowConditionsEdit::Density:return S.ReferenceDensity;
    case FStudioFlowConditionsEdit::Reynolds:return S.ReynoldsNumber;
    default:return {};
    }
}
TOptional<double> ReynoldsOf(const FStudioCaseSetup& S, const TOptional<double>& Nu)
{
    if(!S.InletVelocity || !S.ReferenceLength || !Nu || *Nu <= 0)return {};
    const auto& U=*S.InletVelocity;
    const long double Re=std::hypot(U.X,U.Y,U.Z)*static_cast<long double>(*S.ReferenceLength)/ *Nu;
    if(!std::isfinite(Re) || Re<0 || Re>std::numeric_limits<double>::max())return {};
    const double Result=static_cast<double>(Re);
    if(Re>0 && Result==0)return {};
    return Result;
}
}
const FStudioMaterial* FStudioFlowConditionsEdit::Fluid(const FStudioCaseDraft& Case)
{
    return Case.Materials.FindByPredicate([&](const auto& M){return M.Id==Case.Domain.FluidMaterialId&&!M.bSolid;});
}
int32 FStudioFlowConditionsEdit::UnitCount(EField Field)
{return Field==Length?3:Field==Reynolds?1:2;}
const TCHAR* FStudioFlowConditionsEdit::UnitLabel(EField Field,int32 Unit)
{
    if(Field<=VelocityZ)return Unit==1?TEXT("km/h"):TEXT("m/s");
    if(Field==Pressure)return Unit==1?TEXT("kPa"):TEXT("Pa");
    if(Field==Length)return Unit==1?TEXT("cm"):Unit==2?TEXT("mm"):TEXT("m");
    if(Field==Density)return Unit==1?TEXT("g/cm³"):TEXT("kg/m³");
    return TEXT("dimensionless");
}
double FStudioFlowConditionsEdit::UnitScale(EField Field,int32 Unit)
{
    if(Field<=VelocityZ)return Unit==1?1./3.6:1.;
    if(Field==Pressure || Field==Density)return Unit==1?1000.:1.;
    if(Field==Length)return Unit==1?.01:Unit==2?.001:1.;
    return 1.;
}
FString FStudioFlowConditionsEdit::SavedText(int32 Field) const
{
    const auto V=Value(Saved,Field);
    return V?StudioMaterials::ExactNumber(*V/UnitScale(EField(Field),Units[Field])):FString();
}
void FStudioFlowConditionsEdit::Reset(const FStudioCaseDraft& Case,bool bKeepUnits)
{
    CaseId=Case.Id;Saved=Case.Setup;FluidId=Case.Domain.FluidMaterialId;
    const auto* M=Fluid(Case);Viscosity=M?M->KinematicViscosity:TOptional<double>();
    for(int32 I=0;I<FieldCount;++I)
    {
        if(!bKeepUnits || Units[I]<0 || Units[I]>=UnitCount(EField(I)))Units[I]=0;
        const auto V=Value(Saved,I);
        if(V && *V!=0 && *V/UnitScale(EField(I),Units[I])==0)Units[I]=0;
        Values[I]=SavedText(I);
    }
    Error.Empty();ErrorField=INDEX_NONE;
}
bool FStudioFlowConditionsEdit::Matches(const FStudioCaseDraft& Case) const
{
    if(CaseId!=Case.Id || FluidId!=Case.Domain.FluidMaterialId)return false;
    const auto* M=Fluid(Case);
    if(Viscosity!=(M?M->KinematicViscosity:TOptional<double>()))return false;
    for(int32 I=0;I<FieldCount;++I)if(Value(Saved,I)!=Value(Case.Setup,I))return false;
    return true;
}
bool FStudioFlowConditionsEdit::IsDirty() const
{
    for(int32 I=0;I<FieldCount;++I)if(Values[I]!=SavedText(I))return true;
    return false;
}
bool FStudioFlowConditionsEdit::Parse(int32 Field,TOptional<double>& Out,FString& Reason) const
{
    if(Units[Field]<0 || Units[Field]>=UnitCount(EField(Field)))
    {Reason=TEXT("Choose a supported unit.");return false;}
    if(Values[Field]==SavedText(Field)){Out=Value(Saved,Field);return true;}
    if(Values[Field].TrimStartAndEnd().IsEmpty()){Out.Reset();return true;}
    double Parsed;
    if(!StudioColor::ParseNumber(Values[Field],Parsed))
    {Reason=TEXT("Enter a finite number with a decimal point, or leave blank for unspecified.");return false;}
    const double SI=Parsed*UnitScale(EField(Field),Units[Field]);
    if(!FMath::IsFinite(SI) || FMath::Abs(SI)>1.e12 || (Parsed!=0 && SI==0) || (Field>=Length && SI<=0))
    {Reason=Field>=Length?TEXT("Enter a value above zero and at most 1e12 in SI units."):TEXT("Enter a value between −1e12 and 1e12 in SI units.");return false;}
    Out=SI;return true;
}
bool FStudioFlowConditionsEdit::ChangeUnit(EField Field,int32 Unit)
{
    if(Field<0 || Field>=FieldCount || Unit<0 || Unit>=UnitCount(Field))return false;
    const int32 First=Field<=VelocityZ?0:int32(Field),Last=Field<=VelocityZ?2:int32(Field);
    FString NewText[FieldCount];
    for(int32 I=First;I<=Last;++I)
    {
        TOptional<double> SI;FString Reason;
        if(!Parse(I,SI,Reason)){Error=Reason;ErrorField=I;return false;}
        const double Converted=SI?*SI/UnitScale(EField(I),Unit):0;
        if(SI && (!FMath::IsFinite(Converted) || (*SI!=0 && Converted==0)))
        {Error=TEXT("This value cannot be represented in that unit. Keep the current unit.");ErrorField=I;return false;}
        NewText[I]=SI?StudioMaterials::ExactNumber(Converted):FString();
    }
    for(int32 I=First;I<=Last;++I){Units[I]=Unit;Values[I]=MoveTemp(NewText[I]);}
    Error.Empty();ErrorField=INDEX_NONE;return true;
}
bool FStudioFlowConditionsEdit::ParseSetup(FStudioCaseSetup& Out,bool bIgnoreReynolds)
{
    Error.Empty();ErrorField=INDEX_NONE;TOptional<double> V[FieldCount];
    for(int32 I=0;I<FieldCount;++I)
    {
        if(bIgnoreReynolds && I==Reynolds)continue;
        if(!Parse(I,V[I],Error)){ErrorField=I;return false;}
    }
    const int32 Components=int32(V[0].IsSet())+int32(V[1].IsSet())+int32(V[2].IsSet());
    if(Components!=0 && Components!=3)
    {Error=TEXT("Enter all three inlet components, or leave all three blank.");for(int32 I=0;I<3;++I)if(!V[I]){ErrorField=I;break;}return false;}
    auto Candidate=Saved;
    Candidate.InletVelocity=Components==3?TOptional<FVector>(FVector(*V[0],*V[1],*V[2])):TOptional<FVector>();
    Candidate.OutletPressure=V[Pressure];Candidate.ReferenceLength=V[Length];Candidate.ReferenceDensity=V[Density];Candidate.ReynoldsNumber=V[Reynolds];
    Out=MoveTemp(Candidate);return true;
}
bool FStudioFlowConditionsEdit::Build(FStudioCaseSetup& Out){return ParseSetup(Out);}
TOptional<double> FStudioFlowConditionsEdit::CalculatedReynolds() const
{
    auto Copy=*this;FStudioCaseSetup Parsed;
    if(!Copy.ParseSetup(Parsed,true))return {};
    return ReynoldsOf(Parsed,Viscosity);
}
bool FStudioFlowConditionsEdit::UseCalculatedReynolds()
{
    FStudioCaseSetup Parsed;if(!ParseSetup(Parsed,true))return false;
    const auto Re=ReynoldsOf(Parsed,Viscosity);
    if(!Re || *Re<=0 || *Re>1.e12)
    {Error=TEXT("Calculate Re needs a nonzero inlet, reference length and assigned fluid viscosity; the result must be above zero and at most 1e12.");ErrorField=Reynolds;return false;}
    Values[Reynolds]=StudioMaterials::ExactNumber(*Re);return true;
}
bool FStudioFlowConditionsEdit::UseTargetSpeed()
{
    FStudioCaseSetup Parsed;if(!ParseSetup(Parsed))return false;
    const double Speed=Parsed.InletVelocity?std::hypot(Parsed.InletVelocity->X,Parsed.InletVelocity->Y,Parsed.InletVelocity->Z):0;
    if(!Parsed.ReynoldsNumber || !Parsed.ReferenceLength || !Viscosity || *Viscosity<=0 || Speed<=0)
    {Error=TEXT("Set speed needs a target Re, reference length, assigned fluid viscosity and a nonzero inlet direction.");ErrorField=Reynolds;return false;}
    const long double Target=static_cast<long double>(*Parsed.ReynoldsNumber)* *Viscosity/ *Parsed.ReferenceLength;
    if(!std::isfinite(Target) || Target<=0 || Target>1.e12 || static_cast<double>(Target)==0)
    {Error=TEXT("Calculated inlet speed is outside the supported SI range. Adjust Re, length or viscosity.");ErrorField=Reynolds;return false;}
    FString NewText[3];
    for(int32 I=0;I<3;++I)
    {
        const double Component=((*Parsed.InletVelocity)[I]/Speed)*static_cast<double>(Target);
        if((*Parsed.InletVelocity)[I]!=0 && Component==0)
        {Error=TEXT("Calculated direction loses a component at this scale. Adjust Re, length or viscosity.");ErrorField=I;return false;}
        NewText[I]=StudioMaterials::ExactNumber(Component/UnitScale(EField(I),Units[I]));
    }
    for(int32 I=0;I<3;++I)Values[I]=MoveTemp(NewText[I]);
    return true;
}
void FStudioFlowConditionsEdit::CopySettings(const FStudioCaseSetup& From,FStudioCaseSetup& To)
{
    To.InletVelocity=From.InletVelocity;To.OutletPressure=From.OutletPressure;
    To.ReferenceLength=From.ReferenceLength;To.ReferenceDensity=From.ReferenceDensity;To.ReynoldsNumber=From.ReynoldsNumber;
}
