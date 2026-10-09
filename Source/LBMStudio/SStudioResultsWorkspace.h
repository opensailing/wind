#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioRecording.h"
#include "StudioComparisonExport.h"
#include "Async/Future.h"

class FStudioModel;
class SVerticalBox;
class SBox;
class SButton;
class SEditableTextBox;
class AStudioScene;
class SStudioComparisonWorkspace;

/** One source catalog for recordings and saved run records. Dataset selection
 * uses the model's transactional reader; metadata never borrows case values. */
class SStudioResultsWorkspace final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioResultsWorkspace) {}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model)
        SLATE_ARGUMENT(AStudioScene*,Scene)
        SLATE_EVENT(FSimpleDelegate,OnInspect)
        SLATE_EVENT(FSimpleDelegate,OnImport)
        SLATE_ARGUMENT(TFunction<void(const FString&,const FString&)>,Locate)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    void Tick(const FGeometry& Geometry,double Time,float Delta) override;
    bool IsComparisonOpen() const {return Comparison.IsValid();}
    TOptional<FStudioComparisonExportRequest> ExportComparisonSnapshot(FString& Error) const;
    /** Inspect metadata only; never open a recording or replace the live view. */
    bool RevealNotification(const FGuid& Run,const FString& Recording);
private:
    bool Available() const;
    bool HasRecording() const;
    void SetRuns(bool Value);
    void OpenComparison();
    void CloseComparison();
    void OpenDataset(const FString& Id);
    void RefreshRows();
    TSharedRef<SWidget> RecordingDetails();
    TSharedRef<SWidget> RunDetails();
    TSharedPtr<FStudioModel> M;
    TWeakObjectPtr<AStudioScene> Scene;
    TSharedPtr<SBox> Body;
    TSharedPtr<SWidget> Catalog;
    TSharedPtr<SStudioComparisonWorkspace> Comparison;
    FSimpleDelegate Inspect,Import;
    TFunction<void(const FString&,const FString&)> Locate;
    TSharedPtr<SVerticalBox> Rows;
    TSharedPtr<SBox> Details;
    TSharedPtr<SEditableTextBox> SearchBox;
    TMap<FName,TWeakPtr<SButton>> RowButtons;
    TFuture<TArray<FStudioRecordingEntry>> InstalledTask;
    TArray<FStudioRecordingEntry> Installed;
    TWeakPtr<IStudioSolver,ESPMode::ThreadSafe> LastSource;
    FGuid ProjectId,RunId;
    int32 LastCatalog=INDEX_NONE,LastRunCount=INDEX_NONE;
    FString Search,Opening;
    bool bRuns=false,bRowsDirty=true,bDetailsDirty=true;
};
