#include "StudioSurfaceRenderData.h"

FStudioScalarSurfaceData StudioSurfaceRendering::Build(const FStudioPointFrame& Frame,
    const FStudioSurfaceReconstruction& Surface,const FString& FieldId,const FStudioColorMapping& Mapping,
    const FStudioLoadCancellation& Cancellation,const FBox& DisplayBounds)
{
    FStudioScalarSurfaceData Out;
    const auto Cancelled=[&]{return Cancellation&&Cancellation->load(std::memory_order_relaxed);};
    const auto Fail=[&](const TCHAR* Error)
    {FStudioScalarSurfaceData Failed;Failed.Error=Cancelled()?TEXT("Surface rendering cancelled."):Error;return Failed;};
    if(Cancelled())return Fail(TEXT("Surface rendering cancelled."));
    const auto* Values=Frame.FindValues(FieldId);
    if(!Frame.Geometry||!Frame.Descriptor||!Surface.Surface||!Values||
        Values->Num()!=Frame.Geometry->Positions.Num()||Frame.Descriptor->SpatialDimensions!=2||
        &Surface.Surface->Geometry()!=Frame.Geometry.Get())
        return Fail(TEXT("A matching source frame and reconstructed surface are required."));
    const int32 Count=Values->Num();
    if(Count<3||Count>1000000||Surface.Surface->TriangleCount()>2000000)
        return Fail(TEXT("Reconstructed surface exceeds its rendering limit."));
    const double Span=Mapping.Maximum-Mapping.Minimum;
    if(!FMath::IsFinite(Span)||Span<0)
        return Fail(TEXT("Surface color range must have a finite span."));
    Out.Vertices.Reserve(Count);Out.Scalars.Reserve(Count);
    for(int32 Row=0;Row<Count;++Row)
    {
        if((Row&255)==0&&Cancelled())return Fail(TEXT("Surface rendering cancelled."));
        const auto& P=Frame.Geometry->Positions[Row];
        const double Normalized=Span>0?((*Values)[Row]-Mapping.Minimum)/Span:.5;
        // Retain unclamped values for barycentric interpolation, including
        // vertices outside a custom display range. Clamping vertices first
        // would alter the colors inside triangles crossing that range.
        if(!FMath::IsFinite(Normalized)||FMath::Abs(Normalized)>1.e30)
            return Fail(TEXT("Color range exceeds surface shader precision. Widen the range or use original-point mode."));
        Out.Scalars.Add(float(Normalized));
        Out.Vertices.Add(FVector(P.X,0,P.Y)*100.);
    }
    Out.Indices.Reserve(Surface.Surface->TriangleCount()*3);
    int32 Face=0;
    for(const auto& T:Surface.Surface->Triangles())
    {
        if((Face++&255)==0&&Cancelled())return Fail(TEXT("Surface rendering cancelled."));
        if(!DisplayBounds.IsValid){Out.Indices.Append({T.X,T.Y,T.Z});continue;}
        // Clip geometry and transport the selected scalar linearly. Interpolating
        // source-row texture coordinates would address unrelated samples instead.
        struct FVertex{FVector P;double Value;int32 Row;};
        TArray<FVertex,TInlineAllocator<12>> Polygon;
        for(int32 Row:{T.X,T.Y,T.Z})Polygon.Add({Out.Vertices[Row]/100.,(*Values)[Row],Row});
        for(int32 Axis:{0,2})for(bool Upper:{false,true})
        {
            TArray<FVertex,TInlineAllocator<12>> Clipped;
            const double Edge=Upper?DisplayBounds.Max[Axis]:DisplayBounds.Min[Axis];
            for(int32 I=0;I<Polygon.Num();++I)
            {
                const auto& A=Polygon[I];const auto& B=Polygon[(I+1)%Polygon.Num()];
                const bool InA=Upper?A.P[Axis]<=Edge:A.P[Axis]>=Edge,InB=Upper?B.P[Axis]<=Edge:B.P[Axis]>=Edge;
                if(InA)Clipped.Add(A);
                if(InA!=InB)
                {
                    const double U=(Edge-A.P[Axis])/(B.P[Axis]-A.P[Axis]);
                    Clipped.Add({FMath::Lerp(A.P,B.P,U),FMath::Lerp(A.Value,B.Value,U),INDEX_NONE});
                }
            }
            Polygon=MoveTemp(Clipped);
        }
        if(Polygon.Num()<3)continue;
        for(auto& V:Polygon)if(V.Row==INDEX_NONE)
        {
            if(Out.Vertices.Num()>=1000000)return Fail(TEXT("Focused surface exceeds its rendering limit. Turn off Focus wing region."));
            V.Row=Out.Vertices.Add(V.P*100.);Out.Scalars.Add(float(Span>0?(V.Value-Mapping.Minimum)/Span:.5));
        }
        for(int32 I=1;I+1<Polygon.Num();++I)Out.Indices.Append({Polygon[0].Row,Polygon[I].Row,Polygon[I+1].Row});
    }
    const int32 VertexCount=Out.Vertices.Num();
    const int32 Width=FMath::RoundUpToPowerOfTwo(FMath::CeilToInt(FMath::Sqrt(double(VertexCount))));
    const int32 Height=FMath::RoundUpToPowerOfTwo(FMath::DivideAndRoundUp(VertexCount,Width));
    Out.TextureSize=FIntPoint(Width,Height);Out.Scalars.SetNumZeroed(Width*Height);
    Out.TextureCoordinates.Reserve(VertexCount);
    for(int32 Row=0;Row<VertexCount;++Row)Out.TextureCoordinates.Add(FVector2D((Row%Width+.5)/Width,(Row/Width+.5)/Height));
    return Out;
}
