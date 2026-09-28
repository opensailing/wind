#include "../StudioFileDialog.h"
#include "../StudioRecording.h"
#include "Mac/MacApplication.h"
#include "Mac/CocoaThread.h"
#include "Apple/ScopeAutoreleasePool.h"
#include "Misc/Paths.h"
#include "Misc/CoreDelegates.h"
#include "HAL/PlatformProcess.h"
#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <fcntl.h>
#include <unistd.h>

namespace
{
    NSString* BookmarkKey(const FString& Path)
    { return [@"LBMProjectBookmark:" stringByAppendingString:[NSString stringWithUTF8String:TCHAR_TO_UTF8(*Path)]]; }
}

void* StudioFileDialog::BeginAccess(const FString& Path)
{
    SCOPED_AUTORELEASE_POOL;
    FString AccessPath=Path;
    NSData* Bookmark=[[NSUserDefaults standardUserDefaults] dataForKey:BookmarkKey(AccessPath)];
    if(!Bookmark)
    {
        // Recording-folder selection grants both the metadata and payload.
        AccessPath=FPaths::GetPath(Path);
        Bookmark=[[NSUserDefaults standardUserDefaults] dataForKey:BookmarkKey(AccessPath)];
    }
    if(!Bookmark) return nullptr;
    BOOL Stale=NO; NSError* Error=nil;
    NSURL* URL=[NSURL URLByResolvingBookmarkData:Bookmark options:NSURLBookmarkResolutionWithSecurityScope|NSURLBookmarkResolutionWithoutUI relativeToURL:nil bookmarkDataIsStale:&Stale error:&Error];
    if(!URL || ![URL startAccessingSecurityScopedResource]) return nullptr;
    if(Stale)
    {
        NSData* Fresh=[URL bookmarkDataWithOptions:NSURLBookmarkCreationWithSecurityScope includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
        if(Fresh) [[NSUserDefaults standardUserDefaults] setObject:Fresh forKey:BookmarkKey(AccessPath)];
    }
    return [URL retain];
}
void StudioFileDialog::EndAccess(void* Token)
{
    if(Token) { NSURL* URL=(NSURL*)Token; [URL stopAccessingSecurityScopedResource]; [URL release]; }
}
void StudioFileDialog::RememberAccess(const FString& Path)
{
    SCOPED_AUTORELEASE_POOL;
    NSURL* URL=[NSURL fileURLWithPath:[NSString stringWithUTF8String:TCHAR_TO_UTF8(*Path)]];
    NSData* Bookmark=[URL bookmarkDataWithOptions:NSURLBookmarkCreationWithSecurityScope includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
    if(Bookmark) [[NSUserDefaults standardUserDefaults] setObject:Bookmark forKey:BookmarkKey(Path)];
}
static bool WriteDataAtomic(const FString& Path,NSData* Data,FString& OutError)
{
    SCOPED_AUTORELEASE_POOL;
    NSURL* URL=[NSURL fileURLWithPath:[NSString stringWithUTF8String:TCHAR_TO_UTF8(*Path)]];
    NSFileCoordinator* Coordinator=[[NSFileCoordinator alloc] initWithFilePresenter:nil];
    __block BOOL OK=NO; __block NSError* WriteError=nil; NSError* CoordinationError=nil;
    [Coordinator coordinateWritingItemAtURL:URL options:NSFileCoordinatorWritingForReplacing error:&CoordinationError byAccessor:^(NSURL* Destination) {
        OK=[Data writeToURL:Destination options:NSDataWritingAtomic error:&WriteError];
        // NSData owns the safe-save temporary file and sandbox coordination.
        if(OK) { int FD=open(Destination.fileSystemRepresentation,O_RDONLY); if(FD>=0){fsync(FD);close(FD);} }
    }];
    [Coordinator release];
    if(!OK)
    {
        NSError* Failure=WriteError?WriteError:CoordinationError;
        OutError=Failure?FString(UTF8_TO_TCHAR(Failure.localizedDescription.UTF8String)):TEXT("Could not replace the project file.");
    }
    else OutError.Empty();
    return OK;
}
bool StudioFileDialog::WriteAtomic(const FString& Path,const FString& Text,FString& Error)
{
    SCOPED_AUTORELEASE_POOL;
    const FTCHARToUTF8 Bytes(*Text);
    return WriteDataAtomic(Path,[NSData dataWithBytes:Bytes.Get() length:Bytes.Length()],Error);
}
bool StudioFileDialog::WriteAtomicBytes(const FString& Path,const TArray64<uint8>& Bytes,FString& Error)
{
    SCOPED_AUTORELEASE_POOL;
    return WriteDataAtomic(Path,[NSData dataWithBytesNoCopy:const_cast<uint8*>(Bytes.GetData()) length:Bytes.Num() freeWhenDone:NO],Error);
}

bool StudioFileDialog::SnapshotPNG(const FString& SuggestedName,FString& OutPath)
{
#if WITH_DEV_AUTOMATION_TESTS
    bool Automated=false;if(ConsumeSnapshotPNGForAutomation(OutPath,Automated))return Automated;
#endif
    MacApplication->SetCapture(nullptr);
#if WITH_EDITOR
    FCoreDelegates::PreModal.Broadcast();
#endif
    MacApplication->SystemModalMode(true);
    const bool Accepted=MainThreadReturn(^{
        SCOPED_AUTORELEASE_POOL;
        NSSavePanel* Panel=[NSSavePanel savePanel];Panel.title=@"Save Flow Snapshot";
        Panel.message=@"Saves the captured image with its recorded frame, camera and inspection metadata embedded in the PNG.";
        Panel.canCreateDirectories=YES;Panel.allowedContentTypes=@[UTTypePNG];
        Panel.nameFieldStringValue=[NSString stringWithUTF8String:TCHAR_TO_UTF8(*(SuggestedName+TEXT(".png")))];
        const bool OK=[Panel runModal]==NSModalResponseOK;
        // The save panel grants access for this export. A new file does not
        // exist yet and cannot have a security-scoped bookmark.
        if(OK)OutPath=UTF8_TO_TCHAR(Panel.URL.path.UTF8String);
        [Panel close];return OK;
    });
    MacApplication->SystemModalMode(false);MacApplication->ResetModifierKeys();
#if WITH_EDITOR
    FCoreDelegates::PostModal.Broadcast();
#endif
    return Accepted;
}

bool StudioFileDialog::Project(bool bSave, const FString& CurrentPath, const FString& SuggestedName, FString& OutPath)
{
    MacApplication->SetCapture(nullptr);
#if WITH_EDITOR
    FCoreDelegates::PreModal.Broadcast();
#endif
    MacApplication->SystemModalMode(true);
    const bool bAccepted=MainThreadReturn(^{
        SCOPED_AUTORELEASE_POOL;
        NSSavePanel* Panel=bSave ? [NSSavePanel savePanel] : [NSOpenPanel openPanel];
        if(!bSave)
        {
            NSOpenPanel* Open=(NSOpenPanel*)Panel;
            Open.canChooseFiles=YES; Open.canChooseDirectories=NO; Open.allowsMultipleSelection=NO;
        }
        Panel.title=bSave ? @"Save LBM Studio Project" : @"Open LBM Studio Project";
        Panel.canCreateDirectories=bSave;
        UTType* ProjectType=[UTType typeWithFilenameExtension:@"lbms"];
        if(ProjectType) Panel.allowedContentTypes=bSave ? @[ProjectType] : @[ProjectType,UTTypeJSON];
        const FString Directory=CurrentPath.IsEmpty() ? FString(FPlatformProcess::UserDir()) : FPaths::GetPath(CurrentPath);
        Panel.directoryURL=[NSURL fileURLWithPath:[NSString stringWithUTF8String:TCHAR_TO_UTF8(*Directory)]];
        if(bSave) Panel.nameFieldStringValue=[NSString stringWithUTF8String:TCHAR_TO_UTF8(*(SuggestedName+TEXT(".lbms")))];
        const bool bOK=[Panel runModal]==NSModalResponseOK;
        if(bOK)
        {
            OutPath=UTF8_TO_TCHAR(Panel.URL.path.UTF8String);
            // Saving remembers access only after StudioProjectIO creates the file.
            if(!bSave)RememberAccess(OutPath);
        }
        [Panel close]; return bOK;
    });
    MacApplication->SystemModalMode(false);
    MacApplication->ResetModifierKeys();
#if WITH_EDITOR
    FCoreDelegates::PostModal.Broadcast();
#endif
    return bAccepted;
}

bool StudioFileDialog::ProbeCSV(const FString& SuggestedName,FString& OutPath)
{
#if WITH_DEV_AUTOMATION_TESTS
    bool Automated=false;if(ConsumeProbeCSVForAutomation(OutPath,Automated))return Automated;
#endif
    MacApplication->SetCapture(nullptr);
#if WITH_EDITOR
    FCoreDelegates::PreModal.Broadcast();
#endif
    MacApplication->SystemModalMode(true);
    const bool Accepted=MainThreadReturn(^{
        SCOPED_AUTORELEASE_POOL;
        NSSavePanel* Panel=[NSSavePanel savePanel];Panel.title=@"Export Probe Samples";
        Panel.message=@"Exports the samples and source/frame identity captured when Export was clicked.";
        Panel.canCreateDirectories=YES;
        UTType* CSVType=[UTType typeWithFilenameExtension:@"csv"];if(CSVType)Panel.allowedContentTypes=@[CSVType];
        Panel.nameFieldStringValue=[NSString stringWithUTF8String:TCHAR_TO_UTF8(*(SuggestedName+TEXT(".csv")))];
        const bool OK=[Panel runModal]==NSModalResponseOK;
        if(OK)OutPath=UTF8_TO_TCHAR(Panel.URL.path.UTF8String);
        [Panel close];return OK;
    });
    MacApplication->SystemModalMode(false);MacApplication->ResetModifierKeys();
#if WITH_EDITOR
    FCoreDelegates::PostModal.Broadcast();
#endif
    return Accepted;
}

bool StudioFileDialog::Geometry(const FString& CurrentPath,FString& OutPath)
{
    MacApplication->SetCapture(nullptr);
#if WITH_EDITOR
    FCoreDelegates::PreModal.Broadcast();
#endif
    MacApplication->SystemModalMode(true);
    const bool Accepted=MainThreadReturn(^{
        SCOPED_AUTORELEASE_POOL;
        NSOpenPanel* Panel=[NSOpenPanel openPanel];
        Panel.title=@"Locate Original Geometry";
        Panel.message=@"Choose a copy of the original file. Its contents will be verified before the location changes.";
        Panel.canChooseFiles=YES; Panel.canChooseDirectories=NO; Panel.allowsMultipleSelection=NO;
        // A byte-identical source may have been renamed; do not filter by suffix.
        const FString Directory=CurrentPath.IsEmpty()?FString(FPlatformProcess::UserDir()):FPaths::GetPath(CurrentPath);
        Panel.directoryURL=[NSURL fileURLWithPath:[NSString stringWithUTF8String:TCHAR_TO_UTF8(*Directory)]];
        const bool OK=[Panel runModal]==NSModalResponseOK;
        if(OK) {OutPath=UTF8_TO_TCHAR(Panel.URL.path.UTF8String);RememberAccess(OutPath);}
        [Panel close];return OK;
    });
    MacApplication->SystemModalMode(false);MacApplication->ResetModifierKeys();
#if WITH_EDITOR
    FCoreDelegates::PostModal.Broadcast();
#endif
    return Accepted;
}

bool StudioFileDialog::ImportGeometry(FString& OutPath)
{
    MacApplication->SetCapture(nullptr);
#if WITH_EDITOR
    FCoreDelegates::PreModal.Broadcast();
#endif
    MacApplication->SystemModalMode(true);
    const bool Accepted=MainThreadReturn(^{
        SCOPED_AUTORELEASE_POOL;
        NSOpenPanel* Panel=[NSOpenPanel openPanel];Panel.title=@"Import Geometry";
        Panel.message=@"Choose an STL or OBJ. Preview its units and orientation before adding it to the case.";
        Panel.canChooseFiles=YES;Panel.canChooseDirectories=NO;Panel.allowsMultipleSelection=NO;
        Panel.allowedContentTypes=@[[UTType typeWithFilenameExtension:@"stl"],[UTType typeWithFilenameExtension:@"obj"]];
        const bool OK=[Panel runModal]==NSModalResponseOK;
        if(OK){OutPath=UTF8_TO_TCHAR(Panel.URL.path.UTF8String);RememberAccess(OutPath);}[Panel close];return OK;
    });
    MacApplication->SystemModalMode(false);MacApplication->ResetModifierKeys();
#if WITH_EDITOR
    FCoreDelegates::PostModal.Broadcast();
#endif
    return Accepted;
}

bool StudioFileDialog::RecordingFolder(const FString& CurrentPath,FString& OutPath)
{
#if WITH_DEV_AUTOMATION_TESTS
    bool bAutomationAccepted=false;
    if(ConsumeRecordingFolderForAutomation(OutPath,bAutomationAccepted))return bAutomationAccepted;
#endif
    MacApplication->SetCapture(nullptr);
#if WITH_EDITOR
    FCoreDelegates::PreModal.Broadcast();
#endif
    MacApplication->SystemModalMode(true);
    const bool Accepted=MainThreadReturn(^{
        SCOPED_AUTORELEASE_POOL;
        NSOpenPanel* Panel=[NSOpenPanel openPanel];
        Panel.title=@"Choose Recording Folder";
        Panel.message=@"Choose the folder containing recording.json and its data files. The recording will be verified before it opens.";
        Panel.canChooseFiles=NO;Panel.canChooseDirectories=YES;Panel.allowsMultipleSelection=NO;
        Panel.canCreateDirectories=NO;
        if(!CurrentPath.IsEmpty()) Panel.directoryURL=[NSURL fileURLWithPath:[NSString stringWithUTF8String:TCHAR_TO_UTF8(*FPaths::GetPath(CurrentPath))]];
        const bool OK=[Panel runModal]==NSModalResponseOK;
        if(OK)
        {
            const FString Folder=UTF8_TO_TCHAR(Panel.URL.path.UTF8String);
            RememberAccess(Folder);OutPath=StudioRecordings::DescriptorOrPayloadInFolder(Folder);
        }
        [Panel close];return OK;
    });
    MacApplication->SystemModalMode(false);MacApplication->ResetModifierKeys();
#if WITH_EDITOR
    FCoreDelegates::PostModal.Broadcast();
#endif
    return Accepted;
}

bool StudioFileDialog::ReconstructionFolder(const FString& CurrentPath,FString& OutPath)
{
#if WITH_DEV_AUTOMATION_TESTS
    bool bAutomationAccepted=false;
    if(ConsumeReconstructionFolderForAutomation(OutPath,bAutomationAccepted))return bAutomationAccepted;
#endif
    MacApplication->SetCapture(nullptr);
#if WITH_EDITOR
    FCoreDelegates::PreModal.Broadcast();
#endif
    MacApplication->SystemModalMode(true);
    const bool Accepted=MainThreadReturn(^{
        SCOPED_AUTORELEASE_POOL;
        NSOpenPanel* Panel=[NSOpenPanel openPanel];
        Panel.title=@"Choose Reconstruction Folder";
        Panel.message=@"Choose reconstruction.json's folder and its mapping files. The reconstruction must match this recording's original points. No CFD values are created.";
        Panel.canChooseFiles=NO;Panel.canChooseDirectories=YES;Panel.allowsMultipleSelection=NO;
        Panel.canCreateDirectories=NO;
        if(!CurrentPath.IsEmpty())Panel.directoryURL=[NSURL fileURLWithPath:[NSString stringWithUTF8String:TCHAR_TO_UTF8(*FPaths::GetPath(CurrentPath))]];
        const bool OK=[Panel runModal]==NSModalResponseOK;
        if(OK)
        {
            const FString Folder=UTF8_TO_TCHAR(Panel.URL.path.UTF8String);
            RememberAccess(Folder);OutPath=Folder/TEXT("reconstruction.json");
        }
        [Panel close];return OK;
    });
    MacApplication->SystemModalMode(false);MacApplication->ResetModifierKeys();
#if WITH_EDITOR
    FCoreDelegates::PostModal.Broadcast();
#endif
    return Accepted;
}
