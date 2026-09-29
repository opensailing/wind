#pragma once
#include "CoreMinimal.h"
#include "StudioFieldExportTask.h"
#include "StudioFieldSequence.h"
#include "StudioPipelineExport.h"

class FStudioModel;
class AStudioScene;
class SWidget;

/** Session-only export draft. The root owns the task across popup dismissal;
 * widgets retain this state, never the root widget or a mutable solver frame. */
class FStudioFieldExportUI : public TSharedFromThis<FStudioFieldExportUI>
{
public:
    TSharedRef<SWidget> Menu(const TSharedRef<FStudioModel>& Model,AStudioScene* Scene,
        bool bPipelineContext=false,TOptional<FStudioPipelineEvaluationResult> Evaluation={},const FString& Recovery={});
    void MenuOpenChanged(bool bOpen);
    void Tick(FStudioModel& Model);
    FString Status() const;
    bool IsBusy() const{return Task.IsBusy()||Sequence.IsBusy();}
#if WITH_DEV_AUTOMATION_TESTS
    static void SetBeforeNextPublishForAutomation(TFunction<void()> Barrier);
#endif
private:
    void Save(const TSharedRef<FStudioModel>& Model);
    FString Validation() const;
    FString FrameDescription() const;
    FStudioFieldSequenceRequest SequenceRequest() const;
    FStudioFieldExportProgress Progress() const;
    void Cancel();
    void ReleaseSnapshots();
    bool HasSnapshot() const {return bPipeline?Pipeline.IsSet():Draft.Field.IsValid();}
    FStudioFieldExportTask Task;
    FStudioFieldSequenceTask Sequence;
    FStudioFieldExportRequest Draft;
    TOptional<FStudioPipelineExportRequest> Pipeline;
    TSharedPtr<const IStudioSolver,ESPMode::ThreadSafe> Source;
    enum class EScope:uint8 { Current,Range,All };
    EScope Scope=EScope::Current;
    FString FirstText=TEXT("1"),LastText=TEXT("1"),FolderName,SourceKey;
    TArray<FStudioScalarDescriptor> Scalars;
    FGuid Project;
    FString Dataset,Title,FrameLabel,Topology,Notice,Path,PipelineRecovery,Method,ScalarMeaning;
    bool bMenuOpen=false,bError=false,bSaved=false,bPipeline=false,bProbe=false;
};
