#pragma once
#include "CoreMinimal.h"

enum class EStudioLogSeverity : uint8 { Info, Warning, Error };
enum class EStudioLogSource : uint8 { Application, Playback, ControlHarness };

/** An application observation, never a solver timestamp or scientific sample. */
struct FStudioLogEntry
{
    uint64 Sequence=0;
    FDateTime ObservedUTC;
    EStudioLogSeverity Severity=EStudioLogSeverity::Info;
    EStudioLogSource Source=EStudioLogSource::Application;
    FGuid ProjectId,RunId;
    FString SourceReference,Message;
    bool bTruncated=false;
};

struct FStudioLogQuery
{
    EStudioLogSeverity MinimumSeverity=EStudioLogSeverity::Info;
    TOptional<EStudioLogSource> Source;
    FGuid ProjectId,RunId; // Invalid means all retained contexts.
    FString Search; // Case-insensitive literal match; bounded to 256 characters.
};

namespace StudioLog
{
    const TCHAR* SeverityName(EStudioLogSeverity Severity);
    const TCHAR* SourceName(EStudioLogSource Source);
    bool Matches(const FStudioLogEntry& Entry,const FStudioLogQuery& Query);
    FString Line(const FStudioLogEntry& Entry);
}

/** Owner-thread ring. No destructive clear: a view may hide earlier sequences.
 * Sequence order stays monotonic even if the system UTC clock moves backward. */
class FStudioLogJournal
{
public:
    static constexpr int32 Capacity=2048,MessageLimit=2048,ReferenceLimit=256;
    void Append(const FString& Message,EStudioLogSeverity Severity,EStudioLogSource Source,
        const FGuid& ProjectId={},const FGuid& RunId={},const FString& SourceReference={},
        FDateTime ObservedUTC=FDateTime::UtcNow());
    TArray<FStudioLogEntry> Snapshot() const;
    uint64 LastSequence() const { return Sequence; }
    const FStudioLogEntry* Latest() const { return Entries.IsEmpty()?nullptr:&Entries[(First+Entries.Num()-1)%Entries.Num()]; }
    const FStudioLogEntry* Find(uint64 Id) const {return Entries.FindByPredicate([Id](const auto& Entry){return Entry.Sequence==Id;});}
    uint64 EvictedCount() const { return Sequence-uint64(Entries.Num()); }
    int32 Num() const { return Entries.Num(); }
private:
    TArray<FStudioLogEntry> Entries;
    int32 First=0;
    uint64 Sequence=0;
};

/** A bounded view snapshot. Pause freezes entries, not the producer. Filtering
 * paused entries cannot expose newly arrived events. Clear hides only the
 * captured sequences; ShowRetained restores whatever the ring still contains. */
class FStudioLogView
{
public:
    void Refresh(const FStudioLogJournal& Journal);
    void SetFollowing(bool bValue,const FStudioLogJournal& Journal);
    bool IsFollowing() const { return bFollowing; }
    void ClearView() { HiddenThrough=CapturedThrough; }
    void ShowRetained() { HiddenThrough=0; }
    uint64 HiddenSequence() const { return HiddenThrough; }
    uint64 CapturedSequence() const { return CapturedThrough; }
    uint64 PendingCount(const FStudioLogJournal& Journal) const { return Journal.LastSequence()-CapturedThrough; }
    TArray<FStudioLogEntry> Select(const FStudioLogQuery& Query) const;
private:
    TArray<FStudioLogEntry> Captured;
    uint64 CapturedThrough=0,HiddenThrough=0;
    bool bFollowing=true;
};
