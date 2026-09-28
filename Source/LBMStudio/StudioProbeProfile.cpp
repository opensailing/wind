#include "StudioProbeProfile.h"
#include "StudioFileDialog.h"
#include "Async/Async.h"

namespace
{
bool ValidSnapshot(const FStudioProbeResult& R)
{
    if(R.Status!=EStudioProbeStatus::Ready||!R.Identity.IsSet()||!R.ProjectId.IsValid()||!R.PresentationId||
        R.Samples.IsEmpty()||R.Samples.Num()>StudioInspectionObjects::MaxLineSamples||R.Field.IsEmpty())return false;
    if(R.Samples.Num()!=(R.Probe.Kind==EStudioProbeKind::Line?R.Probe.Samples:1))return false;
    const auto& I=R.Identity.GetValue();
    if(!(R.Probe.Source==FStudioInspectionSource{I.Dataset,I.MetadataSHA256,I.PayloadSHA256})||
        I.Ordinal<0||!FMath::IsFinite(I.Frame.Time))return false;
    FStudioInspectionObjects Objects;Objects.Probes.Add(R.Probe);FString Error;
    if(!StudioInspectionObjects::IsValid(Objects,Error))return false;
    double Previous=-1;
    for(const auto& S:R.Samples)
    {
        if(!FMath::IsFinite(S.DistanceAlongLineMeters)||S.DistanceAlongLineMeters<Previous||S.DistanceAlongLineMeters<0||
            (S.Value.IsSet()&&(!FMath::IsFinite(S.Value.GetValue())||S.Status!=EStudioProbeSampleStatus::Value))||
            (!S.Value.IsSet()&&S.Status==EStudioProbeSampleStatus::Value)||
            (S.ScenePosition.IsSet()&&S.ScenePosition->ContainsNaN())||(S.SourcePosition.IsSet()&&S.SourcePosition->ContainsNaN()))return false;
        Previous=S.DistanceAlongLineMeters;
    }
    return true;
}
FString Number(double V){return FString::Printf(TEXT("%.17g"),V);}
FString Quoted(const FString& Text){return TEXT("\"")+Text.Replace(TEXT("\""),TEXT("\"\""))+TEXT("\"");}
void Position(TArray<FString>& Cells,const TOptional<FVector>& P)
{for(int32 Axis=0;Axis<3;++Axis)Cells.Add(P.IsSet()?Number(P.GetValue()[Axis]):FString());}
}

bool FStudioProbeProfile::Matches(const FStudioProbeResult& R) const
{
    return R.Status==EStudioProbeStatus::Ready&&Snapshot.ProjectId==R.ProjectId&&Snapshot.PresentationId==R.PresentationId&&Snapshot.Probe==R.Probe&&
        Snapshot.Field==R.Field&&Snapshot.SampledField.Pin()==R.SampledField.Pin()&&Snapshot.Identity.IsSet()&&R.Identity.IsSet()&&
        Snapshot.Identity->Ordinal==R.Identity->Ordinal;
}
FString FStudioProbeProfile::RangeLabel(bool bMaximum) const
{
    int32 Digits=6;
    while(Digits<17&&Minimum!=Maximum&&FString::Printf(TEXT("%.*g"),Digits,Minimum)==FString::Printf(TEXT("%.*g"),Digits,Maximum))++Digits;
    return FString::Printf(TEXT("%.*g"),Digits,bMaximum?Maximum:Minimum);
}
FVector2D FStudioProbeProfile::NormalizedPosition(int32 Index) const
{
    if(!Snapshot.Samples.IsValidIndex(Index))return FVector2D::ZeroVector;
    const auto& S=Snapshot.Samples[Index];const double X=LengthMeters>0?S.DistanceAlongLineMeters/LengthMeters:.5;
    // Scaling before subtraction avoids overflow for very large finite fields.
    const double Scale=FMath::Max(1.,FMath::Max(FMath::Abs(Minimum),FMath::Abs(Maximum)));
    const double Span=Maximum/Scale-Minimum/Scale;
    return FVector2D(FMath::Clamp(X,0.,1.),S.Value.IsSet()&&Span>0?
        FMath::Clamp((S.Value.GetValue()/Scale-Minimum/Scale)/Span,0.,1.):.5);
}

TSharedPtr<const FStudioProbeProfile> StudioProbeProfile::Build(const FStudioProbeResult& R)
{
    if(!ValidSnapshot(R)||R.Probe.Kind!=EStudioProbeKind::Line)return {};
    auto Out=MakeShared<FStudioProbeProfile>();Out->Snapshot=R;Out->LengthMeters=R.Samples.Last().DistanceAlongLineMeters;
    TArray<int32> Segment;
    for(int32 I=0;I<R.Samples.Num();++I)
    {
        const auto& Sample=R.Samples[I];
        if(!Sample.Value.IsSet()){if(!Segment.IsEmpty()){Out->Segments.Add(MoveTemp(Segment));Segment.Reset();}continue;}
        const double Value=Sample.Value.GetValue();
        if(!Out->ValidSamples){Out->Minimum=Value;Out->Maximum=Value;}
        else {Out->Minimum=FMath::Min(Out->Minimum,Value);Out->Maximum=FMath::Max(Out->Maximum,Value);}
        ++Out->ValidSamples;Segment.Add(I);
    }
    if(!Segment.IsEmpty())Out->Segments.Add(MoveTemp(Segment));
    return Out;
}

