#include "StudioMaterials.h"
#include "StudioColor.h"
#include <charconv>

namespace
{
bool SameOptional(const TOptional<double>& A,const TOptional<double>& B)
{return A.IsSet()==B.IsSet()&&(!A.IsSet()||A.GetValue()==B.GetValue());}
bool ValidProperty(EStudioMaterialProperty P){return uint8(P)<4;}
bool Parse(const FString& Text,EStudioMaterialProperty P,int32 Unit,TOptional<double>& Out,FString& Error)
{
    if(Text.TrimStartAndEnd().IsEmpty()){Out.Reset();return true;}
    double Number=0;
    if(!StudioColor::ParseNumber(Text,Number)||Number<=0)
    {Error=FString(StudioMaterials::PropertyName(P))+TEXT(": enter a positive number, or leave blank for unknown.");return false;}
    const double SI=Number*StudioMaterials::UnitScale(P,Unit);
    if(!FMath::IsFinite(SI)||SI<=0||SI>1.e12)
    {Error=FString(StudioMaterials::PropertyName(P))+TEXT(": converted value is outside the document range (greater than zero, at most 1e12 in SI units).");return false;}
    Out=SI;return true;
}
}

const TCHAR* StudioMaterials::PropertyName(EStudioMaterialProperty P)
{
    switch(P)
    {
    case EStudioMaterialProperty::Density:return TEXT("Density");
    case EStudioMaterialProperty::KinematicViscosity:return TEXT("Kinematic viscosity");
    case EStudioMaterialProperty::ThermalConductivity:return TEXT("Thermal conductivity");
    case EStudioMaterialProperty::SpecificHeat:return TEXT("Specific heat");
    default:return TEXT("Property");
    }
}
int32 StudioMaterials::UnitCount(EStudioMaterialProperty P)
{return P==EStudioMaterialProperty::Density||P==EStudioMaterialProperty::KinematicViscosity?2:1;}
const TCHAR* StudioMaterials::UnitLabel(EStudioMaterialProperty P,int32 Unit)
{
    switch(P)
    {
    case EStudioMaterialProperty::Density:return Unit==1?TEXT("g/cm³"):TEXT("kg/m³");
    case EStudioMaterialProperty::KinematicViscosity:return Unit==1?TEXT("mm²/s"):TEXT("m²/s");
    case EStudioMaterialProperty::ThermalConductivity:return TEXT("W/(m·K)");
    case EStudioMaterialProperty::SpecificHeat:return TEXT("J/(kg·K)");
    default:return TEXT("");
    }
}
double StudioMaterials::UnitScale(EStudioMaterialProperty P,int32 Unit)
{
    if(P==EStudioMaterialProperty::Density&&Unit==1)return 1000.;
    if(P==EStudioMaterialProperty::KinematicViscosity&&Unit==1)return 1.e-6;
    return 1.;
}
FString StudioMaterials::ExactNumber(double Value)
{
    ANSICHAR Buffer[128];const auto Result=std::to_chars(Buffer,Buffer+UE_ARRAY_COUNT(Buffer)-1,Value);
    if(Result.ec==std::errc()){*Result.ptr='\0';return FString(UTF8_TO_TCHAR(Buffer));}
    return FString::Printf(TEXT("%.17g"),Value);
}
TOptional<double> StudioMaterials::Value(const FStudioMaterial& M,EStudioMaterialProperty P)
{
    switch(P)
    {
    case EStudioMaterialProperty::Density:return M.Density;
    case EStudioMaterialProperty::KinematicViscosity:return M.KinematicViscosity;
    case EStudioMaterialProperty::ThermalConductivity:return M.ThermalConductivity;
    case EStudioMaterialProperty::SpecificHeat:return M.SpecificHeat;
    default:return {};
    }
}
void FStudioMaterialEdit::Reset(const FStudioMaterial& Material,bool bKeepUnits)
{
    Saved=Material;Name=Material.Name;bSolid=Material.bSolid;Error.Empty();ErrorProperty=INDEX_NONE;
    for(int32 I=0;I<4;++I)
    {
        const auto P=EStudioMaterialProperty(I);const auto V=StudioMaterials::Value(Material,P);
        if(!bKeepUnits||Units[I]<0||Units[I]>=StudioMaterials::UnitCount(P))Units[I]=0;
        if(V.IsSet()&&V.GetValue()/StudioMaterials::UnitScale(P,Units[I])<=0)Units[I]=0;
        Values[I]=V.IsSet()?StudioMaterials::ExactNumber(V.GetValue()/StudioMaterials::UnitScale(P,Units[I])):FString();
    }
}
bool FStudioMaterialEdit::Matches(const FStudioMaterial& Material) const
{
    if(Material.Id!=Saved.Id||Material.Name!=Saved.Name||Material.bSolid!=Saved.bSolid)return false;
    for(int32 I=0;I<4;++I)if(!SameOptional(StudioMaterials::Value(Material,EStudioMaterialProperty(I)),StudioMaterials::Value(Saved,EStudioMaterialProperty(I))))return false;
    return true;
}
bool FStudioMaterialEdit::IsDirty() const
{
    if(Name!=Saved.Name||bSolid!=Saved.bSolid)return true;
    for(int32 I=0;I<4;++I)
    {
        const auto V=StudioMaterials::Value(Saved,EStudioMaterialProperty(I));
        const FString Expected=V.IsSet()?StudioMaterials::ExactNumber(V.GetValue()/StudioMaterials::UnitScale(EStudioMaterialProperty(I),Units[I])):FString();
        if(Values[I]!=Expected)return true;
    }
    return false;
}
bool FStudioMaterialEdit::ChangeUnit(EStudioMaterialProperty P,int32 Unit)
{
    if(!ValidProperty(P)||Unit<0||Unit>=StudioMaterials::UnitCount(P))return false;
    const int32 I=int32(P);if(Unit==Units[I])return true;
    TOptional<double> SI;
    const auto Previous=StudioMaterials::Value(Saved,P);
    const FString Existing=Previous.IsSet()?StudioMaterials::ExactNumber(Previous.GetValue()/StudioMaterials::UnitScale(P,Units[I])):FString();
    if(Values[I]==Existing)SI=Previous;
    else if(!Parse(Values[I],P,Units[I],SI,Error)){ErrorProperty=I;return false;}
    const double Converted=SI.IsSet()?SI.GetValue()/StudioMaterials::UnitScale(P,Unit):0;
    if(SI.IsSet()&&(!FMath::IsFinite(Converted)||Converted<=0))
    {Error=TEXT("This value cannot be represented in the selected unit. Keep the current unit.");ErrorProperty=I;return false;}
    Values[I]=SI.IsSet()?StudioMaterials::ExactNumber(Converted):FString();
    Units[I]=Unit;Error.Empty();ErrorProperty=INDEX_NONE;return true;
}
bool FStudioMaterialEdit::Build(FStudioMaterial& Out)
{
    Error.Empty();ErrorProperty=INDEX_NONE;
    const FString Clean=Name.TrimStartAndEnd();
    if(Clean.IsEmpty()||Clean.Len()>120){Error=TEXT("Name: enter 1–120 characters.");return false;}
    TOptional<double> Parsed[4];
    for(int32 I=0;I<4;++I)
    {
        if(Units[I]<0||Units[I]>=StudioMaterials::UnitCount(EStudioMaterialProperty(I)))
        {Error=TEXT("Choose a supported property unit.");ErrorProperty=I;return false;}
        // Unchanged text keeps the exact stored SI value even after a unit switch.
        const auto Previous=StudioMaterials::Value(Saved,EStudioMaterialProperty(I));
        const FString Existing=Previous.IsSet()?StudioMaterials::ExactNumber(Previous.GetValue()/StudioMaterials::UnitScale(EStudioMaterialProperty(I),Units[I])):FString();
        if(Values[I]==Existing)Parsed[I]=Previous;
        else if(!Parse(Values[I],EStudioMaterialProperty(I),Units[I],Parsed[I],Error)){ErrorProperty=I;return false;}
    }
    FStudioMaterial Candidate=Saved;Candidate.Name=Clean;Candidate.bSolid=bSolid;
    Candidate.Density=Parsed[0];Candidate.KinematicViscosity=Parsed[1];Candidate.ThermalConductivity=Parsed[2];Candidate.SpecificHeat=Parsed[3];
    Out=MoveTemp(Candidate);return true;
}
int32 StudioMaterials::AssignmentCount(const FStudioCaseDraft& Case,const FGuid& Id)
{
    if(!Id.IsValid())return 0;
    int32 Count=Case.Domain.FluidMaterialId==Id?1:0;
    for(const auto& Geometry:Case.Geometry)if(Geometry.MaterialId==Id)++Count;
    return Count;
}
bool StudioMaterials::Remove(FStudioCaseDraft& Case,const FGuid& Id,bool bUnassign,FString& Error)
{
    if(!Case.Materials.ContainsByPredicate([Id](const auto& M){return M.Id==Id;}))
    {Error=TEXT("This material no longer exists in the current case.");return false;}
    if(!bUnassign&&AssignmentCount(Case,Id))
    {Error=TEXT("Material is assigned. Use Unassign and delete to clear its draft assignments in the same undoable edit.");return false;}
    if(Case.Domain.FluidMaterialId==Id)Case.Domain.FluidMaterialId.Invalidate();
    for(auto& Geometry:Case.Geometry)if(Geometry.MaterialId==Id)Geometry.MaterialId.Invalidate();
    Case.Materials.RemoveAll([Id](const auto& M){return M.Id==Id;});Error.Empty();return true;
}
