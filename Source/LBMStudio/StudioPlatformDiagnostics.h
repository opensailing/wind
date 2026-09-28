#pragma once
#include "CoreMinimal.h"

namespace StudioPlatformDiagnostics
{
    /** Current native device allocations in bytes, or -1 when unsupported. */
    int64 DeviceAllocatedBytes();
    /** Observe native activation/minimize events for an explicit acceptance run.
     * Handle is the platform window; release the returned token on shutdown. */
    void* BeginWindowEvents(void* Handle);
    void EndWindowEvents(void* Token);
}
