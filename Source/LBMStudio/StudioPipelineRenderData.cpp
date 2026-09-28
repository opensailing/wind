#include "StudioPipelineRenderData.h"
#include <cmath>

namespace StudioPipelineRenderingPrivate
{
bool Cancelled(const FStudioLoadCancellation& C){return C&&C->load(std::memory_order_relaxed);}
void Triangle(FStudioPipelineRenderMesh& M,const FVector& A,const FVector& B,const FVector& C,const FLinearColor& Color)
{
    const int32 I=M.Vertices.Num();M.Vertices.Append({A,B,C});M.Colors.Append({Color,Color,Color});M.Indices.Append({I,I+1,I+2});
}
void Tube(FStudioPipelineRenderMesh& M,const FVector& A,const FVector& B,double Radius,const FLinearColor& Color)
{
    const FVector Delta=B-A;const double Length=std::hypot(Delta.X,Delta.Y,Delta.Z);if(Length==0)return;
    const FVector D=Delta/Length;
    const FVector N=FVector::CrossProduct(D,FMath::Abs(D.Z)<.9?FVector::UpVector:FVector::RightVector).GetSafeNormal()*Radius;
    const FVector T=FVector::CrossProduct(D,N);const FVector Offsets[]={N,T,-N,-T};
    for(int32 I=0;I<4;++I)
    {const FVector P=Offsets[I],Q=Offsets[(I+1)%4];Triangle(M,A+P,B+P,B+Q,Color);Triangle(M,A+P,B+Q,A+Q,Color);}
}
}

