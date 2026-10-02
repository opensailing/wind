#pragma once
#include "StudioLog.h"

enum class EStudioNotificationKind : uint8 { Warning, Error, Completed };

/** A local application observation. It carries no solver/sample timestamp.
 * The journal retains contexts from earlier projects without opening them. */
struct FStudioNotification
{
    uint64 Sequence=0,LogSequence=0;
    FDateTime ObservedUTC;
    EStudioNotificationKind Kind=EStudioNotificationKind::Warning;
    EStudioLogSource Source=EStudioLogSource::Application;
    FGuid ProjectId,RunId;
    FString Message,SourceReference;
    bool bRead=false,bTruncated=false;
};

namespace StudioNotifications
{
    FString Title(const FStudioNotification& Entry);
}

/** Owner-thread bounded session history. Reading never clears history; bulk
 * acknowledgement ends at a captured sequence so new arrivals stay unread. */
class FStudioNotificationJournal
{
public:
    static constexpr int32 Capacity=256;
    bool Observe(const FStudioLogEntry& Entry,bool bCompleted=false);
    TArray<FStudioNotification> Snapshot() const;
    const FStudioNotification* Find(uint64 Id) const;
    bool SetRead(uint64 Id,bool bRead);
    void MarkReadThrough(uint64 Id);
    int32 UnreadCount() const;
    uint64 LastSequence() const {return Sequence;}
    uint64 Revision() const {return ChangeRevision;}
    uint64 EvictedCount() const {return Sequence-uint64(Entries.Num());}
private:
    TArray<FStudioNotification> Entries;
    int32 First=0;
    uint64 Sequence=0,ChangeRevision=0,LastLogSequence=0;
};
