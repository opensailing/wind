#pragma once
#include "StudioPipeline.h"
#include "StudioModel.h"
#include "Async/Future.h"
struct FStudioVolumeStencil;

/** One scalar node in the prepared graph. Original arrays retain source metadata;
 * derived arrays describe a Euclidean norm, with no implicit unit conversion. */
struct FStudioPipelineScalarNode
{
    FStudioScalarDescriptor Scalar;
    FString Expression;
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> Original;
    TArray<int32> Components;
};

/** Immutable numerical input for pipeline geometry and queries. Interpolation
 * happens in the verified reader before a magnitude is computed. Original row
 * access and topology deliberately ignore clip/slice masks; Includes() filters
 * those rows, and geometry evaluation must clip the returned topology.
 * Query methods apply every box/plane restriction. Contour extraction and final
 * probe tables are separate evaluator operations, not implicit field mutations. */
class FStudioPipelineField final : public IStudioField
{
public:
    bool IsValid() const override {return !Nodes.IsEmpty()&&Nodes.IsValidIndex(Selected);}
    TOptional<FStudioFieldIdentity> Identity() const override {return PinnedIdentity;}
    TOptional<FStudioScalarDescriptor> Scalar(const FString& FieldId) const override;
    FString ScalarExpression(const FString& FieldId) const override;
    bool OriginalPoint(int32 Row,int64& Id,FVector& Position) const override;
    int32 OriginalPointCount() const override;
    int32 OriginalTriangleCount() const override;
    bool OriginalTriangle(int32 Index,FIntVector& Triangle) const override;
    bool OriginalScalar(int32 Row,const FString& FieldId,double& Out) const override;
    bool SampleScalar(const FVector& Position,const FString& FieldId,double& Out) const override;
    /** Geometry construction queries original support before clipping. */
    bool SampleUnderlyingScalar(const FVector& Position,const FString& FieldId,double& Out) const;
    /** Verified grid-node stencil, interpolating components before magnitude.
     * Caller still verifies full fluid-cell coverage before emitting geometry. */
    bool SampleVolumeNode(int32 Index,const FString& FieldId,double& Out) const;
    bool Sample(const FVector&,FStudioFieldValue&) const override {return false;}
    bool SampleVelocity(const FVector&,FVector&) const override {return false;}
    bool IsSolid(const FVector& Position) const override;
    bool SupportsSegment(const FVector& A,const FVector& B,const FStudioLoadCancellation& Cancellation={}) const override;
    const TArray<FVector2D>& Boundary() const override;
    const TArray<FIntVector>& BoundaryTriangles() const override;
    TSharedPtr<const FStudioSurfaceReconstruction,ESPMode::ThreadSafe> Reconstruction() const override;
    TSharedPtr<const FStudioVolumeReconstruction,ESPMode::ThreadSafe> VolumeReconstruction() const override;
    int32 MeshTriangleCount() const override;
    bool MeshTriangle(int32 Index,FVector (&Positions)[3]) const override;
    bool Includes(const FVector& ScenePosition) const;
    /** The immutable graph belongs to this exact operation list and source pin. */
    bool MatchesRecipe(const FStudioSavedPipeline& Recipe) const;
    /** Share immutable numerical inputs with an updated name/camera only.
     * Any source, operation or other recipe change requires fresh evaluation. */
    TSharedPtr<const FStudioPipelineField,ESPMode::ThreadSafe> WithPresentation(const FStudioSavedPipeline& Recipe) const;
    const FStudioScalarDescriptor& SelectedScalar() const {return Nodes[Selected].Scalar;}
    const TArray<FStudioPipelineScalarNode>& Scalars() const {return Nodes;}
    const FBox& DomainBounds() const {return Bounds;}
    bool HasEmptyDomain() const {return !Bounds.IsValid;}
    /** Share immutable inputs with the earlier domain of a geometry stage. */
    TSharedRef<const FStudioPipelineField,ESPMode::ThreadSafe> WithDomain(const FBox& Domain) const;
    const TOptional<FStudioPipelineOperation>& Slice() const {return Plane;}
    /** Logical original scalar rows retained, excluding reader cache/geometry,
     * legacy packed-frame members and allocator overhead. */
    int64 InputValueBytes() const {return ValueBytes;}
private:
    friend struct FStudioPipelineFieldBuilder;
    TArray<FStudioPipelineScalarNode> Nodes;
    FStudioFieldIdentity PinnedIdentity;
    FStudioSavedPipeline PreparedRecipe;
    FBox Bounds=FBox(ForceInit);
    TOptional<FStudioPipelineOperation> Plane;
    int32 Selected=INDEX_NONE;
    int64 ValueBytes=0;
    const IStudioField& Base() const;
    int32 NodeIndex(const FString& FieldId) const;
    bool Values(int32 Last,int32 Row,const FVector* Position,double* Out,const FStudioVolumeStencil* Stencil=nullptr) const;
};

struct FStudioPipelinePrepareRequest
{
    FGuid ProjectId;
    uint64 Revision=0;
    FStudioSavedPipeline Recipe;
    // Optional existing reader. Omit to open pinned source/reference off-thread.
    TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> Source;
    TArray<FStudioRecordingReference> References;
};
struct FStudioPipelinePrepared
{
    FGuid ProjectId;
    uint64 Revision=0;
    FStudioSavedPipeline Recipe;
    TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> Source;
    TSharedPtr<const FStudioPipelineField,ESPMode::ThreadSafe> Field;
    TArray<FStudioPipelineStage> Stages;
    FString Error;
    bool bCancelled=false;
    bool Matches(const FGuid& CurrentProject,uint64 CurrentRevision,const FStudioSavedPipeline& CurrentRecipe) const;
};
namespace StudioPipelineFields
{
    constexpr int32 MaxScalarNodes=StudioPipelines::MaxOperations*4;
    constexpr int32 MaxSourcePoints=2*1024*1024;
    constexpr int64 MaxInputValueBytes=64LL*1024*1024;
    /** Worker-only, all inputs or none. No fallback field/frame, geometry output,
     * Solve mutation or fabricated data. Source metadata/topology allocations
     * retain their reader budgets in addition to this scalar ownership limit. */
    FStudioPipelinePrepared Prepare(const FStudioPipelinePrepareRequest& Request,const FStudioLoadCancellation& Cancellation={});
}
class FStudioPipelinePrepareTask
{
public:
    ~FStudioPipelinePrepareTask();
    bool Start(FStudioPipelinePrepareRequest Request,FString& Error);
    void Cancel();
    void Shutdown();
    bool IsBusy() const {return Pending.IsValid();}
    TOptional<FStudioPipelinePrepared> Poll();
private:
    bool bShutdown=false;
    FStudioLoadCancellation Cancellation;
    TFuture<FStudioPipelinePrepared> Pending;
};
