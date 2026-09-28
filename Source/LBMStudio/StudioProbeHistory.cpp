#include "StudioProbeHistory.h"
#include "StudioModel.h"
#include "Async/Async.h"

namespace
{
bool SameHistorySource(const FStudioFieldIdentity& A,const FStudioFieldIdentity& B)
{
    return A.Dataset==B.Dataset&&A.MetadataSHA256==B.MetadataSHA256&&A.PayloadSHA256==B.PayloadSHA256&&
        A.ReconstructionSHA256==B.ReconstructionSHA256&&A.SpatialDimensions==B.SpatialDimensions&&
        A.SourceOffset==B.SourceOffset&&A.Interpolation==B.Interpolation;
}
}

bool FStudioProbeHistoryResult::Matches(const FStudioProbeHistoryRequest& R) const
{
    return Status==EStudioProbeHistoryStatus::Complete&&ProjectId==R.ProjectId&&Probe==R.Probe&&Scalar==R.Scalar&&
        FirstOrdinal==R.FirstOrdinal&&LastOrdinal==R.LastOrdinal&&R.Source&&SampledSource.Pin()==R.Source;
}

bool StudioProbeHistory::Validate(const FStudioProbeHistoryRequest& R,FString& Error)
{
    Error=TEXT("A saved probe, verified recording and exact scalar are required.");
    FStudioInspectionObjects Objects;Objects.Probes.Add(R.Probe);FString ProbeError;
    if(!R.ProjectId.IsValid()||!R.Source||!StudioInspectionObjects::IsValid(Objects,ProbeError)||R.Scalar.IsEmpty()||
        (!R.Probe.Field.IsEmpty()&&R.Probe.Field!=R.Scalar))return false;
    const auto& D=R.Source->Descriptor();
    if(!(R.Probe.Source==FStudioInspectionSource{D.Id,D.MetadataSHA256,D.PayloadSHA256}))
    {Error=TEXT("This probe belongs to another recording. Open its original source.");return false;}
    if(!D.Scalars.ContainsByPredicate([&](const auto& S){return S.Id==R.Scalar;}))
    {Error=TEXT("The requested probe scalar is not supplied by this recording.");return false;}
    if(R.Source->FrameCount()!=D.Frames.Num()||R.FirstOrdinal<0||R.LastOrdinal<R.FirstOrdinal||!D.Frames.IsValidIndex(R.LastOrdinal))
    {Error=TEXT("Choose an inclusive frame range inside this recording.");return false;}
    const int64 Count=int64(R.LastOrdinal)-R.FirstOrdinal+1;
    const int32 Samples=R.Probe.Kind==EStudioProbeKind::Point?1:R.Probe.Samples;
    if(Count>MaxFrames||Count*Samples>MaxSamples)
    {Error=TEXT("Probe history supports 100,000 frames and 1,000,000 samples per request. Reduce the frame range or line sample count.");return false;}
    Error.Empty();return true;
}

