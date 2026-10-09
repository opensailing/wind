#include "StudioModel.h"
#include "Async/Async.h"

void FStudioModel::InvalidateMonitorSession()
{
    if(MonitorCancellation)*MonitorCancellation=true;
    LoadedMonitor.Reset();MonitorProject.Invalidate();MonitorNotice.Empty();++MonitorRevision;
}

TSharedPtr<const FStudioHistory,ESPMode::ThreadSafe> FStudioModel::MonitorHistory() const
{
    if(MonitorProject!=Project.Id||!LoadedMonitor||LoadedMonitor->Id!=Project.Monitor.HistoryId||
        !LoadedMonitor->MetadataSHA256.Equals(Project.Monitor.MetadataSHA256,ESearchCase::IgnoreCase))return {};
    return LoadedMonitor;
}
bool FStudioModel::StartMonitorHistory(const FStudioMonitorSettings& Settings,bool bChoose)
{
    if(PendingMonitor.IsValid())return false;
    const auto Catalog=StudioHistories::Installed();
    const auto* Entry=Catalog.FindByPredicate([&](const auto& E){return E.Id==Settings.HistoryId;});
    if(!Entry||!Entry->MetadataSHA256.Equals(Settings.MetadataSHA256,ESearchCase::IgnoreCase))
    {MonitorNotice=TEXT("The saved history is unavailable or its interpretation changed. Select a verified history.");++MonitorRevision;return false;}
    ReadingMonitorProject=Project.Id;ReadingMonitorSettings=Settings;bChooseMonitor=bChoose;
    MonitorCancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    const auto Cancel=MonitorCancellation;const FString Path=Entry->Path,Hash=Settings.MetadataSHA256;
    PendingMonitor=Async(EAsyncExecution::ThreadPool,[Path,Hash,Cancel]{return StudioHistories::Load(Path,Cancel,Hash);});
    MonitorNotice=TEXT("Verifying published history…");++MonitorRevision;return true;
}
bool FStudioModel::RequestMonitorHistory(const FString& Id)
{
    if(IsProjectOpenPending()||PendingMonitor.IsValid())
    {MonitorNotice=TEXT("Wait for loading to finish, or cancel the history request.");return false;}
    const auto Catalog=StudioHistories::Installed();const auto* E=Catalog.FindByPredicate([&](const auto& V){return V.Id==Id;});
    if(!E){MonitorNotice=TEXT("This history is not in the verified installed catalog.");return false;}
    FStudioMonitorSettings Settings;Settings.HistoryId=E->Id;Settings.MetadataSHA256=E->MetadataSHA256;
    // Synchronize ownership before dispatch so the next tick cannot cancel this request.
    if(MonitorProject!=Project.Id){LoadedMonitor.Reset();MonitorProject=Project.Id;}
    MonitorId=Project.Monitor.HistoryId;MonitorHash=Project.Monitor.MetadataSHA256;
    return StartMonitorHistory(Settings,true);
}
void FStudioModel::CancelMonitorHistory()
{
    if(MonitorCancellation)*MonitorCancellation=true;
    MonitorNotice=TEXT("History loading cancelled. The previous selection is retained.");++MonitorRevision;
}
void FStudioModel::ClearMonitorHistory()
{
    CancelMonitorHistory();LoadedMonitor.Reset();Project.Monitor=FStudioMonitorSettings();
    MonitorProject=Project.Id;MonitorId.Empty();MonitorHash.Empty();bDirty=true;
    MonitorNotice=TEXT("History removed from this project. Published source files are unchanged.");++MonitorRevision;
}
bool FStudioModel::UpdateMonitorSettings(const FStudioMonitorSettings& Settings)
{
    const auto H=MonitorHistory();FString Error;
    if(!H||IsProjectOpenPending()||PendingMonitor.IsValid())
    {MonitorNotice=TEXT("Wait for a verified history before changing its chart.");return false;}
    if(!StudioMonitor::ValidateSource(Settings,*H,Error)){MonitorNotice=Error;return false;}
    Project.Monitor=Settings;bDirty=true;MonitorNotice.Empty();++MonitorRevision;return true;
}
void FStudioModel::PollMonitor()
{
    if(MonitorProject!=Project.Id||MonitorId!=Project.Monitor.HistoryId||MonitorHash!=Project.Monitor.MetadataSHA256)
    {
        if(MonitorCancellation)*MonitorCancellation=true;
        LoadedMonitor.Reset();MonitorProject=Project.Id;MonitorId=Project.Monitor.HistoryId;MonitorHash=Project.Monitor.MetadataSHA256;
        MonitorNotice.Empty();++MonitorRevision;
    }
    if(PendingMonitor.IsValid())
    {
        if(!PendingMonitor.IsReady())return;
        auto Result=PendingMonitor.Get();PendingMonitor=TFuture<FStudioHistoryLoadResult>();
        const bool Cancelled=MonitorCancellation->load(std::memory_order_relaxed);MonitorCancellation.Reset();
        if(!Cancelled&&ReadingMonitorProject==Project.Id)
        {
            if(Result.History)
            {
                auto Settings=bChooseMonitor?StudioMonitor::Defaults(*Result.History):ReadingMonitorSettings;
                FString Error;
                if(StudioMonitor::ValidateSource(Settings,*Result.History,Error))
                {
                    LoadedMonitor=Result.History;
                    if(bChooseMonitor){Project.Monitor=Settings;bDirty=true;}
                    MonitorId=Project.Monitor.HistoryId;MonitorHash=Project.Monitor.MetadataSHA256;
                    MonitorNotice=TEXT("Verified published history loaded. Flow recording and active run are unchanged.");
                }
                else MonitorNotice=Error;
            }
            else MonitorNotice=Result.Error;
            ++MonitorRevision;
        }
    }
    // A failed or cancelled request is not retried every frame. Explicit selection
    // retries it; a new project/source clears the notice and schedules its own read.
    if(!LoadedMonitor&&!PendingMonitor.IsValid()&&!Project.Monitor.HistoryId.IsEmpty()&&MonitorNotice.IsEmpty())
        StartMonitorHistory(Project.Monitor,false);
}
