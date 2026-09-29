#include "StudioFlowPresentation.h"
#include "StudioModel.h"

bool StudioFlowPresentation::Overview(const IStudioField& Field,const FBox& Bounds,const FString& Scalar,
    double Aspect,const FStudioInspectionState& Current,FStudioInspectionState& Out)
{
    const auto Identity=Field.Identity();
    if(!Field.IsValid()||!Identity||Identity->SpatialDimensions!=2||Field.Boundary().IsEmpty()||
        !Bounds.IsValid||!FMath::IsFinite(Aspect)||Aspect<=0)return false;
    auto Next=Current;auto& D=Next.Display;
    D.bVolume=false;D.bVectors=false;D.bCutPlane=true;D.MeshStyle=0;
    D.SliceAxis=1;D.SlicePosition=Identity->SourceOffset.Y;
    D.bStreamlines=Identity->SpatialDimensions==2&&(!Field.Boundary().IsEmpty()||Field.Reconstruction());
    D.StreamlineSettings.bAutomaticSeeds=true;D.StreamlineSettings.AutomaticSeedCount=48;
    D.StreamlineSettings.WidthFraction=.00105;D.StreamlineSettings.StepFraction=.003125;
    D.StreamlineSettings.MaximumSteps=768;D.StreamlineSettings.WorkBudget=65536;
    D.bReconstructedSurface=Field.Reconstruction().IsValid();
    double Minimum=DBL_MAX,Maximum=-DBL_MAX;
    // Include boundary-layer source nodes that a regular display grid misses.
    // Bounded deterministic stride for very large sources; no source values change.
    const int32 Stride=FMath::Max(1,FMath::DivideAndRoundUp(Field.OriginalPointCount(),50000));
    for(int32 I=0;I<Field.OriginalPointCount();I+=Stride)
    {
        FVector Source;int64 Row;double Value;
        if(!Field.OriginalPoint(I,Row,Source)||!Field.OriginalScalar(I,Scalar,Value)||!FMath::IsFinite(Value))continue;
        const FVector P(Source.X+Identity->SourceOffset.X,D.SlicePosition,Source.Y+Identity->SourceOffset.Z);
        if(Bounds.IsInsideOrOn(P)){Minimum=FMath::Min(Minimum,Value);Maximum=FMath::Max(Maximum,Value);}
    }
    if(!(Maximum>Minimum))return false;
    if(Maximum>Minimum)
    {
        const double Pad=(Maximum-Minimum)*.15;
        FStudioScalarStyle Style;Style.Dataset=Identity->Dataset;Style.Field=Scalar;
        if(const auto* Existing=D.ScalarStyles.FindByPredicate([&](const auto& S){return S.Dataset==Style.Dataset&&S.Field==Style.Field;}))Style=*Existing;
        Style.bManualRange=true;Style.Minimum=Minimum>=0?FMath::Max(0.,Minimum-Pad):Minimum-Pad;Style.Maximum=Maximum+Pad;
        D.ScalarStyles.RemoveAll([&](const auto& S){return S.Dataset==Style.Dataset&&S.Field==Style.Field;});
        D.ScalarStyles.Add(Style);
    }
    auto& C=Next.Camera;C.bOrthographic=false;C.bFreeCamera=false;C.bDepthClipping=false;C.FieldOfView=38;
    C.Orientation=FVector(.24,-1.,-.18).Rotation().Quaternion();
    C=StudioView::FitBounds(C,Bounds,Aspect,.01);
    if(!StudioView::IsValid(Next))return false;Out=MoveTemp(Next);return true;
}

StudioFlowPresentation::FSlice StudioFlowPresentation::OriginalSlice(const IStudioField& Field,const FBox& B,double Y,
    const FStudioColorMapping& Mapping,const FString& Scalar,const FStudioLoadCancellation& Cancel)
{
    FSlice Out;const auto Id=Field.Identity();
    if(!Id||Id->SpatialDimensions!=2||!B.IsValid||Y<B.Min.Y||Y>B.Max.Y||Field.OriginalTriangleCount()>131072)return Out;
    struct FVertex{FVector P;double V;FVector2D Velocity;};
    const bool Speed=Scalar==TEXT("velocity_magnitude");
    for(int32 I=0;I<Field.OriginalTriangleCount();++I)
    {
        if(Cancel&&Cancel->load())return {};
        FIntVector T;if(!Field.OriginalTriangle(I,T))return {};
        TArray<FVertex,TInlineAllocator<12>> Polygon;
        for(int32 K=0;K<3;++K)
        {
            FVector P;double V;int64 Row;
            if(!Field.OriginalPoint(T[K],Row,P)||!Field.OriginalScalar(T[K],Scalar,V))return {};
            double U=0,W=0;
            if(Speed&&(!Field.OriginalScalar(T[K],TEXT("velocity_x"),U)||!Field.OriginalScalar(T[K],TEXT("velocity_y"),W)))return {};
            Polygon.Add({FVector(P.X+Id->SourceOffset.X,Y,P.Y+Id->SourceOffset.Z),V,FVector2D(U,W)});
        }
        for(int32 Axis:{0,2})for(bool Upper:{false,true})
        {
            if(Polygon.IsEmpty())break;
            TArray<FVertex,TInlineAllocator<12>> Clipped;
            const double Edge=Upper?B.Max[Axis]:B.Min[Axis];
            for(int32 J=0;J<Polygon.Num();++J)
            {
                const auto& A=Polygon[J];const auto& C=Polygon[(J+1)%Polygon.Num()];
                const bool InA=Upper?A.P[Axis]<=Edge:A.P[Axis]>=Edge,InC=Upper?C.P[Axis]<=Edge:C.P[Axis]>=Edge;
                if(InA)Clipped.Add(A);
                if(InA!=InC){const double U=(Edge-A.P[Axis])/(C.P[Axis]-A.P[Axis]);Clipped.Add({FMath::Lerp(A.P,C.P,U),FMath::Lerp(A.V,C.V,U),FMath::Lerp(A.Velocity,C.Velocity,U)});}
            }
            Polygon=MoveTemp(Clipped);
        }
        for(int32 J=1;J+1<Polygon.Num();++J)for(int32 K:{0,J,J+1})
        {
            const auto& V=Polygon[K];const double Span=Mapping.Maximum-Mapping.Minimum;
            const double S=Span>0?(V.V-Mapping.Minimum)/Span:.5;
            if(!FMath::IsFinite(S)||FMath::Abs(S)>1.e30)return {};
            Out.Indices.Add(Out.Positions.Num());Out.Positions.Add(V.P*100.);Out.ScalarOpacity.Add(FVector2D(S,.18));Out.Velocity.Add(V.Velocity);
        }
    }
    return Out;
}
