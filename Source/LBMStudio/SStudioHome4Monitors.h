#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4Telemetry.h"
#include "StudioHome4SciencePresentation.h"
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
    SLATE_BEGIN_ARGS(SStudioHome4Monitors) : _UnitDisplay(EStudioHome4UnitDisplay::Lattice) {}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>, Model)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4TelemetryStream>, Stream)
        SLATE_ATTRIBUTE(EStudioHome4UnitDisplay, UnitDisplay)
        SLATE_EVENT(FStudioHome4LocateCell, OnLocateCell)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    ~SStudioHome4Monitors();
    void Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime) override;
    bool SupportsKeyboardFocus() const override { return true; }
    FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;
    /** Starts a transactional asynchronous read of an original JSONL file. */
    bool BeginImportPath(const FString& Path, const FString& OriginalRunId = FString());
    void CancelImport();
    void PollImport();
    bool IsImporting() const { return Pending.IsValid(); }
    bool IsImportedReplay() const { return ImportedStream.IsValid() && bShowImported; }
    const TSharedPtr<FStudioHome4TelemetryStream>& ImportedReplay() const { return ImportedStream; }
    /** The displayed source remains science/replay data, never a job acknowledgement. */
    FStudioHome4TelemetryStream* DisplayedTelemetry() const { return DisplayStream(); }
    const FStudioHome4Sample* LatestDisplayedMeasurement() const { return Sample(); }
    /** Imported logs bind spatially only when the owner supplied their original run GUID. */
    TOptional<FGuid> OriginalRunIdentity() const;
    FString StatusText() const { return Status; }
    const FStudioHome4DiagnosticPolicy& DiagnosticPolicy() const { return Policy; }
    StudioHome4SciencePresentation::FHistory PresentedHistory() const;
    StudioHome4SciencePresentation::FHistory PresentedForces() const;
    FString SelectedBodyIdentity() const { return SelectedBody; }

private:
    struct FImportResult
    {
        TUniquePtr<FStudioHome4TelemetryStream> Stream;
        FString Path, Error;
        bool bOriginalRunIdentity = false;
        int64 Bytes = 0, Lines = 0, Malformed = 0, Unknown = 0, Oversized = 0, Regressing = 0;
    };
    static FImportResult ReadImport(const FString& Path, const FStudioHome4Source& Source, bool bOriginalRunIdentity,
        const TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe>& Cancel);
    FStudioHome4TelemetryStream* DisplayStream() const;
    const FStudioHome4Sample* Sample() const;
    FStudioHome4HealthSignal Health(int32 Index) const;
    FString SourceText() const;
    FString DetailText(FName Key) const;
    void ImportDialog();
    void CycleBody();
    void CycleLevel();
    void CyclePhase();
    FString HistoryCaption(bool Forces) const;

    void ApplyPolicy();
    void RefreshOutputs();
    void ScopeProject();
    bool CanLocate() const;
    void Locate();
    TWeakPtr<FStudioModel> Model;
    TSharedPtr<FStudioHome4TelemetryStream> SessionStream, ImportedStream;
    TSharedPtr<SScrollBox> Scroll;
    TSharedPtr<SVerticalBox> OutputRows;
    FStudioHome4LocateCell OnLocate;
    FStudioHome4DiagnosticPolicy Policy;
    TArray<FString> PolicyDraft;
    TAttribute<EStudioHome4UnitDisplay> UnitDisplay;
    StudioHome4SciencePresentation::EMetric SelectedMetric = StudioHome4SciencePresentation::EMetric::Mass;
    int32 PlotComponent = 0, SelectedLevel = 0;
    FString SelectedBody, SelectedPhase;
    bool bNormalizeForces = false;
    TMap<FName, bool> Sections;

    FString Status, ImportPath, OriginalRunIdDraft;
    bool bShowImported = true;
    bool bImportedOriginalRunIdentity = false;
    FGuid ScopedProjectId, ImportProjectId;
    uint64 DisplayedOutputIndex = MAX_uint64;
    const FStudioHome4TelemetryStream* DisplayedStream = nullptr;
    TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> Cancellation;
    TFuture<FImportResult> Pending;
};
