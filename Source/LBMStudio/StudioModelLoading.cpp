#include "StudioModel.h"
#include "Async/Async.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

namespace
{
// Camera gestures, display edits and replay are intentionally available while
// opening. All durable authoring changes invalidate an earlier replacement.
FString ReplacementContent(FStudioProject Project)
{
    Project.Camera=FStudioCameraState();
    Project.View=FStudioViewSettings();
    Project.SelectedFrame=0;
    return StudioProjectIO::Serialize(Project);
}
}

bool FStudioModel::RequestProjectOpen(const FString& Path,const FString& ReplacedRecentPath,bool bRecovery,
    const FString& RecordingReplacement,const FString& ReconstructionReplacement,bool bRemoveReconstruction)
{
    if(!CanReplaceProject())return false;
    if(IsProjectOpenPending())
    { Notice=TEXT("A project is already opening. Cancel it and wait for its read to finish before opening another."); return false; }
    if(IsRecordingLoadPending())
    { Notice=TEXT("Wait for the recording change to finish, or cancel it before opening a project."); return false; }
    if(Path.IsEmpty()) { Notice=TEXT("Choose a project file to open."); return false; }
    if(bRemoveReconstruction&&!ReconstructionReplacement.IsEmpty())
    { Notice=TEXT("Choose either a reconstruction replacement or original points."); return false; }
    RecordingRepair.Reset();

    OpeningProjectPath=FPaths::ConvertRelativePathToFull(Path);
    OpeningPreviousPath=ProjectPath;
    OpeningContent=ReplacementContent(SnapshotProject());
    OpeningReplacedRecent=ReplacedRecentPath;
    bOpeningRecovery=bRecovery;
    bOpeningLegacy=false;
    ProjectLoadCancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    ProjectLoadStage=MakeShared<std::atomic<EStudioProjectLoadStage>,ESPMode::ThreadSafe>(EStudioProjectLoadStage::Document);
    // One worker owns its candidate, loader and archives. No model, Slate or
    // scene pointer crosses the boundary, including during shutdown.
    PendingProjectOpen=Async(EAsyncExecution::ThreadPool,
        [Path=OpeningProjectPath,Cancel=ProjectLoadCancellation,Stage=ProjectLoadStage,
            RecordingReplacement,ReconstructionReplacement,bRemoveReconstruction]
    {
        FStudioProjectLoadResult Result;
        if(Cancel->load()) return Result;
        const auto Before=IFileManager::Get().GetTimeStamp(*Path);
        const auto Size=IFileManager::Get().FileSize(*Path);
        if(!StudioProjectIO::Load(Path,Result.Project,Result.Error)||Cancel->load()) return Result;
        if(Before!=IFileManager::Get().GetTimeStamp(*Path)||Size!=IFileManager::Get().FileSize(*Path))
        { Result.Error=TEXT("The project file changed while it was read. Open it again."); return Result; }
        const bool bRepairSurface=bRemoveReconstruction||!ReconstructionReplacement.IsEmpty();
        if(!RecordingReplacement.IsEmpty()||bRepairSurface)
        {
            auto* Ref=Result.Project.Recordings.FindByPredicate([&](const auto& R){return R.Id==Result.Project.Dataset;});
            if(!Ref)
            {Result.Error=TEXT("Only a saved external recording can be repaired. Current project kept.");return Result;}
            if(bRepairSurface&&!Ref->Reconstruction.IsSet())
            {Result.Error=TEXT("The project no longer has the reconstruction selected for repair. Open it again.");return Result;}
            // Keep repaired locations on the candidate even if another member
            // fails. A source-then-surface retry must retain the verified source
            // location while all hashes and the live document stay unchanged.
            if(!RecordingReplacement.IsEmpty())Ref->Path=FPaths::ConvertRelativePathToFull(RecordingReplacement);
            if(bRemoveReconstruction)
            {Ref->Reconstruction.Reset();Result.bRemovedReconstruction=true;}
            else if(!ReconstructionReplacement.IsEmpty())
                Ref->Reconstruction->Path=FPaths::ConvertRelativePathToFull(ReconstructionReplacement);
            Result.bRelocated=true;
            Result.ResolutionNote=bRemoveReconstruction?
                TEXT("Opened original points without the display reconstruction. Save the project to keep this choice."):
                !ReconstructionReplacement.IsEmpty()?
                TEXT("Opened with verified source and reconstruction locations. Save the project to keep these paths."):
                TEXT("Opened with a verified recording location. Save the project to keep the new path.");
        }
        Stage->store(EStudioProjectLoadStage::Recording);
        auto Recording=StudioRecordings::Open(Result.Project.Dataset,Result.Project.Recordings,
            Result.Project.SelectedFrame,Cancel);
        if(Cancel->load()) return Result;
        if(!Recording.Source)
        {
            Result.Error=Recording.Error;
            Result.bRecordingFailed=Result.Project.Recordings.ContainsByPredicate([&](const auto& R){return R.Id==Result.Project.Dataset;});
            Result.bReconstructionFailed=Recording.bReconstructionFailed;
            return Result;
        }
        if(Recording.Reference.IsSet())
            for(auto& R:Result.Project.Recordings) if(R.Id==Result.Project.Dataset) R=*Recording.Reference;
        Stage->store(EStudioProjectLoadStage::Frame);
        Result.Source=MoveTemp(Recording.Source);
        Stage->store(EStudioProjectLoadStage::Ready);
        return Result;
    });
    Notice=TEXT("Opening project. You can continue using the current view or cancel opening.");
    return true;
}

