#include "StudioFieldExportTask.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioFieldExportLifecycle,"Studio.VTKExport.AtomicPublishingAndLifecycle",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioFieldExportLifecycle::RunTest(const FString&)
{
    const FString Directory=FPaths::ProjectSavedDir()/TEXT("Automation")/(TEXT("field-export-")+FGuid::NewGuid().ToString());
    IFileManager::Get().MakeDirectory(*Directory,true);
    ON_SCOPE_EXIT{IFileManager::Get().DeleteDirectory(*Directory,false,true);};
    const FString Path=Directory/TEXT("existing.vtp"),Sentinel=TEXT("Existing destination must survive");
    if(!TestTrue(TEXT("Create existing destination"),FFileHelper::SaveStringToFile(Sentinel,*Path)))return false;
    auto Unchanged=[&]{FString Text;return FFileHelper::LoadFileToString(Text,*Path)&&Text==Sentinel;};
    auto StagingFiles=[]
    {
        TArray<FString> Files;IFileManager::Get().FindFiles(Files,*(FPaths::ProjectSavedDir()/TEXT("ExportStaging/*.partial")),true,false);
        Files.Sort();return Files;
    };
    const auto Before=StagingFiles();
    auto Await=[](FStudioFieldExportTask& Task)
    {
        const double Deadline=FPlatformTime::Seconds()+5.;
        while(FPlatformTime::Seconds()<Deadline)
        {auto Result=Task.Poll();if(Result.IsSet())return Result;FPlatformProcess::SleepNoStats(.001f);}
        return TOptional<FStudioFieldExportResult>();
    };
    struct FPublishGate
    {
        FEvent* Reached=FPlatformProcess::GetSynchEventFromPool(true);
        FEvent* Release=FPlatformProcess::GetSynchEventFromPool(true);
        ~FPublishGate(){FPlatformProcess::ReturnSynchEventToPool(Reached);FPlatformProcess::ReturnSynchEventToPool(Release);}
    };
    FRecordedSolver Source;auto Read=Source.ReadScalarFrame(420,TEXT("pressure"));
    if(!TestTrue(*Read.Error,Read.Field.IsValid()))return false;
    FStudioFieldExportRequest Request{Read.Field,{TEXT("pressure")}};
    FStudioFieldExportTask Task;auto Gate=MakeShared<FPublishGate,ESPMode::ThreadSafe>();
    Task.BeforePublishForAutomation=[Gate]{Gate->Reached->Trigger();Gate->Release->Wait(5000);};
    TestTrue(TEXT("Start export"),Task.Start(Request,Path));
    const bool Reached=Gate->Reached->Wait(5000);TestTrue(TEXT("Worker reaches publication boundary"),Reached);
    const auto Progress=Task.Progress();
    TestTrue(TEXT("Completed encoding still permits cancellation"),Progress.State==EStudioFieldExportState::Writing&&Progress.Total>0&&Progress.Completed==Progress.Total);
    TestFalse(TEXT("No queued second export"),Task.Start(Request,Directory/TEXT("second.vtp")));
    TestTrue(TEXT("Cancellation wins before replacement"),Task.Cancel());
    TestFalse(TEXT("Cancellation is accepted once"),Task.Cancel());
    Gate->Release->Trigger();auto Result=Await(Task);
    TestTrue(TEXT("Cancelled result never claims success"),Result&&Result->bCancelled&&!Result->bSuccess&&Result->Path==Path);
    TestTrue(TEXT("Cancelled destination remains byte-for-byte intact"),Unchanged());
    TestTrue(TEXT("Cancellation removes its staging file"),StagingFiles()==Before);
    TestFalse(TEXT("Rejected request created no file"),IFileManager::Get().FileExists(*(Directory/TEXT("second.vtp"))));
    if(Task.IsBusy())return false;

    auto Invalid=Request;Invalid.Scalars={TEXT("unavailable")};
    TestTrue(TEXT("Validation runs on worker"),Task.Start(MoveTemp(Invalid),Path));Result=Await(Task);
    TestTrue(TEXT("Validation failure preserves destination"),Result&&!Result->bSuccess&&!Result->bCancelled&&!Result->Error.IsEmpty()&&Unchanged());
    if(Task.IsBusy())return false;
    TestTrue(TEXT("Start export to impossible child of a file"),Task.Start(Request,Path/TEXT("blocked.vtp")));Result=Await(Task);
    TestTrue(TEXT("Native write error preserves existing file"),Result&&!Result->bSuccess&&!Result->bCancelled&&!Result->Error.IsEmpty()&&Unchanged());
    TestTrue(TEXT("Errors remove their staging files"),StagingFiles()==Before);
    if(Task.IsBusy())return false;

    Gate=MakeShared<FPublishGate,ESPMode::ThreadSafe>();
    Task.BeforePublishForAutomation=[Gate]{Gate->Reached->Trigger();Gate->Release->Wait(5000);};
    TestTrue(TEXT("Retry replacing destination"),Task.Start(Request,Path));
    TestTrue(TEXT("Second worker reaches publication boundary"),Gate->Reached->Wait(5000));
    Source.PrepareFrame(17,{}); // A later source read must not change the frozen export.
    Gate->Release->Trigger();Result=Await(Task);
    TestTrue(TEXT("Atomic replacement keeps frozen original identity"),Result&&Result->bSuccess&&!Result->bCancelled&&Result->Identity.Ordinal==420);
    TestFalse(TEXT("Published file cannot report cancellation"),Task.Cancel());
    FString XML;FFileHelper::LoadFileToString(XML,*Path);
    TestTrue(TEXT("Replacement is a complete VTK document"),XML.StartsWith(TEXT("<?xml"))&&XML.EndsWith(TEXT("</VTKFile>\n")));
    TestTrue(TEXT("Success removes its staging file"),StagingFiles()==Before);
    if(Task.IsBusy())return false;
    TestTrue(TEXT("Create new destination"),Task.Start(Request,Directory/TEXT("new.vtp")));Result=Await(Task);
    TestTrue(TEXT("New file published"),Result&&Result->bSuccess&&IFileManager::Get().FileExists(*(Directory/TEXT("new.vtp"))));

    FString Error;auto Pinned=Read.Field->LoadScalarSnapshot(TEXT("pressure"),{},Error);
    TWeakPtr<const IStudioField,ESPMode::ThreadSafe> Weak=Pinned;
    FStudioFieldExportTask Closing;
    TestTrue(TEXT("Start an owned snapshot"),Closing.Start({MoveTemp(Pinned),{TEXT("pressure")}},Directory/TEXT("closing.vtp")));
    Closing.Shutdown();Closing.Shutdown();
    TestTrue(TEXT("Shutdown joins and releases pinned snapshot"),!Closing.IsBusy()&&!Weak.IsValid()&&!Closing.Poll());
    TestFalse(TEXT("Shutdown task cannot restart"),Closing.Start(Request,Path));
    TestTrue(TEXT("Shutdown leaves no staging file"),StagingFiles()==Before);
    return true;
}
#endif
