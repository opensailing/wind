#pragma once

#include "StudioCase.h"

enum class EStudioMaterialProperty : uint8 { Density, KinematicViscosity, ThermalConductivity, SpecificHeat };

/** Retained UI text is separate from the saved SI material record. Empty is unknown. */
struct FStudioMaterialEdit
{
    FStudioMaterial Saved;
    FString Name;
    bool bSolid = false;
    FString Values[4];
    int32 Units[4] = {0,0,0,0};
    FString Error;
    int32 ErrorProperty = INDEX_NONE;

    void Reset(const FStudioMaterial& Material,bool bKeepUnits=false);
    bool IsDirty() const;
    bool Matches(const FStudioMaterial& Material) const;
    bool ChangeUnit(EStudioMaterialProperty Property,int32 Unit);
    bool Build(FStudioMaterial& Out);
};

namespace StudioMaterials
{
    const TCHAR* PropertyName(EStudioMaterialProperty Property);
    const TCHAR* UnitLabel(EStudioMaterialProperty Property,int32 Unit);
    int32 UnitCount(EStudioMaterialProperty Property);
    double UnitScale(EStudioMaterialProperty Property,int32 Unit);
    FString ExactNumber(double Value);
    TOptional<double> Value(const FStudioMaterial& Material,EStudioMaterialProperty Property);
    int32 AssignmentCount(const FStudioCaseDraft& Case,const FGuid& MaterialId);
    /** Clear only draft assignments; frozen run snapshots remain immutable. */
    bool Remove(FStudioCaseDraft& Case,const FGuid& MaterialId,bool bUnassign,FString& Error);
}
