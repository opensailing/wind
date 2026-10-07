#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4Session.h"
#include "StudioHome4Telemetry.h"
#include "SStudioHome4SpatialDiagnostics.h"
class FStudioModel;
class FStudioHome4RuntimeSession;
class FStudioHome4AuthoringSession;
class SStudioHome4Monitors;
class SEditableTextBox;
class SVerticalBox;
class SScrollBox;
struct FStudioHome4ValidationState;

DECLARE_DELEGATE_OneParam(FStudioHome4RecipeAction,const FString&);
DECLARE_DELEGATE_TwoParams(FStudioHome4FieldFocus,const FString&,const FString&);
/** Shared HOME4 authoring component inside the existing single-sidebar shell. */
class SStudioHome4Panel final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4Panel){}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4Session>,Session)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4RuntimeSession>,Runtime)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4AuthoringSession>,Authoring)
        SLATE_ARGUMENT(TSharedPtr<SStudioHome4Monitors>,Monitors)
        SLATE_ARGUMENT(FString,Page)
        SLATE_ARGUMENT(FString,ImportPath)
        SLATE_ARGUMENT(FString,ExportPath)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4SpatialSession>,Spatial)
        SLATE_EVENT(FStudioHome4SpatialLocate,OnLocateSpatial)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4ValidationState>,Validation)
        SLATE_EVENT(FSimpleDelegate,OnSubmit)
        SLATE_ATTRIBUTE(const FStudioHome4TelemetryStream*,Telemetry)
        SLATE_ATTRIBUTE(TOptional<FStudioHome4TelemetryProvenance>,TelemetryProvenance)
        SLATE_EVENT(FStudioHome4RecipeAction,OnRecipe)
        SLATE_EVENT(FStudioHome4FieldFocus,OnFocusField)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    void Tick(const FGeometry&,double,float) override;
    bool FocusField(const FString& Key);
    bool HasField(const FString& Key)const;
private:
    TSharedRef<SWidget> Editor(const FStudioHome4Field& Field);
    TSharedRef<SWidget> Recipes();
    TSharedRef<SWidget> Feasibility();
    TSharedRef<SWidget> Action(const FString& Label,FName Tag,TFunction<void()> Callback);
    FString Summary() const;
    FString SummaryTooltip() const;
    FString Command() const;
    void ExportSpec();
    void ImportSpec();
    void ExportReport();
    void Sync();
    bool CommitPending();
    EStudioHome4UnitDisplay FieldDisplay(const FStudioHome4Field&)const;
    FString FieldUnit(const FStudioHome4Field&)const;
    TSharedPtr<FStudioModel> Model;
    TSharedPtr<FStudioHome4Session> Session;
    TSharedPtr<FStudioHome4RuntimeSession> Runtime;
    TSharedPtr<FStudioHome4AuthoringSession> Authoring;
    TSharedPtr<SStudioHome4Monitors> Monitors;
    bool bSyncing=false;
    TSharedPtr<FStudioHome4ValidationState> Validation;
    TSharedPtr<FStudioHome4SpatialSession> Spatial;
    FStudioHome4RecipeAction OnRecipe;FStudioHome4FieldFocus OnFocusField;
    TSharedPtr<SScrollBox> EditorScroll;TSharedPtr<SVerticalBox> IssueRows;
    TMap<FString,TWeakPtr<SWidget>> FieldTargets;
    TAttribute<const FStudioHome4TelemetryStream*> Telemetry;
    TAttribute<TOptional<FStudioHome4TelemetryProvenance>> TelemetryProvenance;
    FString Page,LadderText,ImportPath,ExportPath,ReportName=TEXT("home4_viz");
    TMap<FString,TWeakPtr<SEditableTextBox>> Inputs;
    FString LastValues,PendingLocalFocus;int32 LocalFocusRetries=0;
    FStudioHome4Spec Preview;
    FStudioHome4Derived Derived;
    FString PreviewError;
};
