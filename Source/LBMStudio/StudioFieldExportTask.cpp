#include "StudioFieldExportTask.h"
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
bool FStudioFieldExportTask::Start(FStudioVTKExportRequest Request,const FString& Path)
{
    if(bShutdown||Pending.IsValid()||!Request.Field||Path.IsEmpty())return false;
    Work=MakeShared<FStudioFieldExportWork,ESPMode::ThreadSafe>();
    Pending=Async(EAsyncExecution::ThreadPool,[Request=MoveTemp(Request),Path,State=Work
#if WITH_DEV_AUTOMATION_TESTS
        ,BeforePublish=MoveTemp(BeforePublishForAutomation)
#endif
    ]() mutable
    {
        const FString Directory=FPaths::ProjectSavedDir()/TEXT("ExportStaging");
        const FString Staged=Directory/(FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT(".vtp.partial"));
        FStudioVTKExportResult Result;Result.Path=Path;
        ON_SCOPE_EXIT{IFileManager::Get().Delete(*Staged,false,true);Request.Field.Reset();};
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
        Result=StudioVTKExport::Write(Request,*Archive,State->Cancellation,[State](int64 Done,int64 Total)
            {State->Total.store(Total);State->Completed.store(Done);});Result.Path=Path;
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
TOptional<FStudioVTKExportResult> FStudioFieldExportTask::Poll()
{if(!Pending.IsValid()||!Pending.IsReady())return {};return Pending.Consume();}
