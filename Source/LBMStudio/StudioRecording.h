#pragma once
#include "CoreMinimal.h"
#include <atomic>

using FStudioLoadCancellation = TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe>;

struct FStudioFrame
{
    int32 Index = 0;
    double Time = 0;
};

struct FStudioScalarDescriptor
{
    FString Id = TEXT("velocity_magnitude"), Label = TEXT("Velocity magnitude"), Unit = TEXT("m/s");
    double Minimum = 0, Maximum = 400;
    FString Origin = TEXT("derived");
};

enum class EStudioFieldInterpolation : uint8 { None, SourceTriangles, ReconstructedTriangles, ReconstructedGrid };

/** Identity is carried by the immutable field itself, not a mutable playback
 * cursor or a caller-supplied label. Used by samples and frozen exports. */
struct FStudioFieldIdentity
{
    FString Dataset, MetadataSHA256, PayloadSHA256, ReconstructionSHA256;
    int32 Ordinal = INDEX_NONE;
    FStudioFrame Frame;
    int32 SpatialDimensions = 0;
    FVector SourceOffset = FVector::ZeroVector;
    EStudioFieldInterpolation Interpolation = EStudioFieldInterpolation::None;
};

/** Metadata belongs to the recording, independently of any editable case. */
struct FStudioRecordingDescriptor
{
    FString Id, Title, SourceURL, SourceLabel, TimeNote, FieldNote;
    FString MetadataSHA256, PayloadSHA256;
    int32 SpatialDimensions = 2;
    int32 NodeCount = 0, TriangleCount = 0;
    FVector SourceOffset = FVector::ZeroVector;
    FBox DisplayBounds = FBox(ForceInit);
    TArray<FStudioFrame> Frames;
    TArray<FStudioScalarDescriptor> Scalars = { FStudioScalarDescriptor() };
    FString DefaultScalar = TEXT("velocity_magnitude");
    bool bSourcePoints = false, bPointVelocity = false;
    // This reader's format is explicitly 2D, node-associated U/V/pressure/density.
    // The display extrusion never advertises a third velocity component.
    bool bHasResidualHistory = false, bHasForceHistory = false;
};

struct FStudioFrameCacheStats
{
    int64 BudgetBytes = 0, ResidentBytes = 0;
    int32 ResidentFrames = 0;
    uint64 Loads = 0, Hits = 0;
};

/** Process-wide recorded-data ownership, including evicted but pinned frames. */
struct FStudioRecordingLiveStats
{
    int32 Readers = 0, Frames = 0;
    int64 FrameBytes = 0;
};

struct FStudioRecordingEntry
{
    FString Id, Title, Path;
};

struct FStudioReconstructionReference
{
    FString Path, MetadataSHA256;
};

/** Legacy pairs pin both files; point_v3 pins the descriptor and transitively every
 * member hash in it. Relocation never changes the scientific identity. */
struct FStudioRecordingReference
{
    FString Id, Title, Path, MetadataSHA256, PayloadSHA256;
    FString Format = TEXT("flow_v2");
    TOptional<FStudioReconstructionReference> Reconstruction;
};

class IStudioSolver;
struct FStudioRecordingLoadResult
{
    TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> Source;
    TOptional<FStudioRecordingReference> Reference;
    FString Error;
    bool bReconstructionFailed = false;
};
namespace StudioRecordings
{
    FStudioRecordingLiveStats LiveStats();
    TArray<FStudioRecordingEntry> Installed();
    FString PathForId(const FString& Id);
    FString DescriptorOrPayloadInFolder(const FString& Folder);
    bool IsValidReference(const FStudioRecordingReference& Reference);
    /** Worker-only, bounded streaming verification followed by first-frame preparation. */
    FStudioRecordingLoadResult Import(const FString& Path, int32 Frame, const FStudioLoadCancellation& Cancellation);
    FStudioRecordingLoadResult ImportReconstruction(const FStudioRecordingReference& Source, const FString& Path,
        int32 Frame, const FStudioLoadCancellation& Cancellation);
    FStudioRecordingLoadResult Open(const FString& Id, const TArray<FStudioRecordingReference>& References,
        int32 Frame, const FStudioLoadCancellation& Cancellation, const FString& ReplacementPath = FString());
}
