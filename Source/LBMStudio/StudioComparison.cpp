#include "StudioComparison.h"
#include "StudioModel.h"
#include "StudioSurfaceReconstruction.h"
#include "StudioVolume.h"
#include "Async/Async.h"

namespace
{
bool ComparisonCancelled(const FStudioLoadCancellation& C)
{return C&&C->load(std::memory_order_relaxed);}

bool ComparisonSettingsValid(const FStudioComparisonAlignment& A)
{
    if(A.Mode!=EStudioTimeAlignment::RecordedTime&&A.Mode!=EStudioTimeAlignment::ElapsedFromStart&&
        A.Mode!=EStudioTimeAlignment::ManualOffset)return false;
    if(A.Match!=EStudioTimeMatch::Exact&&A.Match!=EStudioTimeMatch::Nearest)return false;
    return FMath::IsFinite(A.SecondaryOffsetSeconds)&&FMath::IsFinite(A.MaximumMismatchSeconds)&&
        A.MaximumMismatchSeconds>=0&&(A.Mode==EStudioTimeAlignment::ManualOffset||A.SecondaryOffsetSeconds==0)&&
        (A.Match==EStudioTimeMatch::Nearest||A.MaximumMismatchSeconds==0);
}

double ComparisonTime(const FStudioRecordingDescriptor& D,int32 Ordinal,
    const FStudioComparisonAlignment& A,bool bSecondary)
{
    const double T=D.Frames[Ordinal].Time;
    if(A.Mode==EStudioTimeAlignment::ElapsedFromStart)return T-D.Frames[0].Time;
    return bSecondary&&A.Mode==EStudioTimeAlignment::ManualOffset?T+A.SecondaryOffsetSeconds:T;
}

EStudioComparisonStatus ComparisonTimeline(const FStudioRecordingDescriptor& D,
    const FStudioComparisonAlignment& A,bool bSecondary,const FStudioLoadCancellation& C)
{
    if(D.Frames.IsEmpty()||D.Frames.Num()>StudioComparison::MaxTimelineFrames)return EStudioComparisonStatus::InvalidTimeline;
    double Previous=0;
    for(int32 I=0;I<D.Frames.Num();++I)
    {
        if(ComparisonCancelled(C))return EStudioComparisonStatus::Cancelled;
        const auto& F=D.Frames[I];const double Aligned=ComparisonTime(D,I,A,bSecondary);
        if(F.Index<0||!FMath::IsFinite(F.Time)||F.Time<0||!FMath::IsFinite(Aligned)||
            (I&&(F.Index<=D.Frames[I-1].Index||F.Time<=D.Frames[I-1].Time||Aligned<=Previous)))
            return EStudioComparisonStatus::InvalidTimeline;
        Previous=Aligned;
    }
    return EStudioComparisonStatus::Ready;
}

bool ComparisonKnownUnit(const FString& Unit)
{
    const auto Trimmed=Unit.TrimStartAndEnd();
    return !Trimmed.IsEmpty()&&!Trimmed.Equals(TEXT("unspecified"),ESearchCase::IgnoreCase)&&
        !Trimmed.Equals(TEXT("unknown"),ESearchCase::IgnoreCase);
}

bool ComparisonFieldMatches(const IStudioSolver& Source,int32 Ordinal,
    const FStudioScalarDescriptor& Scalar,const IStudioField& Field)
{
    const auto Identity=Field.Identity();const auto Actual=Field.Scalar(Scalar.Id);const auto& D=Source.Descriptor();
    if(!Field.IsValid()||!Identity.IsSet()||!Actual.IsSet())return false;
    FString Reconstruction;auto Interpolation=D.bSourcePoints?EStudioFieldInterpolation::None:EStudioFieldInterpolation::SourceTriangles;
    if(const auto Surface=Source.Reconstruction())
    {Reconstruction=Surface->MetadataSHA256;Interpolation=EStudioFieldInterpolation::ReconstructedTriangles;}
    if(const auto Volume=Source.VolumeReconstruction())
    {Reconstruction=Volume->MetadataSHA256;Interpolation=EStudioFieldInterpolation::ReconstructedGrid;}
    return Identity->Dataset==D.Id&&Identity->MetadataSHA256==D.MetadataSHA256&&Identity->PayloadSHA256==D.PayloadSHA256&&
        Identity->Ordinal==Ordinal&&Identity->Frame.Index==D.Frames[Ordinal].Index&&Identity->Frame.Time==D.Frames[Ordinal].Time&&
        Identity->SpatialDimensions==D.SpatialDimensions&&Identity->SourceOffset==D.SourceOffset&&
        Identity->ReconstructionSHA256==Reconstruction&&Identity->Interpolation==Interpolation&&
        Actual->Id==Scalar.Id&&Actual->Label==Scalar.Label&&Actual->Unit==Scalar.Unit&&Actual->Origin==Scalar.Origin&&
        Actual->Minimum==Scalar.Minimum&&Actual->Maximum==Scalar.Maximum;
}
}

