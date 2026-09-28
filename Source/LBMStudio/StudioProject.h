#pragma once
#include "CoreMinimal.h"
#include "StudioCase.h"
#include "StudioJobs.h"
#include "StudioRecording.h"
#include "StudioColor.h"
#include "StudioInspectionObjects.h"
#include "StudioStreamlines.h"
#include "StudioMonitor.h"
#include "StudioComparison.h"

/** All coordinates in the document are solver-space meters. */
struct FStudioCameraState
{
    FVector Position = FVector(-.8, 6.9, 1.25);
    FVector Focus = FVector(.6, 0, 0);
    FQuat Orientation = (Focus-Position).Rotation().Quaternion();
    double OrbitDistance = (Focus-Position).Size();
    double FieldOfView = 48;
    double OrthoWidth = 6;
    // Distances along the camera's forward axis, independent of source/domain.
    // Disabled retains the renderer defaults used by older project documents.
    bool bDepthClipping = false;
    double NearClipMeters = .001;
    double FarClipMeters = 1000.;
    bool bOrthographic = false;
    bool bFreeCamera = false;
};

struct FStudioViewSettings
{
    bool bStreamlines = true;
    bool bVectors = true;
    bool bCutPlane = true;
    bool bVolume = true;
    bool bMesh = false;
    // 0: field layers, 1: field layers + actual triangle edges,
    // 2: edges replacing scalar surface/point fill (other tools stay available).
    int32 MeshStyle = 0;
    int32 SliceAxis = 1;
    double SlicePosition = 0;
    double StreamlineDensity = .55;
    FStudioStreamlineSettings StreamlineSettings;
    double VectorScale = 1;
    int32 VectorCount = 384;
    bool bUniformVectors = false;
    double VolumeOpacity = .28;
    FVector VolumeClipMinimum = FVector::ZeroVector;
    FVector VolumeClipMaximum = FVector::OneVector;
    FVector VolumeOpacityCurve = FVector(0,.4,1);
    double VolumeStepVoxels = 1;
    // Scalar thresholds and isovalue are physical values in the selected field unit.
    double VolumeThresholdMinimum = 0, VolumeThresholdMaximum = 1;
    bool bVolumeThreshold = false;
    bool bVolumeIsosurface = false;
    double VolumeIsovalue = .5;
    double PlaybackRate = 1;
    bool bLoopPlayback = false;
    FString ScalarField; // Empty selects the recording's declared default.
    bool bSourcePoints = true;
    bool bReconstructedSurface = true; // Effective only with an explicitly attached reconstruction.
    double PointSize = 1;
    TArray<FStudioScalarStyle> ScalarStyles;
    FStudioInspectionObjects InspectionObjects;
};

struct FStudioCameraBookmark
{
    FGuid Id = FGuid::NewGuid();
    FString Name;
    FStudioCameraState Camera;
};

/** Each side owns its source location as well as the exact original-frame pin.
 * An installed source needs no external reference. Paths rebase with the project. */
struct FStudioSavedComparisonSide
{
    FString Title;
    FStudioFieldIdentity Identity;
    TOptional<FStudioRecordingReference> Reference;
    FStudioCameraState Camera;
};

struct FStudioSavedComparison
{
    FGuid Id=FGuid::NewGuid();
    FString Name,Scalar,Unit;
    FStudioComparisonAlignment Alignment;
    FStudioSavedComparisonSide Primary,Secondary;
    bool bSharedRange=true;
};

struct FStudioProject
{
    static constexpr int32 CurrentVersion = 20;
    FGuid Id = FGuid::NewGuid();
    FString Name = TEXT("Airfoil SU2 009");
    FString Dataset = TEXT("MeshGraphNets_Airfoil_test009");
    TArray<FStudioRecordingReference> Recordings;
    FStudioViewSettings View;
    FStudioCameraState Camera;
    TArray<FStudioCameraBookmark> Cameras;
    TArray<FStudioSavedComparison> Comparisons;
    FStudioCaseDraft Draft;
    FStudioMonitorSettings Monitor;
    FStudioResidualSettings Residual;
    TArray<FStudioRunRecord> Runs = { FStudioRunRecord::Recording(TEXT("Airfoil SU2 009"),TEXT("MeshGraphNets_Airfoil_test009")) };
    TArray<FStudioJobHistory> JobHistory;
    bool bControlHarness = false;
    int32 SelectedFrame = 0;
    bool bFavorite = false;
    // Recovery metadata is not a dataset location or an instruction to overwrite.
    FString RecoverySource;
    // Runtime context only. Load resolves references; Save writes a relative copy.
    // Never serialize this machine-specific directory into the document.
    FString AssetBaseDirectory;
};

namespace StudioProjectIO
{
    TSharedRef<FJsonObject> CameraToJSON(const FStudioCameraState& Camera);
    bool CameraFromJSON(const TSharedPtr<FJsonObject>& Object,FStudioCameraState& Camera);
    /** The same versioned display settings stored in project documents. */
    TSharedRef<FJsonObject> ViewToJSON(const FStudioViewSettings& View);
    FString Serialize(const FStudioProject& Project);
    bool Parse(const FString& Text, FStudioProject& Out, FString& Error);
    bool Load(const FString& Path, FStudioProject& Out, FString& Error);
    bool Save(const FString& Path, const FStudioProject& Project, FString& Error);
    FString BackupPath(const FString& Path);
    /** Adjacent temporary file, durable flush, then atomic replacement. */
    bool WriteAtomic(const FString& Path, const FString& Text, FString& Error);
}