bool FStudioModel::RequestRecoveryOpen()
{
    if(PendingRecovery.IsEmpty()) { Notice=TEXT("No recovery project is available."); return false; }
    return RequestProjectOpen(PendingRecovery,FString(),true);
}

bool FStudioModel::RetryRecordingRepair(const FString& ReplacementPath)
{
    if(!RecordingRepair.IsSet()||ReplacementPath.IsEmpty())return false;
    const auto Repair=*RecordingRepair;
    return Repair.bReconstruction?
        RequestProjectOpen(Repair.ProjectPath,Repair.ReplacedRecentPath,Repair.bRecovery,Repair.SourcePath,ReplacementPath):
        RequestProjectOpen(Repair.ProjectPath,Repair.ReplacedRecentPath,Repair.bRecovery,ReplacementPath,
            Repair.ReconstructionPath,Repair.bRemoveReconstruction);
}

bool FStudioModel::RetryWithoutReconstruction()
{
    if(!RecordingRepair.IsSet()||!RecordingRepair->bReconstruction)return false;
    const auto Repair=*RecordingRepair;
    return RequestProjectOpen(Repair.ProjectPath,Repair.ReplacedRecentPath,Repair.bRecovery,
        Repair.SourcePath,FString(),true);
}

void FStudioModel::CancelProjectOpen(bool bNotify)
{
    if(!IsProjectOpenPending()) return;
    if(ProjectLoadCancellation) ProjectLoadCancellation->store(true);
    if(bNotify) Notice=TEXT("Opening cancelled. The current project has been kept.");
}

FString FStudioModel::ProjectOpenStatus() const
{
    if(!IsProjectOpenPending()) return {};
    if(!IsProjectOpening()) return TEXT("Cancelling project read…");
    FString Stage=TEXT("Reading project");
    switch(ProjectLoadStage->load())
    {
    case EStudioProjectLoadStage::Recording: Stage=TEXT("Preparing recording"); break;
    case EStudioProjectLoadStage::Frame: Stage=TEXT("Checking saved frame"); break;
    case EStudioProjectLoadStage::Ready: Stage=TEXT("Opening project"); break;
    default: break;
    }
    return Stage+TEXT(": ")+FPaths::GetCleanFilename(OpeningProjectPath)+TEXT(" · Current view remains available");
}

