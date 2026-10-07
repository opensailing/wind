#include "StudioProbeSampling.h"
#include "StudioModel.h"
#include "StudioPointRecording.h"

namespace
{
FStudioInspectionSource SourceOf(const FStudioFieldIdentity& I)
{ return {I.Dataset,I.MetadataSHA256,I.PayloadSHA256}; }
FVector ToSource(const FVector& Scene,const FStudioFieldIdentity& I)
{
    const FVector P=Scene-I.SourceOffset;
    return FVector(P.X,P.Z,P.Y);
}
bool SameProbeFrame(const FStudioFieldIdentity& A,const FStudioFieldIdentity& B)
{
    return A.Dataset==B.Dataset&&A.MetadataSHA256==B.MetadataSHA256&&A.PayloadSHA256==B.PayloadSHA256&&
        A.ReconstructionSHA256==B.ReconstructionSHA256&&A.Ordinal==B.Ordinal&&A.Frame.Index==B.Frame.Index&&
        A.Frame.Time==B.Frame.Time&&A.SpatialDimensions==B.SpatialDimensions&&A.SourceOffset==B.SourceOffset&&A.Interpolation==B.Interpolation;
}
bool SameProbeScalar(const FStudioScalarDescriptor& A,const FStudioScalarDescriptor& B)
{
    return A.Id==B.Id&&A.Label==B.Label&&A.Unit==B.Unit&&A.Origin==B.Origin&&A.Minimum==B.Minimum&&A.Maximum==B.Maximum;
}
FString InterpolationMethod(EStudioFieldInterpolation Method)
{
    switch(Method)
    {
    case EStudioFieldInterpolation::SourceGrid:return TEXT("Trilinear interpolation on original affine source nodes");
    case EStudioFieldInterpolation::SourceSlice:return TEXT("Bilinear interpolation on the original source plane; no out-of-plane support");
    case EStudioFieldInterpolation::SourceTriangles:return TEXT("Barycentric interpolation on original source triangles");
    case EStudioFieldInterpolation::ReconstructedTriangles:return TEXT("Barycentric interpolation on reconstructed triangles");
    case EStudioFieldInterpolation::ReconstructedGrid:return TEXT("Trilinear interpolation of a reconstructed grid from original source values");
    default:return TEXT("No interpolation supplied");
    }
}
}

bool FStudioProbeResult::Matches(const FStudioProbeRequest& Current) const
{
    const FString ExpectedField=Current.Probe.Field.IsEmpty()?Current.DisplayedScalar:Current.Probe.Field;
    return Status!=EStudioProbeStatus::Cancelled&&ProjectId==Current.ProjectId&&PresentationId==Current.PresentationId&&
        Probe==Current.Probe&&Field==ExpectedField&&Current.Field&&SampledField.Pin()==Current.Field;
}

