#include "StudioModel.h"
#include "StudioAssetPaths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Async/Async.h"

FStudioModel::FStudioModel(const FString& SessionDirectory) : Solver(MakeShared<FRecordedSolver,ESPMode::ThreadSafe>())
{
    ResetJobSession();
    StorageDirectory=SessionDirectory.IsEmpty()?FPaths::ProjectSavedDir():SessionDirectory;
    for(int32 I=0;I<Solver->FrameCount();++I) Frames.Add(Solver->EvaluateFrame(I));
    if(Frames.IsEmpty()) Frames.Add(FStudioFrame());
    SavedSnapshot=StudioProjectIO::Serialize(SnapshotProject());
    Notice=Solver->LoadError();
    if(!Notice.IsEmpty()) AddLog(Notice);
    else
    {
        AddLog(FString::Printf(TEXT("Loaded %s: %d published snapshots."),*Solver->Descriptor().Title,Solver->FrameCount()));
        AddLog(TEXT("Published 2D fields; display extrusion adds no spanwise flow."));
        AddLog(FString::Printf(TEXT("Source time: %.4f–%.4f s. Playback: %.1f s at 20 snapshots/s."),Frames[0].Time,Frames.Last().Time,(Frames.Num()-1)*PlaybackInterval));
        AddLog(TEXT("Residual and force histories are not supplied."));
    }
}
void FStudioModel::AddLog(const FString& M)
{
    Log.Add(M); if(Log.Num()>120) Log.RemoveAt(0);
}
void FStudioModel::BeginRun()
{
    PlaybackFrame=0; SelectedFrame=0;
    WallSeconds=0; Accumulator=0; bReviewing=false; DisplayChanged();
}
void FStudioModel::Run()
{
    if(Solver->FrameCount()==0) { Notice=Solver->LoadError(); return; }
    if(State==EStudioRunState::Running) return;
    if(bReviewing) { PlaybackFrame=SelectedFrame; bReviewing=false; Accumulator=0; }
    else if(State!=EStudioRunState::Paused) BeginRun();
    State=EStudioRunState::Running; AddLog(TEXT("Recorded CFD playback started."));
}
void FStudioModel::Pause()
{
    if(State==EStudioRunState::Running) { State=EStudioRunState::Paused; DisplayChanged(); AddLog(TEXT("Playback paused; camera remains live.")); }
    else if(State==EStudioRunState::Paused) Run();
}
void FStudioModel::Stop()
{
    if(State==EStudioRunState::Ready||State==EStudioRunState::Stopped) return;
    State=EStudioRunState::Stopped; DisplayChanged(); AddLog(TEXT("Playback stopped. Run restarts at source frame 0."));
}
void FStudioModel::Advance()
{
    if(PlaybackFrame+1>=Solver->FrameCount())
    {
        if(bLoopPlayback && State==EStudioRunState::Running)
        { PlaybackFrame=0; AddLog(TEXT("Replay loop restarted at source frame 0.")); }
        else { State=EStudioRunState::Complete; return; }
    }
    else ++PlaybackFrame;
    if(!bReviewing) { SelectedFrame=PlaybackFrame; ++Revision; }
    if(PlaybackFrame%100==0) AddLog(FString::Printf(TEXT("Source frame %d · elapsed time %.4f s"),Frames[PlaybackFrame].Index,Frames[PlaybackFrame].Time));
    if(PlaybackFrame+1==Solver->FrameCount()&&!bLoopPlayback)
    { State=EStudioRunState::Complete; AddLog(FString::Printf(TEXT("All %d source snapshots replayed."),Solver->FrameCount())); }
}
void FStudioModel::Step()
{
    if(Solver->FrameCount()==0||State==EStudioRunState::Running||State==EStudioRunState::Complete) return;
    if(bReviewing) { PlaybackFrame=SelectedFrame; bReviewing=false; }
    else if(State==EStudioRunState::Ready||State==EStudioRunState::Stopped) BeginRun();
    Accumulator=0; State=EStudioRunState::Paused; ++RenderIntentRevision; Advance();
}
void FStudioModel::Tick(double Delta)
{
    if(!FMath::IsFinite(Delta)||Delta<0)return;
    TickJob(Delta);
    PollProjectOpen();
    PollProjectCatalog();
    PollRecording();
    PollAssets();
    PollGeometry();
    PollDomainGeometry();
    PollLatticePreview();
    PollMonitor();
    DirtyCheckSeconds+=Delta; AutosaveSeconds+=Delta;
    if(DirtyCheckSeconds>=.5) { bDirty=HasUnsavedChanges(); DirtyCheckSeconds=0; }
    if(AutosaveSeconds>=30) { WriteRecovery(); AutosaveSeconds=0; }
    if(State!=EStudioRunState::Running) return;
    WallSeconds+=Delta; Accumulator+=FMath::Min(Delta,0.25)*PlaybackRate;
    while(Accumulator+1e-9>=PlaybackInterval && State==EStudioRunState::Running) { Accumulator-=PlaybackInterval; Advance(); }
}
void FStudioModel::Scrub(double Fraction)
{ bReviewing=true; SelectedFrame=FMath::Clamp(FMath::RoundToInt(Fraction*(Frames.Num()-1)),0,Frames.Num()-1); DisplayChanged(); }
void FStudioModel::ReturnToLive() { bReviewing=false; SelectedFrame=PlaybackFrame; DisplayChanged(); }
void FStudioModel::Reset() { State=EStudioRunState::Ready; BeginRun(); }
const FStudioFrame& FStudioModel::DisplayFrame() const { return Frames[FMath::Clamp(SelectedFrame,0,Frames.Num()-1)]; }
FString FStudioModel::StatusText() const
{
    if(Solver->FrameCount()==0) return TEXT("Data unavailable");
    switch(State) { case EStudioRunState::Running:return TEXT("Playing"); case EStudioRunState::Paused:return TEXT("Paused"); case EStudioRunState::Stopped:return TEXT("Stopped"); case EStudioRunState::Complete:return TEXT("Complete"); default:return TEXT("Ready"); }
}
FStudioProject FStudioModel::SnapshotProject() const
{
    FStudioProject P=Project;
    P.View=static_cast<const FStudioViewSettings&>(*this); P.SelectedFrame=SelectedFrame;
    return P;
}
bool FStudioModel::HasUnsavedChanges() const
{ return StudioProjectIO::Serialize(SnapshotProject())!=SavedSnapshot; }
void FStudioModel::NewProject(const FString& Name)
{
    if(!CanReplaceProject())return;
    const auto Source=PrepareRecording(FStudioProject(),0); if(!Source) return;
    RecordingRepair.Reset(); CancelProjectOpen(false); CancelRecording(); UseRecording(Source);
    Project=FStudioProject(); ResetJobSession(); Project.Name=Name.IsEmpty()?TEXT("Untitled airfoil"):Name.Left(120);
    ClearCaseHistory(); ClearViewHistory();
    static_cast<FStudioViewSettings&>(*this)=Project.View;
    ProjectPath.Empty(); Reset(); SavedSnapshot.Empty(); bDirty=true; Notice=TEXT("New project using the published airfoil recording.");
}
bool FStudioModel::SaveProject(const FString& Path)
{
    EndViewEdit();
    auto P=SnapshotProject(); FString Error;
    if(!StudioAssetPaths::Resolve(P,Project.AssetBaseDirectory,Error)) { Notice=Error; return false; }
    if(!StudioProjectIO::Save(Path,P,Error)) { Notice=Error; AddLog(Error); return false; }
    ProjectPath=FPaths::ConvertRelativePathToFull(Path);
    Project.Draft=P.Draft; Project.Runs=P.Runs; Project.Recordings=P.Recordings;
    Project.AssetBaseDirectory=FPaths::GetPath(ProjectPath);
    SavedSnapshot=StudioProjectIO::Serialize(P); bDirty=false;
    if(PendingRecovery.IsEmpty()) DiscardRecovery();
    RememberProject();
    Notice=TEXT("Project saved: ")+ProjectPath; AddLog(Notice); return true;
}
bool FStudioModel::LoadProject(const FString& Path)
{
    if(!CanReplaceProject())return false;
    FStudioProject Candidate; FString Error;
    if(!StudioProjectIO::Load(Path,Candidate,Error)) { Notice=Error; return false; }
    const auto Source=PrepareRecording(Candidate,Candidate.SelectedFrame); if(!Source) return false;
    ApplyLoadedProject(MoveTemp(Candidate),Source,Path,false);
    return true;
}
FStudioInspectionState FStudioModel::InspectionState() const
{ return {Project.Camera,static_cast<const FStudioViewSettings&>(*this)}; }
void FStudioModel::ApplyInspection(const FStudioInspectionState& S)
{
    const auto Before=InspectionState();
    const double Rate=PlaybackRate; const bool Loop=bLoopPlayback;
    Project.Camera=S.Camera; static_cast<FStudioViewSettings&>(*this)=S.Display;
    // Presentation speed and the playback cursor never travel through view history.
    PlaybackRate=Rate; bLoopPlayback=Loop;
    if(!StudioView::CameraEquals(Before.Camera,S.Camera)) ++CameraRevision;
    if(!StudioView::RenderEquals(Before.Display,S.Display)) DisplayChanged();
    if(!(Before.Display.InspectionObjects==S.Display.InspectionObjects))
    {
        ++InspectionObjectsRevision;
        if(SelectedInspectionObject.IsValid()&&!FindInspectionObject(SelectedInspectionObject))
        {SelectedInspectionObject.Invalidate();++InspectionSelectionRevision;}
    }
    bDirty=true;
}
bool FStudioModel::EditView(const FString& Label,TFunctionRef<void(FStudioInspectionState&)> Edit)
{
    const auto Before=InspectionState(); auto After=Before; Edit(After);
    // Physical scalar controls cannot carry values across fields with different units.
    if(After.Display.ScalarField!=Before.Display.ScalarField)
    {
        After.Display.bVolumeThreshold=false;After.Display.bVolumeIsosurface=false;
        const auto& D=Solver->Descriptor();
        const auto* Field=D.Scalars.FindByPredicate([&](const auto& F){return F.Id==After.Display.ScalarField;});
        if(!Field)Field=D.Scalars.FindByPredicate([&](const auto& F){return F.Id==D.DefaultScalar;});
        if(Field){After.Display.VolumeThresholdMinimum=Field->Minimum;After.Display.VolumeThresholdMaximum=Field->Maximum;
            After.Display.VolumeIsovalue=(Field->Minimum+Field->Maximum)*.5;}
    }
    if(!StudioView::IsValid(After)) { Notice=TEXT("The view contains invalid values. The previous view has been kept."); return false; }
    if(Before.Equals(After)) return true;
    ViewHistory.Record(Label,Before,After); ApplyInspection(After); return true;
}
bool FStudioModel::EditCamera(const FString& Label,const FStudioCameraState& Camera)
{ return EditView(Label,[&Camera](auto& View){View.Camera=Camera;}); }
bool FStudioModel::ApplyViewHistory(bool bRedo)
{
    FStudioInspectionState Restored; FString Label;
    if(!ViewHistory.Restore(bRedo,InspectionState(),Restored,Label))
    { Notice=TEXT("No matching view history is available. The current view has been kept."); return false; }
    ApplyInspection(Restored); Notice=(bRedo?TEXT("Redo view: "):TEXT("Undo view: "))+Label; return true;
}
bool FStudioModel::UndoView() { return ApplyViewHistory(false); }
bool FStudioModel::RedoView() { return ApplyViewHistory(true); }
void FStudioModel::RememberProject()
{
    if(!ProjectPath.IsEmpty()) { RecentProjects.Remove(ProjectPath); RecentProjects.Insert(ProjectPath,0); }
    if(RecentProjects.Num()>12) RecentProjects.SetNum(12);
    SaveSession();
    RefreshProjectCatalog();
}
void FStudioModel::SaveSession()
{
    auto O=MakeShared<FJsonObject>(); O->SetStringField(TEXT("lastProject"),ProjectPath);
    O->SetBoolField(TEXT("sidebarCollapsed"),bSidebarCollapsed);
    O->SetBoolField(TEXT("viewportExpanded"),bViewportExpanded);
    O->SetNumberField(TEXT("inspectorTab"),InspectorTab);
    TArray<TSharedPtr<FJsonValue>> Paths;
    for(const auto& P:RecentProjects) Paths.Add(MakeShared<FJsonValueString>(P));
    O->SetArrayField(TEXT("recentProjects"),Paths);
    FString Text,Error; FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&Text));
    if(!StudioProjectIO::WriteAtomic(StorageDirectory/TEXT("StudioSession.json"),Text,Error)) AddLog(Error);
}
void FStudioModel::OpenSession()
{
    FString Text; TSharedPtr<FJsonObject> O;
    if(FFileHelper::LoadFileToString(Text,*(StorageDirectory/TEXT("StudioSession.json"))) &&
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O) && O)
    {
        O->TryGetBoolField(TEXT("sidebarCollapsed"),bSidebarCollapsed);
        O->TryGetBoolField(TEXT("viewportExpanded"),bViewportExpanded);
        double Tab=3;
        if(O->TryGetNumberField(TEXT("inspectorTab"),Tab)&&FMath::IsFinite(Tab)&&Tab>=0&&Tab<=3&&Tab==FMath::FloorToDouble(Tab))InspectorTab=int32(Tab);
        const TArray<TSharedPtr<FJsonValue>>* Paths;
        if(O->TryGetArrayField(TEXT("recentProjects"),Paths))
            for(const auto& V:*Paths) { FString Path; if(RecentProjects.Num()<12 && V->TryGetString(Path) && !Path.IsEmpty()) RecentProjects.AddUnique(FPaths::ConvertRelativePathToFull(Path)); }
        FString Last; if(O->TryGetStringField(TEXT("lastProject"),Last) && !Last.IsEmpty()) RequestProjectOpen(Last);
    }
    else
    {
        const FString Legacy=StorageDirectory/TEXT("StudioProject.json");
        if(IFileManager::Get().FileExists(*Legacy) && RequestProjectOpen(Legacy)) bOpeningLegacy=true;
    }
    const FString Recovery=StorageDirectory/TEXT("Recovery/StudioRecovery.lbms");
    if(IFileManager::Get().FileExists(*Recovery)) PendingRecovery=Recovery;
    RefreshProjectCatalog();
}
void FStudioModel::WriteRecovery()
{
    if(SuppressRecoveryOnClose || !HasUnsavedChanges() || !PendingRecovery.IsEmpty()) return;
    auto P=SnapshotProject(); P.RecoverySource=ProjectPath;
    FString Error;
    if(!StudioAssetPaths::Resolve(P,Project.AssetBaseDirectory,Error))
    { Notice=TEXT("Recovery save failed: ")+Error; AddLog(Notice); return; }
    if(!StudioProjectIO::WriteAtomic(StorageDirectory/TEXT("Recovery/StudioRecovery.lbms"),StudioProjectIO::Serialize(P),Error))
    { Notice=TEXT("Recovery save failed: ")+Error; AddLog(Notice); }
}
bool FStudioModel::RestoreRecovery()
{
    if(!CanReplaceProject())return false;
    FStudioProject P; FString Error;
    if(PendingRecovery.IsEmpty() || !StudioProjectIO::Load(PendingRecovery,P,Error))
    { Notice=Error.IsEmpty()?TEXT("The recovery frame is unavailable."):Error; return false; }
    const auto Source=PrepareRecording(P,P.SelectedFrame); if(!Source) return false;
    ApplyLoadedProject(MoveTemp(P),Source,PendingRecovery,true);
    return true;
}
void FStudioModel::DiscardRecovery()
{
    if(RecordingRepair.IsSet()&&RecordingRepair->bRecovery)RecordingRepair.Reset();
    if(bOpeningRecovery&&IsProjectOpenPending()) CancelProjectOpen(false);
    IFileManager::Get().Delete(*(StorageDirectory/TEXT("Recovery/StudioRecovery.lbms")),false,true);
    PendingRecovery.Empty();
}