FString StudioProbeProfile::SampleStatus(EStudioProbeSampleStatus Status)
{
    switch(Status)
    {
    case EStudioProbeSampleStatus::Value:return TEXT("value");
    case EStudioProbeSampleStatus::OutsideCoverage:return TEXT("outside_coverage");
    case EStudioProbeSampleStatus::OffPlane:return TEXT("outside_source_plane");
    case EStudioProbeSampleStatus::MissingPoint:return TEXT("original_point_not_found");
    case EStudioProbeSampleStatus::FieldUnavailable:return TEXT("field_unavailable");
    case EStudioProbeSampleStatus::NoInterpolation:return TEXT("no_interpolation");
    }
    return TEXT("invalid");
}

bool StudioProbeProfile::CSV(const FStudioProbeResult& R,FString& Out,FString& Error)
{
    if(!ValidSnapshot(R)){Error=TEXT("A complete identified probe result is required for export.");return false;}
    const auto& I=R.Identity.GetValue();
    FString Text=TEXT("schema,project_id,presentation_id,probe_id,probe_name,probe_kind,sampling_method,source_dataset,source_metadata_sha256,source_payload_sha256,reconstruction_sha256,source_dimensions,frame_ordinal,frame_index,time_s,scalar_id,scalar_label,scalar_unit,scalar_origin,interpolation_method,sample_index,distance_m,scene_x_m,scene_y_m,scene_z_m,source_x_m,source_y_m,source_z_m,original_point_id,status,value\r\n");
    const TArray<FString> Metadata={TEXT("1"),Quoted(R.ProjectId.ToString()),FString::Printf(TEXT("%llu"),R.PresentationId),Quoted(R.Probe.Id.ToString()),Quoted(R.Probe.Name),
        Quoted(R.Probe.Kind==EStudioProbeKind::Line?TEXT("line"):TEXT("point")),Quoted(R.Probe.Method==EStudioProbeMethod::OriginalPoint?TEXT("original_point"):TEXT("interpolated")),
        Quoted(I.Dataset),Quoted(I.MetadataSHA256),Quoted(I.PayloadSHA256),Quoted(I.ReconstructionSHA256),FString::FromInt(I.SpatialDimensions),FString::FromInt(I.Ordinal),FString::FromInt(I.Frame.Index),Number(I.Frame.Time),
        Quoted(R.Field),Quoted(R.Label),Quoted(R.Unit),Quoted(R.Origin),Quoted(R.Method)};
    const FString Prefix=FString::Join(Metadata,TEXT(","))+TEXT(",");
    for(int32 Index=0;Index<R.Samples.Num();++Index)
    {
        const auto& S=R.Samples[Index];TArray<FString> Cells={FString::FromInt(Index),Number(S.DistanceAlongLineMeters)};
        Position(Cells,S.ScenePosition);Position(Cells,S.SourcePosition);
        Cells.Add(S.PointId.IsSet()?FString::Printf(TEXT("%lld"),S.PointId.GetValue()):FString());
        Cells.Add(Quoted(SampleStatus(S.Status)));Cells.Add(S.Value.IsSet()?Number(S.Value.GetValue()):FString());
        Text+=Prefix+FString::Join(Cells,TEXT(","))+TEXT("\r\n");
    }
    Out=MoveTemp(Text);Error.Empty();return true;
}

FStudioProbeExportTask::~FStudioProbeExportTask(){if(Pending.IsValid())Pending.Wait();}
bool FStudioProbeExportTask::Start(FStudioProbeResult Snapshot,const FString& Path)
{
    check(IsInGameThread());if(Pending.IsValid()||Path.IsEmpty()||!ValidSnapshot(Snapshot))return false;
    Pending=Async(EAsyncExecution::ThreadPool,[Snapshot=MoveTemp(Snapshot),Path]
    {
        FStudioProbeExportResult Result;Result.Path=Path;Result.ProbeName=Snapshot.Probe.Name;Result.Frame=Snapshot.Identity->Frame;
        FString CSV;if(StudioProbeProfile::CSV(Snapshot,CSV,Result.Error))
        {const FStudioFileAccess Access(Path);Result.bSuccess=StudioFileDialog::WriteAtomic(Path,CSV,Result.Error);}
        return Result;
    });return true;
}
TOptional<FStudioProbeExportResult> FStudioProbeExportTask::Poll()
{check(IsInGameThread());return Pending.IsValid()&&Pending.IsReady()?TOptional<FStudioProbeExportResult>(Pending.Consume()):TOptional<FStudioProbeExportResult>();}
