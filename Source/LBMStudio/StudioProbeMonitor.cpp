#include "StudioProbeMonitor.h"
#include "StudioProbeProfile.h"
#include "StudioModel.h"
#include <limits>

namespace
{
bool ValidateProbeHistory(const FStudioProbeHistoryResult& R,FString& Error)
{
    Error=TEXT("A complete identified probe history is required.");
    if(R.Status!=EStudioProbeHistoryStatus::Complete||!R.ProjectId.IsValid()||!R.Identity.IsSet()||
        R.Scalar.IsEmpty()||R.FirstOrdinal<0||R.LastOrdinal<R.FirstOrdinal||R.Frames.IsEmpty()||
        int64(R.LastOrdinal)-R.FirstOrdinal+1!=R.Frames.Num()||R.Frames.Num()>StudioProbeHistory::MaxFrames)return false;
    FStudioInspectionObjects Objects;Objects.Probes.Add(R.Probe);FString ObjectError;
    if(!StudioInspectionObjects::IsValid(Objects,ObjectError)||
        !(R.Probe.Source==FStudioInspectionSource{R.Identity->Dataset,R.Identity->MetadataSHA256,R.Identity->PayloadSHA256})||
        (R.Identity->SpatialDimensions!=2&&R.Identity->SpatialDimensions!=3))return false;
    const int32 Count=R.Probe.Kind==EStudioProbeKind::Point?1:R.Probe.Samples;
    if(int64(Count)*R.Frames.Num()>StudioProbeHistory::MaxSamples)return false;
    for(int32 I=0;I<R.Frames.Num();++I)
    {
        const auto& F=R.Frames[I];
        if(F.Ordinal!=R.FirstOrdinal+I||!FMath::IsFinite(F.Frame.Time)||F.Frame.Time<0||F.Frame.Index<0||F.Samples.Num()!=Count||
            (I&&(F.Frame.Time<=R.Frames[I-1].Frame.Time||F.Frame.Index<=R.Frames[I-1].Frame.Index)))return false;
        for(const auto& S:F.Samples)
        {
            if(uint8(S.Status)>uint8(EStudioProbeSampleStatus::NoInterpolation)||
                S.Value.IsSet()!=(S.Status==EStudioProbeSampleStatus::Value)||(S.Value.IsSet()&&!FMath::IsFinite(*S.Value))||
                !FMath::IsFinite(S.DistanceAlongLineMeters)||S.DistanceAlongLineMeters<0||
                (S.ScenePosition.IsSet()&&S.ScenePosition->ContainsNaN())||(S.SourcePosition.IsSet()&&S.SourcePosition->ContainsNaN()))return false;
            if(S.Value.IsSet()&&(!S.ScenePosition.IsSet()||!S.SourcePosition.IsSet()))return false;
            if(R.Probe.Method==EStudioProbeMethod::OriginalPoint&&S.PointId!=R.Probe.PointId)return false;
        }
    }
    const auto& First=R.Frames[0];
    if(R.Identity->Ordinal!=First.Ordinal||R.Identity->Frame.Index!=First.Frame.Index||R.Identity->Frame.Time!=First.Frame.Time)return false;
    Error.Empty();return true;
}
FString ProbeCSVQuote(FString Value){Value.ReplaceInline(TEXT("\""),TEXT("\"\""));return TEXT("\"")+Value+TEXT("\"");}
FString ProbeNumber(double V){return FString::Printf(TEXT("%.17g"),V);}
FString ProbePosition(const TOptional<FVector>& P)
{return P.IsSet()?ProbeNumber(P->X)+TEXT(",")+ProbeNumber(P->Y)+TEXT(",")+ProbeNumber(P->Z):TEXT(",,");}
}

