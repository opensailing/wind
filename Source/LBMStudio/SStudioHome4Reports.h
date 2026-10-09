#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4Reports.h"
#include "Async/Future.h"
class FStudioModel;
class FStudioHome4RuntimeSession;
class FStudioHome4SpatialSession;
class SStudioHome4Monitors;
struct FStudioHome4ValidationState;

class SStudioHome4Reports final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4Reports) {}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4RuntimeSession>,Runtime)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4ValidationState>,Validation)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4SpatialSession>,Spatial)
        SLATE_ARGUMENT(TSharedPtr<SStudioHome4Monitors>,Monitors)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    void Tick(const FGeometry& Geometry,double Time,float Delta)override;
    bool ImportPublishedPath(const FString& Path);
    void PollImport();
    bool IsImporting()const{return PendingPublished.IsValid();}
    void CancelImport(){bCancelImport=true;}
    bool QueueFigureRun();
    bool ExportTo(const FString& Parent,const FString& Folder);
    FString StatusText()const{return Status;}
private:
    void ExportDialog();
    void ImportPublished();
    void Scope();
    TWeakPtr<FStudioModel> Model;
    TSharedPtr<FStudioHome4RuntimeSession> Runtime;
    TSharedPtr<FStudioHome4ValidationState> Validation;
    TSharedPtr<FStudioHome4SpatialSession> Spatial;
    TWeakPtr<SStudioHome4Monitors> Monitors;
    TSharedPtr<const FStudioHome4ReferenceEvidence> Published;
    FGuid ProjectId,CaseId;
    FString FolderDraft=TEXT("home4-report"),BodyDraft,PhaseDraft,LevelDraft=TEXT("0"),StartDraft,EndDraft,AxisDraft;
    FString AbsoluteDraft,RelativeDraft,Status;
    bool bFigurePublication=false;
    struct FPublishedResult { TSharedPtr<FStudioHome4ReferenceEvidence> Evidence;FString Error,Recipe;FGuid Project,Case; };
    TFuture<FPublishedResult> PendingPublished;
    bool bCancelImport=false;
};
