#pragma once
#include "StudioHome4Authoring.h"
#include "StudioCase.h"
struct FStudioHome4GeometricMassProperties
{
    double Volume=0,Mass=0;
    FVector CenterOfGravity=FVector::ZeroVector,InertiaDiagonal=FVector::ZeroVector,InertiaProducts=FVector::ZeroVector;
    FString Frame;
};
namespace StudioHome4Body
{
    /** Explicit uniform-density mass properties of verified closed posed triangle geometry. */
    bool MassProperties(const FStudioHome4AuthoringPreview&,double Density,FStudioHome4GeometricMassProperties&,FString& Error);
    bool AdoptMassProperties(const FStudioHome4AuthoringPreview&,double Density,FStudioHome4Spec&,FString& Error);
    bool Bind(const FStudioGeometryAsset&,const FStudioHome4Spec&,FStudioHome4Spec&,FString& Error);
    /** K heave/pitch only; preserve every other user matrix entry. */
    bool AdoptHydrostaticStiffness(const FStudioHome4AuthoringPreview&,FStudioHome4Spec&,FString& Error);
}
