#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4Validation.h"
#include "StudioHome4ReferenceSources.h"
#include "Async/Future.h"
class FStudioModel;
class FStudioHome4RuntimeSession;
class SVerticalBox;

class SStudioHome4Validation final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4Validation) {}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>, Model)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4ValidationState>, State)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4RuntimeSession>, Runtime)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    void Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime) override;
    bool ImportPath(const FString& Path, const FStudioHome4ReferenceExpectation& Expected);
    void PollImport();
    bool IsImporting() const { return Pending.IsValid()||PendingSource.IsValid(); }
    bool BuildLadder();
    bool QueueLadder();
    bool ApplyExtraction();
    bool AssembleConvergence();
    bool ImportSourcePath(const FString& Path,bool Reference);
    bool AlignSources();
    bool ClearLadder();
    bool RetryRung(int32 Index);
    bool SetAlignedEvidence(FStudioHome4ReferenceEvidence Evidence);
    const TSharedPtr<FStudioHome4ValidationState>& EvidenceState() const { return State; }
private:
    struct FImportResult { FStudioHome4ReferenceEvidence Evidence; FString Error; };
    struct FSourceResult { FStudioHome4SeriesSource Source; FString Error; FGuid ProjectId,CaseId; FString Recipe; bool bReference=false; };
    void ImportSourceDialog(bool Reference);
    void ImportDialog();
    void ExportReference();
    void ExportQueue();
    void Refresh();
    void ScopeState();
    TWeakPtr<FStudioModel> Model;
    TSharedPtr<FStudioHome4ValidationState> State;
    TSharedPtr<FStudioHome4RuntimeSession> Runtime;
    TSharedPtr<SVerticalBox> SeriesRows, RungRows;
    TFuture<FImportResult> Pending;
    TFuture<FSourceResult> PendingSource;
    TSharedPtr<const FStudioHome4SeriesSource> ActualSource,ReferenceSource;
    FString AlignmentAbsoluteDraft,AlignmentRelativeDraft,AlignmentStartDraft,AlignmentEndDraft;
    FString RefinementDraft = TEXT("1,2,4"), ExpectedRunDraft, ExpectedActualDraft, ExpectedReferenceDraft;
    FString BundleName = TEXT("home4-evidence");
    FString MetricDraft, UnitDraft, TimeUnitDraft, EpochDraft, MethodDraft=TEXT("trapezoid_mean"), StartDraft, EndDraft, TripletDraft=TEXT("0");
    const FStudioHome4ReferenceEvidence* ShownEvidence = nullptr;
    int32 ShownRungs = INDEX_NONE;
    FGuid ImportProjectId, ImportCaseId;
};
