#include "StudioSurfaceRenderData.h"

FStudioScalarSurfaceData StudioSurfaceRendering::Build(const FStudioPointFrame& Frame,
    const FStudioSurfaceReconstruction& Surface,const FString& FieldId,const FStudioColorMapping& Mapping,
    const FStudioLoadCancellation& Cancellation)
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
    const int32 Width=FMath::RoundUpToPowerOfTwo(FMath::CeilToInt(FMath::Sqrt(double(Count))));
    const int32 Height=FMath::RoundUpToPowerOfTwo(FMath::DivideAndRoundUp(Count,Width));
    Out.TextureSize=FIntPoint(Width,Height);
    Out.Vertices.Reserve(Count);Out.TextureCoordinates.Reserve(Count);Out.Scalars.SetNumZeroed(Width*Height);
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
        Out.Scalars[Row]=float(Normalized);
        Out.Vertices.Add(FVector(P.X,0,P.Y)*100.);
        Out.TextureCoordinates.Add(FVector2D((Row%Width+.5)/Width,(Row/Width+.5)/Height));
    }
    Out.Indices.Reserve(Surface.Surface->TriangleCount()*3);
    int32 Face=0;
    for(const auto& T:Surface.Surface->Triangles())
    {
        if((Face++&255)==0&&Cancelled())return Fail(TEXT("Surface rendering cancelled."));
        Out.Indices.Append({T.X,T.Y,T.Z});
    }
    return Out;
}
