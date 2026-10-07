#include "StudioModel.h"

namespace
{
/** An authoring document has no CFD evidence until original output is attached. */
class FStudioEmptyField final : public IStudioField
{
public:
    bool IsValid() const override { return false; }
    bool Sample(const FVector&,FStudioFieldValue&) const override { return false; }
    bool SampleScalar(const FVector&,const FString&,double&) const override { return false; }
    bool SampleVelocity(const FVector&,FVector&) const override { return false; }
    bool IsSolid(const FVector&) const override { return false; }
    const TArray<FVector2D>& Boundary() const override { return EmptyBoundary; }
    const TArray<FIntVector>& BoundaryTriangles() const override { return EmptyTriangles; }
private:
    TArray<FVector2D> EmptyBoundary;
    TArray<FIntVector> EmptyTriangles;
};
class FStudioEmptyRecording final : public IStudioSolver
{
public:
    FStudioEmptyRecording()
    {
        Meta.Title=TEXT("Original results not attached");Meta.SourceLabel=Meta.Title;
        Meta.SpatialDimensions=0;Meta.DefaultScalar=TEXT("unavailable");
        FStudioScalarDescriptor Placeholder;Placeholder.Id=Meta.DefaultScalar;
        Placeholder.Label=TEXT("No original scalar fields");Placeholder.Unit=TEXT("unavailable");
        Placeholder.Origin=TEXT("unavailable");Placeholder.Minimum=0;Placeholder.Maximum=1;
        Meta.Scalars={Placeholder};Meta.DisplayBounds=FBox(FVector::ZeroVector,FVector::OneVector);
    }
    int32 FrameCount() const override { return 0; }
    FStudioFrame EvaluateFrame(int32) const override { return {}; }
    TSharedRef<const IStudioField,ESPMode::ThreadSafe> CaptureField(int32) const override
    { return MakeShared<FStudioEmptyField,ESPMode::ThreadSafe>(); }
    bool ExportField(int32,const FString&) const override { return false; }
    FString LoadError() const override { return {}; }
    const FStudioRecordingDescriptor& Descriptor() const override { return Meta; }
private:
    FStudioRecordingDescriptor Meta;
};
}
TSharedRef<IStudioSolver,ESPMode::ThreadSafe> StudioRecordings::Empty()
{ return MakeShared<FStudioEmptyRecording,ESPMode::ThreadSafe>(); }
