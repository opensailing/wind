#pragma once
#include "Widgets/SCompoundWidget.h"
class FStudioModel;
class AStudioScene;
class SBox;
enum class EStudioHelpPage : uint8 { Workspace,Shortcuts,Diagnostics,About };

/** One bounded header popup. Local help/diagnostic state is separate from the
 * project, case, playback and camera. Results retains provenance ownership. */
class SStudioHelpPanel final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHelpPanel):_Page(EStudioHelpPage::Workspace){}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>,Model)
        SLATE_ARGUMENT(AStudioScene*,Scene)
        SLATE_ARGUMENT(EStudioHelpPage,Page)
        SLATE_EVENT(FSimpleDelegate,OnResults)
        SLATE_EVENT(FSimpleDelegate,OnClose)
        SLATE_ARGUMENT(TFunction<void(const FString&)>,CopyText)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    FReply OnPreviewKeyDown(const FGeometry& Geometry,const FKeyEvent& Event) override;
private:
    void Show(EStudioHelpPage Page);
    TSharedRef<SWidget> Body();
    void RefreshDiagnostics();
    TSharedPtr<FStudioModel> M;
    TWeakObjectPtr<AStudioScene> Scene;
    TSharedPtr<SBox> Content;
    EStudioHelpPage Page=EStudioHelpPage::Workspace;
    FString DiagnosticSnapshot,CopyNotice;
    FSimpleDelegate Results,Close;
    TFunction<void(const FString&)> Copy;
};
