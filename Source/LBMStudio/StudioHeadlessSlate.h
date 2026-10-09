#pragma once

#if WITH_DEV_AUTOMATION_TESTS
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Widgets/SVirtualWindow.h"

/** Real Slate widget layout and routed keyboard input without an OS window,
 * GPU submission or screenshot. Use stable widget tags as the test interface.
 * This verifies widget behavior/layout, not rasterization or native dialogs. */
class FStudioHeadlessSlate final
{
public:
    FStudioHeadlessSlate(FAutomationTestBase& Test, const TSharedRef<SWidget>& Content, FVector2D Size);
    ~FStudioHeadlessSlate();
    FStudioHeadlessSlate(const FStudioHeadlessSlate&)=delete;
    FStudioHeadlessSlate& operator=(const FStudioHeadlessSlate&)=delete;
    void Layout();
    TSharedPtr<SWidget> Find(FName Tag);
    bool Exists(FName Tag);
    bool Focus(FName Tag);
    void Key(FKey Key);
    bool Press(FName Tag);
    bool Type(FName Tag, const FString& Value);
    FString Text(FName Tag);
    /** Validate required controls are arranged inside the virtual window and
     * serialize tagged geometry/text/state for inspection by tools. */
    bool Inspect(const FString& Name, const TArray<FName>& Required);
private:
    FAutomationTestBase& Test;
    TSharedRef<SVirtualWindow> Window;
    TWeakPtr<SWidget> PreviousFocus;
    TSharedPtr<class GenericApplication> PreviousApplication;
    FVector2D Size;
    TSharedPtr<SWidget> FindIn(const TSharedRef<SWidget>& Widget, FName Tag);
    TSharedPtr<SWidget> Focusable(const TSharedRef<SWidget>& Widget);
    bool Focus(const TSharedPtr<SWidget>& Widget);
};
#endif
