#include "StudioSliceRendering.h"
#include "StudioModel.h"
#include "StudioVolume.h"
#include "StudioSurfaceReconstruction.h"

FStudioSliceRenderData StudioSliceRendering::Build(const IStudioField& Field,const FBox& Bounds,
    const TArray<FStudioSliceObject>& Slices,const FString& Scalar,const FStudioLoadCancellation& Cancellation,TOptional<bool> AirMaskOverride)
{
    FStudioSliceRenderData Out;
    auto Cancelled=[&]{return Cancellation&&Cancellation->load(std::memory_order_relaxed);};
    const auto Identity=Field.Identity();
    if(!Identity.IsSet()||!Field.IsValid()||!Field.Scalar(Scalar).IsSet()||Cancelled())return Out;
    const FStudioInspectionSource Source{Identity->Dataset,Identity->MetadataSHA256,Identity->PayloadSHA256};
    int32 Count=0;for(const auto& S:Slices)if(S.bVisible&&S.Opacity>0&&S.Source==Source)++Count;
    if(!Count)return Out;
    const int32 Steps=FMath::Clamp(FMath::FloorToInt(FMath::Sqrt(double(MaximumVertices)/Count))-1,4,159);
    for(const auto& Slice:Slices)
    {
        if(Cancelled())return {};
        if(!Slice.bVisible||Slice.Opacity<=0||!(Slice.Source==Source))continue;
        if(Identity->Interpolation==EStudioFieldInterpolation::None)
        {Out.Notices.Add(Slice.Id,TEXT("A verified interpolation mesh is required. Original points remain available."));continue;}
        if(Identity->SpatialDimensions==2&&(FMath::Abs(Slice.Normal.Y)!=1.||Slice.Origin.Y!=Identity->SourceOffset.Y))
        {Out.Notices.Add(Slice.Id,TEXT("This recording is 2D. A field slice must lie on its original X/Z plane; this plane has an outline only."));continue;}
        const auto Polygon=StudioInspectionObjects::SlicePolygon(Slice,Bounds);
        if(Polygon.Num()<3){Out.Notices.Add(Slice.Id,TEXT("The plane does not cross the displayed domain."));continue;}
        const FVector U=FVector::CrossProduct(Slice.Normal,FMath::Abs(Slice.Normal.Z)<.9?FVector::UpVector:FVector::RightVector).GetSafeNormal();
        const FVector V=FVector::CrossProduct(Slice.Normal,U).GetSafeNormal();
        FBox2D Range(ForceInit);for(const auto& P:Polygon)Range+=FVector2D(FVector::DotProduct(P-Slice.Origin,U),FVector::DotProduct(P-Slice.Origin,V));
        const int32 Base=Out.PositionsMeters.Num(),Row=Steps+1;
        TArray<uint8> OriginalMask;
        const auto OriginalVolume=Field.VolumeReconstruction();
        if(OriginalVolume&&OriginalVolume->OriginalGrid)
        {
            const auto Frame=Field.OriginalPoints();FString Error;
            if(Frame)OriginalMask=StudioVolumes::SourceMask(*Frame,*OriginalVolume,Scalar,
                Frame->Descriptor&&!Frame->Descriptor->FindField(Scalar),Error,Cancellation,AirMaskOverride);
            if(!Frame||!Error.IsEmpty()||OriginalMask.IsEmpty())
            {Out.Notices.Add(Slice.Id,Error.IsEmpty()?TEXT("Original source masks are unavailable for this slice."):Error);continue;}
        }
        TBitArray<> Valid(false,Row*Row);
        auto Sample=[&](FVector P,double& Value)
        {
            if(!Bounds.IsInsideOrOn(P))return false;
            if(Identity->SpatialDimensions==2)P.Y=Identity->SourceOffset.Y;
            if(OriginalVolume&&OriginalVolume->OriginalGrid)
            {
                const auto Frame=Field.OriginalPoints();
                if(Frame&&Frame->Descriptor->FindField(Scalar))
                {P-=Identity->SourceOffset;return StudioVolumes::SampleSource(*Frame,*OriginalVolume,OriginalMask,Scalar,FVector(P.X,P.Z,P.Y),Value);}
            }
            return !Field.IsSolid(P)&&Field.SampleScalar(P,Scalar,Value)&&FMath::IsFinite(Value);
        };
        for(int32 J=0;J<Row;++J)for(int32 I=0;I<Row;++I)
        {
            if(Cancelled())return {};
            const FVector2D UV=Range.Min+Range.GetSize()*FVector2D(double(I)/Steps,double(J)/Steps);
            FVector P=Slice.Origin+U*UV.X+V*UV.Y;if(Identity->SpatialDimensions==2)P.Y=Identity->SourceOffset.Y;
            double Value=0;Valid[J*Row+I]=Sample(P,Value);
            // Missing samples are never indexed. Keep their GPU placeholders
            // finite so an unused NaN cannot reject the entire valid slice.
            Out.PositionsMeters.Add(P);Out.Scalars.Add(Valid[J*Row+I]?Value:0.);Out.Opacities.Add(float(Slice.Opacity));
        }
        const int32 Before=Out.Indices.Num();
        for(int32 J=0;J<Steps;++J)for(int32 I=0;I<Steps;++I)
        {
            if(Cancelled())return {};
            const int32 N=J*Row+I;
            if(!Valid[N]||!Valid[N+1]||!Valid[N+Row]||!Valid[N+Row+1])continue;
            const FVector A=Out.PositionsMeters[Base+N],B=Out.PositionsMeters[Base+N+1],C=Out.PositionsMeters[Base+N+Row+1],D=Out.PositionsMeters[Base+N+Row];
            FBox Cell(ForceInit);Cell+=A;Cell+=B;Cell+=C;Cell+=D;
            if(const auto Volume=Field.VolumeReconstruction())
            {
                const auto ToSource=[&](FVector P){P-=Identity->SourceOffset;return FVector(P.X,P.Z,P.Y);};
                const FBox SourceCell(ToSource(Cell.Min),ToSource(Cell.Max));
                if(Volume->OriginalGrid?!StudioVolumes::SupportsSourceRegion(*Volume,OriginalMask,SourceCell,Cancellation):
                    !Volume->SupportsRegion(SourceCell,Cancellation))continue;
            }
            // Conservative solid-boundary rejection. The bounding box may
            // omit an extra edge cell; it must never paint across the airfoil.
            bool BoundaryCrosses=false;
            if(Identity->SpatialDimensions==2)
            {
                const auto Reconstruction=Field.Reconstruction();
                const auto& Boundary=Reconstruction?Reconstruction->Boundary:Field.Boundary();
                for(int32 K=0;K<Boundary.Num();++K)
                {
                    const FVector2D P=Boundary[K],Q=Boundary[(K+1)%Boundary.Num()];
                    double Enter=0,Exit=1;const FVector2D Delta=Q-P;
                    for(int32 Axis=0;Axis<2;++Axis)
                    {
                        const double Low=Axis?Cell.Min.Z:Cell.Min.X,High=Axis?Cell.Max.Z:Cell.Max.X;
                        if(FMath::Abs(Delta[Axis])<1.e-15){if(P[Axis]<Low||P[Axis]>High){Enter=1;Exit=0;break;}}
                        else{double T0=(Low-P[Axis])/Delta[Axis],T1=(High-P[Axis])/Delta[Axis];if(T0>T1)Swap(T0,T1);Enter=FMath::Max(Enter,T0);Exit=FMath::Min(Exit,T1);}
                    }
                    if(Enter<=Exit){BoundaryCrosses=true;break;}
                }
            }
            if(BoundaryCrosses)continue;
            double Value;
            if(!Sample((A+B)*.5,Value)||!Sample((B+C)*.5,Value)||!Sample((C+D)*.5,Value)||!Sample((D+A)*.5,Value)||!Sample((A+C)*.5,Value))continue;
            Out.Indices.Append({Base+N,Base+N+1,Base+N+Row+1,Base+N,Base+N+Row+1,Base+N+Row});
        }
        if(Out.Indices.Num()>Before)Out.RenderedSlices.Add(Slice.Id);
        Out.Notices.Add(Slice.Id,Out.Indices.Num()==Before?TEXT("No supported field cells on this plane."):
            FString::Printf(TEXT("Sampled %d × %d grid; scalar interpolation precedes color mapping. Solid and unsupported cells are omitted."),Row,Row));
    }
    return Cancelled()?FStudioSliceRenderData():MoveTemp(Out);
}
