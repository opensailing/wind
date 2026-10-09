#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioFlowConditions.h"
class FStudioModel;
class SEditableTextBox;

/** Draft lifetime follows the workspace, not the visible inspector category. */
class SStudioFlowConditions final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioFlowConditions){}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model)
        SLATE_EVENT(FSimpleDelegate,OnMaterials)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    void Refresh();
    bool HasUnapplied();
    void RequireResolution();
private:
    bool Available() const;
    void Synchronize();
    void Changed();
    void Apply();
    void Revert();
    void Calculate(bool bSpeed);
    void Focus(int32 Field);
    TSharedRef<SWidget> Input(int32 Field);
    TSharedRef<SWidget> Unit(FStudioFlowConditionsEdit::EField Field);
    TSharedRef<SWidget> Field(int32 Index,const TCHAR* Label);
    TSharedRef<SWidget> Action(const TCHAR* Label,FName Tag,TFunction<void()> Callback);
    FString Status() const;
    TWeakPtr<FStudioModel> Model;
    FStudioFlowConditionsEdit Edit;
    FGuid Project;
    int64 Revision=-1;
    bool bConflict=false,bSynchronizing=false,bAdvanced=false;
    TOptional<double> Computed;
    FString Notice;
    TWeakPtr<SEditableTextBox> Inputs[FStudioFlowConditionsEdit::FieldCount];
    static const TCHAR* SaveGuard;
};
