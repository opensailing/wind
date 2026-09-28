#pragma once

#include "CoreMinimal.h"
#include "StudioRecording.h"

/** One column of source or explicitly derived scalar history. No absent values become zero. */
struct FStudioHistoryColumn
{
    FString Id, Label, Unit, Origin, Expression;
    TArray<double> Values;
};

/** Immutable after publication. A history may have no associated spatial recording. */
struct FStudioHistory
{
    FString Id, Title, SourceURL, SourceDOI, TimeNote;
    FString MetadataSHA256, PayloadSHA256, SourceSHA256;
    TOptional<FString> FieldRecordingId;
    TArray<double> Times;
    FString TimeUnit;
    TArray<FStudioHistoryColumn> Columns;
    TMap<FString,double> ReferenceValues;
    TArray<FString> Limitations;
    const FStudioHistoryColumn* FindColumn(const FString& ColumnId) const;
};

struct FStudioHistoryLoadResult
{
    TSharedPtr<const FStudioHistory,ESPMode::ThreadSafe> History;
    FString Error;
};

struct FStudioHistoryEntry
{
    FString Id, Title, Path, MetadataSHA256;
};

namespace StudioHistories
{
    /** Small installed catalog. Loading a history remains a separate worker operation. */
    TArray<FStudioHistoryEntry> Installed();
    /** Worker-only, bounded, cancellable load. CSV must match the descriptor hash.
     * ExpectedMetadataSHA256 pins a previously selected interpretation on reopen.
     * Version 1 accepts complete finite scalar samples; gaps need a future schema.
     */
    FStudioHistoryLoadResult Load(const FString& DescriptorPath,
        const FStudioLoadCancellation& Cancellation = {},
        const FString& ExpectedMetadataSHA256 = FString());
}
