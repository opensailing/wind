#include "StudioLogExport.h"
#include "StudioFileDialog.h"
#include "Async/Async.h"

namespace
{
FString LogCSVQuote(FString Value){Value.ReplaceInline(TEXT("\""),TEXT("\"\""));return TEXT("\"")+Value+TEXT("\"");}
bool ValidLogExport(const TArray<FStudioLogEntry>& Entries)
{
    if(Entries.IsEmpty()||Entries.Num()>FStudioLogJournal::Capacity)return false;
    uint64 Previous=0;
    for(const auto& Entry:Entries)
    {
        if(Entry.Sequence<=Previous||Entry.Message.Len()>FStudioLogJournal::MessageLimit||
            Entry.SourceReference.Len()>FStudioLogJournal::ReferenceLimit||
            uint8(Entry.Severity)>uint8(EStudioLogSeverity::Error)||uint8(Entry.Source)>uint8(EStudioLogSource::ControlHarness))return false;
        Previous=Entry.Sequence;
    }
    return true;
}
}
bool StudioLogExport::CSV(const TArray<FStudioLogEntry>& Entries,FString& Out,FString& Error)
{
    if(!ValidLogExport(Entries))
    {Error=Entries.IsEmpty()?TEXT("No visible entries to export."):TEXT("The log snapshot exceeds its limits or has invalid observation order.");return false;}
    FString Text=TEXT("observation_sequence,observed_at_utc,severity,source,project_id,run_id,source_reference,truncated,message\r\n");
    for(const auto& Entry:Entries)
    {
        Text+=FString::Printf(TEXT("%llu,"),static_cast<unsigned long long>(Entry.Sequence))+
            LogCSVQuote(Entry.ObservedUTC.ToIso8601())+TEXT(",")+LogCSVQuote(StudioLog::SeverityName(Entry.Severity))+TEXT(",")+
            LogCSVQuote(StudioLog::SourceName(Entry.Source))+TEXT(",")+LogCSVQuote(Entry.ProjectId.IsValid()?Entry.ProjectId.ToString():FString())+TEXT(",")+
            LogCSVQuote(Entry.RunId.IsValid()?Entry.RunId.ToString():FString())+TEXT(",")+LogCSVQuote(Entry.SourceReference)+TEXT(",")+
            (Entry.bTruncated?TEXT("true,"):TEXT("false,"))+LogCSVQuote(Entry.Message)+TEXT("\r\n");
    }
    Out=MoveTemp(Text);Error.Empty();return true;
}
FStudioLogExportTask::~FStudioLogExportTask(){if(Pending.IsValid())Pending.Wait();}
bool FStudioLogExportTask::Start(TArray<FStudioLogEntry> Entries,const FString& Path)
{
    if(Pending.IsValid()||Path.IsEmpty()||!ValidLogExport(Entries))return false;
    Pending=Async(EAsyncExecution::ThreadPool,[Entries=MoveTemp(Entries),Path]
    {
        FStudioLogExportResult Result;Result.Path=Path;Result.Entries=Entries.Num();FString CSV;
        if(StudioLogExport::CSV(Entries,CSV,Result.Error))Result.bSuccess=StudioFileDialog::WriteAtomic(Path,CSV,Result.Error);
        return Result;
    });return true;
}
TOptional<FStudioLogExportResult> FStudioLogExportTask::Poll()
{
    if(!Pending.IsValid()||!Pending.IsReady())return {};
    auto Result=Pending.Get();Pending=TFuture<FStudioLogExportResult>();return Result;
}
