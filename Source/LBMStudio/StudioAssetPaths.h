#pragma once
#include "CoreMinimal.h"

struct FStudioCaseDraft;
struct FStudioProject;

/** Local asset references. No file I/O, existence requirement, or implicit cwd base. */
namespace StudioAssetPaths
{
    /** Transactionally resolve draft references against an explicit absolute directory. */
    bool Resolve(FStudioCaseDraft& Draft, const FString& Directory, FString& Error);
    /** Includes frozen run configurations; identity, hashes and authored values are retained. */
    bool Resolve(FStudioProject& Project, const FString& Directory, FString& Error);
    /** Make a storage copy relative to the destination; the live document stays resolved. */
    bool ForStorage(const FStudioProject& Project, const FString& Destination,
        FStudioProject& Out, FString& Error);
}
