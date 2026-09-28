#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioComparison.h"
#include "StudioProject.h"

class FStudioModel;
class AStudioScene;
class UWorld;
class SBox;

/** Results-only comparison. The owning Solve model supplies catalog context;
 * it is never a comparison cursor, camera, worker or persistence destination. */
class SStudioComparisonWorkspace final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioComparisonWorkspace) {}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model)
        SLATE_ARGUMENT(UWorld*,World)
        SLATE_ARGUMENT(TArray<FStudioRecordingEntry>,Recordings)
        SLATE_EVENT(FSimpleDelegate,OnBack)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    bool SupportsKeyboardFocus() const override { return true; }
    ~SStudioComparisonWorkspace();
    void Tick(const FGeometry& Geometry,double Time,float Delta) override;
private:
    bool Busy() const;
    bool Current() const;
    bool Visible() const;
    void InvalidatePair();
    void CloseViews();
    void OpenSource(int32 Side,const FString& Id);
    void Cancel();
    void Compare();
    void Present(FStudioComparisonResult Result);
    void SetRanges();
    void RefreshScalar();
    FStudioComparisonRequest Request() const;
    TSharedRef<SWidget> SourceMenu(int32 Side);
    TSharedRef<SWidget> ScalarMenu();
    TSharedRef<SWidget> AlignmentMenu();
    TSharedRef<SWidget> MatchMenu();
    TSharedRef<SWidget> CameraMenu(int32 Side);
    TSharedRef<SWidget> View(int32 Side);
    TSharedPtr<FStudioModel> M;
    TWeakObjectPtr<UWorld> World;
    TArray<FStudioRecordingEntry> Recordings;
    FSimpleDelegate Back;
    FGuid ProjectId;
    TSharedPtr<IStudioSolver,ESPMode::ThreadSafe> Sources[2];
    TWeakObjectPtr<AStudioScene> Scenes[2];
    TOptional<FStudioCameraState> Cameras[2];
    TSharedPtr<SBox> Views[2];
    FStudioComparisonTask Task;
    TOptional<FStudioComparisonResult> Pair;
    TFuture<FStudioRecordingLoadResult> PendingSource;
    FStudioLoadCancellation SourceCancellation;
    int32 LoadingSide=INDEX_NONE,Ordinal=0;
    FString Scalar,Notice;
    FStudioComparisonAlignment Alignment;
    bool bCommonRange=true,bError=false;
};
