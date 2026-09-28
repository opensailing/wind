#include "StudioSnapshotSource.h"
#include "StudioPointRecording.h"
#include "StudioSurfaceReconstruction.h"
#include "StudioVolume.h"
#include "StudioPipelineEvaluation.h"

namespace
{
// IStudioSolver's capture API returns a reference even on failure. An invalid
// field carries no numerical values, topology or borrowed snapshot identity.
class FUnavailableSnapshotField final : public IStudioField
{
public:
    bool IsValid() const override { return false; }
    bool Sample(const FVector&,FStudioFieldValue&) const override { return false; }
    bool IsSolid(const FVector&) const override { return false; }
    const TArray<FVector2D>& Boundary() const override { return EmptyBoundary; }
    const TArray<FIntVector>& BoundaryTriangles() const override { return EmptyTriangles; }
private:
    TArray<FVector2D> EmptyBoundary;
    TArray<FIntVector> EmptyTriangles;
};
TSharedRef<const IStudioField,ESPMode::ThreadSafe> UnavailableSnapshot()
{
    static const TSharedRef<const IStudioField,ESPMode::ThreadSafe> Empty=
        MakeShared<FUnavailableSnapshotField,ESPMode::ThreadSafe>();
    return Empty;
}
}

TSharedPtr<FStudioSnapshotSource,ESPMode::ThreadSafe> FStudioSnapshotSource::Create(const IStudioSolver& Source,
    int32 Ordinal,const FStudioScalarDescriptor& Scalar,TSharedPtr<const IStudioField,ESPMode::ThreadSafe> Field,FString& Error)
{
    const auto& D=Source.Descriptor();
    auto Fail=[&]() -> TSharedPtr<FStudioSnapshotSource,ESPMode::ThreadSafe>
    {Error=TEXT("The snapshot does not match its original source, frame, scalar or reconstruction.");return {};};
    if(!Field||!Field->IsValid()||D.Id.IsEmpty()||D.Frames.Num()>1000000||
        Source.FrameCount()!=D.Frames.Num()||!D.Frames.IsValidIndex(Ordinal))return Fail();
    if((D.SpatialDimensions!=2&&D.SpatialDimensions!=3)||!D.DisplayBounds.IsValid||
        D.DisplayBounds.Min.ContainsNaN()||D.DisplayBounds.Max.ContainsNaN()||D.Frames[Ordinal].Index<0||
        !FMath::IsFinite(D.Frames[Ordinal].Time)||D.Frames[Ordinal].Time<0)return Fail();
    const auto Identity=Field->Identity();const auto Actual=Field->Scalar(Scalar.Id);
    const auto* Declared=D.Scalars.FindByPredicate([&](const auto& S){return S.Id==Scalar.Id;});
    if(!Identity.IsSet()||!Actual.IsSet()||!Declared)return Fail();
    auto SameScalar=[](const auto& A,const auto& B)
    {return A.Id==B.Id&&A.Label==B.Label&&A.Unit==B.Unit&&A.Origin==B.Origin&&A.Minimum==B.Minimum&&A.Maximum==B.Maximum;};
    FString Reconstruction;auto Interpolation=D.bSourcePoints?EStudioFieldInterpolation::None:EStudioFieldInterpolation::SourceTriangles;
    if(const auto Surface=Source.Reconstruction())
    {Reconstruction=Surface->MetadataSHA256;Interpolation=EStudioFieldInterpolation::ReconstructedTriangles;}
    if(const auto Volume=Source.VolumeReconstruction())
    {Reconstruction=Volume->MetadataSHA256;Interpolation=EStudioFieldInterpolation::ReconstructedGrid;}
    if(Source.Reconstruction()!=Field->Reconstruction()||Source.VolumeReconstruction()!=Field->VolumeReconstruction())return Fail();
    if(const auto Points=Field->OriginalPoints();Points&&!Points->FindValues(Scalar.Id))return Fail();
    if(Identity->Dataset!=D.Id||Identity->MetadataSHA256!=D.MetadataSHA256||Identity->PayloadSHA256!=D.PayloadSHA256||
        Identity->Ordinal!=Ordinal||Identity->Frame.Index!=D.Frames[Ordinal].Index||Identity->Frame.Time!=D.Frames[Ordinal].Time||
        Identity->SpatialDimensions!=D.SpatialDimensions||Identity->SourceOffset!=D.SourceOffset||
        Identity->ReconstructionSHA256!=Reconstruction||Identity->Interpolation!=Interpolation||
        !SameScalar(*Actual,Scalar)||!SameScalar(*Declared,Scalar))return Fail();
    TSharedPtr<FStudioSnapshotSource,ESPMode::ThreadSafe> Out=MakeShareable(new FStudioSnapshotSource());
    Out->Meta=D;Out->FrozenField=MoveTemp(Field);Out->FrozenOrdinal=Ordinal;Out->FrozenScalar=Scalar.Id;
    Error.Empty();return Out;
}
FStudioFrame FStudioSnapshotSource::EvaluateFrame(int32 Index) const
{return Meta.Frames.IsValidIndex(Index)?Meta.Frames[Index]:FStudioFrame();}
TSharedPtr<FStudioSnapshotSource,ESPMode::ThreadSafe> FStudioSnapshotSource::CreatePipeline(
    const FStudioPipelineEvaluationResult& Result,FString& Error)
{
    const auto& P=Result.Prepared;
    auto Fail=[&]() -> TSharedPtr<FStudioSnapshotSource,ESPMode::ThreadSafe>
    {Error=TEXT("The evaluated pipeline no longer matches its pinned source, frame or operations. Evaluate it again.");return {};};
    if(!Result.Matches(P.ProjectId,P.Revision,P.Recipe))return Fail();
    const auto& D=P.Source->Descriptor();const auto& S=P.Field->SelectedScalar();
    TArray<FStudioPipelineStage> Stages;
    if(!StudioPipelines::Compile(P.Recipe,D,Stages,Error)||Stages.IsEmpty()||Stages.Last().Field!=S.Id||Stages.Last().Unit!=S.Unit||
        D.Frames.Num()>1000000||D.Frames.Num()!=P.Source->FrameCount()||!D.DisplayBounds.IsValid||
        D.DisplayBounds.Min.ContainsNaN()||D.DisplayBounds.Max.ContainsNaN()||
        !FMath::IsFinite(S.Minimum)||!FMath::IsFinite(S.Maximum)||S.Minimum>S.Maximum||
        P.Source->Reconstruction()!=P.Field->Reconstruction()||P.Source->VolumeReconstruction()!=P.Field->VolumeReconstruction())return Fail();
    TSharedPtr<FStudioSnapshotSource,ESPMode::ThreadSafe> Out=MakeShareable(new FStudioSnapshotSource());
    Out->Meta=D;Out->Meta.Scalars={S};Out->FrozenField=P.Field;Out->FrozenOutput=Result.Output;
    Out->FrozenOrdinal=P.Recipe.Source.Identity.Ordinal;Out->FrozenScalar=S.Id;
    Error.Empty();return Out;
}
TSharedRef<const IStudioField,ESPMode::ThreadSafe> FStudioSnapshotSource::CaptureField(int32 Index) const
{return Index==FrozenOrdinal?FrozenField.ToSharedRef():UnavailableSnapshot();}
TSharedRef<const IStudioField,ESPMode::ThreadSafe> FStudioSnapshotSource::CaptureViewField(int32 Index,
    const FString& Scalar,bool bVectors,const FStudioLoadCancellation& Cancellation) const
{
    if(bVectors)return UnavailableSnapshot();
    const auto Read=ReadScalarFrame(Index,Scalar,Cancellation);
    return Read.Field?Read.Field.ToSharedRef():UnavailableSnapshot();
}
FStudioFieldReadResult FStudioSnapshotSource::ReadScalarFrame(int32 Index,const FString& Scalar,
    const FStudioLoadCancellation& Cancellation) const
{
    if(Cancellation&&Cancellation->load(std::memory_order_relaxed))return {{},TEXT("Snapshot read cancelled.")};
    if(Index!=FrozenOrdinal||Scalar!=FrozenScalar)return {{},TEXT("This view contains one recorded frame and scalar. Evaluate a new view to change them.")};
    return {FrozenField,{}};
}
TSharedPtr<const FStudioSurfaceReconstruction,ESPMode::ThreadSafe> FStudioSnapshotSource::Reconstruction() const
{return FrozenField->Reconstruction();}
TSharedPtr<const FStudioVolumeReconstruction,ESPMode::ThreadSafe> FStudioSnapshotSource::VolumeReconstruction() const
{return FrozenField->VolumeReconstruction();}
