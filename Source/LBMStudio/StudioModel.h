#pragma once
#include "CoreMinimal.h"
#include "StudioProject.h"
#include "StudioRecording.h"
#include "StudioView.h"
#include "StudioCameraPlacement.h"
#include "StudioAssets.h"
#include "StudioMeshImport.h"
#include "StudioGeometryEdit.h"
#include "StudioRunSettings.h"
#include "StudioDomain.h"
#include "StudioLattice.h"
#include "Async/Future.h"

enum class EStudioWorkspace : uint8
{ Dashboard, Projects, Geometry, Domain, Materials, BoundaryConditions, Meshing, Solve, Monitors, Results, PostProcessing, Settings, Setup };

struct FStudioProjectSummary
{
    FString Path;
    FString Name;
    FString Error;
    FGuid Id;
    bool bFavorite = false;
    int32 CameraCount = 0;
    int32 RunCount = 0;
    FDateTime Modified;
};

struct FStudioFieldValue
{
    FVector Velocity = FVector::ZeroVector;
    double Pressure = 0;
    double Density = 0;
};
struct FStudioPointFrame;
struct FStudioSurfaceReconstruction;
struct FStudioVolumeReconstruction;
/** Immutable recorded field. Queries report missing data explicitly; they never
 * substitute zero for an unavailable component, frame or spatial location.
 */
class IStudioField
{
public:
    virtual ~IStudioField() = default;
    virtual bool IsValid() const = 0;
    virtual TOptional<FStudioFieldIdentity> Identity() const { return {}; }
    virtual TOptional<FStudioScalarDescriptor> Scalar(const FString& FieldId) const { return {}; }
    /** Worker-only optional-field read of this exact immutable frame. Retains
     * source/reconstruction identity and never changes a solver cursor or view.
     * A failure is explicit; implementations must not substitute another field. */
    virtual TSharedPtr<const IStudioField,ESPMode::ThreadSafe> LoadScalarSnapshot(const FString& FieldId,
        const FStudioLoadCancellation& Cancellation,FString& Error) const
    { Error=TEXT("This field snapshot cannot load additional scalar arrays.");return {}; }
    /** Legacy complete U/V/pressure/density tuple. Optional-field readers use
     * the individual queries below, rather than inventing members of this tuple.
     */
    virtual bool Sample(const FVector& PositionMeters, FStudioFieldValue& Out) const = 0;
    virtual bool SampleScalar(const FVector& PositionMeters, const FString& FieldId, double& Out) const;
    /** Scene-axis velocity; a represented 2D vector has no spanwise component. */
    virtual bool SampleVelocity(const FVector& PositionMeters, FVector& Out) const;
    /** Conservative continuous coverage. New backends must implement this before
     * enabling integration; isolated valid endpoints never imply a valid step. */
    virtual bool SupportsSegment(const FVector& A,const FVector& B,
        const FStudioLoadCancellation& Cancellation={}) const { return false; }
    virtual bool IsSolid(const FVector& PositionMeters) const = 0;
    virtual const TArray<FVector2D>& Boundary() const = 0;
    virtual const TArray<FIntVector>& BoundaryTriangles() const = 0;
    virtual TSharedPtr<const FStudioPointFrame,ESPMode::ThreadSafe> OriginalPoints() const { return {}; }
    virtual TSharedPtr<const FStudioSurfaceReconstruction,ESPMode::ThreadSafe> Reconstruction() const { return {}; }
    virtual TSharedPtr<const FStudioVolumeReconstruction,ESPMode::ThreadSafe> VolumeReconstruction() const { return {}; }
    /** Verified triangle connectivity. Output is scene meters; the immutable
     * field owns topology. Zero means unavailable, never implicit triangulation. */
    virtual int32 MeshTriangleCount() const;
    virtual bool MeshTriangle(int32 Index,FVector (&PositionsMeters)[3]) const;
};
/** Replaceable backend contract. Recorded CFD has no configurable physics. */
class IStudioSolver
{
public:
    virtual ~IStudioSolver() = default;
    virtual int32 FrameCount() const = 0;
    virtual FStudioFrame EvaluateFrame(int32 Ordinal) const = 0;
    virtual TSharedRef<const IStudioField,ESPMode::ThreadSafe> CaptureField(int32 Ordinal) const = 0;
    virtual TSharedRef<const IStudioField,ESPMode::ThreadSafe> CaptureViewField(int32 Ordinal,
        const FString& ScalarId, bool bVectors, const FStudioLoadCancellation& Cancellation = {}) const { return CaptureField(Ordinal); }
    virtual bool ExportField(int32 Ordinal, const FString& Path) const = 0;
    virtual FString LoadError() const = 0;
    virtual const FStudioRecordingDescriptor& Descriptor() const = 0;
    virtual FStudioFrameCacheStats CacheStats() const { return {}; }
    virtual TSharedPtr<const FStudioSurfaceReconstruction,ESPMode::ThreadSafe> Reconstruction() const { return {}; }
    virtual TSharedPtr<const FStudioVolumeReconstruction,ESPMode::ThreadSafe> VolumeReconstruction() const { return {}; }
};
struct FRecordedFlowData;
class FRecordedSolver final : public IStudioSolver
{
public:
    explicit FRecordedSolver(const FString& Path = FString(), int64 CacheBudgetBytes = 8LL*1024*1024,
        const FStudioLoadCancellation& Cancellation = {});
    bool PrepareFrame(int32 Ordinal, const FStudioLoadCancellation& Cancellation);
    int32 FrameCount() const override;
    FStudioFrame EvaluateFrame(int32 Ordinal) const override;
    TSharedRef<const IStudioField,ESPMode::ThreadSafe> CaptureField(int32 Ordinal) const override;
    TSharedRef<const IStudioField,ESPMode::ThreadSafe> CaptureViewField(int32 Ordinal, const FString& ScalarId,
        bool bVectors, const FStudioLoadCancellation& Cancellation = {}) const override;
    bool ExportField(int32 Ordinal, const FString& Path) const override;
    FString LoadError() const override;
    const FStudioRecordingDescriptor& Descriptor() const override;
    FStudioFrameCacheStats CacheStats() const override;
private:
    TSharedPtr<const FRecordedFlowData,ESPMode::ThreadSafe> Data;
    FString Error;
};

