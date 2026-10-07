#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4Archive.h"
#include "StudioHome4ArchiveWatch.h"
class FStudioModel;
class SVerticalBox;
/** Retained original-source import form; no next-run unit/spec dependency. */
class SStudioHome4ArchivePanel final:public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4ArchivePanel){}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    ~SStudioHome4ArchivePanel();
    void Poll(double Time);
    void Tick(const FGeometry&,double,float)override;
#if WITH_DEV_AUTOMATION_TESTS
    /** One-shot picker results for routed widget tests. Once enabled, missing
     * results cancel; a virtual test can never fall back to a native dialog. */
    void SetNextSourcePathForAutomation(const FString& Path){check(IsInGameThread());bAutomationFileDialogs=true;NextSourcePath=Path;}
    void SetNextWatchDirectoryForAutomation(const FString& Path){bAutomationFileDialogs=true;NextWatchDirectory=Path;}
    void SetNextOutputParentForAutomation(const FString& Path){check(IsInGameThread());bAutomationFileDialogs=true;NextOutputParent=Path;}
    bool IsOperationPendingForAutomation()const{return PendingInspection.IsValid()||Task.IsBusy()||bImportPending;}
    int32 SourceCountForAutomation()const{return Inspections.Num();}
    const FString& CompletedPathForAutomation()const{return CompletedRecording;}
    FStudioHome4ArchiveProgress ProgressForAutomation()const{return Task.Progress();}
#endif
private:
    void AddSource();
    void StartWatch();
    void RebuildSources();
    void Start(bool bVTI);
    void OpenCompleted();
    bool Request(FStudioHome4ArchiveRequest& Out,FString& Error)const;
    FString Status()const;
    TSharedRef<SWidget> Text(const FString& Label,FName Tag,FString& Value,const FString& Help={});
    TSharedRef<SWidget> Choice(const FString& Label,FName Tag,FString& Value,const TArray<FString>& Options,const FString& Help);
    TWeakPtr<FStudioModel> Model;
    TSharedPtr<SVerticalBox> SourceList;
    TArray<FStudioHome4ArchiveInspection> Inspections;
    TFuture<FStudioHome4ArchiveInspection> PendingInspection;
    FStudioLoadCancellation InspectionCancel;
    FStudioHome4ArchiveTask Task;
    FStudioHome4ArchiveWatch Watch;
    FString WatchOutput,WatchPattern=TEXT("*_viz*.npz");int32 WatchRevision=0;bool bWatchDirty=false;
    FGuid ScopeProject;
    TWeakPtr<IStudioSolver,ESPMode::ThreadSafe> ScopeSource;
    FString AxisOrder,MetadataOrder,CoordinateUnits,VelocityUnits,Dx,Dt,Density;
    FString Liquid=TEXT("0.5"),TimeOrigin=TEXT("0"),Crop,Stride=TEXT("1");
    FString Title=TEXT("HOME4 original snapshots"),URI,Attribution,Folder=TEXT("home4-import"),Notice,CompletedRecording;
    bool bDerivatives=true,bWBPressure=false,bImportPending=false;
#if WITH_DEV_AUTOMATION_TESTS
    bool bAutomationFileDialogs=false;
    TOptional<FString> NextSourcePath,NextOutputParent,NextWatchDirectory;
#endif
};
