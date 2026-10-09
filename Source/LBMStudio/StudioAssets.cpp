#include "StudioAssets.h"
#include "StudioProject.h"
#include "StudioFileDialog.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
// Same isolation used by Unreal's BuildPatchServices: OpenSSL UI typedef
// otherwise collides with the engine's UObject metadata namespace.
#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#undef UI

TArray<FStudioAssetReference> StudioAssets::References(const FStudioProject& Project)
{
    TArray<FStudioAssetReference> Result; TMap<FString,int32> Indices;
    auto Add=[&](const FStudioCaseDraft& Draft,bool bDraft)
    {
        TSet<FString> Seen;
        for(const auto& Asset:Draft.Geometry)
        {
            FStudioAssetReference Ref; Ref.Name=Asset.Name; Ref.Path=Asset.SourcePath; Ref.SHA256=Asset.SourceSHA256;
            const FString Key=Ref.Key();
            int32* Index=Indices.Find(Key);
            const int32 I=Index?*Index:Result.Add(Ref);
            if(!Index) Indices.Add(Key,I);
            Result[I].bDraft|=bDraft;
            if(!bDraft&&!Seen.Contains(Key)) ++Result[I].RunCount;
            Seen.Add(Key);
        }
    };
    Add(Project.Draft,true);
    for(const auto& Run:Project.Runs) if(Run.GetConfiguration()) Add(*Run.GetConfiguration(),false);
    return Result;
}

bool StudioAssets::HashFile(const FString& Path,const FStudioAssetCancellation& Cancel,FString& Hash,FString& Error)
{
    Hash.Empty(); Error.Empty();
    if(Cancel->load(std::memory_order_relaxed)) {Error=TEXT("File check cancelled.");return false;}
    FStudioFileAccess Access(Path);
    const auto Before=IFileManager::Get().GetTimeStamp(*Path);
    TUniquePtr<FArchive> File(IFileManager::Get().CreateFileReader(*Path,FILEREAD_Silent));
    if(!File) {Error=TEXT("Cannot read this file. Locate an accessible copy.");return false;}
    const int64 Size=File->TotalSize();
    if(Size<0) {Error=TEXT("File size is unavailable.");return false;}
    struct FContext { EVP_MD_CTX* Value=EVP_MD_CTX_new(); ~FContext(){EVP_MD_CTX_free(Value);} } Context;
    if(!Context.Value||EVP_DigestInit_ex(Context.Value,EVP_sha256(),nullptr)!=1)
    {Error=TEXT("Could not start the file checksum.");return false;}
    TArray<uint8> Buffer; Buffer.SetNumUninitialized(64*1024);
    for(int64 Offset=0;Offset<Size;)
    {
        if(Cancel->load(std::memory_order_relaxed)) {Error=TEXT("File check cancelled.");return false;}
        const int64 Count=FMath::Min<int64>(Size-Offset,Buffer.Num()); File->Serialize(Buffer.GetData(),Count);
        if(File->IsError()||EVP_DigestUpdate(Context.Value,Buffer.GetData(),Count)!=1)
        {Error=TEXT("File could not be read completely. Check access and try again.");return false;}
        Offset+=Count;
    }
    if(Cancel->load(std::memory_order_relaxed)) {Error=TEXT("File check cancelled.");return false;}
    if(Size!=IFileManager::Get().FileSize(*Path)||Before!=IFileManager::Get().GetTimeStamp(*Path))
    {Error=TEXT("The file changed during verification. Try again after writing finishes.");return false;}
    uint8 Digest[EVP_MAX_MD_SIZE]; unsigned int Count=0;
    if(EVP_DigestFinal_ex(Context.Value,Digest,&Count)!=1||Count!=32)
    {Error=TEXT("Could not finish the file checksum.");return false;}
    Hash=BytesToHex(Digest,Count).ToLower(); return true;
}

FStudioAssetCheckResult StudioAssets::Check(TArray<FStudioAssetReference> References,const FStudioAssetCancellation& Cancel)
{
    FStudioAssetCheckResult Result;
    for(auto& Ref:References)
    {
        if(Cancel->load(std::memory_order_relaxed)) {Result.bCancelled=true;break;}
        FStudioFileAccess Access(Ref.Path);
        if(!IFileManager::Get().FileExists(*Ref.Path))
        {Ref.State=EStudioAssetState::Missing;Ref.Detail=TEXT("File missing or inaccessible. Locate a copy to verify it.");continue;}
        FString Hash;
        if(!HashFile(Ref.Path,Cancel,Hash,Ref.Detail)) Ref.State=EStudioAssetState::Unreadable;
        else if(!Hash.Equals(Ref.SHA256,ESearchCase::IgnoreCase))
        {Ref.State=EStudioAssetState::Changed;Ref.Detail=TEXT("Contents differ from the saved geometry. Locate the original file; changed geometry must be imported separately.");}
        else {Ref.State=EStudioAssetState::Verified;Ref.Detail=TEXT("SHA-256 matches the saved source. Last check only; files can change later.");}
    }
    Result.bCancelled|=Cancel->load(std::memory_order_relaxed); Result.References=MoveTemp(References); return Result;
}

void StudioAssets::Relocate(FStudioCaseDraft& Draft,const FStudioAssetReference& Source,const FString& Destination)
{
    for(auto& Asset:Draft.Geometry)
        if(Asset.SourcePath==Source.Path&&Asset.SourceSHA256.Equals(Source.SHA256,ESearchCase::IgnoreCase)) Asset.SourcePath=Destination;
}
bool StudioAssets::Relocate(FStudioProject& Project,const FStudioAssetReference& Source,const FString& Destination,FString& Error)
{
    auto Candidate=Project; Relocate(Candidate.Draft,Source,Destination);
    for(auto& Run:Candidate.Runs)
    {
        if(!Run.GetConfiguration()) continue;
        auto Config=*Run.GetConfiguration(); Relocate(Config,Source,Destination);
        auto JSON=Run.ToJSON(); JSON->SetObjectField(TEXT("configuration"),StudioCaseIO::ToJSON(Config));
        FStudioRunRecord Replacement;
        if(!FStudioRunRecord::FromJSON(JSON,Replacement,Error)) return false;
        Run=MoveTemp(Replacement);
    }
    Project=MoveTemp(Candidate); return true;
}
FString StudioAssets::StateText(EStudioAssetState State)
{
    switch(State)
    {
    case EStudioAssetState::Verified:return TEXT("Verified");
    case EStudioAssetState::Missing:return TEXT("Missing / inaccessible");
    case EStudioAssetState::Changed:return TEXT("Content changed");
    case EStudioAssetState::Unreadable:return TEXT("Could not verify");
    case EStudioAssetState::Cancelled:return TEXT("Check cancelled");
    default:return TEXT("Not checked");
    }
}
