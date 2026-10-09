#pragma once

#include "Framework/Application/SlateApplication.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Slate/SGameLayerManager.h"
#include "Widgets/SWindow.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioAuthoringTestCapture
{
    inline void DismissTooltips()
    {
        auto& App=FSlateApplication::Get();App.CloseToolTip();
        // UE5.8 CloseToolTip resets the visualizer reference without clearing
        // SGameLayerManager's in-window presenter. Disabling tooltips skips its
        // usual replacement path, leaving already-presented content visible.
        // Clear that presenter through the public widget API before the next
        // frame. This is test setup only; interactive tooltips remain enabled.
        if(GEngine&&GEngine->GameViewport)
            if(const auto Layer=GEngine->GameViewport->GetGameLayerManager())Layer->AsWidget()->OnVisualizeTooltip(nullptr);
    }
}
#endif
