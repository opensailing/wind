#include "StudioModel.h"
#include "StudioResiduals.h"
#include "Async/Async.h"
#include "Misc/Paths.h"

namespace
{
FString ResidualSourceKey(const FStudioResidualSettings& S)
{return S.Path+TEXT("\n")+S.Chart.HistoryId+TEXT("\n")+S.Chart.MetadataSHA256;}
}

void FStudioModel::InvalidateResidualSession()
{
    if(ResidualCancellation)*ResidualCancellation=true;
    LoadedResidual.Reset();ResidualProject.Invalidate();ResidualKey.Empty();ResidualNotice.Empty();++ResidualRevision;
}
TSharedPtr<const FStudioHistory,ESPMode::ThreadSafe> FStudioModel::ResidualHistory() const
{
    const auto& S=Project.Residual;
    if(ResidualProject!=Project.Id||ResidualKey!=ResidualSourceKey(S)||!LoadedResidual||!LoadedResidual->bResiduals||
        LoadedResidual->Id!=S.Chart.HistoryId||!LoadedResidual->MetadataSHA256.Equals(S.Chart.MetadataSHA256,ESearchCase::IgnoreCase))return {};
    return LoadedResidual;
}
bool FStudioModel::StartResidualLog(const FStudioResidualSettings& S,bool bChoose,bool bNewSource)
{
    if(PendingResidual.IsValid())return false;
    ReadingResidualProject=Project.Id;ReadingResidualSettings=S;bChooseResidual=bChoose;bNewResidual=bNewSource;
    ResidualCancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    const auto Cancel=ResidualCancellation;const FString Path=S.Path,Hash=bNewSource?FString():S.Chart.MetadataSHA256;
    PendingResidual=Async(EAsyncExecution::ThreadPool,[Path,Hash,Cancel]{return StudioResiduals::Load(Path,Cancel,Hash);});
    ResidualNotice=TEXT("Verifying completed residual log…");++ResidualRevision;return true;
}
bool FStudioModel::RequestResidualLog(const FString& Path,bool bLocate)
{
    if(IsProjectOpenPending()||PendingResidual.IsValid())
    {ResidualNotice=TEXT("Wait for loading to finish, or cancel the residual request.");return false;}
    if(bLocate&&Project.Residual.Path.IsEmpty())
    {ResidualNotice=TEXT("Import a residual log before locating a saved source.");return false;}
    FString Full=Path;
    if(Full.IsEmpty()||Full.Len()>4096||FPaths::IsRelative(Full)||Full.Contains(TEXT("://")))
    {ResidualNotice=TEXT("Choose a local residual log with an absolute path.");return false;}
    for(TCHAR C:Full)if(C<32||C==127){ResidualNotice=TEXT("Residual log path contains unsupported characters.");return false;}
    FPaths::NormalizeFilename(Full);
    if(!FPaths::CollapseRelativeDirectories(Full))
    {ResidualNotice=TEXT("Residual log path cannot be resolved.");return false;}
    if(ResidualProject!=Project.Id||ResidualKey!=ResidualSourceKey(Project.Residual))
    {LoadedResidual.Reset();ResidualProject=Project.Id;ResidualKey=ResidualSourceKey(Project.Residual);}
    FStudioResidualSettings Candidate=bLocate?Project.Residual:FStudioResidualSettings();Candidate.Path=Full;
    return StartResidualLog(Candidate,true,!bLocate);
}
void FStudioModel::CancelResidualLog()
{
    if(ResidualCancellation)*ResidualCancellation=true;
    ResidualNotice=TEXT("Residual loading cancelled. The previous selection is retained.");++ResidualRevision;
}
void FStudioModel::ClearResidualLog()
{
    if(IsProjectOpenPending()){ResidualNotice=TEXT("Wait for the project to finish opening.");return;}
    CancelResidualLog();LoadedResidual.Reset();Project.Residual=FStudioResidualSettings();
    ResidualProject=Project.Id;ResidualKey=ResidualSourceKey(Project.Residual);bDirty=true;
    ResidualNotice=TEXT("Residual history removed from this project. The original log is unchanged.");++ResidualRevision;
}
bool FStudioModel::UpdateResidualSettings(const FStudioMonitorSettings& Settings)
{
    const auto H=ResidualHistory();FString Error;
    if(!H||IsProjectOpenPending()||PendingResidual.IsValid())
    {ResidualNotice=TEXT("Wait for a verified residual history before changing its chart.");return false;}
    if(!StudioMonitor::ValidateSource(Settings,*H,Error)){ResidualNotice=Error;return false;}
    Project.Residual.Chart=Settings;bDirty=true;ResidualNotice.Empty();++ResidualRevision;return true;
}
void FStudioModel::PollResidual()
{
    if(ResidualProject!=Project.Id||ResidualKey!=ResidualSourceKey(Project.Residual))
    {
        if(ResidualCancellation)*ResidualCancellation=true;
        LoadedResidual.Reset();ResidualProject=Project.Id;ResidualKey=ResidualSourceKey(Project.Residual);
        ResidualNotice.Empty();++ResidualRevision;
    }
    if(PendingResidual.IsValid())
    {
        if(!PendingResidual.IsReady())return;
        const auto Result=PendingResidual.Get();PendingResidual=TFuture<FStudioHistoryLoadResult>();
        const bool bCancelled=ResidualCancellation->load(std::memory_order_relaxed);ResidualCancellation.Reset();
        if(!bCancelled&&ReadingResidualProject==Project.Id)
        {
            if(Result.History)
            {
                auto Settings=ReadingResidualSettings;
                if(bNewResidual)Settings.Chart=StudioMonitor::Defaults(*Result.History);
                FString Error;
                if(StudioResidualSettings::Validate(Settings,Error)&&StudioMonitor::ValidateSource(Settings.Chart,*Result.History,Error))
                {
                    LoadedResidual=Result.History;
                    if(bChooseResidual){Project.Residual=Settings;bDirty=true;}
                    ResidualKey=ResidualSourceKey(Project.Residual);
                    ResidualNotice=TEXT("Verified residual history loaded. Force history, flow recording and active run are unchanged.");
                }
                else ResidualNotice=Error;
            }
            else ResidualNotice=Result.Error;
            ++ResidualRevision;
        }
    }
    // Failed/cancelled requests stay visible until explicit import/locate/retry.
    if(!LoadedResidual&&!PendingResidual.IsValid()&&!Project.Residual.Path.IsEmpty()&&ResidualNotice.IsEmpty())
        StartResidualLog(Project.Residual,false,false);
}
