#pragma once

#include "Async/Future.h"
#include "StudioColor.h"
#include "StudioFieldDisplay.h"
#include "StudioInspectionOverlay.h"
#include "StudioMeshDisplay.h"

class IStudioField;

struct FStudioSnapshotOptions
{
    FIntPoint Size=FIntPoint(1920,1080);
    bool bAnnotations=true,bLegend=true,bFrameInfo=true;
};

/** Centered output window within the presented camera. A new aspect crops
 * the existing view; it never moves the camera or stretches scientific pixels. */
struct FStudioSnapshotFraming
{
    FVector2D Minimum=FVector2D::ZeroVector,Span=FVector2D(1,1);
    FVector2D ToOutput(FVector2D SourcePixel,FVector2D SourceSize,FVector2D OutputSize) const;
};

/** Value-only export: no model, field, renderer, UObject or source-file owner. */
struct FStudioSnapshot
{
    FStudioSnapshotOptions Options;
    FStudioFieldIdentity Identity;
    FGuid Project,SelectedObject;
    FString SourceTitle,ProbeCSV,MetadataFile,OriginalSourceJSON;
    EStudioHome4UnitDisplay UnitDisplay=EStudioHome4UnitDisplay::Lattice;
    TOptional<FStudioHome4Spec> SourceUnitMap;
    uint64 Capture=0;
    FStudioCameraState Camera;
    FIntPoint SourceSize=FIntPoint::ZeroValue;
    FStudioSnapshotFraming Framing;
    FMatrix Projection=FMatrix::Identity;
    FStudioScalarDescriptor Scalar;
    FStudioColorMapping Mapping;
    FStudioViewSettings DisplaySettings;
    FStudioVectorSummary Vectors;
    FStudioStreamlineSummary Streams;
    FStudioMeshSummary Mesh;
    FBox FlowBounds=FBox(ForceInit);
    bool bVolumeRendererActive=false;
    TMap<FGuid,FString> SliceNotices;
    FStudioInspectionObjects Objects;
    StudioInspectionOverlay::FGeometry Overlay;
    TArray<FColor> Pixels;
};

namespace StudioSnapshot
{
    bool ValidSize(FIntPoint Size);
    FStudioSnapshotFraming Frame(FIntPoint Source,FIntPoint Output);
    /** PNG includes a UTF-8 iTXt chunk named LBMStudio with complete frame,
     * camera, crop, scalar, mapping, inspection and optional probe identity. */
    bool Encode(const FStudioSnapshot& Snapshot,TArray64<uint8>& PNG,FString& Error);
    FString Metadata(const FStudioSnapshot& Snapshot);
    /** Capture original-grid map and actual clipping statistics from the immutable displayed frame. */
    FString SourceMetadata(const IStudioField& Field,const FString& Scalar,const FStudioColorMapping& Mapping,bool AirMask);
}

enum class EStudioSnapshotExportState : uint8 { Encoding,Cancelled,Writing,Complete };
struct FStudioSnapshotExportResult
{
    bool bSuccess=false,bCancelled=false;
    FString Path,Error,SidecarPath;
    FStudioFrame Frame;
};

/** One bounded encode/write. Cancellation succeeds only before the atomic
 * replacement starts; destruction cancels encoding and joins the worker. */
class FStudioSnapshotExportTask
{
public:
    ~FStudioSnapshotExportTask();
    bool Start(FStudioSnapshot Snapshot,const FString& Path);
    bool IsBusy() const { return Pending.IsValid(); }
    bool Cancel();
    EStudioSnapshotExportState State() const;
    TOptional<FStudioSnapshotExportResult> Poll();
private:
    TFuture<FStudioSnapshotExportResult> Pending;
    TSharedPtr<std::atomic<EStudioSnapshotExportState>,ESPMode::ThreadSafe> Progress;
#if WITH_DEV_AUTOMATION_TESTS
    friend class FStudioSnapshotLifecycleTest;
    // Gates the actual worker at the encoding/write boundary for race tests.
    TFunction<void()> BeforeWriteForAutomation;
#endif
};