namespace
{
    FString CaseContent(FStudioCaseDraft Draft)
    { Draft.Revision=0; return StudioCaseIO::Serialize(Draft); }
    bool ParseCaseHistory(const FString& Text, FStudioCaseDraft& Out, FString& Error)
    {
        TSharedPtr<FJsonObject> O;
        return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O) && StudioCaseIO::FromJSON(O,Out,Error);
    }
}
bool FStudioModel::EditCase(const FString& Label, TFunctionRef<void(FStudioCaseDraft&)> Edit)
{
    FStudioCaseDraft Candidate=Project.Draft;
    Edit(Candidate);
    if(Candidate.Id!=Project.Draft.Id) { Notice=TEXT("An edit cannot replace the case identity."); return false; }
    Candidate.Revision=Project.Draft.Revision;
    FString Error;
    if(!StudioAssetPaths::Resolve(Candidate,Project.AssetBaseDirectory,Error)) { Notice=Error; return false; }
    if(!StudioCaseIO::Validate(Candidate,Error)) { Notice=Error; return false; }
    if(CaseContent(Candidate)==CaseContent(Project.Draft)) return true;
    ++Candidate.Revision;
    auto Check=SnapshotProject(); Check.Draft=Candidate;
    FStudioProject Validated;
    if(!StudioProjectIO::Parse(StudioProjectIO::Serialize(Check),Validated,Error)) { Notice=Error; return false; }
    FCaseEdit Entry{Label.Left(120),StudioCaseIO::Serialize(Project.Draft),StudioCaseIO::Serialize(Candidate)};
    auto Bytes=[](const FCaseEdit& E) { return int64(E.Label.Len()+E.Before.Len()+E.After.Len())*sizeof(TCHAR); };
    int64 HistoryBytes=Bytes(Entry);
    if(HistoryBytes>MaxCaseHistoryBytes) { Notice=TEXT("This edit exceeds the case history memory limit."); return false; }
    for(const auto& E:CaseUndo) HistoryBytes+=Bytes(E);
    while(!CaseUndo.IsEmpty() && (CaseUndo.Num()>=MaxCaseEdits || HistoryBytes>MaxCaseHistoryBytes))
    { HistoryBytes-=Bytes(CaseUndo[0]); CaseUndo.RemoveAt(0); }
    CaseUndo.Add(MoveTemp(Entry)); CaseRedo.Reset();
    Project.Draft=MoveTemp(Candidate); bDirty=true;
    InvalidateAssets();
    InvalidateGeometry();
    Notice=TEXT("Case draft updated: ")+Label+TEXT(". Recording unchanged."); return true;
}
bool FStudioModel::ApplyCaseHistory(bool bRedo)
{
    auto& From=bRedo?CaseRedo:CaseUndo;
    auto& To=bRedo?CaseUndo:CaseRedo;
    if(From.IsEmpty()) return false;
    const auto& Entry=From.Last();
    FStudioCaseDraft Expected,Candidate; FString Error;
    if(!ParseCaseHistory(bRedo?Entry.Before:Entry.After,Expected,Error) ||
        !ParseCaseHistory(bRedo?Entry.After:Entry.Before,Candidate,Error))
    { Notice=TEXT("Case history is unreadable; the draft has been kept."); return false; }
    if(CaseContent(Expected)!=CaseContent(Project.Draft))
    { Notice=TEXT("The case changed outside its edit history; the draft has been kept."); return false; }
    Candidate.Revision=Project.Draft.Revision+1;
    auto Check=SnapshotProject(); Check.Draft=Candidate; FStudioProject Validated;
    if(!StudioProjectIO::Parse(StudioProjectIO::Serialize(Check),Validated,Error)) { Notice=Error; return false; }
    const FString Label=Entry.Label;
    To.Add(From.Pop()); Project.Draft=MoveTemp(Candidate); bDirty=true;
    InvalidateAssets();
    InvalidateGeometry();
    Notice=(bRedo?TEXT("Redo: "):TEXT("Undo: "))+Label; return true;
}
bool FStudioModel::UndoCase() { return ApplyCaseHistory(false); }
bool FStudioModel::RedoCase() { return ApplyCaseHistory(true); }