TSharedPtr<const FStudioHistory,ESPMode::ThreadSafe> StudioProbeMonitor::MakeHistory(
    TSharedPtr<const FStudioProbeHistoryResult,ESPMode::ThreadSafe> R,FString& Error)
{
    if(!R||!ValidateProbeHistory(*R,Error))return {};
    auto H=MakeShared<FStudioHistory,ESPMode::ThreadSafe>();H->ProbeHistory=R;
    H->Id=TEXT("probe-")+R->Probe.Id.ToString(EGuidFormats::Digits);H->Title=R->Probe.Name+TEXT(" · ")+R->Label;
    H->SourceURL=R->SourceURL;H->MetadataSHA256=R->Identity->MetadataSHA256;H->PayloadSHA256=R->Identity->PayloadSHA256;
    H->FieldRecordingId=R->Identity->Dataset;H->TimeUnit=TEXT("s");H->TimeNote=R->TimeNote;
    H->Limitations.Add(R->Method+TEXT(". Each sample belongs to its original recorded frame; no temporal interpolation."));
    H->Limitations.Add(TEXT("Missing spatial values remain gaps. This history samples a recording and is independent of live solver jobs."));
    H->Times.Reserve(R->Frames.Num());for(const auto& F:R->Frames)H->Times.Add(F.Frame.Time);
    for(int32 I=0;I<R->Frames[0].Samples.Num();++I)
    {
        FStudioHistoryColumn C;C.Id=FString::Printf(TEXT("sample_%d"),I);
        C.Label=R->Probe.Kind==EStudioProbeKind::Point?R->Label:FString::Printf(TEXT("Position %d · %.6g m"),I+1,R->Frames[0].Samples[I].DistanceAlongLineMeters);
        C.Unit=R->Unit;C.Origin=R->Origin;C.Expression=R->Method;C.Values.Reserve(R->Frames.Num());
        for(const auto& F:R->Frames)C.Values.Add(F.Samples[I].Value.Get(std::numeric_limits<double>::quiet_NaN()));
        H->Columns.Add(MoveTemp(C));
    }
    return H;
}

bool StudioProbeMonitor::CSV(const FStudioHistory& H,const FStudioMonitorSettings& S,FString& Out,int32& Rows,FString& Error)
{
    if(!H.ProbeHistory||!ValidateProbeHistory(*H.ProbeHistory,Error)||!StudioMonitor::ValidateSource(S,H,Error))return false;
    const auto& R=*H.ProbeHistory;
    if(S.Series.IsEmpty()){Error=TEXT("Select at least one probe position to export.");return false;}
    if(H.Times.Num()!=R.Frames.Num()||H.Columns.Num()!=R.Frames[0].Samples.Num())
    {Error=TEXT("The chart does not match its frozen probe samples.");return false;}
    TArray<int32> Indices;
    for(const auto& Id:S.Series)
    {
        const int32 I=H.Columns.IndexOfByPredicate([&](const auto& C){return C.Id==Id;});
        if(I==INDEX_NONE||Id!=FString::Printf(TEXT("sample_%d"),I))
        {Error=TEXT("A selected series is not a sampled probe position.");return false;}
        Indices.Add(I);
    }
    FString Text=TEXT("# probe_history_schema,1\n# project_id,")+ProbeCSVQuote(R.ProjectId.ToString())+
        TEXT("\n# probe_id,")+ProbeCSVQuote(R.Probe.Id.ToString())+TEXT("\n# probe_name,")+ProbeCSVQuote(R.Probe.Name)+
        TEXT("\n# source_dataset,")+ProbeCSVQuote(R.Identity->Dataset)+TEXT("\n# source_title,")+ProbeCSVQuote(R.SourceTitle)+
        TEXT("\n# source_url,")+ProbeCSVQuote(R.SourceURL)+TEXT("\n# source_metadata_sha256,")+R.Identity->MetadataSHA256+
        TEXT("\n# source_payload_sha256,")+R.Identity->PayloadSHA256+TEXT("\n# reconstruction_sha256,")+R.Identity->ReconstructionSHA256+
        TEXT("\n# source_dimensions,")+FString::FromInt(R.Identity->SpatialDimensions)+TEXT("\n# time_note,")+ProbeCSVQuote(R.TimeNote)+
        TEXT("\n# scalar_id,")+ProbeCSVQuote(R.Scalar)+TEXT("\n# scalar_label,")+ProbeCSVQuote(R.Label)+
        TEXT("\n# scalar_unit,")+ProbeCSVQuote(R.Unit)+TEXT("\n# scalar_origin,")+ProbeCSVQuote(R.Origin)+
        TEXT("\n# sampling_method,")+ProbeCSVQuote(R.Method)+TEXT("\n# probe_kind,")+(R.Probe.Kind==EStudioProbeKind::Point?TEXT("point"):TEXT("line"))+
        TEXT("\n# line_start_scene_m,")+ProbePosition(R.Probe.A)+TEXT("\n# line_end_scene_m,")+ProbePosition(R.Probe.B)+
        TEXT("\n# sample_index,zero-based along the saved probe; missing values have empty cells and explicit statuses\n")+
        TEXT("frame_ordinal,source_step,time_s,sample_index,distance_m,scene_x_m,scene_y_m,scene_z_m,source_x_m,source_y_m,source_z_m,original_point_id,status,value\n");
    int32 Count=0;
    for(int32 I=0;I<R.Frames.Num();++I)
    {
        const auto& F=R.Frames[I];if(H.Times[I]!=F.Frame.Time){Error=TEXT("The chart times do not match the frozen source frames.");return false;}
        if(S.bManualTime&&(F.Frame.Time<S.TimeMinimum||F.Frame.Time>S.TimeMaximum))continue;
        for(int32 SampleIndex:Indices)
        {
            const auto& P=F.Samples[SampleIndex];
            Text+=FString::Printf(TEXT("%d,%d,%.17g,%d,%.17g,"),F.Ordinal,F.Frame.Index,F.Frame.Time,SampleIndex,P.DistanceAlongLineMeters)+
                ProbePosition(P.ScenePosition)+TEXT(",")+ProbePosition(P.SourcePosition)+TEXT(",")+
                (P.PointId.IsSet()?FString::Printf(TEXT("%lld"),*P.PointId):FString())+TEXT(",")+
                ProbeCSVQuote(StudioProbeProfile::SampleStatus(P.Status))+TEXT(",")+(P.Value.IsSet()?ProbeNumber(*P.Value):FString())+TEXT("\n");
            ++Count;
            if(Text.Len()>32*1024*1024){Error=TEXT("This CSV exceeds the 32 Mi-character export limit. Narrow the time window or position selection.");return false;}
        }
    }
    if(!Count){Error=TEXT("No sampled frames in the selected time window.");return false;}
    Out=MoveTemp(Text);Rows=Count;Error.Empty();return true;
}

