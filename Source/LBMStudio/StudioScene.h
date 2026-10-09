#pragma once
#include "CoreMinimal.h"
#include "Async/Future.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "StudioModel.h"
#include "StudioFieldDisplay.h"
#include "StudioMeshDisplay.h"
#include "StudioScene.generated.h"

class UProceduralMeshComponent;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UTexture2D;
class SStudioWorkspace;
class UStudioVolumeComponent;
struct FStudioGeometry;
struct FStudioSnapshot;
struct FStudioProbeMarkerResult;
struct FStudioPipelineOutput;

struct FStudioSceneResourceStats
{
    int64 MeshBytes=0,Vertices=0,Indices=0,RenderTargetBytes=0,ScalarTextureBytes=0;
    int32 Workers=0,Sections=0;
    uint64 CancelledBuilds=0,DiscardedBuilds=0;
};

UCLASS()
class AStudioScene : public AActor
{
    GENERATED_BODY()
public:
    AStudioScene();
    void Initialize(TSharedRef<FStudioModel> InModel);
    /** An independent scene for evaluated output; ordinary source rendering
     * never substitutes for a clipped/sliced/contoured pipeline result. */
    bool InitializePipeline(TSharedRef<FStudioSnapshotSource,ESPMode::ThreadSafe> Snapshot);
    TSharedPtr<const FStudioPipelineOutput,ESPMode::ThreadSafe> PipelineOutput() const {return FrozenPipelineOutput;}
    /** Additional visibility gate for independent inspection scenes. Evaluated
     * on the game thread even when their Slate workspace is hidden. */
    void SetViewVisibility(TFunction<bool()> Visible) { ViewVisibility=MoveTemp(Visible); }
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    void FitCamera();
    /** Undoable presentation of the current original field; never changes its frame or values. */
    void FlowOverview();
    void Orbit(double DX,double DY);
    void Pan(double DX,double DY);
    void Zoom(double Amount);
    void Look(double DX,double DY);
    void Fly(const FVector& LocalDirection,double Delta);
    void SetCameraPosition(const FVector& Position);
    void SetCameraRotation(const FRotator& Rotation);
    void SetCameraMode(bool bFree);
    void AlignCamera(const FIntVector& Direction);
    void RestoreCamera(const FStudioCameraState& State, const FString& Label);
    FVector CameraPosition() const;
    FRotator CameraRotation() const;
    FStudioCameraState CameraState() const;
    FStudioCameraState SavedCameraState() const { return Model?Model->Project.Camera:CameraState(); }
    void ApplyCamera(const FStudioCameraState& State);
    bool SetDepthClipping(bool bEnabled,double NearMeters,double FarMeters);
    void ResizeViewport(int32 Width,int32 Height,bool bExact=false);
    bool Snapshot(const FString& Path);
    /** Applies any pending camera capture, then freezes and renders a bounded
     * separate export target without advancing replay or replacing the live target. */
    bool CaptureSnapshot(FStudioSnapshot& Snapshot,const FStudioProbeMarkerResult* Markers,FString& Error);
    bool HasCurrentFrame() const;
    bool HasPresentedFrame() const { return !PresentedDataset.IsEmpty(); }
    bool HasGeometryPreview() const { return bGeometryView&&!bDomainView&&Model&&PreviewBounds.IsValid&&PreviewRevision==Model->GeometryRevision&&!PendingPreview.IsValid(); }
    bool HasDomainPreview() const { return bDomainView&&Model&&PreviewBounds.IsValid&&PreviewRevision==Model->DomainPreviewRevision&&!PendingPreview.IsValid(); }
    bool HasBoundaryPreview() const { return bBoundaryView&&HasDomainPreview()&&!bCaptureDirty; }
    bool HasLatticePreview() const { return bLatticeView&&HasDomainPreview()&&!bCaptureDirty; }
    FBox GetPreviewBounds() const { return PreviewBounds; }
    FStudioFrame PresentedFrame() const { return CapturedFrame; }
    FString PresentedSource() const { return PresentedTitle; }
    FString PresentedDatasetId() const { return PresentedDataset; }
    const FStudioScalarDescriptor& PresentedScalar() const { return CapturedScalar; }
    const FStudioColorMapping& PresentedColorMapping() const { return CapturedColorMapping; }
    const FStudioVectorSummary& PresentedVectors() const { return CapturedVectors; }
    const FStudioStreamlineSummary& PresentedStreams() const { return CapturedStreams; }
    const FStudioMeshSummary& PresentedMesh() const { return CapturedMesh; }
    FGuid PresentedProjectId() const { return CapturedProjectId; }
    FStudioCameraState PresentedCamera() const { return CapturedCamera; }
    double PresentedNearClipMeters() const { return CapturedNearClipMeters; }
    FIntPoint PresentedViewportSize() const { return CapturedViewportSize; }
    /** Same immutable field as the captured pixels. Never reads the requested
     * playback cursor, and returns empty outside a presented flow frame. */
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> PresentedField() const { return HasPresentedFrame()?CapturedField:nullptr; }
    FString PresentedSliceNotice(const FGuid& Id) const { const auto* Notice=CapturedSliceNotices.Find(Id);return Notice?*Notice:FString(); }
    UTextureRenderTarget2D* GetRenderTarget() const { return RenderTarget; }
    uint64 GetCaptureCount() const { return CaptureCount; }
    TOptional<double> LastCaptureSubmitMilliseconds() const { return CaptureSubmitMs; }
    double PresentedBuildMilliseconds() const { return CapturedBuildMs; }
    FBox GetRenderedFlowBounds() const { return RenderedFlowBounds; }
    FStudioSceneResourceStats ResourceStats() const;
    TSharedPtr<FStudioModel> Model;
    bool bFreeCamera=false;
    bool bBuilding=false;
    double RenderMilliseconds=0;
private:
    TSharedPtr<const FStudioPipelineOutput,ESPMode::ThreadSafe> FrozenPipelineOutput;
    TFunction<bool()> ViewVisibility;
    TOptional<double> CaptureSubmitMs;
    double CapturedBuildMs=0;
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> Mesh;
    UPROPERTY() TObjectPtr<UProceduralMeshComponent> Backdrop;
    UPROPERTY() TObjectPtr<UStudioVolumeComponent> VolumeComponent;
    UPROPERTY() TObjectPtr<USceneCaptureComponent2D> Capture;
    UPROPERTY() TObjectPtr<UTextureRenderTarget2D> RenderTarget;
    UPROPERTY() TObjectPtr<UMaterialInterface> OpaqueMaterial;
    UPROPERTY() TObjectPtr<UMaterialInterface> BodyMaterial;
    UPROPERTY() TObjectPtr<UMaterialInterface> MeshEdgeMaterial;
    UPROPERTY() TObjectPtr<UMaterialInterface> TransparentMaterial;
    UPROPERTY() TObjectPtr<UMaterialInterface> ScalarMaterial;
    UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> ScalarInstance;
    UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> FocusScalarInstance;
    UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> InspectionInstance;
    UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> OriginalSliceInstance;
    TMap<FGuid,FString> GeometrySliceNotices,CapturedSliceNotices;
    UPROPERTY(Transient) TObjectPtr<UTexture2D> ScalarTexture;
    TFuture<TSharedPtr<FStudioGeometry>> PendingGeometry;
    FStudioLoadCancellation GeometryCancellation;
    TWeakPtr<IStudioSolver,ESPMode::ThreadSafe> BuildingSolver,RenderedSolver,CapturedSolver;
    FGuid BuildingProjectId,RenderedProjectId,CapturedProjectId;
    uint64 BuildingIntentRevision=0,RenderedIntentRevision=0,CapturedIntentRevision=0;
    int32 CapturedRevision=-1;
    int32 RenderedRevision=-1;
    int32 BuildingRevision=-1;
    int32 AppliedCameraRevision=-1;
    FString RenderedDataset,RenderedTitle,PresentedDataset,PresentedTitle;
    FStudioFrame GeometryFrame,CapturedFrame;
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> RenderedField,CapturedField;
    FStudioScalarDescriptor GeometryScalar,CapturedScalar;
    FStudioColorMapping GeometryColorMapping,CapturedColorMapping;
    FStudioVectorSummary GeometryVectors,CapturedVectors;
    FStudioStreamlineSummary GeometryStreams,CapturedStreams;
    FStudioMeshSummary GeometryMesh,CapturedMesh;
    FBox RenderedFlowBounds=FBox(ForceInit);
    bool bCaptureDirty=true;
    bool bGeometryView=false;
    bool bDomainView=false,bBuildingDomainPreview=false;
    bool bBoundaryView=false,bBuildingBoundaryPreview=false;
    bool bLatticeView=false,bBuildingLatticePreview=false;
    FStudioCameraState GeometryCamera,DomainCamera;
    FGuid DomainCameraProject;
    FBox PreviewBounds=FBox(ForceInit);
    TFuture<TSharedPtr<FStudioGeometry>> PendingPreview;
    FStudioLoadCancellation PreviewCancellation;
    FGuid BuildingPreviewProjectId;
    int32 PreviewRevision=-1,BuildingPreviewRevision=-1;
    bool bFitPreview=true;
    void UpdateGeometryPreview();
    void UpdateBackdrop();
    void UpdateDomainPreview();
    uint64 CaptureCount=0;
    uint64 CancelledBuildCount=0,DiscardedBuildCount=0;
    FStudioCameraState CapturedCamera;
    double CapturedNearClipMeters=1.e-6;
    FIntPoint CapturedViewportSize=FIntPoint::ZeroValue;
    FTransform LastCapturedTransform;
    FVector Focus=FVector(50,0,0);
    double CameraDistance=520;
    void RequestGeometry();
    bool IsGeometryRequestCurrent() const;
    void CancelBuild(const FStudioLoadCancellation& Cancellation);
    void ApplyGeometry(const FStudioGeometry& Geometry);
    void CaptureIfChanged();
    void UpdateProjection();
    bool bCameraDepthClipping=false;
    double CameraNearClipMeters=.001,CameraFarClipMeters=1000.;
};

UCLASS()
class AStudioGameMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    AStudioGameMode();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    UPROPERTY() TObjectPtr<AStudioScene> Scene;
    TSharedPtr<SStudioWorkspace> Workspace;
    bool bPreviousWorldRenderingDisabled=false;
};
