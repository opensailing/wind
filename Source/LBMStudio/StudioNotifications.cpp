#include "StudioNotifications.h"

FString StudioNotifications::Title(const FStudioNotification& Entry)
{
    const FString Origin=Entry.Source==EStudioLogSource::ControlHarness?TEXT("Control harness"):
        Entry.Source==EStudioLogSource::Playback?TEXT("Recorded playback"):TEXT("Application");
    return Origin+(Entry.Kind==EStudioNotificationKind::Completed?TEXT(" completed"):
        Entry.Kind==EStudioNotificationKind::Error?TEXT(" error"):TEXT(" warning"));
}
bool FStudioNotificationJournal::Observe(const FStudioLogEntry& Entry,bool bCompleted)
{
    if(Entry.Sequence==0||Entry.Sequence<=LastLogSequence)return false;
    LastLogSequence=Entry.Sequence;
    if(Entry.Message.IsEmpty()||(Entry.Severity==EStudioLogSeverity::Info&&!bCompleted))return false;
    FStudioNotification Item;Item.Sequence=++Sequence;Item.LogSequence=Entry.Sequence;
    Item.ObservedUTC=Entry.ObservedUTC;Item.Source=Entry.Source;Item.ProjectId=Entry.ProjectId;Item.RunId=Entry.RunId;
    Item.Kind=Entry.Severity==EStudioLogSeverity::Error?EStudioNotificationKind::Error:
        Entry.Severity==EStudioLogSeverity::Warning?EStudioNotificationKind::Warning:EStudioNotificationKind::Completed;
    Item.Message=Entry.Message.Left(FStudioLogJournal::MessageLimit);
    Item.SourceReference=Entry.SourceReference.Left(FStudioLogJournal::ReferenceLimit);
    Item.bTruncated=Entry.bTruncated||Item.Message.Len()!=Entry.Message.Len()||Item.SourceReference.Len()!=Entry.SourceReference.Len();
    if(Entries.Num()<Capacity)Entries.Add(MoveTemp(Item));
    else {Entries[First]=MoveTemp(Item);First=(First+1)%Capacity;}
    ++ChangeRevision;return true;
}
TArray<FStudioNotification> FStudioNotificationJournal::Snapshot() const
{
    TArray<FStudioNotification> Result;Result.Reserve(Entries.Num());
    for(int32 I=0;I<Entries.Num();++I)Result.Add(Entries[(First+I)%Entries.Num()]);
    return Result;
}
const FStudioNotification* FStudioNotificationJournal::Find(uint64 Id) const
{return Entries.FindByPredicate([Id](const auto& Entry){return Entry.Sequence==Id;});}
bool FStudioNotificationJournal::SetRead(uint64 Id,bool bRead)
{
    auto* Item=Entries.FindByPredicate([Id](const auto& Entry){return Entry.Sequence==Id;});
    if(!Item)return false;
    if(Item->bRead!=bRead){Item->bRead=bRead;++ChangeRevision;}
    return true;
}
void FStudioNotificationJournal::MarkReadThrough(uint64 Id)
{
    bool Changed=false;
    for(auto& Item:Entries)if(Item.Sequence<=Id&&!Item.bRead){Item.bRead=true;Changed=true;}
    if(Changed)++ChangeRevision;
}
int32 FStudioNotificationJournal::UnreadCount() const
{int32 Count=0;for(const auto& Entry:Entries)if(!Entry.bRead)++Count;return Count;}
