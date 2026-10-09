#pragma once
#include "Widgets/SCompoundWidget.h"
class FStudioModel;
class FStudioHome4Session;
class FStudioHome4CheckpointSession;

/** Native warm-start picker; all checkpoint reads run through the shared worker session. */
class SStudioHome4Checkpoint final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SStudioHome4Checkpoint) {}
        SLATE_ARGUMENT(TSharedPtr<FStudioModel>, Model)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4Session>, Editor)
        SLATE_ARGUMENT(TSharedPtr<FStudioHome4CheckpointSession>, Session)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
#if WITH_DEV_AUTOMATION_TESTS
    static void SetNextPathForAutomation(const FString& Path);
#endif
private:
    void Pick();
    void Inspect();
    FString Details() const;
    TSharedPtr<FStudioHome4Session> Editor;
    TSharedPtr<FStudioHome4CheckpointSession> Session;
    FString Error;
};
