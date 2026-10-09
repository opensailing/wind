#pragma once
#include "Widgets/SCompoundWidget.h"
#include "StudioCommands.h"

class SEditableTextBox;
class SComboButton;
class FStudioModel;

/** One application command entry point in the expanded activity log. Responses
 * remain visible independently of log filters and collection/follow state. */
class SStudioCommandInput final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioCommandInput) {}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model)
        SLATE_ARGUMENT(TFunction<bool(EStudioCommand,FString&)>,Execute)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    void Tick(const FGeometry& Geometry,double Time,float Delta) override;
private:
    void SetDraft(const FString& Value,bool bFocus=false);
    void Submit();
    FReply Key(const FGeometry& Geometry,const FKeyEvent& Event);
    TSharedRef<SWidget> CommandsMenu();
    TWeakPtr<FStudioModel> Model;
    TWeakPtr<SEditableTextBox> Input;
    TWeakPtr<SComboButton> Menu;
    TFunction<bool(EStudioCommand,FString&)> Execute;
    FStudioCommandHistory History;
    FGuid Project;
    FString Draft,Response,Suggestions;
    bool bSettingText=false,bSuccess=true,bOversize=false;
};