bool FStudioComparisonAlignment::operator==(const FStudioComparisonAlignment& Other) const
{
    return Mode==Other.Mode&&Match==Other.Match&&SecondaryOffsetSeconds==Other.SecondaryOffsetSeconds&&
        MaximumMismatchSeconds==Other.MaximumMismatchSeconds;
}

bool FStudioComparisonResult::Matches(const FStudioComparisonRequest& Current) const
{
    return Frames.Status==EStudioComparisonStatus::Ready&&Primary.Field&&Secondary.Field&&ProjectId==Current.ProjectId&&Scalar==Current.Scalar&&
        Frames.PrimaryOrdinal==Current.PrimaryOrdinal&&Alignment==Current.Alignment&&
        Current.Primary&&Current.Secondary&&PrimarySource.Pin()==Current.Primary&&SecondarySource.Pin()==Current.Secondary;
}

FStudioComparisonFrames StudioComparison::Align(const FStudioRecordingDescriptor& A,
    const FStudioRecordingDescriptor& B,int32 PrimaryOrdinal,const FStudioComparisonAlignment& Settings,
    const FStudioLoadCancellation& Cancellation)
{
    FStudioComparisonFrames Out;Out.PrimaryOrdinal=PrimaryOrdinal;
    auto Fail=[&](EStudioComparisonStatus Status,const TCHAR* Message)
    {Out.Status=Status;Out.Error=Message;return Out;};
    if(ComparisonCancelled(Cancellation))return Fail(EStudioComparisonStatus::Cancelled,TEXT("Comparison cancelled."));
    if(!ComparisonSettingsValid(Settings)||!A.Frames.IsValidIndex(PrimaryOrdinal))
        return Fail(EStudioComparisonStatus::InvalidRequest,TEXT("Choose an alignment mode, a valid original frame and a finite nonnegative matching tolerance. Use an offset only in manual-offset mode."));
    for(const bool bSecondary:{false,true})
    {
        const auto Status=ComparisonTimeline(bSecondary?B:A,Settings,bSecondary,Cancellation);
        if(Status!=EStudioComparisonStatus::Ready)return Fail(Status,Status==EStudioComparisonStatus::Cancelled?
            TEXT("Comparison cancelled."):TEXT("Comparison needs finite, strictly increasing source steps and aligned times, with at most 1,000,000 frames per recording. Reduce an offset that collapses timestamp precision."));
    }
    Out.PrimaryFrame=A.Frames[PrimaryOrdinal];Out.PrimaryAlignedTime=ComparisonTime(A,PrimaryOrdinal,Settings,false);
    const double Target=Out.PrimaryAlignedTime;
    if(Target<ComparisonTime(B,0,Settings,true)||Target>ComparisonTime(B,B.Frames.Num()-1,Settings,true))
        return Fail(EStudioComparisonStatus::NoMatch,TEXT("This frame is outside the second recording's aligned time range. No extrapolated frame is shown."));
    int32 Low=0,High=B.Frames.Num();
    while(Low<High)
    {const int32 Mid=Low+(High-Low)/2;if(ComparisonTime(B,Mid,Settings,true)<Target)Low=Mid+1;else High=Mid;}
    int32 Ordinal=Low;
    if(Settings.Match==EStudioTimeMatch::Nearest&&Low>0&&
        (Low==B.Frames.Num()||Target-ComparisonTime(B,Low-1,Settings,true)<=ComparisonTime(B,Low,Settings,true)-Target))Ordinal=Low-1;
    if(!B.Frames.IsValidIndex(Ordinal))return Fail(EStudioComparisonStatus::NoMatch,TEXT("No original frame matches this time."));
    const double Time=ComparisonTime(B,Ordinal,Settings,true),Delta=Time-Target;
    if(!FMath::IsFinite(Delta)||(Settings.Match==EStudioTimeMatch::Exact?Time!=Target:FMath::Abs(Delta)>Settings.MaximumMismatchSeconds))
        return Fail(EStudioComparisonStatus::NoMatch,TEXT("No original frame matches the selected time policy. Choose an explicit nearest-frame tolerance to allow a time difference."));
    if(ComparisonCancelled(Cancellation))return Fail(EStudioComparisonStatus::Cancelled,TEXT("Comparison cancelled."));
    Out.SecondaryOrdinal=Ordinal;Out.SecondaryFrame=B.Frames[Ordinal];Out.SecondaryAlignedTime=Time;Out.MismatchSeconds=Delta;
    Out.Status=EStudioComparisonStatus::Ready;return Out;
}