FStudioProbeResult StudioProbeSampling::Evaluate(const FStudioProbeRequest& R,const FStudioLoadCancellation& Cancellation)
{
    FStudioProbeResult Out;Out.ProjectId=R.ProjectId;Out.PresentationId=R.PresentationId;Out.Probe=R.Probe;Out.SampledField=R.Field;
    Out.Field=R.Probe.Field.IsEmpty()?R.DisplayedScalar:R.Probe.Field;
    auto Cancelled=[&](){return Cancellation&&Cancellation->load(std::memory_order_relaxed);};
    auto Cancel=[&]()
    {
        Out.Status=EStudioProbeStatus::Cancelled;Out.Message=TEXT("Probe sampling cancelled.");Out.Samples.Reset();
        return MoveTemp(Out);
    };
    if(Cancelled())return Cancel();
    FStudioInspectionObjects Objects;Objects.Probes.Add(R.Probe);FString Error;
    if(!R.ProjectId.IsValid()||R.PresentationId==0||!R.Field||!R.Field->IsValid()||
        !StudioInspectionObjects::IsValid(Objects,Error)||Out.Field.IsEmpty())
    {Out.Message=TEXT("A valid probe and presented field are required.");return Out;}
    Out.Identity=R.Field->Identity();
    if(!Out.Identity.IsSet()||!StudioInspectionObjects::IsValid(SourceOf(*Out.Identity))||Out.Identity->Ordinal<0||
        !FMath::IsFinite(Out.Identity->Frame.Time)||(Out.Identity->SpatialDimensions!=2&&Out.Identity->SpatialDimensions!=3))
    {Out.Status=EStudioProbeStatus::UnidentifiedFrame;Out.Message=TEXT("This field has no verified frame identity.");return Out;}
    if(!(R.Probe.Source==SourceOf(*Out.Identity)))
    {Out.Status=EStudioProbeStatus::SourceMismatch;Out.Message=TEXT("This probe belongs to a different recording. Open its original source.");return Out;}
    const auto Scalar=R.Field->Scalar(Out.Field);
    if(!Scalar.IsSet())
    {Out.Status=EStudioProbeStatus::FieldUnavailable;Out.Message=TEXT("The requested scalar is not supplied by this recording.");return Out;}
    Out.Label=Scalar->Label;Out.Unit=Scalar->Unit;Out.Origin=Scalar->Origin;
    Out.Method=R.Probe.Method==EStudioProbeMethod::OriginalPoint?TEXT("Exact original point ID"):InterpolationMethod(Out.Identity->Interpolation);
    auto SamplingField=R.Field;
    auto Points=SamplingField->OriginalPoints();
    if(Points&&!Points->FindValues(Out.Field))
    {
        SamplingField=R.Field->LoadScalarSnapshot(Out.Field,Cancellation,Error);
        if(Cancelled())return Cancel();
        if(!SamplingField||!SamplingField->IsValid())
        {Out.Status=EStudioProbeStatus::FieldLoadFailed;Out.Message=TEXT("Could not load probe samples. ")+Error;return Out;}
        const auto Identity=SamplingField->Identity();const auto LoadedScalar=SamplingField->Scalar(Out.Field);
        if(!Identity.IsSet()||!SameProbeFrame(*Out.Identity,*Identity)||!LoadedScalar.IsSet()||!SameProbeScalar(*Scalar,*LoadedScalar))
        {Out.Status=EStudioProbeStatus::FrameMismatch;Out.Message=TEXT("Probe samples do not match the displayed frame and source. No values published.");return Out;}
        Points=SamplingField->OriginalPoints();
        if(!Points||!Points->FindValues(Out.Field))
        {Out.Status=EStudioProbeStatus::FieldLoadFailed;Out.Message=TEXT("The loaded frame does not contain the requested probe scalar.");return Out;}
    }
    Out.Status=EStudioProbeStatus::Ready;
    if(R.Probe.Method==EStudioProbeMethod::OriginalPoint)
    {
        FStudioProbeSample Sample;Sample.PointId=R.Probe.PointId;Sample.Status=EStudioProbeSampleStatus::MissingPoint;
        if(Points)
        {
            const auto& Geometry=*Points->Geometry;const auto& Values=*Points->FindValues(Out.Field);
            for(int32 Row=0;Row<Geometry.PointIds.Num();++Row)
            {
                if((Row&255)==0&&Cancelled())return Cancel();
                if(Geometry.PointIds[Row]!=R.Probe.PointId.GetValue())continue;
                const FVector P=Geometry.Positions[Row];Sample.SourcePosition=P;
                Sample.ScenePosition=FVector(P.X,P.Z,P.Y)+Out.Identity->SourceOffset;
                if(Values.IsValidIndex(Row)&&FMath::IsFinite(Values[Row]))
                {Sample.Value=Values[Row];Sample.Status=EStudioProbeSampleStatus::Value;}
                else Sample.Status=EStudioProbeSampleStatus::FieldUnavailable;
                break;
            }
        }
        Out.Samples.Add(MoveTemp(Sample));
    }
    else
    {
        const auto Positions=StudioInspectionObjects::ProbeLocations(R.Probe);Out.Samples.Reserve(Positions.Num());
        for(const FVector& P:Positions)
        {
            if(Cancelled())return Cancel();
            FStudioProbeSample Sample;Sample.ScenePosition=P;Sample.SourcePosition=ToSource(P,*Out.Identity);
            Sample.DistanceAlongLineMeters=(P-R.Probe.A).Size();
            if(Out.Identity->SpatialDimensions==2&&P.Y!=Out.Identity->SourceOffset.Y)Sample.Status=EStudioProbeSampleStatus::OffPlane;
            else if(Out.Identity->Interpolation==EStudioFieldInterpolation::None)Sample.Status=EStudioProbeSampleStatus::NoInterpolation;
            else
            {
                double Value;
                if(SamplingField->SampleScalar(P,Out.Field,Value)&&FMath::IsFinite(Value))
                {Sample.Value=Value;Sample.Status=EStudioProbeSampleStatus::Value;}
            }
            Out.Samples.Add(MoveTemp(Sample));
        }
    }
    if(Cancelled())return Cancel();
    return Out;
}