void FStudioProbeMonitorSession::DropHistory(){Loaded.Reset();Chart={};++Revision;}
void FStudioProbeMonitorSession::Select(FStudioProbeHistoryRequest Request)
{Task.Cancel();Selected=MoveTemp(Request);DropHistory();Notice=TEXT("Choose a frame range, then Generate history.");}
void FStudioProbeMonitorSession::SetRange(int32 FirstOrdinal,int32 LastOrdinal)
{
    if(!Selected.IsSet()||IsBusy()||(Selected->FirstOrdinal==FirstOrdinal&&Selected->LastOrdinal==LastOrdinal))return;
    Selected->FirstOrdinal=FirstOrdinal;Selected->LastOrdinal=LastOrdinal;DropHistory();Notice=TEXT("Frame range changed. Generate history to sample it.");
}
bool FStudioProbeMonitorSession::Generate()
{
    if(!Selected.IsSet()){Notice=TEXT("Choose a saved probe from Choose history.");return false;}
    FString Error;if(!Task.Start(*Selected,Error)){Notice=Error;return false;}
    DropHistory();Notice=TEXT("Sampling original recorded frames…");return true;
}
void FStudioProbeMonitorSession::Cancel(){Task.Cancel();Notice=TEXT("Probe history cancelled. Generate history to retry.");++Revision;}
void FStudioProbeMonitorSession::Clear(){Task.Cancel();Selected.Reset();DropHistory();Notice=TEXT("Probe history removed. The saved probe remains available.");}
void FStudioProbeMonitorSession::Tick(const FGuid& Project,const TSharedPtr<const IStudioSolver,ESPMode::ThreadSafe>& Source,
    const FStudioInspectionObjects& Objects)
{
    if(Selected.IsSet())
    {
        const auto* Probe=Objects.Probes.FindByPredicate([&](const auto& P){return P.Id==Selected->Probe.Id;});
        if(Project!=Selected->ProjectId||Source!=Selected->Source||!Probe||!(*Probe==Selected->Probe))
        {Clear();Notice=TEXT("The project, recording or saved probe changed. Choose the probe again to rebuild its history.");}
    }
    if(auto Result=Task.Poll())
    {
        if(!Selected.IsSet())return;
        if(Result->Status==EStudioProbeHistoryStatus::Complete&&Result->Matches(*Selected))
        {
            Loaded=StudioProbeMonitor::MakeHistory(MakeShared<FStudioProbeHistoryResult,ESPMode::ThreadSafe>(MoveTemp(*Result)),Notice);
            if(Loaded){Chart=StudioMonitor::Defaults(*Loaded);Notice=FString::Printf(TEXT("Sampled %d original frames. Select positions to inspect or export."),Loaded->Times.Num());}
        }
        else if(Result->Status!=EStudioProbeHistoryStatus::Cancelled)Notice=Result->Error;
        ++Revision;
    }
}
bool FStudioProbeMonitorSession::UpdateSettings(const FStudioMonitorSettings& Value)
{
    if(!Loaded||IsBusy())return false;
    if(!StudioMonitor::ValidateSource(Value,*Loaded,Notice))return false;
    Chart=Value;++Revision;return true;
}
