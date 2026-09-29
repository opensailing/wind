#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioPipelineEvaluation.h"
#include "StudioSnapshotSource.h"

class AStudioScene;
class UWorld;
class SBox;
class SVerticalBox;

/** Saved ordered analysis with its own source/frame/camera. Numerical work is
 * explicit and asynchronous; model edits never advance Solve or change its case. */
class SStudioPipelineWorkspace final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioPipelineWorkspace){}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model)
        SLATE_ARGUMENT(UWorld*,World)
        SLATE_EVENT(FSimpleDelegate,OnResults)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    ~SStudioPipelineWorkspace();
    /** Shell-driven polling also drains work and releases old projects while hidden. */
    void Synchronize();
    bool SupportsKeyboardFocus() const override {return true;}
    /** Flush the independent camera before root Save, navigation or close. */
    void SaveCamera();
    /** Keep unapplied parameter text visible before a save or project replacement. */
    bool EnsureResolved();
private:
    struct FEvaluatedView
    {
        FStudioPipelineEvaluationResult Result;
        TSharedPtr<FStudioSnapshotSource,ESPMode::ThreadSafe> Snapshot;
        FString Error;
    };
    const FStudioSavedPipeline* Selected() const;
    bool Visible() const;
    bool Busy() const;
    bool Current() const;
    bool CanEdit() const;
    bool ApplyOperation();
    bool Update(FStudioSavedPipeline Recipe);
    void Observe();
    void Select(const FGuid& Id,bool Evaluate=true);
    void SelectOperation(const FGuid& Id);
    void NewPipeline(const FString& Id);
    void Evaluate();
    void Cancel();
    void CloseView();
    void Present(FEvaluatedView View);
    void RefreshOperations();
    void RefreshEditor();
    void RefreshOutput();
    void AddOperation(EStudioPipelineOperation Kind);
    void MoveOperation(int32 Offset);
    void DeleteOperation();
    void ToggleOperation(const FGuid& Id,bool Enabled);
    void History(bool Redo);
    TArray<FStudioScalarDescriptor> AvailableScalars(const FGuid& Before) const;
    TSharedRef<SWidget> NewMenu();
    TSharedRef<SWidget> SavedMenu();
    TSharedRef<SWidget> ManageMenu();
    TSharedRef<SWidget> AddMenu();
    TSharedRef<SWidget> ScalarMenu(int32 Component=INDEX_NONE);
    TSharedRef<SWidget> CameraMenu();
    TSharedRef<SWidget> SourceMenu();
    TSharedRef<SWidget> RangeMenu();
    TSharedRef<SWidget> Number(const FString& Key,const FString& Caption,double Value);
    void Error(const FString& Message);
    TSharedPtr<FStudioModel> M;
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<AStudioScene> Scene;
    FSimpleDelegate Results;
    TSharedPtr<SBox> Editor,Output;
    TSharedPtr<SVerticalBox> Operations;
    FGuid ProjectId,SelectedId,OperationId;
    uint64 ObservedRevision=MAX_uint64;
    TOptional<FStudioSavedPipeline> ObservedRecipe,PresentedRecipe;
    TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> Source;
    TOptional<FStudioPipelineEvaluationResult> Evaluation;
    TOptional<FStudioCameraState> WorkingCamera;
    FStudioPipelineOperation EditOperation;
    TMap<FString,FString> Numbers;
    FString EditName,Notice,FormNotice,Rename,RangeMinimum,RangeMaximum;
    bool bError=false,bFormDirty=false,bInitialized=false,bRefreshOutput=false;
    FStudioLoadCancellation Cancellation;
    TFuture<FEvaluatedView> Pending;
    TFuture<FStudioRecordingLoadResult> PendingSource;
    FGuid RequestProject,RequestPipeline;
    uint64 RequestRevision=0;
    FString NewSourceId;
    int32 NewOrdinal=0;
};
