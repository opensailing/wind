#pragma once
#include "CoreMinimal.h"
#include "StudioImageSequenceRenderer.h"

class SWidget;
class FStudioModel;

namespace StudioSnapshotUI
{
    /** Strict one-based UI selection. Returns exact original ordinals, never a
     * clamped or interpolated range; Last is the inclusive requested endpoint. */
    bool Selection(const FString& First,const FString& Last,const FString& Stride,int32 FrameCount,
        int32& OutFirst,int32& OutLast,int32& OutStride,FString& Error);
}

/** One Snapshot owner for PNG and original-frame sequences. Root Tick/Shutdown
 * owns rendering independently of menu, workspace and project lifetime. */
class FStudioSnapshotUI : public TSharedFromThis<FStudioSnapshotUI>
{
public:
    TSharedRef<SWidget> Menu(const TSharedRef<FStudioModel>& Model,AStudioScene* Scene,
        TFunction<FString()> Guard,TFunction<bool(FStudioSnapshot&,FString&)> Capture);
    void Tick(FStudioModel& Model);
    void Shutdown();
    bool IsBusy() const {return PNG.IsBusy()||Sequence.IsBusy();}
    FIntPoint OutputSize() const;
    FString ButtonLabel() const;
#if WITH_DEV_AUTOMATION_TESTS
    static void SetBeforeNextPublishForAutomation(TFunction<void()> Barrier);
#endif
private:
    FString Validation() const;
    FString SelectionLabel() const;
    FString Status() const;
    bool SameContext() const;
    void Save();
    void Cancel();
    void Report(const FString& Message,bool Error=false);
    FStudioSnapshotExportTask PNG;
    FStudioImageSequenceRenderer Sequence;
    TWeakPtr<FStudioModel> Model;
    TWeakObjectPtr<AStudioScene> Scene;
    TWeakPtr<IStudioSolver,ESPMode::ThreadSafe> Source;
    TFunction<FString()> Guard;
    TFunction<bool(FStudioSnapshot&,FString&)> Capture;
    FStudioSnapshotOptions Options;
    FGuid DraftProject,JobProject;
    FString First=TEXT("1"),Last=TEXT("1"),Stride=TEXT("1"),Folder=TEXT("flow-images");
    FString Notice,Path,FrozenSummary;
    int32 Width=1920,Aspect=0;
    bool bSequence=false,bAll=false,bError=false,bSaved=false,bShutdown=false;
};
