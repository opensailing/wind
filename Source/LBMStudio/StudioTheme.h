#pragma once
#include "CoreMinimal.h"
#include "Brushes/SlateColorBrush.h"
#include "Styling/SlateTypes.h"
#include "Widgets/Text/STextBlock.h"

/** Shared native styles, owned by the workspace's established visual system. */
namespace StudioUI
{
    extern const FLinearColor Text,Muted,Cyan,Amber;
    extern const FSlateColorBrush PanelBrush;
    FSlateFontInfo Font(int32 Size=10,bool Bold=false);
    const FButtonStyle& ButtonStyle();
    const FEditableTextBoxStyle& InputStyle();
    TSharedRef<STextBlock> Label(const FString& Value,int32 Size=10,FLinearColor Color=Text,bool Bold=false);
}
