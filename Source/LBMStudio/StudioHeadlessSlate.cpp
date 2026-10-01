#include "StudioHeadlessSlate.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Framework/Application/SlateApplication.h"
#include "Input/HittestGrid.h"
#include "GenericPlatform/GenericApplication.h"
#include "GenericPlatform/ICursor.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"
#include "Types/PaintArgs.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
// Engine FNullCursor is not exported from the macOS ApplicationCore dylib.
// Keep this test cursor in memory; never forward to the desktop cursor.
class FStudioHeadlessCursor final : public ICursor
{
public:
    FVector2D GetPosition() const override{return Position;}
    void SetPosition(int32 X,int32 Y) override{Position=FVector2D(X,Y);}
    void SetType(EMouseCursor::Type Value) override{Type=Value;}
    EMouseCursor::Type GetType() const override{return Type;}
    void GetSize(int32& Width,int32& Height) const override{Width=Height=1;}
    void Show(bool) override{}
    void Lock(const RECT* const) override{}
    void SetTypeShape(EMouseCursor::Type,void*) override{}
private:
    FVector2D Position=FVector2D::ZeroVector;
    EMouseCursor::Type Type=EMouseCursor::Default;
};
}
FStudioHeadlessSlate::FStudioHeadlessSlate(FAutomationTestBase& InTest,const TSharedRef<SWidget>& Content,FVector2D InSize)
    :Test(InTest),Window(SNew(SVirtualWindow).Size(InSize)),Size(InSize)
{
    auto& App=FSlateApplication::Get();PreviousFocus=App.GetKeyboardFocusedWidget();
    PreviousApplication=App.GetPlatformApplication();
    // Route real Slate events without involving the desktop cursor or macOS
    // IME, whose text contexts require native NSWindows. Restore on scope exit.
    App.OverridePlatformApplication(MakeShared<GenericApplication>(MakeShared<FStudioHeadlessCursor>()));
    Window->SetIsFocusable(true);Window->SetContent(Content);App.RegisterVirtualWindow(Window);Layout();
    Test.TestTrue(TEXT("Headless host has no OS window"),Window->GetNativeWindow()->GetOSWindowHandle()==nullptr);
}
FStudioHeadlessSlate::~FStudioHeadlessSlate()
{
    auto& App=FSlateApplication::Get();App.DismissAllMenus();App.ClearKeyboardFocus();App.UnregisterVirtualWindow(Window);
    App.OverridePlatformApplication(PreviousApplication);
    if(const auto Previous=PreviousFocus.Pin())App.SetKeyboardFocus(Previous);
}
void FStudioHeadlessSlate::Layout()
{
    // Paint populates cached geometry, text wrapping and hit-test data. Draw
    // elements stay in memory; no renderer DrawWindows or screenshot is used.
    Window->SlatePrepass(1.f);
    FHittestGrid Grid;Grid.SetHittestArea(FVector2D::ZeroVector,Size);
    FSlateWindowElementList Elements(Window);
    const auto Geometry=FGeometry::MakeRoot(Size,FSlateLayoutTransform());
    Window->Paint(FPaintArgs(nullptr,Grid,FVector2D::ZeroVector,FPlatformTime::Seconds(),0),Geometry,
        FSlateRect(0,0,Size.X,Size.Y),Elements,0,FWidgetStyle(),true);
}
TSharedPtr<SWidget> FStudioHeadlessSlate::FindIn(const TSharedRef<SWidget>& Widget,FName Tag)
{
    Widget->UpdateAllAttributes();if(!Widget->GetVisibility().IsVisible())return {};
    if(Widget->GetTag()==Tag)return Widget;
    auto* Children=Widget->GetChildren();
    for(int32 I=0;I<Children->Num();++I)if(auto Found=FindIn(Children->GetChildAt(I),Tag))return Found;
    return {};
}
TSharedPtr<SWidget> FStudioHeadlessSlate::Find(FName Tag)
{
    Layout();auto Found=FindIn(Window,Tag);Test.TestTrue(TEXT("Visible tagged control: ")+Tag.ToString(),Found.IsValid());return Found;
}
TSharedPtr<SWidget> FStudioHeadlessSlate::Focusable(const TSharedRef<SWidget>& Widget)
{
    if(!Widget->GetVisibility().IsVisible()||!Widget->IsEnabled())return {};
    if(Widget->SupportsKeyboardFocus())return Widget;
    auto* Children=Widget->GetChildren();
    for(int32 I=0;I<Children->Num();++I)if(auto Found=Focusable(Children->GetChildAt(I)))return Found;
    return {};
}
bool FStudioHeadlessSlate::Focus(const TSharedPtr<SWidget>& Widget)
{
    if(!Widget)return false;
    auto Target=Focusable(Widget.ToSharedRef());
    if(!Test.TestTrue(TEXT("Control has an enabled keyboard target"),Target.IsValid()))return false;
    FSlateApplication::Get().SetKeyboardFocus(Target,EFocusCause::Navigation);
    return Test.TestTrue(TEXT("Virtual control receives keyboard focus"),Target->HasKeyboardFocus()||Target->HasFocusedDescendants());
}
void FStudioHeadlessSlate::Key(FKey InKey)
{
    auto& App=FSlateApplication::Get();App.ProcessKeyDownEvent(FKeyEvent(InKey,FModifierKeysState(),0,false,0,0));
    App.ProcessKeyUpEvent(FKeyEvent(InKey,FModifierKeysState(),0,false,0,0));
}
bool FStudioHeadlessSlate::Press(FName Tag)
{
    if(!Focus(Find(Tag)))return false;Key(EKeys::Enter);Layout();return true;
}
bool FStudioHeadlessSlate::Type(FName Tag,const FString& Value)
{
    if(!Focus(Find(Tag)))return false;
    auto& App=FSlateApplication::Get();const FModifierKeysState SelectAll(false,false,true,false,false,false,false,false,false);
    App.ProcessKeyDownEvent(FKeyEvent(EKeys::A,SelectAll,0,false,0,0));App.ProcessKeyUpEvent(FKeyEvent(EKeys::A,SelectAll,0,false,0,0));
    if(Value.IsEmpty())Key(EKeys::BackSpace);
    else for(TCHAR Character:Value)App.ProcessKeyCharEvent(FCharacterEvent(Character,FModifierKeysState(),0,false));
    Layout();return true;
}
FString FStudioHeadlessSlate::Text(FName Tag)
{
    const auto Widget=Find(Tag);if(!Widget)return {};
    if(Widget->GetTypeAsString()==TEXT("STextBlock"))return StaticCastSharedPtr<STextBlock>(Widget)->GetText().ToString();
    if(Widget->GetTypeAsString()==TEXT("SFlowValueBox")||Widget->GetTypeAsString()==TEXT("SEditableTextBox"))
        return StaticCastSharedPtr<SEditableTextBox>(Widget)->GetText().ToString();
    Test.AddError(TEXT("Text requested from unsupported control: ")+Tag.ToString());return {};
}
bool FStudioHeadlessSlate::Inspect(const FString& Name,const TArray<FName>& Required)
{
    Layout();bool Valid=true;
    for(const auto Tag:Required)
    {
        const auto Widget=Find(Tag);if(!Widget){Valid=false;continue;}
        const auto& G=Widget->GetCachedGeometry();const auto P=G.GetAbsolutePosition(),S=G.GetLocalSize();
        Valid&=Test.TestTrue(TEXT("Arranged inside host: ")+Tag.ToString(),FMath::IsFinite(P.X)&&FMath::IsFinite(P.Y)&&
            FMath::IsFinite(S.X)&&FMath::IsFinite(S.Y)&&S.X>0&&S.Y>0&&P.X>=-.1&&P.Y>=-.1&&P.X+S.X<=Size.X+.1&&P.Y+S.Y<=Size.Y+.1);
    }
    TArray<TSharedPtr<FJsonValue>> Controls;
    TFunction<void(const TSharedRef<SWidget>&,bool)> Visit=[&](const TSharedRef<SWidget>& W,bool Enabled)
    {
        W->UpdateAllAttributes();if(!W->GetVisibility().IsVisible())return;Enabled&=W->IsEnabled();
        if(!W->GetTag().IsNone())
        {
            const auto& G=W->GetCachedGeometry();const auto P=G.GetAbsolutePosition(),S=G.GetLocalSize();
            auto Item=MakeShared<FJsonObject>();Item->SetStringField(TEXT("tag"),W->GetTag().ToString());Item->SetStringField(TEXT("type"),W->GetTypeAsString());
            Item->SetBoolField(TEXT("enabled"),Enabled);Item->SetBoolField(TEXT("focused"),W->HasKeyboardFocus()||W->HasFocusedDescendants());
            Item->SetNumberField(TEXT("x"),P.X);Item->SetNumberField(TEXT("y"),P.Y);Item->SetNumberField(TEXT("width"),S.X);Item->SetNumberField(TEXT("height"),S.Y);
            if(W->GetTypeAsString()==TEXT("STextBlock"))Item->SetStringField(TEXT("text"),StaticCastSharedRef<STextBlock>(W)->GetText().ToString());
            else if(W->GetTypeAsString()==TEXT("SFlowValueBox")||W->GetTypeAsString()==TEXT("SEditableTextBox"))
                Item->SetStringField(TEXT("text"),StaticCastSharedRef<SEditableTextBox>(W)->GetText().ToString());
            Controls.Add(MakeShared<FJsonValueObject>(Item));
        }
        auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)Visit(Children->GetChildAt(I),Enabled);
    };
    Visit(Window,true);
    auto Report=MakeShared<FJsonObject>();Report->SetStringField(TEXT("scope"),TEXT("Virtual Slate layout and routed keyboard behavior; no native window or GPU pixels"));
    Report->SetNumberField(TEXT("width"),Size.X);Report->SetNumberField(TEXT("height"),Size.Y);Report->SetArrayField(TEXT("controls"),Controls);Report->SetBoolField(TEXT("passed"),Valid);
    FString Root;FParse::Value(FCommandLine::Get(),TEXT("StudioHeadlessOutput="),Root);
    if(Root.IsEmpty())Root=FPaths::ProjectDir()/TEXT("tmp/debug/headless-widgets");
    IFileManager::Get().MakeDirectory(*Root,true);FString Json;FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json));
    return Test.TestTrue(TEXT("Write structural widget report"),FFileHelper::SaveStringToFile(Json,*(Root/(Name+TEXT(".json"))),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))&&Valid;
}
#endif
