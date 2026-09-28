#pragma once
#include "StudioDomain.h"

/** Authoring preview divides the applied domain into these physical cells.
 * Backend node placement and numerical preparation remain separately attested. */
struct FStudioLatticeLayout
{
    FBox Bounds=FBox(ForceInit);
    FIntVector Resolution=FIntVector::ZeroValue;
    FVector Spacing=FVector::ZeroVector;
    uint64 Cells=0;
    bool Cell(const FIntVector& Index,FVector& Center,FBox& Box) const;
};
struct FStudioLatticeEdit
{
    FIntVector Saved=FIntVector::ZeroValue;
    FString Counts[3],RequestedSpacing,Error;
    int32 ErrorField=INDEX_NONE;
    void Reset(const FIntVector& Resolution);
    bool IsDirty() const;
    bool Build(FIntVector& Out);
    /** Ceiling per axis guarantees spacing no larger than the requested value. */
    bool UseSpacing(const FStudioDomain& Domain);
};
struct FStudioLatticeCapabilities
{
    FString BackendId;
    bool bLayoutRulesSupplied=false;
    TOptional<uint64> MaximumCells,BytesPerCell,FixedBytes;
    bool bUniformSpacingRequired=false;
    double SpacingRelativeTolerance=1.e-9;
    bool bRefinementSupported=false;
};
struct FStudioLatticeValidation
{
    bool bBackendKnown=false;
    TOptional<uint64> EstimatedBytes;
    TArray<FString> Issues;
    bool Compatible() const { return bBackendKnown&&Issues.IsEmpty(); }
};
struct FStudioLatticePreviewSettings
{
    int32 Axis=2; // -1: whole domain; 0/1/2: one actual cell layer.
    int32 Layer=0;
    int32 MaximumSamples=32768;
};
struct FStudioLatticeSamplePlan
{
    FStudioLatticeLayout Layout;
    FStudioLatticePreviewSettings Settings;
    uint64 CandidateCells=0;
    int32 Samples=0;
    bool bSampled=false;
    /** Returns an original cell index, never a larger replacement cell. */
    FIntVector Index(int32 Sample) const;
};
enum class EStudioLatticeCell : uint8 { OutsideGeometry, InsideClosedGeometry, Surface, Unknown };
struct FStudioLatticeCellSample
{
    FIntVector Index=FIntVector::ZeroValue;
    EStudioLatticeCell Kind=EStudioLatticeCell::Unknown;
    FGuid SurfacePatch;
};
struct FStudioLatticePreviewProgress
{
    std::atomic<int32> Stage{0}; // 0 waiting, 1 geometry index, 2 cells, 3 finished.
    std::atomic<int32> Completed{0},Total{0};
};
struct FStudioLatticePreview
{
    FString Key,Error;
    FStudioLatticeSamplePlan Plan;
    TArray<FStudioLatticeCellSample> Samples;
    int32 Outside=0,Inside=0,Surface=0,Unknown=0;
    bool bCancelled=false;
    bool Complete() const { return !bCancelled&&Error.IsEmpty()&&Samples.Num()==Plan.Samples&&Plan.Samples>0; }
};
namespace StudioLattice
{
    constexpr int32 MaximumResolution=1048576;
    constexpr int32 MaximumPreviewSamples=32768;
    bool Layout(const FStudioDomain& Domain,const FIntVector& Resolution,FStudioLatticeLayout& Out,FString& Error);
    bool SamplePlan(const FStudioLatticeLayout& Layout,const FStudioLatticePreviewSettings& Settings,FStudioLatticeSamplePlan& Out,FString& Error);
    FStudioLatticeValidation Validate(const FStudioCaseDraft& Case,const FStudioLatticeCapabilities* Backend=nullptr);
    FString PreviewKey(const FStudioCaseDraft& Case,const FStudioLatticePreviewSettings& Settings);
    /** CPU geometry classification only; never production meshing or CFD. */
    FStudioLatticePreview Preview(const FStudioCaseDraft& Case,const TSharedPtr<const FStudioDomainGeometry,ESPMode::ThreadSafe>& Geometry,
        const FStudioLatticePreviewSettings& Settings,const FStudioAssetCancellation& Cancel,const TSharedRef<FStudioLatticePreviewProgress,ESPMode::ThreadSafe>& Progress);
}