class FStudioPointRecording;
class FPointRecordedSolver final : public IStudioSolver
{
public:
    explicit FPointRecordedSolver(TSharedRef<FStudioPointRecording,ESPMode::ThreadSafe> InRecording,
        TSharedPtr<const FStudioSurfaceReconstruction,ESPMode::ThreadSafe> InReconstruction = {},
        TSharedPtr<const FStudioVolumeReconstruction,ESPMode::ThreadSafe> InVolume = {});
    bool PrepareFrame(int32 Ordinal, const FStudioLoadCancellation& Cancellation);
    int32 FrameCount() const override;
    FStudioFrame EvaluateFrame(int32 Ordinal) const override;
    TSharedRef<const IStudioField,ESPMode::ThreadSafe> CaptureField(int32 Ordinal) const override;
    TSharedRef<const IStudioField,ESPMode::ThreadSafe> CaptureViewField(int32 Ordinal, const FString& ScalarId,
        bool bVectors, const FStudioLoadCancellation& Cancellation = {}) const override;
    bool ExportField(int32 Ordinal, const FString& Path) const override;
    FString LoadError() const override;
    const FStudioRecordingDescriptor& Descriptor() const override { return Meta; }
    FStudioFrameCacheStats CacheStats() const override;
    TSharedPtr<const FStudioSurfaceReconstruction,ESPMode::ThreadSafe> Reconstruction() const override { return SurfaceReconstruction; }
    TSharedPtr<const FStudioVolumeReconstruction,ESPMode::ThreadSafe> VolumeReconstruction() const override { return Volume; }
private:
    TSharedRef<FStudioPointRecording,ESPMode::ThreadSafe> Recording;
    TSharedPtr<const FStudioSurfaceReconstruction,ESPMode::ThreadSafe> SurfaceReconstruction;
    TSharedPtr<const FStudioVolumeReconstruction,ESPMode::ThreadSafe> Volume;
    FStudioRecordingDescriptor Meta;
    mutable FCriticalSection ErrorMutex;
    mutable FString Error;
    TArray<FString> RequestedFields(const FString& ScalarId, bool bVectors) const;
    void SetError(const FString& Message) const;
};

