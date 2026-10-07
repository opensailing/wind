#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4Runtime.h"
class FStudioModel;
class SVerticalBox;

/** Native shared launch/queue/target/cost surface. All development controls say
 * protocol; original status and measured histories have separate file imports. */
class SStudioHome4Runtime final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4Runtime) {}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>, Model)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4RuntimeSession>, Runtime)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    void Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime) override;
    bool SelectDevelopmentTarget(const FString& Target, EStudioHome4Backend Backend);
    bool SubmitCurrent();
    FString Summary() const;
    bool ImportPath(const FString& Path,bool History);
    void PollImport();
    bool IsImporting() const { return PendingImport.IsValid(); }
    void CancelImport(){bCancelImport=true;}
    FString ImportStatus()const{return Notice;}
#if WITH_DEV_AUTOMATION_TESTS
    void SetImportVerificationForAutomation(TFunction<void()> BeforeVerify){BeforeImportVerify=MoveTemp(BeforeVerify);}
#endif
private:
    struct FOriginalImport { FString JSON,Path,SHA256,Error; FGuid ProjectId,CaseId; bool bHistory=false; };
    static FOriginalImport ReadOriginal(const FString& Path,bool History,FGuid Project,FGuid Case,const TFunction<void()>& BeforeVerify);
    TFuture<FOriginalImport> PendingImport;
    bool bCancelImport=false;
#if WITH_DEV_AUTOMATION_TESTS
    TFunction<void()> BeforeImportVerify;
#endif
    void Refresh();
    void Import(bool History);
    void ExportHistory();
    void AttachLog();
    TWeakPtr<FStudioModel> Model;
    TSharedPtr<FStudioHome4RuntimeSession> Runtime;
    TSharedPtr<SVerticalBox> Jobs, Costs;
    FString OwnerDraft, RunDraft, LogPathDraft, StepDraft = TEXT("1"), TargetDraft, Notice;
    FString EstimateSourceDraft,EstimateAssumptionDraft;
    TArray<FString> SizeDraft;
    FGuid ScopedProjectId,ScopedCaseId;
    FString ShownOutputConfig;
    void SyncOutputDraft();
    bool ApplyOutputEstimates();
    uint64 QueueSignature = MAX_uint64;
    uint64 CostSignature=MAX_uint64;
};
