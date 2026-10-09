#include "StudioMonitorExport.h"
#include "StudioFileDialog.h"
#include "StudioProbeMonitor.h"
#include "Async/Async.h"

namespace
{
FString MonitorCSVQuote(FString Text){Text.ReplaceInline(TEXT("\""),TEXT("\"\""));return TEXT("\"")+Text+TEXT("\"");}
}
bool StudioMonitorExport::CSV(const FStudioHistory& H,const FStudioMonitorSettings& S,FString& Out,int32& Rows,FString& Error)
{
    if(H.ProbeHistory)return StudioProbeMonitor::CSV(H,S,Out,Rows,Error);
    if(!StudioMonitor::ValidateSource(S,H,Error))return false;
    if(S.Series.IsEmpty()){Error=TEXT("Select at least one series before exporting.");return false;}
    FString Text=TEXT("# history_id,")+MonitorCSVQuote(H.Id)+TEXT("\n# title,")+MonitorCSVQuote(H.Title)+
        TEXT("\n# source_url,")+MonitorCSVQuote(H.SourceURL)+TEXT("\n# metadata_sha256,")+H.MetadataSHA256+
        TEXT("\n# payload_sha256,")+H.PayloadSHA256+TEXT("\n# original_source_sha256,")+H.SourceSHA256+
        TEXT("\n# field_recording_id,")+MonitorCSVQuote(H.FieldRecordingId.Get(FString()))+
        TEXT("\n# time_note,")+MonitorCSVQuote(H.TimeNote)+TEXT("\n# selection,")+
        (H.bResiduals?TEXT("first initial and last final per field and source time; each value names its original log line; display scale does not change values\n"):
            TEXT("original rows within saved time window; display scale and reduction do not change exported values\n"));
    if(H.bResiduals)
        Text+=TEXT("# source_path,")+MonitorCSVQuote(H.SourcePath)+TEXT("\n# source_line_numbering,one-based in the hash-verified original log\n");
    for(const auto& Id:S.Series)
    {
        const auto& C=*H.FindColumn(Id);Text+=TEXT("# column,")+MonitorCSVQuote(Id)+TEXT(",")+MonitorCSVQuote(C.Label)+TEXT(",")+
            MonitorCSVQuote(C.Unit)+TEXT(",")+MonitorCSVQuote(C.Origin)+TEXT(",")+MonitorCSVQuote(C.Expression)+TEXT("\n");
    }
    TArray<FString> Keys;H.ReferenceValues.GetKeys(Keys);Keys.Sort();
    for(const auto& Key:Keys)Text+=TEXT("# reference,")+MonitorCSVQuote(Key)+FString::Printf(TEXT(",%.17g\n"),H.ReferenceValues[Key]);
    Text+=TEXT("source_sample,")+MonitorCSVQuote(TEXT("solver_time (")+H.TimeUnit+TEXT(")"));
    if(H.bResiduals)Text+=TEXT(",time_source_line");
    for(const auto& Id:S.Series)
    {Text+=TEXT(",")+MonitorCSVQuote(Id);if(H.bResiduals)Text+=TEXT(",")+MonitorCSVQuote(Id+TEXT(".source_line"));}
    Text+=TEXT("\n");
    int32 Count=0;
    for(int32 I=0;I<H.Times.Num();++I)
    {
        if(S.bManualTime&&(H.Times[I]<S.TimeMinimum||H.Times[I]>S.TimeMaximum))continue;
        Text+=FString::Printf(TEXT("%d,%.17g"),I,H.Times[I]);
        if(H.bResiduals)Text+=FString::Printf(TEXT(",%d"),H.TimeSourceLines[I]);
        for(const auto& Id:S.Series)
        {
            const auto* C=H.FindColumn(Id);Text+=FString::Printf(TEXT(",%.17g"),C->Values[I]);
            if(H.bResiduals)Text+=FString::Printf(TEXT(",%d"),C->SourceLines[I]);
        }
        Text+=TEXT("\n");++Count;
        if(Text.Len()>32*1024*1024){Error=TEXT("This CSV exceeds the 32 Mi-character export limit. Narrow the time window or series selection.");return false;}
    }
    if(!Count){Error=TEXT("No original samples in the selected time window.");return false;}
    Out=MoveTemp(Text);Rows=Count;Error.Empty();return true;
}
FStudioMonitorExportTask::~FStudioMonitorExportTask(){if(Pending.IsValid())Pending.Wait();}
bool FStudioMonitorExportTask::Start(TSharedPtr<const FStudioHistory,ESPMode::ThreadSafe> H,FStudioMonitorSettings S,const FString& Path)
{
    if(Pending.IsValid()||!H||Path.IsEmpty())return false;
    Pending=Async(EAsyncExecution::ThreadPool,[H,S,Path]
    {
        FStudioMonitorExportResult R;R.Path=Path;R.Source=H->Title;FString CSV;
        if(StudioMonitorExport::CSV(*H,S,CSV,R.Samples,R.Error))R.bSuccess=StudioFileDialog::WriteAtomic(Path,CSV,R.Error);
        return R;
    });return true;
}
TOptional<FStudioMonitorExportResult> FStudioMonitorExportTask::Poll()
{
    if(!Pending.IsValid()||!Pending.IsReady())return {};
    auto R=Pending.Get();Pending=TFuture<FStudioMonitorExportResult>();return R;
}