FStudioPipelineRenderData StudioPipelineRendering::Build(const FStudioPipelineOutput& O,const FStudioColorMapping& Mapping,
    const FStudioLoadCancellation& C)
{
    using namespace StudioPipelineRenderingPrivate;
    FStudioPipelineRenderData Out;
    auto Fail=[&](const TCHAR* Why)
    {FStudioPipelineRenderData Failed;Failed.bCancelled=Cancelled(C);Failed.Error=Failed.bCancelled?TEXT("Pipeline rendering cancelled."):Why;return Failed;};
    if(Cancelled(C))return Fail(TEXT("Pipeline rendering cancelled."));
    if(O.Vertices.Num()>StudioPipelineEvaluation::MaxOutputVertices||O.Triangles.Num()>StudioPipelineEvaluation::MaxOutputTriangles||
        O.Lines.Num()>StudioPipelineEvaluation::MaxOutputLines||O.AllocatedBytes()>StudioPipelineEvaluation::MaxOutputBytes)
        return Fail(TEXT("Pipeline output exceeds its geometry budget. Choose a smaller clip region."));
    FStudioScalarStyle Style;Style.Dataset=TEXT("pipeline");Style.Field=TEXT("selected");
    Style.Palette=Mapping.Palette;Style.bManualRange=true;Style.Minimum=Mapping.Minimum;Style.Maximum=Mapping.Maximum;
    Style.LowColor=Mapping.LowColor;Style.MiddleColor=Mapping.MiddleColor;Style.HighColor=Mapping.HighColor;
    const double Span=Mapping.Maximum-Mapping.Minimum;
    // Equal supplied extrema use the midpoint; custom display styles otherwise
    // share the app's finite range, palette and color validation.
    if(!FMath::IsFinite(Span)||Span<0)return Fail(TEXT("Pipeline colors need a finite scalar range."));
    if(Span==0){Style.Minimum=0;Style.Maximum=1;}
    if(!StudioColor::IsValid(Style))return Fail(TEXT("Pipeline color mapping is invalid."));
    const bool Points=O.Kind==EStudioPipelineOutputKind::OriginalPoints,Lines=O.Kind==EStudioPipelineOutputKind::ContourLines;
    const bool Surface=O.Kind==EStudioPipelineOutputKind::Surface,Contour=O.Kind==EStudioPipelineOutputKind::ContourSurface;
    if(O.Kind==EStudioPipelineOutputKind::ProbeTable)
    {
        if(!O.Probe.IsSet()||!O.Vertices.IsEmpty()||!O.Triangles.IsEmpty()||!O.Lines.IsEmpty())return Fail(TEXT("Pipeline probe output has incompatible geometry."));
        return Out;
    }
    if((!Points&&!Lines&&!Surface&&!Contour)||(Points&&(!O.Triangles.IsEmpty()||!O.Lines.IsEmpty()))||
        (Lines&&!O.Triangles.IsEmpty())||((Surface||Contour)&&!O.Lines.IsEmpty()))return Fail(TEXT("Pipeline geometry does not match its output type."));
    if(Points&&O.Vertices.Num()>MaxPoints)return Fail(TEXT("This view exceeds 50,000 point glyphs. Choose a smaller clip region; the complete numerical result is retained."));
    if(Lines&&O.Lines.Num()>MaxLines)return Fail(TEXT("This view exceeds 37,500 contour segments. Choose a smaller clip region; the complete numerical result is retained."));
    for(int32 I=0;I<O.Vertices.Num();++I)
    {
        if((I&255)==0&&Cancelled(C))return Fail(TEXT("Pipeline rendering cancelled."));
        const auto& V=O.Vertices[I];
        if(V.PositionMeters.ContainsNaN()||!FMath::IsFinite(V.Scalar)||(V.PositionMeters*100.).GetAbsMax()>1.e12)
            return Fail(TEXT("Pipeline geometry has nonfinite values or coordinates outside rendering precision."));
    }
    // Bounds include only geometry that is actually displayed. Unused evaluator
    // vertices never enlarge the camera fit or the point/line glyph width.
    if(Points)for(const auto& V:O.Vertices)Out.Bounds+=V.PositionMeters;
    for(int32 I=0;I<O.Lines.Num();++I)
    {
        if((I&255)==0&&Cancelled(C))return Fail(TEXT("Pipeline rendering cancelled."));
        const auto& L=O.Lines[I];
        if(!O.Vertices.IsValidIndex(L.X)||!O.Vertices.IsValidIndex(L.Y)||O.Vertices[L.X].Scalar!=O.Vertices[L.Y].Scalar)
            return Fail(TEXT("Pipeline contour segment indices or scalar values are invalid."));
        Out.Bounds+=O.Vertices[L.X].PositionMeters;Out.Bounds+=O.Vertices[L.Y].PositionMeters;
    }
    for(int32 I=0;I<O.Triangles.Num();++I)
    {
        if((I&255)==0&&Cancelled(C))return Fail(TEXT("Pipeline rendering cancelled."));
        const auto& T=O.Triangles[I];
        if(!O.Vertices.IsValidIndex(T.X)||!O.Vertices.IsValidIndex(T.Y)||!O.Vertices.IsValidIndex(T.Z))return Fail(TEXT("Pipeline surface indices are invalid."));
        if(Contour&&(O.Vertices[T.X].Scalar!=O.Vertices[T.Y].Scalar||O.Vertices[T.X].Scalar!=O.Vertices[T.Z].Scalar))
            return Fail(TEXT("Pipeline contour triangles require a constant isovalue."));
        for(const int32 J:{T.X,T.Y,T.Z})Out.Bounds+=O.Vertices[J].PositionMeters;
    }
    if(!Out.Bounds.IsValid)return Out;
    const double Radius=FMath::Max(Out.Bounds.GetSize().GetMax(),.001)*.0011*100.;
    if(Points)
    {
        const FVector Axis[]={FVector(Radius,0,0),FVector(0,Radius,0),FVector(0,0,Radius)};
        Out.Glyphs.Vertices.Reserve(O.Vertices.Num()*18);Out.Glyphs.Colors.Reserve(O.Vertices.Num()*18);Out.Glyphs.Indices.Reserve(O.Vertices.Num()*18);
        for(int32 I=0;I<O.Vertices.Num();++I)
        {
            if((I&255)==0&&Cancelled(C))return Fail(TEXT("Pipeline rendering cancelled."));
            const auto& V=O.Vertices[I];const FVector P=V.PositionMeters*100.;const auto Color=StudioColor::Map(V.Scalar,Mapping);
            for(int32 A=0;A<3;++A)
            {const FVector U=Axis[A],W=Axis[(A+1)%3];Triangle(Out.Glyphs,P-U-W,P+U-W,P+U+W,Color);Triangle(Out.Glyphs,P-U-W,P+U+W,P-U+W,Color);}
        }
    }
    else if(Lines)
    {
        Out.Glyphs.Vertices.Reserve(O.Lines.Num()*24);Out.Glyphs.Colors.Reserve(O.Lines.Num()*24);Out.Glyphs.Indices.Reserve(O.Lines.Num()*24);
        for(int32 I=0;I<O.Lines.Num();++I)
        {
            if((I&255)==0&&Cancelled(C))return Fail(TEXT("Pipeline rendering cancelled."));
            const auto& L=O.Lines[I];const auto& A=O.Vertices[L.X];const auto& B=O.Vertices[L.Y];
            Tube(Out.Glyphs,A.PositionMeters*100.,B.PositionMeters*100.,Radius*1.2,StudioColor::Map(A.Scalar,Mapping));
        }
    }
    else
    {
        // Compact referenced vertices so the scalar texture never exceeds
        // 1024² texels. Their centers stay exact in half-precision mesh UVs.
        TArray<int32> Remap;Remap.Init(INDEX_NONE,O.Vertices.Num());TArray<int32> Rows;
        TArray<int32>& Indices=Surface?Out.Surface.Indices:Out.Contour.Indices;Indices.Reserve(O.Triangles.Num()*3);
        for(int32 I=0;I<O.Triangles.Num();++I)
        {
            if((I&255)==0&&Cancelled(C))return Fail(TEXT("Pipeline rendering cancelled."));
            const auto& T=O.Triangles[I];for(const int32 J:{T.X,T.Y,T.Z}){if(Remap[J]==INDEX_NONE)Remap[J]=Rows.Add(J);Indices.Add(Remap[J]);}
        }
        const int32 Count=Rows.Num();if(Count>MaxVertices)return Fail(TEXT("Pipeline surface exceeds its rendering budget. Choose a smaller clip region."));
        if(Surface)
        {
            const int32 Width=FMath::RoundUpToPowerOfTwo(FMath::CeilToInt(FMath::Sqrt(double(Count))));
            const int32 Height=FMath::RoundUpToPowerOfTwo(FMath::DivideAndRoundUp(Count,Width));
            Out.Surface.TextureSize=FIntPoint(Width,Height);Out.Surface.Scalars.SetNumZeroed(Width*Height);
            Out.Surface.Vertices.Reserve(Count);Out.Surface.TextureCoordinates.Reserve(Count);
            for(int32 I=0;I<Count;++I)
            {
                if((I&255)==0&&Cancelled(C))return Fail(TEXT("Pipeline rendering cancelled."));
                const auto& V=O.Vertices[Rows[I]];const double N=Span>0?(V.Scalar-Mapping.Minimum)/Span:.5;
                if(!FMath::IsFinite(N)||FMath::Abs(N)>1.e30)return Fail(TEXT("Color range exceeds surface shader precision. Widen the display range."));
                Out.Surface.Vertices.Add(V.PositionMeters*100.);Out.Surface.Scalars[I]=float(N);
                Out.Surface.TextureCoordinates.Add(FVector2D((I%Width+.5)/Width,(I/Width+.5)/Height));
            }
        }
        else
        {
            auto& M=Out.Contour;M.Vertices.Reserve(Count);M.Colors.Reserve(Count);M.Normals.Init(FVector::ZeroVector,Count);
            for(int32 I=0;I<Count;++I)
            {
                if((I&255)==0&&Cancelled(C))return Fail(TEXT("Pipeline rendering cancelled."));
                const auto& V=O.Vertices[Rows[I]];M.Vertices.Add(V.PositionMeters*100.);M.Colors.Add(StudioColor::Map(V.Scalar,Mapping));
            }
            for(int32 I=0;I<Indices.Num();I+=3)
            {
                if((I&255)==0&&Cancelled(C))return Fail(TEXT("Pipeline rendering cancelled."));
                const int32 A=Indices[I],B=Indices[I+1],D=Indices[I+2];
                const auto N=FVector::CrossProduct(M.Vertices[B]-M.Vertices[A],M.Vertices[D]-M.Vertices[A]);
                M.Normals[A]+=N;M.Normals[B]+=N;M.Normals[D]+=N;
            }
            for(auto& N:M.Normals)
            {
                // CFD cells can be much smaller than Unreal's default normal
                // tolerance. Normalize by actual length, without deleting them.
                const double Length=std::hypot(N.X,N.Y,N.Z);N=Length>0?N/Length:FVector::UpVector;
            }
        }
    }
    if(Cancelled(C))return Fail(TEXT("Pipeline rendering cancelled."));return Out;
}
