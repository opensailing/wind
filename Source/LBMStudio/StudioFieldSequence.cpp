#include "StudioFieldSequence.h"
#include "StudioCSVExport.h"
#include "StudioModel.h"
#include "StudioInspectionObjects.h"
#include "StudioFileDialog.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"

namespace
{
bool SameSequenceScalar(const FStudioScalarDescriptor& A,const FStudioScalarDescriptor& B)
{return A.Id==B.Id&&A.Label==B.Label&&A.Unit==B.Unit&&A.Origin==B.Origin&&A.Minimum==B.Minimum&&A.Maximum==B.Maximum;}
bool SameSequenceSource(const FStudioFieldIdentity& A,const FStudioFieldIdentity& B)
{return A.Dataset==B.Dataset&&A.MetadataSHA256==B.MetadataSHA256&&A.PayloadSHA256==B.PayloadSHA256&&
    A.ReconstructionSHA256==B.ReconstructionSHA256&&A.Interpolation==B.Interpolation&&
    A.SpatialDimensions==B.SpatialDimensions&&A.SourceOffset==B.SourceOffset;}
bool SequenceText(FArchive& Archive,const FString& Text,int64& Bytes)
{
    const FTCHARToUTF8 UTF8(*Text);Archive.Serialize(const_cast<char*>(UTF8.Get()),UTF8.Length());
    Bytes+=UTF8.Length();return !Archive.IsError();
}
}

bool StudioFieldSequence::Validate(const FStudioFieldSequenceRequest& R,FString& Error)
{
    Error=TEXT("Select a verified recording and at least one original scalar array.");
    if(!R.Source||R.Scalars.IsEmpty()||R.Scalars.Num()>64)return false;
    const auto& D=R.Source->Descriptor();
    if(!StudioInspectionObjects::IsValid(FStudioInspectionSource{D.Id,D.MetadataSHA256,D.PayloadSHA256})||
        D.SourceOffset.ContainsNaN()||(D.SpatialDimensions!=2&&D.SpatialDimensions!=3)||
        (R.Coordinates!=EStudioExportCoordinates::Source&&R.Coordinates!=EStudioExportCoordinates::Scene)||
        (R.Format!=EStudioFieldExportFormat::VTK&&R.Format!=EStudioFieldExportFormat::CSV))return false;
    if(R.Source->FrameCount()!=D.Frames.Num()||R.FirstOrdinal<0||R.LastOrdinal<R.FirstOrdinal||
        !D.Frames.IsValidIndex(R.LastOrdinal)||int64(R.LastOrdinal)-R.FirstOrdinal+1>MaximumFrames)
    {Error=TEXT("Choose an inclusive range of at most 100,000 original frames inside the recording.");return false;}
    TSet<FString> Seen;
    for(const auto& Id:R.Scalars)
    {
        bool Valid=!Id.IsEmpty()&&Id.Len()<=256;for(const TCHAR C:Id)Valid&=C>=32;
        if(!Valid||Seen.Contains(Id)||!D.Scalars.ContainsByPredicate([&](const auto& S){return S.Id==Id;}))
        {Error=TEXT("Select distinct scalar arrays supplied by the recording.");return false;}
        Seen.Add(Id);
    }
    for(int32 N=R.FirstOrdinal;N<=R.LastOrdinal;++N)
    {
        const auto& F=D.Frames[N];
        if(!FMath::IsFinite(F.Time)||F.Time<0||F.Index<0||
            (N>R.FirstOrdinal&&(F.Time<=D.Frames[N-1].Time||F.Index<=D.Frames[N-1].Index)))
        {Error=TEXT("Original frame times and steps must be finite and strictly increasing.");return false;}
    }
    Error.Empty();return true;
}

