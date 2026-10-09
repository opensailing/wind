#include "StudioMeshDisplay.h"
#include "StudioModel.h"

FStudioMeshDisplayData StudioMeshDisplay::Build(const IStudioField& Field,const FStudioLoadCancellation& Cancellation)
{
    FStudioMeshDisplayData Out;
    const auto Identity=Field.Identity();const int32 Count=Field.MeshTriangleCount();
    if(!Identity.IsSet()||!Field.IsValid()||Count<=0)
    {Out.Notice=TEXT("No triangle mesh supplied. Original points are not connected automatically.");return Out;}
    Out.bDerived=Identity->Interpolation==EStudioFieldInterpolation::ReconstructedTriangles;
    if(Count>MaximumTriangles)
    {Out.Notice=TEXT("Triangle mesh exceeds the display budget (131,072 faces). No partial mesh is shown.");return Out;}
    Out.PositionsMeters.Reserve(Count*3);Out.Indices.Reserve(Count*3);
    for(int32 Face=0;Face<Count;++Face)
    {
        if(Cancellation&&Cancellation->load())return {};
        FVector P[3];
        if(!Field.MeshTriangle(Face,P)||P[0].ContainsNaN()||P[1].ContainsNaN()||P[2].ContainsNaN())
        {Out={};Out.Notice=TEXT("Triangle mesh is unavailable for this recorded frame.");return Out;}
        const int32 Base=Out.PositionsMeters.Num();
        Out.PositionsMeters.Append({P[0],P[1],P[2]});Out.Indices.Append({Base,Base+1,Base+2});
    }
    Out.TriangleCount=Count;
    Out.Notice=Out.bDerived?TEXT("Derived triangle mesh · original point positions"):TEXT("Original CFD triangle mesh");
    return Out;
}