enum class EStudioRunState : uint8 { Ready, Running, Paused, Stopped, Complete };
enum class EStudioProjectLoadStage : uint8 { Document, Recording, Frame, Ready };
struct FStudioProjectLoadResult
{
    FStudioProject Project;
    TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> Source;
    FString Error;
    bool bRelocated = false;
    bool bRecordingFailed = false;
    bool bReconstructionFailed = false;
    bool bRemovedReconstruction = false;
    FString ResolutionNote;
};
struct FStudioRecordingRepair
{
    FString ProjectPath,RecordingPath,Title,ReplacedRecentPath;
    bool bRecovery=false;
    bool bReconstruction=false;
    FString SourcePath,ReconstructionPath;
    bool bRemoveReconstruction=false;
};
class FStudioModel : public FStudioViewSettings, public TSharedFromThis<FStudioModel>
{
public:
    explicit FStudioModel(const FString& SessionDirectory=FString());
    ~FStudioModel() { if(AssetCancellation) *AssetCancellation=true; if(MeshCancellation) *MeshCancellation=true;
        if(DomainCancellation) *DomainCancellation=true; if(LatticeCancellation) *LatticeCancellation=true; if(ProjectLoadCancellation) *ProjectLoadCancellation=true; if(RecordingCancellation) *RecordingCancellation=true; }
    static constexpr double PlaybackInterval = 0.05; // 20 snapshots/s; 601 snapshots play for 30 seconds.
    static constexpr double ColorMax = 400.0; // Fixed velocity legend across all source frames.
    TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> Solver;
    TArray<FStudioFrame> Frames;
    TArray<FString> Log;
    EStudioRunState State = EStudioRunState::Ready;
    int32 SelectedFrame = 0;
    int32 PlaybackFrame = 0;
    bool bReviewing = false;
    double WallSeconds = 0;
    double Accumulator = 0;
    int32 Revision = 0;
    // Explicit selection/display changes obsolete pending work. The playback
    // clock only advances Revision so slower rendering still makes progress.
    uint64 RenderIntentRevision = 0;
    int32 CameraRevision = 0;
    FStudioProject Project;
    FString ProjectPath;
    TArray<FString> RecentProjects;
    FString PendingRecovery;
    bool bDirty = false;
    bool SuppressRecoveryOnClose = false;
    FString Notice;
    EStudioWorkspace Workspace = EStudioWorkspace::Solve;
    bool bSidebarCollapsed = false;
    bool bViewportExpanded = false; // Session layout, never case or camera state.
    int32 InspectorTab = 3; // Setup, Physics, BCs, Display; session preference only.
    TArray<FStudioProjectSummary> ProjectCatalog;
    int32 CatalogRevision = 0;
    bool bCatalogLoading = false;
    bool bRecordingLoading = false;
    bool IsRecordingLoadPending() const { return PendingRecording.IsValid(); }
    bool RequestRecording(const FString& Id);
    bool RequestExternalRecording(const FString& Path);
    bool RequestRecordingRelink(const FString& Id,const FString& Path);
    bool RequestReconstruction(const FString& Path, bool bRelocate = false);
    bool RemoveReconstruction();
    void CancelRecording();
    TArray<FStudioAssetReference> AssetReferences;
    int32 AssetRevision = 0;
    bool IsCheckingAssets() const { return PendingAssets.IsValid(); }
    void RefreshAssets();
    void CancelAssetCheck();
    bool LocateAsset(const FStudioAssetReference& Source,const FString& Path);
    TSharedPtr<const FStudioDomainGeometry,ESPMode::ThreadSafe> DomainGeometry;
    int32 DomainPreviewRevision=0;
    int32 SelectedDomainFace=0;
    FGuid SelectedBoundaryTarget;
    FString DomainNotice;
    bool RequestDomainGeometry();
    void CancelDomainGeometry();
    bool IsReadingDomainGeometry() const { return PendingDomainGeometry.IsValid(); }
    bool UpdateDomain(const FStudioDomain& Domain);
    void SelectDomainFace(int32 Index);
    bool SelectBoundaryTarget(const FGuid& Id);
    FStudioMeshImportResult GeometrySource;
    FStudioMeshImportOptions ImportOptions;
    FGuid SelectedGeometry;
    int32 GeometryRevision = 0;
    bool bImportPreview = false;
    FString GeometryNotice;
    bool IsReadingGeometry() const { return PendingMesh.IsValid(); }
    bool RequestGeometryImport(const FString& Path);
    void CancelGeometryImport();
    bool CommitGeometryImport();
    bool SelectGeometry(const FGuid& Id);
    /** Apply a retained name/transform to a verified selected object in one case edit. */
    bool UpdateGeometry(FStudioGeometryEdit& Edit);
    bool UpdateRunSettings(FStudioRunSettingsEdit& Edit);
    bool GeometryAssetForPreview(FStudioGeometryAsset& Asset,FString& Error) const;
    void GeometryOptionsChanged() { ++GeometryRevision; }

