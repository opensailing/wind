#include "StudioLog.h"

const TCHAR* StudioLog::SeverityName(EStudioLogSeverity Severity)
{
    switch(Severity)
    {
    case EStudioLogSeverity::Info:return TEXT("Info");
    case EStudioLogSeverity::Warning:return TEXT("Warning");
    case EStudioLogSeverity::Error:return TEXT("Error");
    }
    return TEXT("Unknown");
}
const TCHAR* StudioLog::SourceName(EStudioLogSource Source)
{
    switch(Source)
    {
    case EStudioLogSource::Application:return TEXT("Application");
    case EStudioLogSource::Playback:return TEXT("Playback");
    case EStudioLogSource::ControlHarness:return TEXT("Control harness");
    }
    return TEXT("Unknown");
}
bool StudioLog::Matches(const FStudioLogEntry& Entry,const FStudioLogQuery& Query)
{
    if(Entry.Severity<Query.MinimumSeverity||(Query.Source&&Entry.Source!=*Query.Source)||
        (Query.ProjectId.IsValid()&&Entry.ProjectId!=Query.ProjectId)||
        (Query.RunId.IsValid()&&Entry.RunId!=Query.RunId))return false;
    const FString Search=Query.Search.Left(256).TrimStartAndEnd();
    return Search.IsEmpty()||Entry.Message.Contains(Search)||Entry.SourceReference.Contains(Search)||
        FString(SourceName(Entry.Source)).Contains(Search)||FString(SeverityName(Entry.Severity)).Contains(Search)||
        Entry.ObservedUTC.ToIso8601().Contains(Search)||Entry.ProjectId.ToString().Contains(Search)||
        (Entry.RunId.IsValid()&&Entry.RunId.ToString().Contains(Search));
}
FString StudioLog::Line(const FStudioLogEntry& Entry)
{
    return Entry.ObservedUTC.ToString(TEXT("%H:%M:%S"))+TEXT(" UTC  ")+SeverityName(Entry.Severity)+
        TEXT(" · ")+SourceName(Entry.Source)+TEXT("\n")+Entry.Message+(Entry.bTruncated?TEXT(" [truncated]"):TEXT(""));
}
void FStudioLogJournal::Append(const FString& Message,EStudioLogSeverity Severity,EStudioLogSource Source,
    const FGuid& ProjectId,const FGuid& RunId,const FString& SourceReference,FDateTime ObservedUTC)
{
    if(Message.IsEmpty())return;
    FStudioLogEntry Entry;Entry.Sequence=++Sequence;Entry.ObservedUTC=ObservedUTC;
    Entry.Severity=Severity;Entry.Source=Source;Entry.ProjectId=ProjectId;Entry.RunId=RunId;
    Entry.SourceReference=SourceReference.Left(ReferenceLimit);Entry.Message=Message.Left(MessageLimit);
    Entry.bTruncated=Message.Len()>MessageLimit||SourceReference.Len()>ReferenceLimit;
    if(Entries.Num()<Capacity)Entries.Add(MoveTemp(Entry));
    else {Entries[First]=MoveTemp(Entry);First=(First+1)%Capacity;}
}
TArray<FStudioLogEntry> FStudioLogJournal::Snapshot() const
{
    TArray<FStudioLogEntry> Out;Out.Reserve(Entries.Num());
    for(int32 I=0;I<Entries.Num();++I)Out.Add(Entries[(First+I)%Entries.Num()]);
    return Out;
}
void FStudioLogView::Refresh(const FStudioLogJournal& Journal)
{
    if(!bFollowing||CapturedThrough==Journal.LastSequence())return;
    Captured=Journal.Snapshot();CapturedThrough=Journal.LastSequence();
}
void FStudioLogView::SetFollowing(bool bValue,const FStudioLogJournal& Journal)
{
    // Capture current entries before pausing, including events since last paint.
    if(bFollowing)Refresh(Journal);
    bFollowing=bValue;if(bFollowing)Refresh(Journal);
}
TArray<FStudioLogEntry> FStudioLogView::Select(const FStudioLogQuery& Query) const
{
    TArray<FStudioLogEntry> Out;
    for(const auto& Entry:Captured)
        if(Entry.Sequence>HiddenThrough&&StudioLog::Matches(Entry,Query))Out.Add(Entry);
    return Out;
}
