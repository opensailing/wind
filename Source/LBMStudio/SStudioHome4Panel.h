#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioHome4Session.h"
#include "StudioHome4Telemetry.h"
class FStudioModel;
class SEditableTextBox;
class SVerticalBox;
struct FStudioHome4ValidationState;

DECLARE_DELEGATE_OneParam(FStudioHome4RecipeAction,const FString&);
/** Shared HOME4 authoring component inside the existing single-sidebar shell. */
class SStudioHome4Panel final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4Panel){}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4Session>,Session)
        SLATE_ARGUMENT(FString,Page)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4ValidationState>,Validation)
        SLATE_EVENT(FSimpleDelegate,OnSubmit)
        SLATE_ATTRIBUTE(const FStudioHome4TelemetryStream*,Telemetry)
        SLATE_EVENT(FStudioHome4RecipeAction,OnRecipe)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    void Tick(const FGeometry&,double,float) override;
private:
    TSharedRef<SWidget> Editor(const FStudioHome4Field& Field);
    TSharedRef<SWidget> Recipes();
    TSharedRef<SWidget> Feasibility();
    TSharedRef<SWidget> Action(const FString& Label,FName Tag,TFunction<void()> Callback);
    FString Summary() const;
    FString Command() const;
    void ExportSpec();
    void ImportSpec();
    void ExportReport();
    void Sync();
    TSharedPtr<FStudioModel> Model;
    TSharedPtr<FStudioHome4Session> Session;
    TSharedPtr<FStudioHome4ValidationState> Validation;
    FStudioHome4RecipeAction OnRecipe;
    TAttribute<const FStudioHome4TelemetryStream*> Telemetry;
    FString Page,LadderText,ReportName=TEXT("home4_viz");
    TMap<FString,TWeakPtr<SEditableTextBox>> Inputs;
    FString LastValues;
    FStudioHome4Spec Preview;
    FStudioHome4Derived Derived;
    FString PreviewError;
};
