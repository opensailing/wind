#include "StudioProbeMarkers.h"
#include "Async/Async.h"

bool FStudioProbeMarkerRequest::operator==(const FStudioProbeMarkerRequest& Other) const
{return Project==Other.Project&&Source==Other.Source&&Offset==Other.Offset&&Geometry==Other.Geometry&&Queries==Other.Queries;}

TOptional<FVector> FStudioProbeMarkerResult::Position(const FGuid& ProjectId,const FStudioProbeObject& Probe) const
{
    if(bCancelled||Project!=ProjectId||!(Source==Probe.Source)||!Probe.bVisible||
        Probe.Method!=EStudioProbeMethod::OriginalPoint||!Probe.PointId.IsSet())return {};
    if(!Queries.Contains(FStudioProbeMarkerQuery{Probe.Id,Probe.PointId.GetValue()}))return {};
    const auto* Value=Positions.Find(Probe.Id);return Value?TOptional<FVector>(*Value):TOptional<FVector>();
}

FStudioProbeMarkerResult StudioProbeMarkers::Resolve(const FStudioProbeMarkerRequest& R,const FStudioLoadCancellation& Cancellation)
{
    FStudioProbeMarkerResult Out;Out.Project=R.Project;Out.Source=R.Source;Out.Queries=R.Queries;
    if(!R.Project.IsValid()||!StudioInspectionObjects::IsValid(R.Source)||!R.Geometry||R.Offset.ContainsNaN()||
        R.Queries.Num()>StudioInspectionObjects::MaxObjectsPerKind||R.Geometry->Positions.Num()!=R.Geometry->PointIds.Num())return Out;
    TMap<int64,TArray<FGuid>> Wanted;
    for(const auto& Query:R.Queries)if(Query.Id.IsValid())Wanted.FindOrAdd(Query.PointId).Add(Query.Id);
    for(int32 Row=0;Row<R.Geometry->PointIds.Num()&&!Wanted.IsEmpty();++Row)
    {
        if((Row&255)==0&&Cancellation&&Cancellation->load(std::memory_order_relaxed))
        {Out.bCancelled=true;Out.Positions.Reset();return Out;}
        const int64 Id=R.Geometry->PointIds[Row];const auto* Matches=Wanted.Find(Id);if(!Matches)continue;
        const FVector P=R.Geometry->Positions[Row];
        if(!P.ContainsNaN())for(const auto& Object:*Matches)Out.Positions.Add(Object,FVector(P.X,P.Z,P.Y)+R.Offset);
        Wanted.Remove(Id);
    }
    if(Cancellation&&Cancellation->load(std::memory_order_relaxed)){Out.bCancelled=true;Out.Positions.Reset();}
    return Out;
}

FStudioProbeMarkerScheduler::~FStudioProbeMarkerScheduler()
{Clear();if(Pending.IsValid())Pending.Wait();}

void FStudioProbeMarkerScheduler::Submit(FStudioProbeMarkerRequest Request)
{
    check(IsInGameThread());
    if(!Request.Geometry||Request.Queries.IsEmpty()){Clear();return;}
    if(!Desired.IsSet()||!(*Desired==Request))
    {
        Completed.Reset();Desired=MoveTemp(Request);
        if(InFlight.IsSet()&&!(*InFlight==*Desired)&&Cancellation)Cancellation->store(true,std::memory_order_relaxed);
    }
    Tick();
}

void FStudioProbeMarkerScheduler::Tick()
{
    check(IsInGameThread());
    if(Pending.IsValid()&&Pending.IsReady())
    {
        auto Value=Pending.Consume();
        if(!Value.bCancelled&&Desired.IsSet()&&InFlight.IsSet()&&*InFlight==*Desired)
        {Value.Serial=++Serial;Completed=MoveTemp(Value);}
        InFlight.Reset();Cancellation.Reset();
    }
    if(Pending.IsValid()||Completed.IsSet()||!Desired.IsSet())return;
    InFlight=Desired;Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);++Started;
    Pending=Async(EAsyncExecution::ThreadPool,[Request=*InFlight,Cancel=Cancellation]{return StudioProbeMarkers::Resolve(Request,Cancel);});
}

void FStudioProbeMarkerScheduler::Clear()
{
    check(IsInGameThread());Desired.Reset();Completed.Reset();
    if(Cancellation)Cancellation->store(true,std::memory_order_relaxed);
    if(Pending.IsValid()&&Pending.IsReady()){Pending.Consume();InFlight.Reset();Cancellation.Reset();}
}
const FStudioProbeMarkerResult* FStudioProbeMarkerScheduler::Result() const
{return Completed.IsSet()?&Completed.GetValue():nullptr;}