FStudioProbeHistoryResult StudioProbeHistory::Evaluate(const FStudioProbeHistoryRequest& R,
    const FStudioLoadCancellation& Cancellation,std::atomic<int32>* CompletedFrames)
{
    FStudioProbeHistoryResult Out;Out.ProjectId=R.ProjectId;Out.Probe=R.Probe;Out.Scalar=R.Scalar;
    Out.FirstOrdinal=R.FirstOrdinal;Out.LastOrdinal=R.LastOrdinal;Out.SampledSource=R.Source;
    if(CompletedFrames)CompletedFrames->store(0,std::memory_order_relaxed);
    auto Cancelled=[&]{return Cancellation&&Cancellation->load(std::memory_order_relaxed);};
    auto Fail=[&](EStudioProbeHistoryStatus Status,const FString& Error,int32 Ordinal=INDEX_NONE)
    {Out.Status=Status;Out.Error=Error;Out.FailedOrdinal=Ordinal;Out.Frames.Empty();return MoveTemp(Out);};
    if(Cancelled())return Fail(EStudioProbeHistoryStatus::Cancelled,TEXT("Probe history cancelled."));
    if(!Validate(R,Out.Error))return Out;
    const auto& D=R.Source->Descriptor();
    Out.SourceTitle=D.Title;Out.SourceURL=D.SourceURL;Out.TimeNote=D.TimeNote;
    const auto& Scalar=*D.Scalars.FindByPredicate([&](const auto& S){return S.Id==R.Scalar;});
    Out.Label=Scalar.Label;Out.Unit=Scalar.Unit;Out.Origin=Scalar.Origin;
    Out.Frames.Reserve(R.LastOrdinal-R.FirstOrdinal+1);
    for(int32 Ordinal=R.FirstOrdinal;Ordinal<=R.LastOrdinal;++Ordinal)
    {
        if(Cancelled())return Fail(EStudioProbeHistoryStatus::Cancelled,TEXT("Probe history cancelled."));
        const auto Frame=D.Frames[Ordinal];
        if(!FMath::IsFinite(Frame.Time)||Frame.Time<0||Frame.Index<0||(!Out.Frames.IsEmpty()&&
            (Frame.Time<=Out.Frames.Last().Frame.Time||Frame.Index<=Out.Frames.Last().Frame.Index)))
            return Fail(EStudioProbeHistoryStatus::IdentityMismatch,TEXT("Recorded times and source steps must be finite and strictly increasing."),Ordinal);
        const auto Read=R.Source->ReadScalarFrame(Ordinal,R.Scalar,Cancellation);
        if(Cancelled())return Fail(EStudioProbeHistoryStatus::Cancelled,TEXT("Probe history cancelled."));
        if(!Read.Field||!Read.Field->IsValid()||!Read.Error.IsEmpty())
            return Fail(EStudioProbeHistoryStatus::ReadFailed,Read.Error.IsEmpty()?TEXT("Could not read the exact recorded frame."):Read.Error,Ordinal);
        const auto Identity=Read.Field->Identity();const auto LoadedScalar=Read.Field->Scalar(R.Scalar);
        if(!Identity.IsSet()||Identity->Ordinal!=Ordinal||Identity->Frame.Index!=Frame.Index||Identity->Frame.Time!=Frame.Time||
            !(R.Probe.Source==FStudioInspectionSource{Identity->Dataset,Identity->MetadataSHA256,Identity->PayloadSHA256})||
            Identity->SpatialDimensions!=D.SpatialDimensions||Identity->SourceOffset!=D.SourceOffset||
            (Out.Identity.IsSet()&&!SameHistorySource(*Out.Identity,*Identity))||!LoadedScalar.IsSet()||
            LoadedScalar->Id!=Scalar.Id||LoadedScalar->Label!=Scalar.Label||LoadedScalar->Unit!=Scalar.Unit||
            LoadedScalar->Origin!=Scalar.Origin||LoadedScalar->Minimum!=Scalar.Minimum||LoadedScalar->Maximum!=Scalar.Maximum)
            return Fail(EStudioProbeHistoryStatus::IdentityMismatch,TEXT("A sampled frame does not match this recording, scalar or reconstruction. No history published."),Ordinal);
        FStudioProbeRequest Query;Query.ProjectId=R.ProjectId;Query.PresentationId=uint64(Ordinal)+1;
        Query.Probe=R.Probe;Query.DisplayedScalar=R.Scalar;Query.Field=Read.Field;
        auto Result=StudioProbeSampling::Evaluate(Query,Cancellation);
        if(Cancelled()||Result.Status==EStudioProbeStatus::Cancelled)
            return Fail(EStudioProbeHistoryStatus::Cancelled,TEXT("Probe history cancelled."));
        if(Result.Status!=EStudioProbeStatus::Ready)
            return Fail(EStudioProbeHistoryStatus::ReadFailed,Result.Message,Ordinal);
        if(!Out.Identity.IsSet()){Out.Identity=Result.Identity;Out.Method=Result.Method;}
        FStudioProbeHistoryFrame Row;Row.Ordinal=Ordinal;Row.Frame=Frame;Row.Samples=MoveTemp(Result.Samples);
        Out.Frames.Add(MoveTemp(Row));
        if(CompletedFrames)CompletedFrames->store(Out.Frames.Num(),std::memory_order_relaxed);
    }
    if(Cancelled())return Fail(EStudioProbeHistoryStatus::Cancelled,TEXT("Probe history cancelled."));
    Out.Status=EStudioProbeHistoryStatus::Complete;return Out;
}

FStudioProbeHistoryTask::~FStudioProbeHistoryTask(){Shutdown();}
bool FStudioProbeHistoryTask::Start(FStudioProbeHistoryRequest Request,FString& Error)
{
    check(IsInGameThread());
    if(bShutdown||Pending.IsValid()){Error=TEXT("Wait for the current probe history operation to finish or cancel.");return false;}
    if(!StudioProbeHistory::Validate(Request,Error))return false;
    Total=Request.LastOrdinal-Request.FirstOrdinal+1;
    Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    Progress=MakeShared<std::atomic<int32>,ESPMode::ThreadSafe>(0);
    Pending=Async(EAsyncExecution::ThreadPool,[Request=MoveTemp(Request),Cancel=Cancellation,Count=Progress]
    {return StudioProbeHistory::Evaluate(Request,Cancel,Count.Get());});
    return true;
}
void FStudioProbeHistoryTask::Cancel()
{check(IsInGameThread());if(Cancellation)Cancellation->store(true,std::memory_order_relaxed);}
void FStudioProbeHistoryTask::Shutdown()
{
    if(bShutdown)return;bShutdown=true;Cancel();
    if(Pending.IsValid()){Pending.Wait();Pending=TFuture<FStudioProbeHistoryResult>();}
    Cancellation.Reset();Progress.Reset();Total=0;
}
TOptional<FStudioProbeHistoryResult> FStudioProbeHistoryTask::Poll()
{
    check(IsInGameThread());
    if(!Pending.IsValid()||!Pending.IsReady())return {};
    auto Result=Pending.Consume();
    // Cancellation can arrive after the worker completed but before publication.
    if(Cancellation&&Cancellation->load(std::memory_order_relaxed))
    {Result.Status=EStudioProbeHistoryStatus::Cancelled;Result.Error=TEXT("Probe history cancelled.");Result.Frames.Empty();}
    Cancellation.Reset();return Result;
}
