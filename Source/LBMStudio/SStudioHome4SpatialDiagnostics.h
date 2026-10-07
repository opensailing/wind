#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4SpatialDiagnostics.h"

DECLARE_DELEGATE_OneParam(FStudioHome4SpatialLocate,const FStudioHome4SpatialLocation&);
enum class EStudioHome4SpatialView : uint8 { All, Multidomain, Zones, Geometry };
/** Multiple pages share Session; importing in any view updates that one source. */
class SStudioHome4SpatialDiagnostics final:public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4SpatialDiagnostics):_View(EStudioHome4SpatialView::All){}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4SpatialSession>,Session)
        SLATE_ARGUMENT(EStudioHome4SpatialView,View)
        SLATE_ARGUMENT(TOptional<FGuid>,ExpectedRunId)
        SLATE_EVENT(FStudioHome4SpatialLocate,OnLocate)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    void Tick(const FGeometry&,double,float) override;
    const TSharedPtr<FStudioHome4SpatialSession>& SharedSession() const{return Session;}
    bool BeginImportPath(const FString& Path){return Session->BeginImport(Path,Expected);}
private:
    void Dialog();
    void Locate();
    void RefreshLevels();
    FString SourceText() const;
    FString PatchText() const;
    FString LevelText() const;
    FString ZoneText() const;
    FString GeometryText() const;
    const FStudioHome4SpatialPatch* Patch() const;
    const FStudioHome4SpatialZone* Zone() const;
    TSharedPtr<FStudioHome4SpatialSession> Session;
    TOptional<FGuid> Expected;
    FStudioHome4SpatialLocate OnLocate;
    FString CellDraft,ActionStatus;
    int32 SelectedPatch=0,SelectedZone=0,Plane=0,SelectedLevel=0;
    TSharedPtr<class SVerticalBox> LevelRows;
    TSharedPtr<const FStudioHome4SpatialEvidence,ESPMode::ThreadSafe> DisplayedEvidence;
};
