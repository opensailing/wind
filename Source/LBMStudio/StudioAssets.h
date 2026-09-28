#pragma once
#include "CoreMinimal.h"
#include <atomic>

struct FStudioProject;
struct FStudioCaseDraft;

enum class EStudioAssetState : uint8 { Unchecked, Verified, Missing, Changed, Unreadable, Cancelled };
struct FStudioAssetReference
{
    FString Name;
    FString Path;
    FString SHA256;
    bool bDraft = false;
    int32 RunCount = 0;
    EStudioAssetState State = EStudioAssetState::Unchecked;
    FString Detail;
    FString Key() const { return Path+TEXT("\n")+SHA256.ToLower(); }
};
using FStudioAssetCancellation = TSharedRef<std::atomic<bool>,ESPMode::ThreadSafe>;
struct FStudioAssetCheckResult
{
    TArray<FStudioAssetReference> References;
    bool bCancelled = false;
};
namespace StudioAssets
{
    TArray<FStudioAssetReference> References(const FStudioProject& Project);
    /** Bounded streaming SHA-256, intended for a worker. Never loads the whole file. */
    bool HashFile(const FString& Path, const FStudioAssetCancellation& Cancel, FString& Hash, FString& Error);
    FStudioAssetCheckResult Check(TArray<FStudioAssetReference> References, const FStudioAssetCancellation& Cancel);
    /** Verified relocation changes paths only. Caller supplies a matching content hash. */
    void Relocate(FStudioCaseDraft& Draft, const FStudioAssetReference& Source, const FString& Destination);
    bool Relocate(FStudioProject& Project, const FStudioAssetReference& Source, const FString& Destination, FString& Error);
    FString StateText(EStudioAssetState State);
}
