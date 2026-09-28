#pragma once
#include "StudioModel.h"
struct FStudioPipelineEvaluationResult;
struct FStudioPipelineOutput;

/** A renderer source pinned to one verified original frame and scalar. The
 * descriptor keeps original ordinals, timestamps and provenance. Requests for
 * other frames/fields fail; this source never performs IO or advances playback.
 * Create off Slate: copying the bounded original timeline can be substantial. */
class FStudioSnapshotSource final : public IStudioSolver
{
public:
    static TSharedPtr<FStudioSnapshotSource,ESPMode::ThreadSafe> Create(const IStudioSolver& Source,
        int32 Ordinal,const FStudioScalarDescriptor& Scalar,
        TSharedPtr<const IStudioField,ESPMode::ThreadSafe> Field,FString& Error);
    /** Worker-only adapter for one evaluated recipe. Retains its immutable
     * output and selected scalar, including explicitly derived magnitudes. */
    static TSharedPtr<FStudioSnapshotSource,ESPMode::ThreadSafe> CreatePipeline(
        const FStudioPipelineEvaluationResult& Result,FString& Error);
    TSharedPtr<const FStudioPipelineOutput,ESPMode::ThreadSafe> PipelineOutput() const {return FrozenOutput;}
    int32 Ordinal() const { return FrozenOrdinal; }
    const FString& ScalarId() const { return FrozenScalar; }
    int32 FrameCount() const override { return Meta.Frames.Num(); }
    FStudioFrame EvaluateFrame(int32 Index) const override;
    TSharedRef<const IStudioField,ESPMode::ThreadSafe> CaptureField(int32 Index) const override;
    TSharedRef<const IStudioField,ESPMode::ThreadSafe> CaptureViewField(int32 Index,
        const FString& Scalar,bool bVectors,const FStudioLoadCancellation& Cancellation={}) const override;
    FStudioFieldReadResult ReadScalarFrame(int32 Index,const FString& Scalar,
        const FStudioLoadCancellation& Cancellation={}) const override;
    bool ExportField(int32,const FString&) const override { return false; }
    FString LoadError() const override { return {}; }
    const FStudioRecordingDescriptor& Descriptor() const override { return Meta; }
    TSharedPtr<const FStudioSurfaceReconstruction,ESPMode::ThreadSafe> Reconstruction() const override;
    TSharedPtr<const FStudioVolumeReconstruction,ESPMode::ThreadSafe> VolumeReconstruction() const override;
private:
    FStudioSnapshotSource() = default;
    FStudioRecordingDescriptor Meta;
    TSharedPtr<const IStudioField,ESPMode::ThreadSafe> FrozenField;
    TSharedPtr<const FStudioPipelineOutput,ESPMode::ThreadSafe> FrozenOutput;
    int32 FrozenOrdinal=INDEX_NONE;
    FString FrozenScalar;
};