FStudioComparisonResult StudioComparison::Evaluate(const FStudioComparisonRequest& R,const FStudioLoadCancellation& C)
{
    FStudioComparisonResult Out;Out.ProjectId=R.ProjectId;Out.Scalar=R.Scalar;Out.Alignment=R.Alignment;
    Out.PrimarySource=R.Primary;Out.SecondarySource=R.Secondary;Out.Frames.PrimaryOrdinal=R.PrimaryOrdinal;
    auto Fail=[&](EStudioComparisonStatus Status,const FString& Error)
    {Out.Frames.Status=Status;Out.Frames.Error=Error;Out.Primary={};Out.Secondary={};return MoveTemp(Out);};
    if(ComparisonCancelled(C))return Fail(EStudioComparisonStatus::Cancelled,TEXT("Comparison cancelled."));
    if(!R.ProjectId.IsValid()||!R.Primary||!R.Secondary||R.Scalar.IsEmpty())
        return Fail(EStudioComparisonStatus::InvalidRequest,TEXT("Choose two verified recordings and one common scalar."));
    const auto& A=R.Primary->Descriptor();const auto& B=R.Secondary->Descriptor();
    if(R.Primary->FrameCount()!=A.Frames.Num()||R.Secondary->FrameCount()!=B.Frames.Num()||A.Id.IsEmpty()||B.Id.IsEmpty())
        return Fail(EStudioComparisonStatus::InvalidRequest,TEXT("The recording timelines are unavailable or inconsistent."));
    Out.Frames=Align(A,B,R.PrimaryOrdinal,R.Alignment,C);
    if(Out.Frames.Status!=EStudioComparisonStatus::Ready)return Out;
    const auto* SA=A.Scalars.FindByPredicate([&](const auto& S){return S.Id==R.Scalar;});
    const auto* SB=B.Scalars.FindByPredicate([&](const auto& S){return S.Id==R.Scalar;});
    if(!SA||!SB||SA->Unit!=SB->Unit||!ComparisonKnownUnit(SA->Unit))
        return Fail(EStudioComparisonStatus::InvalidRequest,TEXT("Comparison requires the same supplied scalar ID and explicit matching units. No automatic unit conversion or scalar fallback is applied."));
    for(const bool bSecondary:{false,true})
    {
        if(ComparisonCancelled(C))return Fail(EStudioComparisonStatus::Cancelled,TEXT("Comparison cancelled."));
        const auto& Source=bSecondary?R.Secondary:R.Primary;const auto& D=Source->Descriptor();
        const auto& Scalar=bSecondary?*SB:*SA;const int32 Ordinal=bSecondary?Out.Frames.SecondaryOrdinal:Out.Frames.PrimaryOrdinal;
        const auto Read=Source->ReadScalarFrame(Ordinal,R.Scalar,C);
        if(ComparisonCancelled(C))return Fail(EStudioComparisonStatus::Cancelled,TEXT("Comparison cancelled."));
        if(!Read.Field||!Read.Error.IsEmpty())return Fail(EStudioComparisonStatus::ReadFailed,
            (bSecondary?TEXT("Second recording: "):TEXT("First recording: "))+(Read.Error.IsEmpty()?TEXT("The original scalar frame is unavailable."):Read.Error));
        if(!ComparisonFieldMatches(*Source,Ordinal,Scalar,*Read.Field))
            return Fail(EStudioComparisonStatus::IdentityMismatch,TEXT("A comparison snapshot does not match its original source, frame, scalar or reconstruction. No pair published."));
        auto& Side=bSecondary?Out.Secondary:Out.Primary;Side.Title=D.Title;Side.SourceURL=D.SourceURL;Side.TimeNote=D.TimeNote;
        Side.Scalar=Scalar;Side.Identity=*Read.Field->Identity();Side.Field=Read.Field;
    }
    if(ComparisonCancelled(C))return Fail(EStudioComparisonStatus::Cancelled,TEXT("Comparison cancelled."));
    return Out;
}

FStudioComparisonTask::~FStudioComparisonTask(){Shutdown();}
bool FStudioComparisonTask::Start(FStudioComparisonRequest Request,FString& Error)
{
    check(IsInGameThread());
    if(bShutdown||Pending.IsValid()){Error=TEXT("Wait for the comparison read to finish or cancel it.");return false;}
    if(!Request.ProjectId.IsValid()||!Request.Primary||!Request.Secondary||Request.Scalar.IsEmpty()||!ComparisonSettingsValid(Request.Alignment))
    {Error=TEXT("Choose two recordings, a scalar and an explicit time alignment.");return false;}
    Error.Empty();Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    Pending=Async(EAsyncExecution::ThreadPool,[Request=MoveTemp(Request),Cancel=Cancellation]{return StudioComparison::Evaluate(Request,Cancel);});
    return true;
}
void FStudioComparisonTask::Cancel()
{check(IsInGameThread());if(Cancellation)Cancellation->store(true,std::memory_order_relaxed);}
void FStudioComparisonTask::Shutdown()
{
    if(bShutdown)return;bShutdown=true;Cancel();
    if(Pending.IsValid()){Pending.Wait();Pending=TFuture<FStudioComparisonResult>();}Cancellation.Reset();
}
TOptional<FStudioComparisonResult> FStudioComparisonTask::Poll()
{
    check(IsInGameThread());if(!Pending.IsValid()||!Pending.IsReady())return {};
    auto Result=Pending.Consume();
    if(ComparisonCancelled(Cancellation))
    {Result.Frames.Status=EStudioComparisonStatus::Cancelled;Result.Frames.Error=TEXT("Comparison cancelled.");Result.Primary={};Result.Secondary={};}
    Cancellation.Reset();return Result;
}
