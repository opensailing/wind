#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4Validation.h"
#include "Async/Future.h"
class FStudioModel;
class SVerticalBox;

class SStudioHome4Validation final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4Validation) {}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>, Model)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4ValidationState>, State)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    void Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime) override;
    bool ImportPath(const FString& Path, const FStudioHome4ReferenceExpectation& Expected);
    void PollImport();
    bool IsImporting() const { return Pending.IsValid(); }
    bool BuildLadder();
    const TSharedPtr<FStudioHome4ValidationState>& EvidenceState() const { return State; }
private:
    struct FImportResult { FStudioHome4ReferenceEvidence Evidence; FString Error; };
    void ImportDialog();
    void ExportReference();
    void ExportQueue();
    void Refresh();
    void ScopeState();
    TWeakPtr<FStudioModel> Model;
    TSharedPtr<FStudioHome4ValidationState> State;
    TSharedPtr<SVerticalBox> SeriesRows, RungRows;
    TFuture<FImportResult> Pending;
    FString RefinementDraft = TEXT("1,2,4"), ExpectedRunDraft, ExpectedActualDraft, ExpectedReferenceDraft;
    FString BundleName = TEXT("home4-evidence");
    const FStudioHome4ReferenceEvidence* ShownEvidence = nullptr;
    int32 ShownRungs = INDEX_NONE;
    FGuid ImportProjectId, ImportCaseId;
};
