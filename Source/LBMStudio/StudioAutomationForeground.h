#pragma once

#include "Framework/Application/SlateApplication.h"

#if WITH_DEV_AUTOMATION_TESTS
/** Native UI assertions require an uninterrupted foreground session. Slate
 * dismisses menus on app deactivation; reactivating cannot restore that state.
 * Latch the event so a deactivate/reactivate pair between ticks is not missed. */
class FStudioAutomationForeground final
{
public:
    FStudioAutomationForeground()=default;
    FStudioAutomationForeground(const FStudioAutomationForeground&)=delete;
    FStudioAutomationForeground& operator=(const FStudioAutomationForeground&)=delete;
    ~FStudioAutomationForeground()
    {
        if(Activation.IsValid()&&FSlateApplication::IsInitialized())
            FSlateApplication::Get().OnApplicationActivationStateChanged().Remove(Activation);
    }

    void Begin()
    {
        check(!Activation.IsValid());
        auto& App=FSlateApplication::Get();
        check(App.IsActive());
        Activation=App.OnApplicationActivationStateChanged().AddLambda([this](bool Active)
        {
            if(!Active&&!bInterrupted){bInterrupted=true;LostAtFrame=GFrameCounter;}
        });
    }

    bool WasInterrupted() const {return bInterrupted;}
    FString Describe(int32 Phase) const
    {
        return FString::Printf(TEXT("STUDIO_AUTOMATION_INTERRUPTED: application lost foreground focus at frame %llu (workflow phase %d). "
            "Slate dismisses menus on deactivation. Run this native UI suite with LBMStudio in the foreground; no menu action was retried."),LostAtFrame,Phase);
    }

private:
    FDelegateHandle Activation;
    uint64 LostAtFrame=0;
    bool bInterrupted=false;
};
#endif
