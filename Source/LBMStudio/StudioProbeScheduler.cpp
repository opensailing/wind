#include "StudioProbeScheduler.h"
#include "Async/Async.h"

namespace
{
FString RequestedProbeField(const FStudioProbeRequest& Request)
{ return Request.Probe.Field.IsEmpty()?Request.DisplayedScalar:Request.Probe.Field; }
bool SameProbeInput(const FStudioProbeRequest& A,const FStudioProbeRequest& B)
{
    return A.ProjectId==B.ProjectId&&A.Probe==B.Probe&&A.Field==B.Field&&
        RequestedProbeField(A)==RequestedProbeField(B);
}
bool SameProbeResult(const FStudioProbeResult& Result,const FStudioProbeRequest& Request)
{
    // A camera-only capture can reuse the same scientific answer. All other
    // identity checks still run, including the exact immutable field instance.
    auto AtOriginalCapture=Request;AtOriginalCapture.PresentationId=Result.PresentationId;
    return Result.Matches(AtOriginalCapture);
}
}

FStudioProbeScheduler::~FStudioProbeScheduler(){ Shutdown(); }

void FStudioProbeScheduler::CancelPending()
{
    if(Cancellation&&!Cancellation->exchange(true,std::memory_order_relaxed))++Cancelled;
}

void FStudioProbeScheduler::Submit(FStudioProbeRequest Request)
{
    check(IsInGameThread());
    if(bShutdown)return;
    if(!Request.Field||!Request.ProjectId.IsValid()||Request.PresentationId==0){Clear();return;}
    Desired=MoveTemp(Request);
    if(Completed.IsSet())
    {
        if(SameProbeResult(*Completed,*Desired))Completed->PresentationId=Desired->PresentationId;
        else Completed.Reset();
    }
    if(InFlight.IsSet()&&!SameProbeInput(*InFlight,*Desired))CancelPending();
    Tick();
}

void FStudioProbeScheduler::Tick()
{
    check(IsInGameThread());
    if(bShutdown)return;
    if(Pending.IsValid()&&Pending.IsReady())
    {
        auto Value=Pending.Consume();
        InFlight.Reset();Cancellation.Reset();
        if(Desired.IsSet()&&SameProbeResult(Value,*Desired))
        {Value.PresentationId=Desired->PresentationId;Completed=MoveTemp(Value);}
        else ++Discarded;
    }
    if(Pending.IsValid()||!Desired.IsSet()||Result())return;
    InFlight=Desired;Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);++Started;
    Pending=Async(EAsyncExecution::ThreadPool,[Request=*InFlight,Cancel=Cancellation]
    {return StudioProbeSampling::Evaluate(Request,Cancel);});
}

void FStudioProbeScheduler::Clear()
{
    check(IsInGameThread());
    Desired.Reset();Completed.Reset();CancelPending();
}

void FStudioProbeScheduler::Shutdown()
{
    if(bShutdown)return;
    bShutdown=true;Desired.Reset();Completed.Reset();CancelPending();
    if(Pending.IsValid()){Pending.Wait();Pending=TFuture<FStudioProbeResult>();}
    InFlight.Reset();Cancellation.Reset();
}

const FStudioProbeResult* FStudioProbeScheduler::Result() const
{
    return Desired.IsSet()&&Completed.IsSet()&&Completed->Matches(*Desired)?&Completed.GetValue():nullptr;
}
