#pragma once
#include "CoreMinimal.h"
#include "StudioRecording.h"
#include "Async/Future.h"
#include "Dom/JsonObject.h"

/** Original archive conventions. Unset strings/anchors block conversions that need them. */
struct FStudioHome4ArchiveMapping
{
    FString AxisOrder, MetadataOrder, CoordinateUnits, VelocityUnits;
    TOptional<double> DxMeters, DtSeconds, DensityReferenceKgM3;
    double LiquidMinimum=.5, TimeOriginSeconds=0;
    bool Validate(FString& Error,bool bPhysical=true) const;
    TSharedRef<FJsonObject> ToJSON() const;
};
struct FStudioHome4ArchiveMember
{
    FString Name, DType;
    TArray<int64> Shape;
    bool bFortran=false;
    int32 ItemBytes=0;
    int64 Count=0, PayloadBytes=0;
};
struct FStudioHome4ArchiveSource
{
    FString Path,SHA256;
};
struct FStudioHome4ArchiveInspection
{
    FStudioHome4ArchiveSource Source;
    TArray<FStudioHome4ArchiveMember> Members;
    TSharedPtr<FJsonObject> OriginalRunSpec;
    FString Error;
    bool bCancelled=false;
};
enum class EStudioHome4ArchiveOutput:uint8 { Recording, VTI };
struct FStudioHome4ArchiveRequest
{
    TArray<FStudioHome4ArchiveSource> Sources;
    FStudioHome4ArchiveMapping Mapping;
    FString OutputParent, FolderName=TEXT("home4-import"),Title=TEXT("HOME4 source snapshots"),SourceURI,Attribution;
    TOptional<FIntVector> CropMinimum,CropMaximum;
    int32 PreviewStride=1;
    bool bDerivatives=true;
    FString PressureConvention; // Empty or explicitly confirmed wb_lattice.
    EStudioHome4ArchiveOutput Output=EStudioHome4ArchiveOutput::Recording;
    bool bPhysicalVTI=true;
};
enum class EStudioHome4ArchiveState:uint8 { Idle,Working,Cancelled,Publishing,Complete };
struct FStudioHome4ArchiveProgress
{
    EStudioHome4ArchiveState State=EStudioHome4ArchiveState::Idle;
    int32 CompletedFrames=0,TotalFrames=0;
    int64 Bytes=0;
};
struct FStudioHome4ArchiveResult
{
    bool bSuccess=false,bCancelled=false;
    FString Path,RecordingJSON,Error;
    int32 Frames=0;
};
namespace StudioHome4Archives
{
    constexpr int64 MaximumArchiveBytes=512LL*1024*1024,MaximumJobInputBytes=8LL*1024*1024*1024;
    constexpr int64 MaximumOutputBytes=64LL*1024*1024*1024;
    constexpr int32 MaximumSourceNodes=2000000,MaximumSelectedNodes=1000000,MaximumFrames=100000;
    FStudioHome4ArchiveInspection Inspect(const FString& Path,const FStudioLoadCancellation& Cancellation={});
    /** Frozen source metadata can populate explicit conventions; conflicts require user correction. */
    bool MappingFromMetadata(const FJsonObject& RunSpec,FStudioHome4ArchiveMapping& Out,FString& Error);
    /** Worker-only. Fully validates its private stage before exclusive publication. */
    FStudioHome4ArchiveResult Convert(const FStudioHome4ArchiveRequest& Request,const FStudioLoadCancellation& Cancellation={});
}
struct FStudioHome4ArchiveWork;
/** No UI/model pointers cross the worker boundary; the worker owns all temporary files. */
class FStudioHome4ArchiveTask
{
public:
    ~FStudioHome4ArchiveTask();
    bool Start(FStudioHome4ArchiveRequest Request,FString& Error);
    bool Cancel();
    bool IsBusy() const;
    FStudioHome4ArchiveProgress Progress() const;
    TOptional<FStudioHome4ArchiveResult> Poll();
private:
    TSharedPtr<FStudioHome4ArchiveWork,ESPMode::ThreadSafe> Work;
    TFuture<FStudioHome4ArchiveResult> Pending;
};
