// UE 5.8's Development startup can disable LLM while a Cocoa allocation scope
// is still active, deleting the thread state that scope later needs. Module
// StartupModule is too late. Finder supplies no command-line options, so keep
// tracking enabled before main/engine startup, using the same executable
// and PID. The second image sees -LLM and proceeds without another exec.
// This is deliberately limited to the affected packaged Development profile;
// editor launches use Tools/run_studio.py. Re-evaluate when upgrading Unreal.
#if defined(__APPLE__) && UE_BUILD_DEVELOPMENT && !WITH_EDITOR

#include <mach-o/dyld.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

__attribute__((constructor))
static void StudioRetainStartupTracking(int ArgC, char** ArgV, char**)
{
    for (int Index = 1; Index < ArgC; ++Index)
    {
        if (strcasecmp(ArgV[Index], "-LLM") == 0)
        {
            return;
        }
    }

    // Use the loaded image's path, never argv[0], PATH, a shell or a helper.
    uint32_t PathSize = 0;
    _NSGetExecutablePath(nullptr, &PathSize);
    char* Executable = static_cast<char*>(malloc(PathSize));
    char** Arguments = static_cast<char**>(calloc(static_cast<size_t>(ArgC) + 2, sizeof(char*)));
    if (!Executable || !Arguments || _NSGetExecutablePath(Executable, &PathSize) != 0)
    {
        dprintf(STDERR_FILENO, "Studio startup: cannot prepare memory-tracker launch.\n");
        _exit(78);
    }
    for (int Index = 0; Index < ArgC; ++Index)
    {
        Arguments[Index] = ArgV[Index];
    }
    char TrackingArgument[] = "-LLM";
    Arguments[ArgC] = TrackingArgument;
    dprintf(STDERR_FILENO, "Studio startup: retaining memory tracking before engine initialization.\n");
    execv(Executable, Arguments);
    const int Error = errno;
    dprintf(STDERR_FILENO, "Studio startup: memory-tracker launch failed: %s.\n", strerror(Error));
    // Do not continue into the known unsafe startup path after a failed exec.
    _exit(78);
}

#endif
