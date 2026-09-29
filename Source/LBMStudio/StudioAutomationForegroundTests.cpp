#include "StudioAutomationForeground.h"
#include "Misc/AutomationTest.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/SWindow.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioForegroundTest
{
TSharedPtr<SWidget> Find(const TSharedRef<SWidget>& Widget,FName Tag)
{
    Widget->UpdateAllAttributes();
    if(!Widget->GetVisibility().IsVisible())return {};
    if(Widget->GetTag()==Tag)return Widget;
    auto* Children=Widget->GetChildren();
    for(int32 I=0;I<Children->Num();++I)if(auto Found=Find(Children->GetChildAt(I),Tag))return Found;
    return {};
}
TSharedPtr<SWidget> Focusable(const TSharedRef<SWidget>& Widget)
{
    if(!Widget->GetVisibility().IsVisible()||!Widget->IsEnabled())return {};
    if(Widget->SupportsKeyboardFocus())return Widget;
    auto* Children=Widget->GetChildren();
    for(int32 I=0;I<Children->Num();++I)if(auto Found=Focusable(Children->GetChildAt(I)))return Found;
    return {};
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioForegroundTest,"Studio.AutomationForeground.MenuDismissalAndLatchedInterruption",
    EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)
bool FStudioForegroundTest::RunTest(const FString&)
{
    auto& App=FSlateApplication::Get();
    TSharedPtr<SMenuAnchor> Anchor;
    for(const auto& Window:App.GetInteractiveTopLevelWindows())
        if(auto Widget=StudioForegroundTest::Find(Window,TEXT("ExportMenu"))){Anchor=StaticCastSharedPtr<SMenuAnchor>(Widget);break;}
    if(!TestTrue(TEXT("Existing shared Export owner available"),Anchor.IsValid()))return false;
    const auto Button=StudioForegroundTest::Focusable(Anchor.ToSharedRef());
    if(!TestTrue(TEXT("Existing menu supports keyboard input"),Button.IsValid()))return false;

    const bool PreviouslyActive=App.IsActive();
    const TWeakPtr<SWidget> PreviousFocus=App.GetKeyboardFocusedWidget();
    App.DismissAllMenus();
    // Deliver the same Slate activation events as the native platform without
    // changing the OS foreground application. This tests the interruption
    // contract, not physical macOS input or foreground stability.
    App.OnApplicationActivationChanged(true);
    {
        FStudioAutomationForeground Guard;Guard.Begin();
        auto Open=[&]
        {
            App.SetKeyboardFocus(Button,EFocusCause::Navigation);
            const FKeyEvent Enter(EKeys::Enter,FModifierKeysState(),0,false,0,0);
            App.ProcessKeyDownEvent(Enter);App.ProcessKeyUpEvent(Enter);
        };
        Open();
        TestTrue(TEXT("Routed input opens the real menu"),Anchor->IsOpen());
        TestFalse(TEXT("Opening a menu is not an interruption"),Guard.WasInterrupted());
        App.OnApplicationActivationChanged(false);
        TestFalse(TEXT("Deactivation dismisses the menu"),Anchor->IsOpen());
        TestTrue(TEXT("Deactivation records an interrupted workflow"),Guard.WasInterrupted());
        App.OnApplicationActivationChanged(true);
        TestTrue(TEXT("Reactivation cannot hide an interruption between test ticks"),Guard.WasInterrupted());
        TestFalse(TEXT("Reactivation does not recreate a dismissed menu"),Anchor->IsOpen());
        TestTrue(TEXT("Interruption has a machine-readable diagnostic"),Guard.Describe(7).StartsWith(TEXT("STUDIO_AUTOMATION_INTERRUPTED:")));
    }
    if(const auto Focus=PreviousFocus.Pin())App.SetKeyboardFocus(Focus,EFocusCause::SetDirectly);
    if(!PreviouslyActive)App.OnApplicationActivationChanged(false);
    return true;
}
#endif
