#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4Telemetry.h"
#include "Async/Future.h"
#include <atomic>

class FStudioModel;
class SScrollBox;
class SVerticalBox;
DECLARE_DELEGATE_OneParam(FStudioHome4LocateCell, const FStudioHome4CellFacts&);

/** Native science inspector. Session stream identity comes from its owner.
 * Imported JSONL is replay data and cannot stop or checkpoint a running job. */
class SStudioHome4Monitors final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4Monitors) {}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>, Model)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4TelemetryStream>, Stream)
        SLATE_EVENT(FStudioHome4LocateCell, OnLocateCell)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    ~SStudioHome4Monitors();
    void Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime) override;
    bool SupportsKeyboardFocus() const override { return true; }
    FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
    /** Starts a transactional asynchronous read of an original JSONL file. */
    bool BeginImportPath(const FString& Path);
    void CancelImport();
    void PollImport();
    bool IsImporting() const { return Pending.IsValid(); }
    bool IsImportedReplay() const { return ImportedStream.IsValid() && bShowImported; }
    const TSharedPtr<FStudioHome4TelemetryStream>& ImportedReplay() const { return ImportedStream; }
    FString StatusText() const { return Status; }
    const FStudioHome4DiagnosticPolicy& DiagnosticPolicy() const { return Policy; }
private:
    struct FImportResult
    {
        TUniquePtr<FStudioHome4TelemetryStream> Stream;
        FString Path, Error;
        int64 Bytes = 0, Lines = 0, Malformed = 0, Unknown = 0, Oversized = 0, Regressing = 0;
    };
    static FImportResult ReadImport(const FString& Path, const TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe>& Cancel);
    const FStudioHome4TelemetryStream* DisplayStream() const;
    const FStudioHome4Sample* Sample() const;
    FStudioHome4HealthSignal Health(int32 Index) const;
    FString SourceText() const;
    FString DetailText(FName Key) const;
    void ImportDialog();
    void ApplyPolicy();
    void RefreshOutputs();
    bool CanLocate() const;
    void Locate();
    TWeakPtr<FStudioModel> Model;
    TSharedPtr<FStudioHome4TelemetryStream> SessionStream, ImportedStream;
    TSharedPtr<SScrollBox> Scroll;
    TSharedPtr<SVerticalBox> OutputRows;
    FStudioHome4LocateCell OnLocate;
    FStudioHome4DiagnosticPolicy Policy;
    TArray<FString> PolicyDraft;
    FString Status, ImportPath;
    bool bShowImported = true;
    uint64 DisplayedOutputIndex = MAX_uint64;
    const FStudioHome4TelemetryStream* DisplayedStream = nullptr;
    TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> Cancellation;
    TFuture<FImportResult> Pending;
};
