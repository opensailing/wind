#pragma once
#include "CoreMinimal.h"
#include "StudioFieldExportTask.h"

class FStudioModel;
class AStudioScene;
class SWidget;

/** Session-only export draft. The root owns the task across popup dismissal;
 * widgets retain this state, never the root widget or a mutable solver frame. */
class FStudioFieldExportUI : public TSharedFromThis<FStudioFieldExportUI>
{
public:
    TSharedRef<SWidget> Menu(const TSharedRef<FStudioModel>& Model,AStudioScene* Scene);
    void MenuOpenChanged(bool bOpen);
    void Tick(FStudioModel& Model);
    FString Status() const;
    bool IsBusy() const{return Task.IsBusy();}
#if WITH_DEV_AUTOMATION_TESTS
    static void SetBeforeNextPublishForAutomation(TFunction<void()> Barrier);
#endif
private:
    void Save(const TSharedRef<FStudioModel>& Model);
    FStudioFieldExportTask Task;
    FStudioVTKExportRequest Draft;
    TArray<FStudioScalarDescriptor> Scalars;
    FGuid Project;
    FString Dataset,Title,FrameLabel,Topology,Notice,Path;
    bool bMenuOpen=false,bError=false,bSaved=false;
};