void FStudioModel::InvalidateAssets()
{
    ++AssetGeneration;
    if(AssetCancellation) *AssetCancellation=true;
    AssetReferences=StudioAssets::References(Project); ++AssetRevision;
    bRefreshAssets=true;
}
void FStudioModel::RefreshAssets() { InvalidateAssets(); }
void FStudioModel::CancelAssetCheck()
{
    ++AssetGeneration; bRefreshAssets=false;
    if(AssetCancellation) *AssetCancellation=true;
    for(auto& Ref:AssetReferences) if(Ref.State==EStudioAssetState::Unchecked) Ref.State=EStudioAssetState::Cancelled;
    ++AssetRevision; Notice=TEXT("Geometry check cancelled. Project references are unchanged.");
}
bool FStudioModel::LocateAsset(const FStudioAssetReference& Source,const FString& Path)
{
    if(IsCheckingAssets()) {Notice=TEXT("Wait for the current file check to finish or cancel it.");return false;}
    const auto Current=StudioAssets::References(Project);
    if(!Current.ContainsByPredicate([&Source](const auto& Ref){return Ref.Key()==Source.Key();}))
    {Notice=TEXT("This reference is no longer in the project. Refresh the file list.");return false;}
    if(Path.IsEmpty()||FPaths::IsRelative(Path)||Path.Contains(TEXT("://")))
    {Notice=TEXT("Select an absolute local file path.");return false;}
    LocatedPath=Path; FPaths::NormalizeFilename(LocatedPath);
    if(!FPaths::CollapseRelativeDirectories(LocatedPath)||LocatedPath.Len()>4096)
    {Notice=TEXT("The selected path cannot be resolved.");return false;}
    LocatedAsset=Source; auto Candidate=Source; Candidate.Path=LocatedPath;
    AssetCancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    CheckingAssetGeneration=++AssetGeneration; CheckingAssetProject=Project.Id;
    bRefreshAssets=false; bLocatingAsset=true;
    PendingAssets=Async(EAsyncExecution::ThreadPool,[Candidate,Cancel=AssetCancellation.ToSharedRef()]
    {return StudioAssets::Check({Candidate},Cancel);});
    Notice=TEXT("Verifying the selected geometry file…"); ++AssetRevision; return true;
}
void FStudioModel::PollAssets()
{
    if(PendingAssets.IsValid())
    {
        if(!PendingAssets.IsReady()) return;
        auto Result=PendingAssets.Get(); PendingAssets=TFuture<FStudioAssetCheckResult>();
        if(CheckingAssetGeneration==AssetGeneration&&CheckingAssetProject==Project.Id&&!Result.bCancelled)
        {
            if(bLocatingAsset)
            {
                if(Result.References.Num()==1&&Result.References[0].State==EStudioAssetState::Verified)
                    ApplyAssetLocation(LocatedAsset,LocatedPath);
                else Notice=Result.References.IsEmpty()?TEXT("File verification failed. Project kept."):
                    Result.References[0].Detail+TEXT(" Project references are unchanged.");
            }
            else {AssetReferences=MoveTemp(Result.References);++AssetRevision;}
        }
        bLocatingAsset=false;
    }
    if(!bRefreshAssets) return;
    bRefreshAssets=false; AssetReferences=StudioAssets::References(Project); ++AssetRevision;
    if(AssetReferences.IsEmpty()) return;
    AssetCancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    CheckingAssetGeneration=AssetGeneration; CheckingAssetProject=Project.Id;
    PendingAssets=Async(EAsyncExecution::ThreadPool,[Refs=AssetReferences,Cancel=AssetCancellation.ToSharedRef()]() mutable
    {return StudioAssets::Check(MoveTemp(Refs),Cancel);});
}
bool FStudioModel::ApplyAssetLocation(const FStudioAssetReference& Source,const FString& Path)
{
    auto Candidate=SnapshotProject(); auto Undo=CaseUndo; auto Redo=CaseRedo; FString Error;
    const auto Refs=StudioAssets::References(Candidate);
    if(!Refs.ContainsByPredicate([&Source](const auto& Ref){return Ref.Key()==Source.Key();}))
    {Notice=TEXT("Geometry reference changed while checking. Project kept.");return false;}
    if(!StudioAssets::Relocate(Candidate,Source,Path,Error)) {Notice=Error;return false;}
    auto RebaseHistory=[&](auto& Entries)
    {
        for(auto& Entry:Entries)
        {
            FStudioCaseDraft Before,After;
            if(!ParseCaseHistory(Entry.Before,Before,Error)||!ParseCaseHistory(Entry.After,After,Error)) return false;
            StudioAssets::Relocate(Before,Source,Path); StudioAssets::Relocate(After,Source,Path);
            Entry.Before=StudioCaseIO::Serialize(Before);Entry.After=StudioCaseIO::Serialize(After);
        }
        return true;
    };
    if(!RebaseHistory(Undo)||!RebaseHistory(Redo)) {Notice=TEXT("Could not retain edit history. Project references are unchanged.");return false;}
    // A longer new location must still respect the shared history memory budget.
    int64 Bytes=0;
    for(const auto* Entries:{&Undo,&Redo}) for(const auto& E:*Entries) Bytes+=int64(E.Label.Len()+E.Before.Len()+E.After.Len())*sizeof(TCHAR);
    FStudioProject Validated;
    if(Bytes>MaxCaseHistoryBytes||!StudioProjectIO::Parse(StudioProjectIO::Serialize(Candidate),Validated,Error))
    {Notice=Bytes>MaxCaseHistoryBytes?TEXT("The relocated paths exceed the edit history limit. Project kept."):Error;return false;}
    Project.Draft=MoveTemp(Candidate.Draft);Project.Runs=MoveTemp(Candidate.Runs);
    CaseUndo=MoveTemp(Undo);CaseRedo=MoveTemp(Redo);bDirty=HasUnsavedChanges();
    InvalidateAssets();
    InvalidateGeometry();
    Notice=TEXT("Geometry located and verified. Save to keep the new location."); AddLog(Notice); return true;
}

