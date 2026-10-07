#pragma once
#include "StudioHome4Archive.h"

namespace StudioHome4ArchivePrivate
{
bool Cancelled(const FStudioLoadCancellation& Cancel);
bool CleanText(const FString& Text,int32 Limit,bool bMultiline=false);
bool JSON(const FString& Text,TSharedPtr<FJsonObject>& Out,FString& Error);
FString Serialize(const TSharedRef<FJsonObject>& Object);
bool ReserveManifestBytes(const FString& SnapshotJSON,int64& UsedBytes,int64 MaximumBytes=8LL*1024*1024);
bool Hash(const FString& Path,FString& Out,const FStudioLoadCancellation& Cancel,FString& Error);
bool DecodeUTF8(const uint8* Bytes,int32 Count,FString& Out);
struct FNumericArray
{
    FStudioHome4ArchiveMember Member;
    TArray<double> Values;
    FString StringValue;
};
class FNpzReader
{
public:
    FNpzReader();
    ~FNpzReader();
    FNpzReader(const FNpzReader&)=delete;
    bool Open(const FString& Path,const FStudioLoadCancellation& Cancel,FString& Error);
    const TArray<FStudioHome4ArchiveMember>& Members() const;
    bool Read(const FString& Name,FNumericArray& Out,FString& Error,bool bNumeric=true);
    bool Scan(FString& Error);
    /** Stream verified literal headers and payload CRCs without materialising numeric arrays. */
    bool ScanHeadersAndCRC(FString& Error);
private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};
}
