#include "StudioHome4ArchiveWatch.h"
#include "StudioFileDialog.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
FStudioHome4ArchiveWatch::~FStudioHome4ArchiveWatch(){Stop();}
bool FStudioHome4ArchiveWatch::Start(const FString& Path,FString& Error,const FString& Pattern)
{
    if(Pending.IsValid()){Error=TEXT("Wait for the previous directory scan to finish.");return false;}
    if(FPaths::IsRelative(Path)||!IFileManager::Get().DirectoryExists(*Path))
    {Error=TEXT("Choose an existing absolute directory containing completed original NPZ files.");return false;}
    if(Pattern.IsEmpty()||Pattern.Len()>128||!Pattern.EndsWith(TEXT(".npz"))||Pattern.Contains(TEXT("/"))||Pattern.Contains(TEXT("\\"))||Pattern.Contains(TEXT(":")))
    {Error=TEXT("Watch pattern must be a plain NPZ filename pattern, for example *_viz*.npz.");return false;}
    Glob=Pattern;Stop();Folder=FPaths::ConvertRelativePathToFull(Path);Seen.Reset();Completed.Reset();Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);bActive=true;NextPoll=0;Message=TEXT("Watching completed NPZ snapshots; partial files are held until verified.");Error.Reset();return true;
}
void FStudioHome4ArchiveWatch::Stop(){bActive=false;if(Cancel)Cancel->store(true);}
TArray<FStudioHome4ArchiveInspection> FStudioHome4ArchiveWatch::TakeCompleted()
{auto Result=MoveTemp(Completed);Completed.Reset();return Result;}
void FStudioHome4ArchiveWatch::Tick(double Now)
{
    if(Pending.IsValid())
    {
        if(!Pending.IsReady())return;auto Result=Pending.Get();Pending={};
        if(bActive&&Cancel&&!Cancel->load()){Seen=MoveTemp(Result.Entries);Completed.Append(MoveTemp(Result.Complete));if(!Result.Error.IsEmpty())Message=Result.Error;else Message=FString::Printf(TEXT("Watching %s · %d candidates checked"),*Folder,Seen.Num());}
    }
    if(!bActive||Now<NextPoll)return;NextPoll=Now+1.;
    Pending=Async(EAsyncExecution::ThreadPool,[Path=Folder,Pattern=Glob,Previous=Seen,Now,C=Cancel]()mutable
    {
        FScan Result;FStudioFileAccess Access(Path);TArray<FString> Names;
        // Accepted evidence survives a writer's temporary unlink/truncate. An
        // empty intermediate file must not erase the identity already delivered.
        for(const auto& Entry:Previous)if(Entry.Value.bDelivered)Result.Entries.Add(Entry.Key,Entry.Value);
        if(Result.Entries.Num()>=4096){Result.Error=TEXT("Watch retained 4096 original identities; start another run watch to continue.");return Result;}
        IFileManager::Get().FindFiles(Names,*(Path/Pattern),true,false);Names.Sort();
        if(Names.Num()>4096){Result.Error=TEXT("Watch directory exceeds 4096 NPZ files; select a smaller run directory.");return Result;}
        int32 Inspected=0;
        for(const auto& Name:Names)
        {
            if(C->load())break;if(Name.StartsWith(TEXT("."))||Name.Contains(TEXT(".partial"))||Name.Contains(TEXT(".tmp")))continue;
            const FString Full=Path/Name;const auto Stat=IFileManager::Get().GetStatData(*Full);
            if(!Result.Entries.Contains(Full)&&Result.Entries.Num()>=4096)
            {Result.Error=TEXT("Watch exceeds 4096 retained original identities; select a new run directory.");break;}
            if(const auto* Accepted=Previous.Find(Full);Accepted&&Accepted->bDelivered)
            {
                auto Retained=*Accepted;
                Retained.bMutated|=!Stat.bIsValid||Stat.bIsDirectory||Stat.FileSize!=Accepted->Bytes||Stat.ModificationTime!=Accepted->Modified;
                if(Retained.bMutated)Result.Error=TEXT("Previously accepted snapshot changed; retained recording remains pinned: ")+Name;
                Result.Entries.Add(Full,MoveTemp(Retained));continue;
            }
            if(!Stat.bIsValid||Stat.bIsDirectory||Stat.FileSize<=0||Stat.FileSize>StudioHome4Archives::MaximumArchiveBytes)continue;
            FSeen Current;Current.Bytes=Stat.FileSize;Current.Modified=Stat.ModificationTime;Current.Since=Now;
            if(const auto* Old=Previous.Find(Full);Old&&Old->Bytes==Current.Bytes&&Old->Modified==Current.Modified)
            {
                Current=*Old;
                if(!Current.bDelivered&&Now-Current.Since>=1.&&Inspected++<8)
                {
                    auto Inspection=StudioHome4Archives::Inspect(Full,C);const auto After=IFileManager::Get().GetStatData(*Full);
                    if(Inspection.Error.IsEmpty()&&!Inspection.bCancelled&&After.bIsValid&&After.FileSize==Current.Bytes&&After.ModificationTime==Current.Modified)
                    {Current.SHA=Inspection.Source.SHA256;Current.bDelivered=true;Result.Complete.Add(MoveTemp(Inspection));}
                    else Result.Error=TEXT("Holding incomplete or invalid snapshot: ")+Name+TEXT(" · ")+Inspection.Error;
                }
            }
            Result.Entries.Add(Full,MoveTemp(Current));
        }
        return Result;
    });
}