FStudioFieldSequenceResult StudioFieldSequence::Write(const FStudioFieldSequenceRequest& R,const FString& Directory,
    const FStudioLoadCancellation& Cancellation,TFunction<void(int32,int64,int64)> Progress)
{
    FStudioFieldSequenceResult Out;
    auto Cancelled=[&]{return Cancellation&&Cancellation->load();};
    auto Fail=[&](const FString& Error,int32 Ordinal=INDEX_NONE)
    {Out.bSuccess=false;Out.bCancelled=Cancelled();Out.Error=Out.bCancelled?TEXT("Frame sequence export cancelled."):Error;
        Out.FailedOrdinal=Ordinal;return MoveTemp(Out);};
    if(Cancelled()||!Validate(R,Out.Error))return Fail(Out.Error);
    const auto& D=R.Source->Descriptor();
    const bool CSV=R.Format==EStudioFieldExportFormat::CSV;
    TUniquePtr<FArchive> Collection(IFileManager::Get().CreateFileWriter(*(Directory/(CSV?TEXT("frames.csv"):TEXT("flow.pvd"))),FILEWRITE_NoReplaceExisting));
    if(!Collection)return Fail(TEXT("Could not create the staged frame collection. Check directory access and free space."));
    if(!SequenceText(*Collection,CSV?TEXT("frame_ordinal,source_step,source_time_s,file\n"):
        TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<VTKFile type=\"Collection\" version=\"0.1\" byte_order=\"LittleEndian\">\n<Collection>\n"),Out.Bytes))
        return Fail(TEXT("Could not write the frame collection."));
    TArray<FString> Expressions;
    for(int32 Ordinal=R.FirstOrdinal;Ordinal<=R.LastOrdinal;++Ordinal)
    {
        if(Cancelled())return Fail(FString(),Ordinal);
        if(Progress)Progress(Out.CompletedFrames,0,0);
        {
            const auto Read=R.Source->ReadScalarFrame(Ordinal,R.Scalars[0],Cancellation);
            if(Cancelled())return Fail(FString(),Ordinal);
            if(!Read.Field||!Read.Field->IsValid()||!Read.Error.IsEmpty())
                return Fail(Read.Error.IsEmpty()?TEXT("Could not read an original frame."):Read.Error,Ordinal);
            const auto I=Read.Field->Identity();const auto F=D.Frames[Ordinal];
            if(!I||I->Dataset!=D.Id||I->MetadataSHA256!=D.MetadataSHA256||I->PayloadSHA256!=D.PayloadSHA256||
                I->Ordinal!=Ordinal||I->Frame.Index!=F.Index||I->Frame.Time!=F.Time||I->SpatialDimensions!=D.SpatialDimensions||
                I->SourceOffset!=D.SourceOffset||(Out.FirstIdentity&&!SameSequenceSource(*I,*Out.FirstIdentity))||
                Read.Field->OriginalPointCount()!=D.NodeCount||Read.Field->OriginalTriangleCount()!=D.TriangleCount)
                return Fail(TEXT("A frame differs from the selected source, original time or topology. No sequence published."),Ordinal);
            for(int32 K=0;K<R.Scalars.Num();++K)
            {
                const auto S=Read.Field->Scalar(R.Scalars[K]);
                const auto* Expected=D.Scalars.FindByPredicate([&](const auto& V){return V.Id==R.Scalars[K];});
                if(!S||!Expected||!SameSequenceScalar(*S,*Expected)||
                    (Out.FirstIdentity&&Expressions[K]!=Read.Field->ScalarExpression(R.Scalars[K])))
                    return Fail(TEXT("A frame changed the selected scalar meaning. No sequence published."),Ordinal);
                if(!Out.FirstIdentity)Expressions.Add(Read.Field->ScalarExpression(R.Scalars[K]));
            }
            const FString Name=FString::Printf(TEXT("frame_%06d.%s"),Ordinal,CSV?TEXT("csv"):TEXT("vtp"));
            TUniquePtr<FArchive> File(IFileManager::Get().CreateFileWriter(*(Directory/Name),FILEWRITE_NoReplaceExisting));
            if(!File)return Fail(TEXT("Could not create a staged frame. Check free space and directory access."),Ordinal);
            const FStudioFieldExportRequest FrameRequest{Read.Field,R.Scalars,R.Coordinates,R.Format};
            auto FrameProgress=[&](int64 Done,int64 Total){if(Progress)Progress(Out.CompletedFrames,Done,Total);};
            const auto Result=CSV?StudioCSVExport::Write(FrameRequest,*File,Cancellation,FrameProgress):
                StudioVTKExport::Write(FrameRequest,*File,Cancellation,FrameProgress);
            const bool Closed=File->Close()&&!File->IsError();File.Reset();
            if(!Result.bSuccess)return Fail(Result.Error,Ordinal);
            if(!Closed)return Fail(TEXT("Could not close a staged frame. Check free space."),Ordinal);
            Out.Bytes+=Result.Bytes;
            const FString Entry=CSV?FString::Printf(TEXT("%d,%d,%.17g,%s\n"),Ordinal,F.Index,F.Time,*Name):
                FString::Printf(TEXT("<DataSet timestep=\"%.17g\" group=\"\" part=\"0\" file=\"%s\"/>\n"),F.Time,*Name);
            if(!SequenceText(*Collection,Entry,Out.Bytes))
                return Fail(TEXT("Could not write the frame collection. Check free space."),Ordinal);
            if(!Out.FirstIdentity)Out.FirstIdentity=*I;Out.LastIdentity=*I;
        } // Release the original and additional scalar snapshots before the next read.
        ++Out.CompletedFrames;if(Progress)Progress(Out.CompletedFrames,0,0);
    }
    if(Cancelled())return Fail(FString());
    const bool Wrote=CSV||SequenceText(*Collection,TEXT("</Collection>\n</VTKFile>\n"),Out.Bytes);
    const bool Closed=Collection->Close()&&!Collection->IsError();Collection.Reset();
    if(!Wrote||!Closed)return Fail(TEXT("Could not finish the frame collection. Check free space."));
    if(Cancelled())return Fail(FString());
    Out.bSuccess=true;return Out;
}