void FStudioModel::ApplyLoadedProject(FStudioProject Candidate,TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> Source,const FString& Path,bool bRecovery)
{
    RecordingRepair.Reset();
    CancelProjectOpen(false);
    CancelRecording(); UseRecording(MoveTemp(Source));
    const FString Original=Candidate.RecoverySource;
    Candidate.RecoverySource.Empty();
    Project=MoveTemp(Candidate);
    ResetJobSession();
    ClearCaseHistory(); ClearViewHistory();
    static_cast<FStudioViewSettings&>(*this)=Project.View;
    SelectedFrame=PlaybackFrame=Project.SelectedFrame;
    State=EStudioRunState::Paused; bReviewing=true; Accumulator=WallSeconds=0; DisplayChanged();
    if(bRecovery)
    {
        ProjectPath=Original;
        Project.AssetBaseDirectory=Original.IsEmpty()?FString():FPaths::GetPath(Original);
        SavedSnapshot.Empty(); bDirty=true; PendingRecovery.Empty();
        Notice=TEXT("Recovered unsaved project. Save to keep these changes.");
    }
    else
    {
        ProjectPath=FPaths::ConvertRelativePathToFull(Path);
        SavedSnapshot=StudioProjectIO::Serialize(SnapshotProject()); bDirty=false;
        RememberProject(); Notice=TEXT("Opened ")+Project.Name;
    }
    AddLog(Notice);
}

void FStudioModel::PollProjectOpen()
{
    if(!PendingProjectOpen.IsValid()||!PendingProjectOpen.IsReady()) return;
    auto Result=PendingProjectOpen.Get(); PendingProjectOpen=TFuture<FStudioProjectLoadResult>();
    const bool Cancelled=ProjectLoadCancellation->load();
    ProjectLoadCancellation.Reset(); ProjectLoadStage.Reset();
    if(Cancelled) return;
    if(ProjectPath!=OpeningPreviousPath||ReplacementContent(SnapshotProject())!=OpeningContent)
    { Notice=TEXT("The current project changed while opening. Nothing was replaced; open the file again when ready."); return; }
    if(bOpeningRecovery&&(PendingRecovery.IsEmpty()||!FPaths::IsSamePath(PendingRecovery,OpeningProjectPath)))
    { Notice=TEXT("The recovery selection changed while opening. Current project kept."); return; }
    if(!Result.Error.IsEmpty()||!Result.Source)
    {
        Notice=Result.Error.IsEmpty()?TEXT("Project opening failed. Current project kept."):Result.Error; AddLog(Notice);
        if(Result.bRecordingFailed)
        {
            const auto* Ref=Result.Project.Recordings.FindByPredicate([&](const auto& R){return R.Id==Result.Project.Dataset;});
            if(Ref)
            {
                FStudioRecordingRepair Repair;
                Repair.ProjectPath=OpeningProjectPath;Repair.SourcePath=Ref->Path;Repair.Title=Ref->Title;
                Repair.ReplacedRecentPath=OpeningReplacedRecent;Repair.bRecovery=bOpeningRecovery;
                Repair.bReconstruction=Result.bReconstructionFailed&&Ref->Reconstruction.IsSet();
                Repair.ReconstructionPath=Ref->Reconstruction.IsSet()?Ref->Reconstruction->Path:FString();
                Repair.bRemoveReconstruction=Result.bRemovedReconstruction;
                Repair.RecordingPath=Repair.bReconstruction?Repair.ReconstructionPath:Repair.SourcePath;
                RecordingRepair=MoveTemp(Repair);
            }
        }
        return;
    }
    const FString ReplacedRecent=OpeningReplacedRecent;
    ApplyLoadedProject(MoveTemp(Result.Project),MoveTemp(Result.Source),OpeningProjectPath,bOpeningRecovery);
    if(Result.bRelocated)
    {
        SavedSnapshot.Empty();bDirty=true;
        Notice=Result.ResolutionNote;AddLog(Notice);
    }
    if(bOpeningLegacy)
    {
        // Legacy preferences become an unsaved document. Never leave a normal
        // Save pointed at the old preferences JSON on this or the next launch.
        RecentProjects.Remove(OpeningProjectPath); ProjectPath.Empty(); SavedSnapshot.Empty(); bDirty=true;
        SaveSession(); RefreshProjectCatalog();
        Notice=TEXT("Previous view imported. Save As creates a new project document.");
    }
    // Route only after a verified candidate exists. Scene applies the loaded
    // camera revision on its next update, including when leaving Geometry.
    Workspace=EStudioWorkspace::Solve;
    if(!ReplacedRecent.IsEmpty()&&!FPaths::IsSamePath(ReplacedRecent,ProjectPath)) ForgetRecentProject(ReplacedRecent);
    OpeningContent.Empty(); OpeningReplacedRecent.Empty();
}
