#include "StudioFieldExportTask.h"
#include "StudioCSVExport.h"
#include "StudioPipelineExport.h"
#include "StudioFileDialog.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

struct FStudioFieldExportWork
{
    std::atomic<EStudioFieldExportState> State{EStudioFieldExportState::Writing};
    std::atomic<int64> Completed{0},Total{0};
    FStudioLoadCancellation Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
};
FStudioFieldExportTask::~FStudioFieldExportTask(){Shutdown();}
bool FStudioFieldExportTask::Start(FStudioFieldExportRequest Request,const FString& Path)
{
    if(bShutdown||Pending.IsValid()||!Request.Field||Path.IsEmpty()||
        (Request.Format!=EStudioFieldExportFormat::VTK&&Request.Format!=EStudioFieldExportFormat::CSV))return false;
    return StartWriter([Request=MoveTemp(Request)](FArchive& Archive,const FStudioLoadCancellation& Cancellation,TFunction<void(int64,int64)> Progress)
    {return Request.Format==EStudioFieldExportFormat::CSV?StudioCSVExport::Write(Request,Archive,Cancellation,MoveTemp(Progress)):
        StudioVTKExport::Write(Request,Archive,Cancellation,MoveTemp(Progress));},Path);
}
bool FStudioFieldExportTask::Start(FStudioPipelineExportRequest Request,const FString& Path)
{
    if(bShutdown||Pending.IsValid()||!Request.Evaluation.Output||Path.IsEmpty()||
        (Request.Format!=EStudioFieldExportFormat::VTK&&Request.Format!=EStudioFieldExportFormat::CSV))return false;
    return StartWriter([Request=MoveTemp(Request)](FArchive& Archive,const FStudioLoadCancellation& Cancellation,TFunction<void(int64,int64)> Progress)
    {return StudioPipelineExport::Write(Request,Archive,Cancellation,MoveTemp(Progress));},Path);
}
bool FStudioFieldExportTask::StartWriter(FWriter Writer,const FString& Path)
{
    Work=MakeShared<FStudioFieldExportWork,ESPMode::ThreadSafe>();
    Pending=Async(EAsyncExecution::ThreadPool,[Writer=MoveTemp(Writer),Path,State=Work
#if WITH_DEV_AUTOMATION_TESTS
        ,BeforePublish=MoveTemp(BeforePublishForAutomation)
#endif
    ]() mutable
    {
        const FString Directory=FPaths::ProjectSavedDir()/TEXT("ExportStaging");
        const FString Staged=Directory/(FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT(".field.partial"));
        FStudioFieldExportResult Result;Result.Path=Path;
        ON_SCOPE_EXIT{IFileManager::Get().Delete(*Staged,false,true);Writer=nullptr;};
        auto FinishFailure=[&]()
        {
            auto Expected=EStudioFieldExportState::Writing;
            if(!State->State.compare_exchange_strong(Expected,EStudioFieldExportState::Complete)&&Expected==EStudioFieldExportState::Cancelled)
            {Result.bCancelled=true;Result.bSuccess=false;Result.Error=TEXT("Field export cancelled. Destination unchanged.");}
            return Result;
        };
        if(State->Cancellation->load())return FinishFailure();
        if(!IFileManager::Get().MakeDirectory(*Directory,true))
        {Result.Error=TEXT("Could not create the field-export staging directory. Check application storage access.");return FinishFailure();}
        TUniquePtr<FArchive> Archive(IFileManager::Get().CreateFileWriter(*Staged,FILEWRITE_NoReplaceExisting));
        if(!Archive){Result.Error=TEXT("Could not open the field-export staging file. Check free space and application storage access.");return FinishFailure();}
        auto Progress=[State](int64 Done,int64 Total){State->Total.store(Total);State->Completed.store(Done);};
        Result=Writer(*Archive,State->Cancellation,Progress);Result.Path=Path;
        const bool Closed=Archive->Close()&&!Archive->IsError();Archive.Reset();
        if(!Closed){Result.bSuccess=false;Result.Error=TEXT("Could not close the staged field export. Check free space.");}
        if(!Result.bSuccess)return FinishFailure();
        Result.bSuccess=false;
#if WITH_DEV_AUTOMATION_TESTS
        if(BeforePublish)BeforePublish();
#endif
        auto Expected=EStudioFieldExportState::Writing;
        if(!State->State.compare_exchange_strong(Expected,EStudioFieldExportState::Publishing))return FinishFailure();
        const FStudioFileAccess Access(Path);
        Result.bSuccess=StudioFileDialog::WriteAtomicFile(Path,Staged,Result.Error);
        State->State.store(EStudioFieldExportState::Complete);return Result;
    });return true;
}
bool FStudioFieldExportTask::Cancel()
{
    if(!Work)return false;auto Expected=EStudioFieldExportState::Writing;
    if(!Work->State.compare_exchange_strong(Expected,EStudioFieldExportState::Cancelled))return false;
    Work->Cancellation->store(true);return true;
}
void FStudioFieldExportTask::Shutdown()
{if(bShutdown)return;bShutdown=true;Cancel();if(Pending.IsValid()){Pending.Wait();Pending={};}Work.Reset();}
FStudioFieldExportProgress FStudioFieldExportTask::Progress() const
{
    if(!Work)return {};
    FStudioFieldExportProgress Out;Out.State=Work->State.load();Out.Total=Work->Total.load();Out.Completed=FMath::Min(Work->Completed.load(),Out.Total);return Out;
}
TOptional<FStudioFieldExportResult> FStudioFieldExportTask::Poll()
{if(!Pending.IsValid()||!Pending.IsReady())return {};return Pending.Consume();}