bool FStudioModel::IsWorkspaceAvailable(EStudioWorkspace Destination)
{
    return Destination==EStudioWorkspace::Dashboard || Destination==EStudioWorkspace::Projects || Destination==EStudioWorkspace::Solve || Destination==EStudioWorkspace::Geometry || Destination==EStudioWorkspace::Materials || Destination==EStudioWorkspace::Domain || Destination==EStudioWorkspace::BoundaryConditions || Destination==EStudioWorkspace::Meshing || Destination==EStudioWorkspace::Monitors;
}
bool FStudioModel::Navigate(EStudioWorkspace Destination)
{
    if(!IsWorkspaceAvailable(Destination)) return false;
    EndViewEdit();
    Workspace=Destination;
    if(Destination==EStudioWorkspace::Domain||Destination==EStudioWorkspace::BoundaryConditions||Destination==EStudioWorkspace::Meshing){InvalidateDomainGeometry();if(!DomainGeometry&&!PendingDomainGeometry.IsValid())RequestDomainGeometry();}
    if(Destination!=EStudioWorkspace::Solve) RefreshProjectCatalog();
    return true;
}
bool FStudioModel::RenameProject(const FString& Name)
{
    const FString Clean=Name.TrimStartAndEnd();
    if(Clean.IsEmpty() || Clean.Len()>120) { Notice=TEXT("Project names must contain 1–120 characters."); return false; }
    if(Project.Name==Clean) return true;
    Project.Name=Clean; bDirty=true; ++CatalogRevision;
    Notice=TEXT("Project renamed. Save to keep the new name."); return true;
}
bool FStudioModel::CreateProject(const FString& Path,const FString& Name)
{
    if(!CanReplaceProject())return false;
    FStudioProject Candidate; Candidate.Name=Name.TrimStartAndEnd(); FString Error;
    const auto Source=PrepareRecording(Candidate,0); if(!Source) return false;
    // Commit the file before replacing any live state, including unsaved work.
    if(!StudioProjectIO::Save(Path,Candidate,Error)) { Notice=Error; return false; }
    RecordingRepair.Reset(); CancelProjectOpen(false); CancelRecording(); UseRecording(Source);
    Project=MoveTemp(Candidate); ResetJobSession(); static_cast<FStudioViewSettings&>(*this)=Project.View;
    ClearCaseHistory(); ClearViewHistory(); Reset(); ProjectPath=FPaths::ConvertRelativePathToFull(Path);
    Project.AssetBaseDirectory=FPaths::GetPath(ProjectPath);
    SavedSnapshot=StudioProjectIO::Serialize(SnapshotProject()); bDirty=false;
    if(PendingRecovery.IsEmpty()) DiscardRecovery();
    RememberProject(); Notice=TEXT("Created ")+Project.Name; return true;
}
bool FStudioModel::DuplicateProject(const FString& Path,const FString& Name)
{
    if(!CanReplaceProject())return false;
    const FString FullPath=FPaths::ConvertRelativePathToFull(Path);
    if(!ProjectPath.IsEmpty() && FPaths::IsSamePath(FullPath,ProjectPath))
    { Notice=TEXT("Choose a different file for the duplicate. The original has been kept."); return false; }
    auto Candidate=SnapshotProject(); Candidate.Id=FGuid::NewGuid(); Candidate.Draft.Id=FGuid::NewGuid();
    Candidate.Name=Name.TrimStartAndEnd(); Candidate.RecoverySource.Empty(); FString Error;
    if(!StudioAssetPaths::Resolve(Candidate,Project.AssetBaseDirectory,Error)) { Notice=Error; return false; }
    if(!StudioProjectIO::Save(FullPath,Candidate,Error)) { Notice=Error; return false; }
    RecordingRepair.Reset(); CancelProjectOpen(false); CancelRecording();
    Project=MoveTemp(Candidate); ResetJobSession(); ProjectPath=FullPath; ClearCaseHistory(); ClearViewHistory();
    Project.AssetBaseDirectory=FPaths::GetPath(ProjectPath);
    SavedSnapshot=StudioProjectIO::Serialize(SnapshotProject()); bDirty=false;
    if(PendingRecovery.IsEmpty()) DiscardRecovery();
    RememberProject(); Notice=TEXT("Created independent copy: ")+Project.Name; return true;
}
void FStudioModel::RefreshProjectCatalog()
{
    bCatalogLoading=true;
    if(PendingCatalog.IsValid()) { bCatalogRefreshPending=true; return; }
    bCatalogRefreshPending=false;
    TArray<FString> Paths;
    for(const auto& Path:RecentProjects)
        if(Paths.Num()<12 && !Path.IsEmpty()) Paths.AddUnique(FPaths::ConvertRelativePathToFull(Path));
    PendingCatalog=Async(EAsyncExecution::ThreadPool,[Paths=MoveTemp(Paths)]
    {
        TArray<FStudioProjectSummary> Result;
        for(const auto& Path:Paths)
        {
            FStudioProjectSummary Item; Item.Path=Path; Item.Name=FPaths::GetBaseFilename(Path);
            FStudioProject P;
            if(StudioProjectIO::Load(Path,P,Item.Error))
            {
                Item.Id=P.Id; Item.Name=P.Name; Item.bFavorite=P.bFavorite;
                Item.CameraCount=P.Cameras.Num(); Item.RunCount=P.Runs.Num();
                Item.Modified=IFileManager::Get().GetTimeStamp(*Path);
            }
            Result.Add(MoveTemp(Item));
        }
        return Result;
    });
}
void FStudioModel::PollProjectCatalog()
{
    if(!PendingCatalog.IsValid() || !PendingCatalog.IsReady()) return;
    auto Result=PendingCatalog.Get(); PendingCatalog=TFuture<TArray<FStudioProjectSummary>>();
    // A save/open/refresh during I/O invalidates this result; never publish stale metadata.
    if(bCatalogRefreshPending) { RefreshProjectCatalog(); return; }
    ProjectCatalog=MoveTemp(Result); bCatalogLoading=false; ++CatalogRevision;
}
void FStudioModel::ForgetRecentProject(const FString& Path)
{
    RecentProjects.RemoveAll([&Path](const FString& Recent){return FPaths::IsSamePath(Recent,Path);}); SaveSession(); RefreshProjectCatalog();
    Notice=TEXT("Removed from recent projects. The file remains on disk.");
}
bool FStudioModel::SetProjectFavorite(const FString& Path,bool bFavorite)
{
    if((Path.IsEmpty() && ProjectPath.IsEmpty()) || (!Path.IsEmpty() && !ProjectPath.IsEmpty() && FPaths::IsSamePath(Path,ProjectPath)))
    { Project.bFavorite=bFavorite; bDirty=true; ++CatalogRevision; return true; }
    FStudioProject Candidate; FString Error;
    if(!StudioProjectIO::Load(Path,Candidate,Error)) { Notice=Error; return false; }
    Candidate.bFavorite=bFavorite;
    if(!StudioProjectIO::Save(Path,Candidate,Error)) { Notice=Error; return false; }
    RefreshProjectCatalog(); Notice=bFavorite?TEXT("Project added to favorites."):TEXT("Project removed from favorites."); return true;
}

TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> FStudioModel::PrepareRecording(const FStudioProject& Candidate,int32 Frame)
{
    // Interactive opens run the identical verifier on a worker. Reusing an ID
    // alone would bypass a changed file or a different saved content hash.
    auto Result=StudioRecordings::Open(Candidate.Dataset,Candidate.Recordings,Frame,{});
    if(!Result.Source) Notice=Result.Error;
    return MoveTemp(Result.Source);
}
void FStudioModel::UseRecording(TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> Source)
{
    Solver=MoveTemp(Source); Frames=Solver->Descriptor().Frames;
    if(Frames.IsEmpty()) Frames.Add(FStudioFrame());
}
const FStudioScalarDescriptor& FStudioModel::ActiveScalar() const
{
    const auto& D=Solver->Descriptor();
    if(const auto* Field=D.Scalars.FindByPredicate([&](const auto& F){return F.Id==ScalarField;}))return *Field;
    if(const auto* Field=D.Scalars.FindByPredicate([&](const auto& F){return F.Id==D.DefaultScalar;}))return *Field;
    return D.Scalars[0];
}
FStudioColorMapping FStudioModel::ActiveColorMapping() const
{return StudioColor::Resolve(Project.Dataset,ActiveScalar(),ScalarStyles);}
bool FStudioModel::SetScalarStyle(int32 Palette,bool bManual,double Minimum,double Maximum,const TArray<FLinearColor>& Colors)
{
    FStudioScalarStyle Style{Project.Dataset,ActiveScalar().Id,Palette,bManual,Minimum,Maximum};
    const auto Previous=ActiveColorMapping();
    if(!Colors.IsEmpty()&&Colors.Num()!=3){Notice=TEXT("Provide low, middle and high colors.");return false;}
    Style.LowColor=Colors.IsEmpty()?Previous.LowColor:Colors[0];
    Style.MiddleColor=Colors.IsEmpty()?Previous.MiddleColor:Colors[1];
    Style.HighColor=Colors.IsEmpty()?Previous.HighColor:Colors[2];
    if(!StudioColor::IsValid(Style))
    {Notice=TEXT("Color range requires finite values with minimum below maximum. Previous colors retained.");return false;}
    if(ScalarStyles.Num()>=128&&!ScalarStyles.ContainsByPredicate([&](const auto& S){return S.Dataset==Style.Dataset&&S.Field==Style.Field;}))
    {Notice=TEXT("This project already stores 128 field color settings.");return false;}
    return EditView(TEXT("Field colors"),[&](auto& S)
    {
        if(auto* Existing=S.Display.ScalarStyles.FindByPredicate([&](const auto& V){return V.Dataset==Style.Dataset&&V.Field==Style.Field;}))*Existing=Style;
        else S.Display.ScalarStyles.Add(Style);
    });
}
bool FStudioModel::RequestRecording(const FString& Id)
{
    return StartRecordingRequest(Id,FString(),ERecordingChange::Select);
}
bool FStudioModel::RequestExternalRecording(const FString& Path)
{
    return StartRecordingRequest(FString(),Path,ERecordingChange::Import);
}
bool FStudioModel::RequestRecordingRelink(const FString& Id,const FString& Path)
{
    if(Path.IsEmpty()) {Notice=TEXT("Choose the original recording folder.");return false;}
    return StartRecordingRequest(Id,Path,ERecordingChange::Relink);
}
bool FStudioModel::RequestReconstruction(const FString& Path,bool bRelocate)
{
    if(Path.IsEmpty()){Notice=TEXT("Choose the folder containing reconstruction.json and its index files.");return false;}
    return StartRecordingRequest(Project.Dataset,Path,bRelocate?ERecordingChange::RelinkSurface:ERecordingChange::ImportSurface);
}
bool FStudioModel::RemoveReconstruction()
{return StartRecordingRequest(Project.Dataset,FString(),ERecordingChange::RemoveSurface);}
bool FStudioModel::StartRecordingRequest(const FString& Id,const FString& Path,ERecordingChange Change)
{
    const bool bImport=Change==ERecordingChange::Import;
    const bool bSurface=Change==ERecordingChange::ImportSurface||Change==ERecordingChange::RelinkSurface||Change==ERecordingChange::RemoveSurface;
    const bool bRelink=Change==ERecordingChange::Relink||bSurface;
    if(IsProjectOpenPending()) { Notice=TEXT("Wait for project opening to finish, or cancel it before changing recordings."); return false; }
    if(PendingRecording.IsValid()) { Notice=TEXT("Wait for the pending recording read to finish, or cancel it."); return false; }
    if(!bImport&&!Project.Recordings.ContainsByPredicate([&](const auto& R){return R.Id==Id;})&&
        (bRelink||StudioRecordings::PathForId(Id).IsEmpty()))
    { Notice=TEXT("Recording has no saved external location or installed source; current view retained."); return false; }
    if(bImport&&Path.IsEmpty()) {Notice=TEXT("Choose the folder containing recording.json and its data files.");return false;}
    if(bSurface)
    {
        const auto* Ref=Project.Recordings.FindByPredicate([&](const auto& R){return R.Id==Id;});
        if(Id!=Project.Dataset||!Ref||Ref->Format!=TEXT("point_v3")||(Solver->Descriptor().SpatialDimensions!=2&&Solver->Descriptor().SpatialDimensions!=3))
        {Notice=TEXT("Select an imported point recording before changing its display reconstruction.");return false;}
        if(Change!=ERecordingChange::ImportSurface&&!Ref->Reconstruction.IsSet())
        {Notice=TEXT("This recording has no reconstruction to relocate or remove.");return false;}
    }
    bRecordingLoading=true;RequestedRecordingId=Id;RecordingProjectId=Project.Id;bRelinkingRecording=bRelink;
    RecordingChange=Change;
    Notice=bSurface?TEXT("Verifying display reconstruction; current source, camera and playback remain available."):bRelink?TEXT("Verifying replacement recording; the current view remains available."):
        TEXT("Loading and verifying recording; your current camera and view remain available.");
    RecordingCancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    const int32 Frame=bRelink&&Id==Project.Dataset?SelectedFrame:0;
    PendingRecording=Async(EAsyncExecution::ThreadPool,[Id,Path,bImport,bSurface,Change,Frame,Refs=Project.Recordings,Cancel=RecordingCancellation]
    {
        if(bSurface)
        {
            auto Ref=*Refs.FindByPredicate([&](const auto& R){return R.Id==Id;});
            if(Change==ERecordingChange::ImportSurface)return StudioRecordings::ImportReconstruction(Ref,Path,Frame,Cancel);
            if(Change==ERecordingChange::RemoveSurface)Ref.Reconstruction.Reset();
            else Ref.Reconstruction->Path=FPaths::ConvertRelativePathToFull(Path);
            return StudioRecordings::Open(Id,{Ref},Frame,Cancel);
        }
        auto Result=bImport?StudioRecordings::Import(Path,Frame,Cancel):StudioRecordings::Open(Id,Refs,Frame,Cancel,Path);
        if(bImport&&Result.Reference.IsSet())
        {
            const auto* Existing=Refs.FindByPredicate([&](const auto& R){return R.Id==Result.Reference->Id;});
            if(Existing&&(Existing->Format!=Result.Reference->Format||Existing->MetadataSHA256!=Result.Reference->MetadataSHA256||Existing->PayloadSHA256!=Result.Reference->PayloadSHA256))
            {
                Result.Source.Reset();Result.Reference.Reset();
                Result.Error=TEXT("This recording ID already names different saved output. Import a dataset with a distinct ID.");
            }
            else if(Existing&&Existing->Reconstruction.IsSet())
            {
                // Preserve the explicit interpretation when the same original is
                // selected again or imported from another location.
                auto Reference=*Result.Reference;Reference.Reconstruction=Existing->Reconstruction;
                Result=StudioRecordings::Open(Reference.Id,{Reference},Frame,Cancel);
            }
        }
        return Result;
    });
    return true;
}
void FStudioModel::CancelRecording()
{
    bRecordingLoading=false;RequestedRecordingId.Empty();
    if(RecordingCancellation) RecordingCancellation->store(true);
    // Drain the one cancellable worker before accepting another request.
}
void FStudioModel::PollRecording()
{
    if(!PendingRecording.IsValid()||!PendingRecording.IsReady()) return;
    auto Result=PendingRecording.Get();PendingRecording=TFuture<FStudioRecordingLoadResult>();
    const bool Accept=bRecordingLoading&&RecordingProjectId==Project.Id;
    const bool Relink=bRelinkingRecording;const FString RequestedId=RequestedRecordingId;const auto Change=RecordingChange;CancelRecording();
    if(!Accept) return;
    auto Source=MoveTemp(Result.Source);
    if(!Source||Source->FrameCount()==0||!Source->LoadError().IsEmpty()||
        (!RequestedId.IsEmpty()&&Source->Descriptor().Id!=RequestedId))
    {Notice=Result.Error.IsEmpty()?TEXT("Recording validation failed; current view retained."):Result.Error;AddLog(Notice);return;}
    const FString Id=Source->Descriptor().Id;
    auto Candidate=SnapshotProject();
    if(Result.Reference.IsSet())
    {
        auto* Ref=Candidate.Recordings.FindByPredicate([&](const auto& R){return R.Id==Id;});
        if(Ref) *Ref=*Result.Reference;else Candidate.Recordings.Add(*Result.Reference);
    }
    if(!Relink)
    {
        Candidate.Dataset=Id;Candidate.SelectedFrame=0;
        if(!Candidate.Runs.ContainsByPredicate([&](const auto& Run){return Run.GetDatasetId()==Id;}))
            Candidate.Runs.Add(FStudioRunRecord::Recording(Source->Descriptor().Title.Left(120),Id,!Result.Reference.IsSet()));
    }
    FStudioProject Validated;FString Error;
    if(!StudioProjectIO::Parse(StudioProjectIO::Serialize(Candidate),Validated,Error))
    {Notice=TEXT("Recording could not be added: ")+Error;return;}
    Project.Recordings=MoveTemp(Candidate.Recordings);Project.Runs=MoveTemp(Candidate.Runs);
    if(!Relink)
    {
        UseRecording(Source);bVolumeThreshold=false;bVolumeIsosurface=false;Project.Dataset=Id;Project.SelectedFrame=0;
        const auto& Scalar=ActiveScalar();VolumeThresholdMinimum=Scalar.Minimum;VolumeThresholdMaximum=Scalar.Maximum;
        VolumeIsovalue=(Scalar.Minimum+Scalar.Maximum)*.5;
        SelectedFrame=PlaybackFrame=0;State=EStudioRunState::Paused;bReviewing=true;Accumulator=WallSeconds=0;
        DisplayChanged();
    }
    else if(Id==Project.Dataset)
    {
        UseRecording(Source);DisplayChanged();
        // Preserve playback, camera, source time and run identity on relocation.
    }
    bDirty=true;++CatalogRevision;
    Notice=Change==ERecordingChange::RemoveSurface?TEXT("Reconstruction removed. Original points retained; save to keep this choice."):
        Change==ERecordingChange::ImportSurface?TEXT("Display reconstruction loaded. Original CFD values retained; save to keep this choice."):
        Change==ERecordingChange::RelinkSurface?TEXT("Reconstruction location verified. Save to keep the new path."):
        Relink?TEXT("Recording location verified. Save to keep the new path."):
        TEXT("Opened ")+Source->Descriptor().Title+TEXT(". Camera and case retained. Use Fit to frame this recording.");
    AddLog(Notice);
}
