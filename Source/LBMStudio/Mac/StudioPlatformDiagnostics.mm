#include "../StudioPlatformDiagnostics.h"
#include "DynamicRHI.h"
#include "Mac/MacSystemIncludes.h"
#include "Mac/CocoaThread.h"
#import <Metal/Metal.h>

int64 StudioPlatformDiagnostics::DeviceAllocatedBytes()
{
    if(!GDynamicRHI||FCString::Stricmp(GDynamicRHI->GetName(),TEXT("Metal"))!=0)return -1;
    id<MTLDevice> Device=(id<MTLDevice>)GDynamicRHI->RHIGetNativeDevice();
    return Device?static_cast<int64>(Device.currentAllocatedSize):-1;
}

void* StudioPlatformDiagnostics::BeginWindowEvents(void* Handle)
{
    if(!Handle)return nullptr;
    return MainThreadReturn(^{
        NSWindow* Window=(NSWindow*)Handle;
        NSMutableArray* Tokens=[[NSMutableArray alloc] init];
        NSArray* Names=@[NSWindowWillMiniaturizeNotification,NSWindowDidMiniaturizeNotification,
            NSWindowDidDeminiaturizeNotification,NSWindowDidBecomeKeyNotification,
            NSWindowDidResignKeyNotification,NSWindowDidResizeNotification,
            NSWindowWillStartLiveResizeNotification,NSWindowDidEndLiveResizeNotification,
            NSWindowDidChangeScreenNotification,NSApplicationDidBecomeActiveNotification,
            NSApplicationDidResignActiveNotification,NSApplicationDidHideNotification,
            NSApplicationDidUnhideNotification];
        for(NSString* Name in Names)
        {
            const bool bWindowEvent=[Name hasPrefix:@"NSWindow"];
            id Token=[[NSNotificationCenter defaultCenter] addObserverForName:Name
                object:bWindowEvent?Window:NSApp queue:nil usingBlock:^(NSNotification* Note){
                UE_LOG(LogTemp,Display,TEXT("Studio native window event: %s; minimized=%d visible=%d key=%d active=%d content=%.0fx%.0f"),
                    UTF8_TO_TCHAR(Note.name.UTF8String),int(Window.miniaturized),int(Window.visible),
                    int(Window.keyWindow),int(NSApp.active),double(Window.contentView.bounds.size.width),double(Window.contentView.bounds.size.height));
            }];
            [Tokens addObject:Token];
        }
        return (void*)Tokens;
    });
}

void StudioPlatformDiagnostics::EndWindowEvents(void* Token)
{
    if(!Token)return;
    MainThreadCall(^{
        NSArray* Tokens=(NSArray*)Token;
        for(id Observer in Tokens)[[NSNotificationCenter defaultCenter] removeObserver:Observer];
        [Tokens release];
    });
}
