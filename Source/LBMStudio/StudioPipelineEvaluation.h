#pragma once
#include "StudioPipelineField.h"
#include "StudioProbeSampling.h"

enum class EStudioPipelineOutputKind : uint8 { OriginalPoints, Surface, ContourLines, ContourSurface, ProbeTable };
struct FStudioPipelineVertex
{
    FVector PositionMeters=FVector::ZeroVector;
    double Scalar=0;
    // INDEX_NONE marks derived positions; they never inherit a fabricated ID.
    int32 OriginalRow=INDEX_NONE;
    int64 OriginalPointId=0;
};
struct FStudioPipelineOutput
{
    EStudioPipelineOutputKind Kind=EStudioPipelineOutputKind::OriginalPoints;
    TArray<FStudioPipelineVertex> Vertices;
    TArray<FIntVector> Triangles;
    TArray<FIntPoint> Lines;
    TOptional<FStudioProbeResult> Probe;
    TOptional<FVector2D> Range;
    FString Method;
    bool bDerivedGeometry=false;
    bool IsEmpty() const;
    int64 AllocatedBytes() const;
};
struct FStudioPipelineEvaluationResult
{
    FStudioPipelinePrepared Prepared;
    TSharedPtr<const FStudioPipelineOutput,ESPMode::ThreadSafe> Output;
    FString Error;
    bool bCancelled=false;
    bool Matches(const FGuid& Project,uint64 Revision,const FStudioSavedPipeline& Recipe) const;
};
namespace StudioPipelineEvaluation
{
    constexpr int32 MaxOutputVertices=2*1024*1024;
    constexpr int32 MaxOutputTriangles=300000;
    constexpr int32 MaxOutputLines=600000;
    constexpr int64 MaxOutputBytes=128LL*1024*1024;
    /** Worker-only. Final geometry/table and source ownership publish together.
     * Slice/contour discretization is explicitly described in Output.Method;
     * geometry bounds do not imply original CFD connectivity. */
    FStudioPipelineEvaluationResult Evaluate(const FStudioPipelinePrepareRequest& Request,const FStudioLoadCancellation& Cancellation={});
}
class FStudioPipelineEvaluationTask
{
public:
    ~FStudioPipelineEvaluationTask();
    bool Start(FStudioPipelinePrepareRequest Request,FString& Error);
    void Cancel();
    void Shutdown();
    bool IsBusy() const {return Pending.IsValid();}
    TOptional<FStudioPipelineEvaluationResult> Poll();
private:
    bool bShutdown=false;
    FStudioLoadCancellation Cancellation;
    TFuture<FStudioPipelineEvaluationResult> Pending;
};
