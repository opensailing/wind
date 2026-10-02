// UE 5.8's Development startup can disable LLM while a Cocoa allocation scope
// is still active. Retain tracking before UEAppDelegate assembles the engine
// command line from NSProcessInfo.arguments. A sandboxed app must not exec
// itself: macOS 15 rejects the second initialization as forbidden-sandbox-reinit.
// This argument adapter is limited to the affected packaged Development build;
// editor launches use Tools/run_studio.py. Re-evaluate when upgrading Unreal.
#if defined(__APPLE__) && UE_BUILD_DEVELOPMENT && !WITH_EDITOR

#import <Foundation/NSArray.h>
#import <Foundation/NSProcessInfo.h>
#import <Foundation/NSString.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <unistd.h>

using FStudioProcessArguments = NSArray<NSString*>* (*)(id, SEL);
static FStudioProcessArguments StudioOriginalProcessArguments = nullptr;

static NSArray<NSString*>* StudioArgumentsWithStartupTracking(id ProcessInfo, SEL Selector)
{
    NSArray<NSString*>* Arguments = StudioOriginalProcessArguments(ProcessInfo, Selector);
    for (NSString* Argument in Arguments)
    {
        if ([Argument caseInsensitiveCompare:@"-LLM"] == NSOrderedSame)
        {
            return Arguments;
        }
    }
    // Return a derived immutable array; do not modify the original argv or
    // Foundation's cached arguments. Repeated reads append exactly one flag.
    return [Arguments arrayByAddingObject:@"-LLM"];
}

__attribute__((constructor))
static void StudioRetainStartupTracking()
{
    // NSProcessInfo uses a concrete runtime subclass. Inspect the actual
    // singleton class rather than replacing a method on its abstract facade.
    Method ArgumentsMethod = class_getInstanceMethod(
        object_getClass([NSProcessInfo processInfo]), @selector(arguments));
    if (!ArgumentsMethod || !method_getImplementation(ArgumentsMethod))
    {
        dprintf(STDERR_FILENO, "Studio startup: cannot prepare memory tracking arguments.\n");
        _exit(78);
    }
    StudioOriginalProcessArguments = reinterpret_cast<FStudioProcessArguments>(
        method_getImplementation(ArgumentsMethod));
    method_setImplementation(ArgumentsMethod,
        reinterpret_cast<IMP>(StudioArgumentsWithStartupTracking));
}

#endif
