#pragma once
#include "StudioHome4Archive.h"
/** Watches completed original NPZ files. A stable stat is only a candidate;
 * NPY/ZIP CRCs, source SHA and a second stat verify publication before delivery. */
class FStudioHome4ArchiveWatch
{
public:
    ~FStudioHome4ArchiveWatch();
    bool Start(const FString& Directory,FString& Error,const FString& Pattern=TEXT("*.npz"));
    void Stop();
    bool Active()const{return bActive;}
    const FString& Directory()const{return Folder;}
    const FString& Notice()const{return Message;}
    void Tick(double Now);
    TArray<FStudioHome4ArchiveInspection> TakeCompleted();
private:
    struct FSeen {int64 Bytes=-1;FDateTime Modified;double Since=0;FString SHA;bool bDelivered=false,bMutated=false;};
    struct FScan {TMap<FString,FSeen> Entries;TArray<FStudioHome4ArchiveInspection> Complete;FString Error;};
    FString Folder,Message,Glob;
    bool bActive=false;double NextPoll=0;
    TMap<FString,FSeen> Seen;
    TFuture<FScan> Pending;
    FStudioLoadCancellation Cancel;
    TArray<FStudioHome4ArchiveInspection> Completed;
};
