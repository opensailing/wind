#include "StudioModel.h"

bool FStudioModel::UpdateFlowConditions(FStudioFlowConditionsEdit& Edit)
{
    const auto Fail=[this,&Edit](const FString& Message){Edit.Error=Notice=Message;return false;};
    if(IsProjectOpenPending() || IsRecordingLoadPending())
        return Fail(TEXT("Wait for project or recording opening before applying flow conditions."));
    if(!Edit.Matches(Project.Draft))
        return Fail(TEXT("Flow conditions or linked viscosity changed. Revert before applying."));
    FStudioCaseSetup Candidate;if(!Edit.Build(Candidate))return Fail(Edit.Error);
    if(!EditCase(TEXT("Edit flow conditions"),[&](auto& Case){FStudioFlowConditionsEdit::CopySettings(Candidate,Case.Setup);}))return Fail(Notice);
    Notice=TEXT("Flow conditions applied to the next-run case. Save to keep them.");return true;
}
