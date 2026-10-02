#pragma once
#include "Widgets/Input/SComboButton.h"
#include "Framework/Application/SlateApplication.h"

/** Form menus focus the first usable control when opened from the keyboard. */
class SStudioMenuButton final : public SComboButton
{
public:
    using FArguments=SComboButton::FArguments;
    void Construct(const FArguments& InArgs)
    {
        auto Args=InArgs;const auto BuildContent=InArgs._OnGetMenuContent;
        if(InArgs._IsFocusable&&BuildContent.IsBound())
            Args.OnGetMenuContent_Lambda([this,BuildContent]
            {const auto Content=BuildContent.Execute();SetMenuContentWidgetToFocus(FirstFocusable(Content));return Content;});
        SComboButton::Construct(Args);
    }
    /** The SComboButton container itself is not a keyboard target. Return to
     * its real inner button after a popup is dismissed. */
    void FocusButton()
    {
        if(const auto Button=FirstFocusable(SharedThis(this)))
            FSlateApplication::Get().SetKeyboardFocus(Button,EFocusCause::Navigation);
    }
private:
    static TSharedPtr<SWidget> FirstFocusable(const TSharedRef<SWidget>& Widget)
    {
        if(!Widget->GetVisibility().IsVisible()||!Widget->IsEnabled())return {};
        if(Widget->SupportsKeyboardFocus())return Widget;
        auto* Children=Widget->GetChildren();
        for(int32 I=0;I<Children->Num();++I)if(const auto Child=FirstFocusable(Children->GetChildAt(I)))return Child;
        return {};
    }
};
