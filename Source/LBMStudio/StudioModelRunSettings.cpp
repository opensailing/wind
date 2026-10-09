#include "StudioModel.h"

bool FStudioModel::UpdateRunSettings(FStudioRunSettingsEdit& Edit)
{
    const auto Fail = [this, &Edit](const FString& Message)
    { Edit.Error = Notice = Message; return false; };
    if (IsProjectOpenPending() || IsRecordingLoadPending())
        return Fail(TEXT("Wait for the project or recording to finish opening before applying run parameters."));
    if (!Edit.Matches(Project.Draft))
        return Fail(TEXT("Run parameters changed outside this form. Revert before applying."));
    FStudioCaseSetup Settings;
    if (!Edit.Build(Settings)) return Fail(Edit.Error);
    if (!EditCase(TEXT("Edit run parameters"), [&Settings](auto& Case)
        { FStudioRunSettingsEdit::CopySettings(Settings, Case.Setup); })) return Fail(Notice);
    Notice = TEXT("Run parameters applied to the case. Existing runs and recorded playback are unchanged.");
    return true;
}
