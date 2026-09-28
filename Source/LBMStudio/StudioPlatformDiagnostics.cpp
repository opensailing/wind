#include "StudioPlatformDiagnostics.h"
#if !PLATFORM_MAC
int64 StudioPlatformDiagnostics::DeviceAllocatedBytes() { return -1; }
void* StudioPlatformDiagnostics::BeginWindowEvents(void*) { return nullptr; }
void StudioPlatformDiagnostics::EndWindowEvents(void*) {}
#endif
