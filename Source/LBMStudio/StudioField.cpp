#include "StudioModel.h"
#include "StudioSurfaceReconstruction.h"
#include <limits>

int32 IStudioField::MeshTriangleCount() const
{const auto R=Reconstruction();return R&&R->Surface?R->Surface->TriangleCount():0;}
bool IStudioField::MeshTriangle(int32 Index,FVector (&Out)[3]) const
{
    const auto R=Reconstruction();
    if(!R||!R->Surface||!R->Surface->Triangles().IsValidIndex(Index))return false;
    const auto T=R->Surface->Triangles()[Index];const auto& P=R->Surface->Geometry().Positions;
    for(int32 I=0;I<3;++I){const auto V=P[T[I]];Out[I]=FVector(V.X,V.Z,V.Y);}
    return true;
}
bool IStudioField::SampleScalar(const FVector& P, const FString& Id, double& Out) const
{
    Out = std::numeric_limits<double>::quiet_NaN();
    // This default adapts the complete legacy tuple. Optional-field readers
    // override it with their exact supplied field IDs and loaded arrays.
    if (Id != TEXT("velocity_magnitude") && Id != TEXT("velocity_x") &&
        Id != TEXT("velocity_y") && Id != TEXT("pressure") && Id != TEXT("density")) return false;
    FStudioFieldValue V;
    if (!Sample(P, V)) return false;
    const double Value = Id == TEXT("velocity_magnitude") ? V.Velocity.Size() :
        Id == TEXT("velocity_x") ? V.Velocity.X : Id == TEXT("velocity_y") ? V.Velocity.Z :
        Id == TEXT("pressure") ? V.Pressure : V.Density;
    if (!FMath::IsFinite(Value)) return false;
    Out = Value;
    return true;
}

bool IStudioField::SampleVelocity(const FVector& P, FVector& Out) const
{
    Out = FVector(std::numeric_limits<double>::quiet_NaN());
    FStudioFieldValue V;
    if (!Sample(P, V) || V.Velocity.ContainsNaN()) return false;
    Out = V.Velocity;
    return true;
}