struct FStudioFieldSequenceWork
{
    std::atomic<EStudioFieldExportState> State{EStudioFieldExportState::Writing};
    FCriticalSection ProgressMutex;
    int32 Frames=0,Total=0;
    int64 Completed=0,FrameTotal=0;
    FStudioLoadCancellation Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
};
FStudioFieldSequenceTask::~FStudioFieldSequenceTask(){Shutdown();}
bool FStudioFieldSequenceTask::Start(FStudioFieldSequenceRequest Request,const FString& Parent,const FString& Name,FString& Error)
{
    if(bShutdown||Pending.IsValid()){Error=TEXT("Wait for the current frame export to finish or cancel.");return false;}
    bool ValidName=!Name.IsEmpty()&&Name.Len()<=128&&Name!=TEXT(".")&&Name!=TEXT("..")&&!Name.StartsWith(TEXT("."));
    for(const TCHAR C:Name)ValidName&=C>=32&&C!=TEXT('/')&&C!=TEXT('\\')&&C!=TEXT(':');
    if(!ValidName||Parent.IsEmpty()||FPaths::IsRelative(Parent))
    {Error=TEXT("Choose an existing destination folder and a simple new folder name.");return false;}
    if(!StudioFieldSequence::Validate(Request,Error))return false;
    Work=MakeShared<FStudioFieldSequenceWork,ESPMode::ThreadSafe>();Work->Total=Request.LastOrdinal-Request.FirstOrdinal+1;
    Pending=Async(EAsyncExecution::ThreadPool,[Request=MoveTemp(Request),Parent,Name,State=Work
#if WITH_DEV_AUTOMATION_TESTS
        ,BeforePublish=MoveTemp(BeforePublishForAutomation)
#endif
    ]() mutable
    {
        FStudioFieldSequenceResult Out;Out.Path=Parent/Name;
        const FStudioFileAccess Access(Parent);FString Stage;
        ON_SCOPE_EXIT{if(!Stage.IsEmpty())IFileManager::Get().DeleteDirectory(*Stage,false,true);Request.Source.Reset();};
        auto Fail=[&]()
        {
            auto Expected=EStudioFieldExportState::Writing;
            if(!State->State.compare_exchange_strong(Expected,EStudioFieldExportState::Complete)&&Expected==EStudioFieldExportState::Cancelled)
            {Out.bCancelled=true;Out.Error=TEXT("Frame sequence export cancelled. Destination unchanged.");}
            Out.bSuccess=false;return Out;
        };
        if(State->Cancellation->load())return Fail();
        if(IFileManager::Get().FileExists(*Out.Path)||IFileManager::Get().DirectoryExists(*Out.Path))
        {Out.Error=TEXT("That destination already exists. Choose a new export folder name.");return Fail();}
        if(!StudioFileDialog::CreateExportStage(Parent,Stage,Out.Error))return Fail();
        Out=StudioFieldSequence::Write(Request,Stage,State->Cancellation,[State](int32 Frames,int64 Done,int64 Total)
            {FScopeLock Lock(&State->ProgressMutex);State->Frames=Frames;State->FrameTotal=Total;State->Completed=Done;});Out.Path=Parent/Name;
        if(!Out.bSuccess)return Fail();Out.bSuccess=false;
#if WITH_DEV_AUTOMATION_TESTS
        if(BeforePublish)BeforePublish();
#endif
        auto Expected=EStudioFieldExportState::Writing;
        if(!State->State.compare_exchange_strong(Expected,EStudioFieldExportState::Publishing))return Fail();
        Out.bSuccess=StudioFileDialog::PublishExportDirectory(Stage,Out.Path,Out.Error);
        if(Out.bSuccess)Stage.Empty();State->State.store(EStudioFieldExportState::Complete);return Out;
    });Error.Empty();return true;
}
bool FStudioFieldSequenceTask::Cancel()
{
    if(!Work)return false;auto Expected=EStudioFieldExportState::Writing;
    if(!Work->State.compare_exchange_strong(Expected,EStudioFieldExportState::Cancelled))return false;
    Work->Cancellation->store(true);return true;
}
void FStudioFieldSequenceTask::Shutdown()
{if(bShutdown)return;bShutdown=true;Cancel();if(Pending.IsValid()){Pending.Wait();Pending={};}Work.Reset();}
FStudioFieldSequenceProgress FStudioFieldSequenceTask::Progress() const
{
    if(!Work)return {};FStudioFieldSequenceProgress P;P.State=Work->State.load();P.TotalFrames=Work->Total;
    FScopeLock Lock(&Work->ProgressMutex);P.CompletedFrames=Work->Frames;P.FrameTotal=Work->FrameTotal;P.FrameCompleted=Work->Completed;return P;
}
TOptional<FStudioFieldSequenceResult> FStudioFieldSequenceTask::Poll()
{if(!Pending.IsValid()||!Pending.IsReady())return {};return Pending.Consume();}