    void Run(); void Pause(); void Stop(); void Step(); void Tick(double DeltaSeconds);
    bool SetControlHarness(bool bEnabled);
    bool CanControl(EStudioJobCommand Command) const;
    bool Control(EStudioJobCommand Command);
    bool HasActiveJob() const;
    bool CanReplaceProject();
    const FStudioJobController& Job() const { return *JobController; }
    FString RunStatus(const FGuid& Id) const;
    const FStudioJobHistory* CurrentJobHistory() const;
    TArray<FString> JobLog;
    // Explicit development actions, never claims about the recorded CFD.
    bool SimulateJobEvent(EStudioJobState State);
    bool CanSimulateJobEvent(EStudioJobState State) const;
    void Scrub(double Fraction); void ReturnToLive(); void Reset();
    void AddLog(const FString& Message);
    void DisplayChanged() { ++Revision; ++RenderIntentRevision; }
    const FStudioFrame& DisplayFrame() const;
    const FStudioScalarDescriptor& ActiveScalar() const;
    FStudioColorMapping ActiveColorMapping() const;
    bool SetScalarStyle(int32 Palette,bool bManual,double Minimum,double Maximum,const TArray<FLinearColor>& Colors={});
    FString StatusText() const;
    double Progress() const { return Solver->FrameCount()>1 ? double(PlaybackFrame)/(Solver->FrameCount()-1) : 0.; }
    FStudioProject SnapshotProject() const;
    bool HasUnsavedChanges() const;
    void AcceptLoadedView() { SavedSnapshot=StudioProjectIO::Serialize(SnapshotProject()); bDirty=false; }
    void NewProject(const FString& Name);
    void OpenSession();
    void SaveSession();
    void WriteRecovery();
    bool RestoreRecovery();
    void DiscardRecovery();
    bool AddCamera(const FString& Name, const FStudioCameraState& Camera);
    const FStudioCameraBookmark* FindCamera(const FGuid& Id) const;
    bool RenameCamera(const FGuid& Id, const FString& Name);
    bool UpdateCamera(const FGuid& Id, const FStudioCameraState& Camera);
    bool DuplicateCamera(const FGuid& Id);
    bool DeleteCamera(const FGuid& Id);
    bool RestoreSavedCamera(const FGuid& Id);
    bool BeginCameraPlacement(const FGuid& Id);
    bool EditCameraPlacement(const FStudioCameraState& Camera);
    bool PreviewCameraPlacement();
    bool ApplyCameraPlacement();
    void CancelCameraPlacement();
    bool IsCameraPlacementCurrent() const;
    const FStudioCameraPlacementDraft* CameraPlacement() const { return Placement.IsSet()?&Placement.GetValue():nullptr; }
    void SetCameraPlacementTool(EStudioCameraPlacementTool Tool);
    int32 CameraPlacementRevision=0;
    FString CameraPlacementNotice;
    bool bCameraPlacementError=false;
    bool UndoSavedCameras();
    bool RedoSavedCameras();
    bool CanUndoSavedCameras() const { return !CameraUndo.IsEmpty(); }
    bool CanRedoSavedCameras() const { return !CameraRedo.IsEmpty(); }
    FString UndoSavedCamerasLabel() const { return CameraUndo.IsEmpty()?FString():CameraUndo.Last().Label; }
    FString RedoSavedCamerasLabel() const { return CameraRedo.IsEmpty()?FString():CameraRedo.Last().Label; }
    int32 CameraCollectionRevision = 0;
    FString CameraCollectionNotice;
    bool bCameraCollectionError = false;
    FStudioInspectionState InspectionState() const;
    FStudioInspectionSource InspectionSource() const;
    const FStudioInspectionObject* FindInspectionObject(const FGuid& Id) const;
    const FStudioSeedObject* FindSeed(const FGuid& Id) const;
    bool AddSeed(FStudioSeedObject Seed);
    bool EditSeed(const FGuid& Id,TFunctionRef<void(FStudioSeedObject&)> Edit);
    const FStudioSliceObject* FindSlice(const FGuid& Id) const;
    const FStudioProbeObject* FindProbe(const FGuid& Id) const;
    const FStudioRulerObject* FindRuler(const FGuid& Id) const;
    bool AddSlice(FStudioSliceObject Slice);
    bool AddProbe(FStudioProbeObject Probe);
    bool AddRuler(FStudioRulerObject Ruler);
    bool EditSlice(const FGuid& Id,TFunctionRef<void(FStudioSliceObject&)> Edit);
    bool EditProbe(const FGuid& Id,TFunctionRef<void(FStudioProbeObject&)> Edit);
    bool EditRuler(const FGuid& Id,TFunctionRef<void(FStudioRulerObject&)> Edit);
    bool RenameInspectionObject(const FGuid& Id,const FString& Name);
    bool DuplicateInspectionObject(const FGuid& Id);
    bool DeleteInspectionObject(const FGuid& Id);
    bool SetInspectionObjectVisible(const FGuid& Id,bool bVisible);
    bool SelectInspectionObject(const FGuid& Id);
    FGuid SelectedInspectionObject;
    int32 InspectionObjectsRevision=0,InspectionSelectionRevision=0;
    FString InspectionNotice;
    bool bInspectionError=false;
    bool EditView(const FString& Label, TFunctionRef<void(FStudioInspectionState&)> Edit);
    bool EditCamera(const FString& Label, const FStudioCameraState& Camera);
    void BeginViewEdit(const FString& Label) { ViewHistory.Begin(Label,InspectionState()); }
    void EndViewEdit() { ViewHistory.End(); }
    bool IsViewEditActive() const { return ViewHistory.IsEditing(); }
    bool UndoView();
    bool RedoView();
    bool CanUndoView() const { return ViewHistory.CanUndo(); }
    bool CanRedoView() const { return ViewHistory.CanRedo(); }
    FString UndoViewLabel() const { return ViewHistory.UndoLabel(); }
    FString RedoViewLabel() const { return ViewHistory.RedoLabel(); }
    bool SaveProject(const FString& Path);
    bool LoadProject(const FString& Path);
    /** Interactive opens prepare the whole candidate off-thread and replace atomically. */
    bool RequestProjectOpen(const FString& Path, const FString& ReplacedRecentPath = FString(), bool bRecovery = false,
        const FString& RecordingReplacement = FString(), const FString& ReconstructionReplacement = FString(), bool bRemoveReconstruction = false);
    bool RequestRecoveryOpen();
    void CancelProjectOpen(bool bNotify = true);
    bool IsProjectOpenPending() const { return PendingProjectOpen.IsValid(); }
    bool IsProjectOpening() const { return IsProjectOpenPending() && ProjectLoadCancellation && !ProjectLoadCancellation->load(); }
    FString ProjectOpenStatus() const;
    TOptional<FStudioRecordingRepair> RecordingRepair;
    bool RetryRecordingRepair(const FString& ReplacementPath);
    bool RetryWithoutReconstruction();
    void DismissRecordingRepair() { RecordingRepair.Reset(); }
    static bool IsWorkspaceAvailable(EStudioWorkspace Destination);
    bool Navigate(EStudioWorkspace Destination);
    bool CreateProject(const FString& Path, const FString& Name);
    bool DuplicateProject(const FString& Path, const FString& Name);
    void RefreshProjectCatalog();
    void ForgetRecentProject(const FString& Path);
    bool SetProjectFavorite(const FString& Path, bool bFavorite);
    bool RenameProject(const FString& Name);
    /** All authoring writes go through an atomic edit; view/playback are excluded. */
    bool EditCase(const FString& Label, TFunctionRef<void(FStudioCaseDraft&)> Edit);
    bool AddMaterial(bool bSolid,FGuid& OutId);
    bool UpdateMaterial(const FStudioMaterial& Material);
    bool DuplicateMaterial(const FGuid& Id,FGuid& OutId);
    bool DeleteMaterial(const FGuid& Id,bool bUnassign);
    bool AssignDomainMaterial(const FGuid& Id);
    bool AssignGeometryMaterial(const FGuid& GeometryId,const FGuid& MaterialId);
    bool UpdateBoundary(const FStudioBoundaryCondition& Boundary,bool bUnpairExisting=false);
    bool UpdateLatticeResolution(const FIntVector& Resolution);
    TSharedPtr<const FStudioLatticePreview,ESPMode::ThreadSafe> LatticePreview;
    TSharedPtr<const FStudioLatticePreviewProgress,ESPMode::ThreadSafe> LatticeProgress;
    FString LatticeNotice;
    bool RequestLatticePreview(const FStudioLatticePreviewSettings& Settings);
    void CancelLatticePreview();
    bool IsBuildingLatticePreview() const { return PendingLatticePreview.IsValid(); }
    bool RemoveBoundary(const FGuid& Target);
    bool UndoCase();
    bool RedoCase();
    bool CanUndoCase() const { return !CaseUndo.IsEmpty(); }
    bool CanRedoCase() const { return !CaseRedo.IsEmpty(); }
    FString UndoCaseLabel() const { return CaseUndo.IsEmpty()?FString():CaseUndo.Last().Label; }
    FString RedoCaseLabel() const { return CaseRedo.IsEmpty()?FString():CaseRedo.Last().Label; }
    bool ExportField(const FString& Path) const { return Solver->ExportField(SelectedFrame,Path); }
private:
    FString StorageDirectory;
    FString SavedSnapshot;
    double AutosaveSeconds = 0;
    double DirtyCheckSeconds = 0;
    struct FCaseEdit { FString Label; FString Before; FString After; };
    TArray<FCaseEdit> CaseUndo;
    TArray<FCaseEdit> CaseRedo;
    static constexpr int32 MaxCaseEdits = 64;
    static constexpr int64 MaxCaseHistoryBytes = 16*1024*1024;
    FStudioViewHistory ViewHistory;
    struct FCameraCollectionEdit
    {
        FString Label;
        TArray<FStudioCameraBookmark> Before,After;
    };
    TArray<FCameraCollectionEdit> CameraUndo,CameraRedo;
    TOptional<FStudioCameraPlacementDraft> Placement;
    bool CommitCameras(const FString& Label,TArray<FStudioCameraBookmark> Cameras);
    bool ApplyCameraCollectionHistory(bool bRedo);
    bool CameraCollectionMessage(const FString& Message,bool bError = false);
    void ClearCameraCollectionHistory();
    void ClearViewHistory() { ViewHistory.Clear(); ++CameraRevision; ClearCameraCollectionHistory();
        SelectedInspectionObject.Invalidate();++InspectionObjectsRevision;++InspectionSelectionRevision;InspectionNotice.Empty();bInspectionError=false; }
    bool CommitInspectionObjects(const FString& Label,FStudioInspectionObjects Objects,bool bContinueGesture=false,bool bUseSavedSeeds=false);
    bool PrepareInspectionObject(FStudioInspectionObject& Object,const TCHAR* BaseName);
    FString UniqueInspectionName(const FString& Base) const;
    bool InspectionMessage(const FString& Message,bool bError=false);
    void ApplyInspection(const FStudioInspectionState& State);
    bool ApplyViewHistory(bool bRedo);
    void ClearCaseHistory() { CaseUndo.Reset(); CaseRedo.Reset(); InvalidateAssets(); InvalidateGeometry(); }
    bool ApplyCaseHistory(bool bRedo);
    TFuture<TArray<FStudioProjectSummary>> PendingCatalog;
    bool bCatalogRefreshPending = false;
    void PollProjectCatalog();
    TFuture<FStudioRecordingLoadResult> PendingRecording;
    FStudioLoadCancellation RecordingCancellation;
    FGuid RecordingProjectId;
    FString RequestedRecordingId;
    bool bRelinkingRecording = false;
    enum class ERecordingChange { Select, Import, Relink, ImportSurface, RelinkSurface, RemoveSurface };
    ERecordingChange RecordingChange=ERecordingChange::Select;
    bool StartRecordingRequest(const FString& Id,const FString& Path,ERecordingChange Change);
    void PollRecording();
    TFuture<FStudioProjectLoadResult> PendingProjectOpen;
    FStudioLoadCancellation ProjectLoadCancellation;
    TSharedPtr<std::atomic<EStudioProjectLoadStage>, ESPMode::ThreadSafe> ProjectLoadStage;
    FString OpeningProjectPath, OpeningPreviousPath, OpeningContent, OpeningReplacedRecent;
    bool bOpeningRecovery = false;
    bool bOpeningLegacy = false;
    void PollProjectOpen();
    void ApplyLoadedProject(FStudioProject Candidate, TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> Source, const FString& Path, bool bRecovery);
    void InvalidateAssets();
    void PollAssets();
    bool ApplyAssetLocation(const FStudioAssetReference& Source,const FString& Path);
    TFuture<FStudioAssetCheckResult> PendingAssets;
    TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe> AssetCancellation;
    uint64 AssetGeneration = 0;
    uint64 CheckingAssetGeneration = 0;
    FGuid CheckingAssetProject;
    bool bRefreshAssets = true;
    bool bLocatingAsset = false;
    FStudioAssetReference LocatedAsset;
    FString LocatedPath;
    TFuture<FStudioMeshImportResult> PendingMesh;
    TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe> MeshCancellation;
    FGuid ReadingMeshProject,ReadingGeometryId;
    uint64 MeshGeneration=0,ReadingMeshGeneration=0;
    bool bReadingImport=false;
    bool bReloadGeometry=false;
    TFuture<FStudioDomainGeometry> PendingDomainGeometry;
    TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe> DomainCancellation;
    FGuid DomainProject,ReadingDomainProject;
    FString DomainGeometryKey,ReadingDomainKey;
    bool bReloadDomainGeometry=false;
    void PollDomainGeometry();
    void InvalidateDomainGeometry();
    TFuture<FStudioLatticePreview> PendingLatticePreview;
    TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe> LatticeCancellation;
    FGuid LatticeProject;
    FString LatticeKey;
    FStudioLatticePreviewSettings LatticeSettings;
    void PollLatticePreview();
    void InvalidateLatticePreview(bool bForce=false);
    void PollGeometry();
    void InvalidateGeometry();
    bool StartMeshRead(const FString& Path,bool bImport,const FGuid& Id);
    void UseRecording(TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> Source);
    TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> PrepareRecording(const FStudioProject& Candidate,int32 Frame);
    void RememberProject();
    void BeginRun(); void Advance();
    void ResetJobSession();
    void TickJob(double Delta);
    void SyncJob();
    TUniquePtr<FStudioJobController> JobController;
    FStudioControlHarness* ControlHarness = nullptr; // Owned by JobController.
    double JobClock = 0;
    double JobStartedAt = 0;
    EStudioJobState LastJobState = EStudioJobState::Idle;
    FString LastJobNotice;
};
