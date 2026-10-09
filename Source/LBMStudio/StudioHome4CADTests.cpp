#include "StudioHome4Authoring.h"
#include "StudioHome4Body.h"
#include "StudioFileDialog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#if PLATFORM_MAC
#include <sys/stat.h>
#endif

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioHome4CADTestsLocal
{
FString Quote(FString Value)
{
    Value.ReplaceInline(TEXT("\\"),TEXT("\\\\"));Value.ReplaceInline(TEXT("\""),TEXT("\\\""));
    return TEXT("\"")+Value+TEXT("\"");
}
bool RunBoundedPython(const FString& Python,const FString& Args,FString& Error)
{
    FProcHandle Process=FPlatformProcess::CreateProc(*Python,*Args,false,true,true,nullptr,0,nullptr,nullptr);
    if(!Process.IsValid()){Error=TEXT("The shipped interpreter could not start inside the native application.");return false;}
    const double Deadline=FPlatformTime::Seconds()+45;
    while(FPlatformProcess::IsProcRunning(Process)&&FPlatformTime::Seconds()<Deadline)FPlatformProcess::Sleep(.02f);
    if(FPlatformProcess::IsProcRunning(Process))
    {FPlatformProcess::TerminateProc(Process,true);FPlatformProcess::CloseProc(Process);Error=TEXT("Shipped CAD fixture generation exceeded 45 seconds.");return false;}
    int32 Code=-1;const bool Returned=FPlatformProcess::GetProcReturnCode(Process,&Code);FPlatformProcess::CloseProc(Process);
    if(!Returned||Code!=0){Error=TEXT("Shipped CAD fixture generation failed inside the native application.");return false;}
    return true;
}
TSet<FString> CacheDirectories()
{
    TArray<FString> Names;IFileManager::Get().FindFiles(Names,*(FPaths::ProjectSavedDir()/TEXT("Home4CAD/*")),false,true);
    TSet<FString> Result;for(const FString& Name:Names)Result.Add(Name);return Result;
}
bool CacheIsClean(const TSet<FString>& Before)
{
    for(const FString& Name:CacheDirectories())if(!Before.Contains(Name))return false;
    return true;
}
}
// Deliberately runs in the real packaged client as well as the editor. The
// package gate relocates the whole sandboxed .app before invoking this test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4PackagedCAD,"Studio.Home4.Authoring.PackagedCADRoundTrip",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4PackagedCAD::RunTest(const FString&)
{
    using namespace StudioHome4CADTestsLocal;
    const FString Root=FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()/TEXT("ThirdParty/Home4CAD"));
    const FString Python=Root/TEXT("bin/python"),Library=Root/TEXT("lib");
    if(!TestTrue(TEXT("The shipped CAD interpreter exists"),IFileManager::Get().FileExists(*Python)))return false;
    const FString Parent=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/Home4CAD"));
    if(!TestTrue(TEXT("Private native fixture parent is writable"),IFileManager::Get().MakeDirectory(*Parent,true)))return false;
    FString Work,Error;
    if(!TestTrue(*Error,StudioFileDialog::CreateExportStage(Parent,Work,Error)))return false;
    struct FFixtureCleanup
    {
        FString Path;
        ~FFixtureCleanup(){IFileManager::Get().DeleteDirectory(*Path,false,true);}
    } Cleanup{Work};
    const FString STEP=Work/TEXT("original with spaces.step"),IGES=Work/TEXT("original with spaces.iges");
    const FString Script=TEXT("import sys;from pathlib import Path;sys.path.insert(0,sys.argv[1]);import FreeCAD,Part;assert sys.flags.isolated==1;assert sys.dont_write_bytecode;assert Path(FreeCAD.__file__).resolve().is_relative_to(Path(sys.argv[1]).resolve());shape=Part.makeBox(2,3,4);assert abs(shape.Volume-24)<1e-12;shape.exportStep(sys.argv[2]);shape.exportIges(sys.argv[3])");
    if(!TestTrue(*Error,RunBoundedPython(Python,TEXT("-I -B -c ")+Quote(Script)+TEXT(" ")+Quote(Library)+TEXT(" ")+Quote(STEP)+TEXT(" ")+Quote(IGES),Error)))return false;
    const auto OriginalCache=CacheDirectories();const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    for(const FString& Input:{STEP,IGES})
    {
        FString SourceHash;
        if(!TestTrue(*Error,StudioAssets::HashFile(Input,Cancel,SourceHash,Error)))return false;
        FStudioHome4AuthoringRequest Request;
        Request.ProjectId=FGuid::NewGuid();Request.CaseId=FGuid::NewGuid();Request.SampleBudget=1024;
        auto& Spec=Request.Spec;Spec.Geometry.SourcePath=Input;Spec.Authoring.SourceSHA256=SourceHash;
        Spec.Authoring.MetersPerSourceUnit=.001;Spec.Authoring.SurfaceTolerance=.01;Spec.Units.DxMeters=.001;
        Spec.Lattice.Extents=FIntVector(16);Spec.Geometry.InitialPositionCells=FVector(4);Spec.Geometry.BandCells=2;
        Spec.Reference.LengthCells=4;
        const FString Before=StudioHome4Config::Serialize(Spec);
        const auto Preview=StudioHome4Authoring::Build(Request,Cancel);
        TestTrue(*Preview.Error,Preview.IsValid());
        TestTrue(TEXT("Real STEP/IGES native import preserves original source identity"),Preview.SourcePath==Input&&Preview.SourceSHA256==SourceHash&&Preview.Method.Contains(TEXT("FreeCAD/OpenCASCADE")));
        TestTrue(TEXT("Source XYZ and source-unit conversion preserve analytical cuboid bounds"),Preview.bClosed&&Preview.Body.Min.Equals(FVector(4),1e-9)&&Preview.Body.Max.Equals(FVector(6,7,8),1e-9));
        FStudioHome4GeometricMassProperties Mass;
        TestTrue(*Error,StudioHome4Body::MassProperties(Preview,1,Mass,Error));
        TestTrue(TEXT("Actual prepared triangles retain analytical 24-unit-cubed volume"),FMath::IsNearlyEqual(Mass.Volume,24.,1e-9));
        TestEqual(TEXT("Native CAD preparation does not mutate its request"),StudioHome4Config::Serialize(Spec),Before);
        TestTrue(TEXT("Successful native conversion removes only its private work directory"),CacheIsClean(OriginalCache));
        FString AfterHash;TestTrue(*Error,StudioAssets::HashFile(Input,Cancel,AfterHash,Error));TestEqual(TEXT("The external original was not changed"),AfterHash,SourceHash);
        Spec.Authoring.SourceSHA256=FString::ChrN(64,'0');const auto Wrong=StudioHome4Authoring::Build(Request,Cancel);
        TestTrue(TEXT("A mismatched original source pin rejects before exposing prepared geometry"),!Wrong.IsValid()&&!Wrong.Mesh&&Wrong.Error.Contains(TEXT("pinned SHA256")));
        TestTrue(TEXT("Rejected source pin leaves no private conversion work"),CacheIsClean(OriginalCache));
        // Change the external original only after the production child has
        // received a valid private copy. The kernel must still parse that copy,
        // and final original verification must reject its changed size.
        Spec.Authoring.SourceSHA256=SourceHash;bool Changed=false;
        Request.OnCADProcessStarted=[&](uint32,const FString&){TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*Input,FILEWRITE_Append));if(Writer){uint8 Byte='\n';Writer->Serialize(&Byte,1);Changed=!Writer->IsError()&&Writer->Close();}};
        const auto ChangedOriginal=StudioHome4Authoring::Build(Request,Cancel);
        TestTrue(TEXT("Changed original size rejects a valid private CAD conversion before publishing geometry"),Changed&&!ChangedOriginal.IsValid()&&!ChangedOriginal.Mesh&&ChangedOriginal.Error.Contains(TEXT("changed size")));
        TestTrue(TEXT("Original change rejection cleans its exact private work"),CacheIsClean(OriginalCache));
    }
    return !HasAnyErrors();
}
#if PLATFORM_MAC
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4CADLifecycle,"Studio.Home4.Authoring.CADActiveChildCancellationAndBoundedMetadata",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4CADLifecycle::RunTest(const FString&)
{
    using namespace StudioHome4CADTestsLocal;
    const FString Python=FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()/TEXT("ThirdParty/Home4CAD/bin/python"));
    if(!TestTrue(TEXT("The staged interpreter exists for the real child lifecycle"),IFileManager::Get().FileExists(*Python)))return false;
    const FString Parent=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/Home4CADLifecycle"));
    if(!TestTrue(TEXT("Owned lifecycle fixture parent is writable"),IFileManager::Get().MakeDirectory(*Parent,true)))return false;
    FString Work,Error;if(!TestTrue(*Error,StudioFileDialog::CreateExportStage(Parent,Work,Error)))return false;
    struct FLifecycleCleanup{FString Path;~FLifecycleCleanup(){IFileManager::Get().DeleteDirectory(*Path,false,true);}}Cleanup{Work};
    const FString Input=Work/TEXT("lifecycle-original.step"),Child=Work/TEXT("child.py"),Launcher=Work/TEXT("owned-interpreter"),Ready=Work/TEXT("ready");
    if(!TestTrue(TEXT("Lifecycle fixture source writes"),FFileHelper::SaveStringToFile(TEXT("explicit process-only test source"),*Input)))return false;
    auto ShellQuote=[](FString Value){Value.ReplaceInline(TEXT("'"),TEXT("'\\''"));return TEXT("'")+Value+TEXT("'");};
    const FString Launch=TEXT("#!/bin/sh\nexec ")+ShellQuote(Python)+TEXT(" -I -B ")+ShellQuote(Child)+TEXT(" \"$@\"\n");
    if(!TestTrue(TEXT("Private lifecycle executable is writable"),FFileHelper::SaveStringToFile(Launch,*Launcher,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))||
       !TestTrue(TEXT("Only the fixture owner may execute its harness"),::chmod(TCHAR_TO_UTF8(*Launcher),0700)==0))return false;
    FStudioHome4AuthoringRequest Request;Request.Spec.Geometry.SourcePath=Input;Request.Spec.Authoring.TessellatorPython=Launcher;
    Request.Spec.Authoring.MetersPerSourceUnit=.001;Request.Spec.Authoring.SurfaceTolerance=.01;Request.Spec.Units.DxMeters=.001;Request.Spec.Lattice.Extents=FIntVector(16);
    FString SourceHash;auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    if(!TestTrue(*Error,StudioAssets::HashFile(Input,Cancel,SourceHash,Error)))return false;Request.Spec.Authoring.SourceSHA256=SourceHash;
    const FString CancelCode=TEXT("import signal,time\nfrom pathlib import Path\nsignal.signal(signal.SIGTERM,signal.SIG_IGN)\nPath(__file__).with_name('ready').write_text('SIGTERM ignored')\ntime.sleep(120)\n");
    if(!TestTrue(TEXT("Active-child barrier fixture writes"),FFileHelper::SaveStringToFile(CancelCode,*Child,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)))return false;
    bool Barrier=false;uint32 ExactChild=0;FString PrivateWork;const auto OriginalCache=CacheDirectories();
    Request.OnCADProcessStarted=[&](uint32 Pid,const FString& Path)
    {
        ExactChild=Pid;PrivateWork=Path;const double Deadline=FPlatformTime::Seconds()+5;
        while(!IFileManager::Get().FileExists(*Ready)&&FPlatformTime::Seconds()<Deadline)FPlatformProcess::Sleep(.01f);
        Barrier=IFileManager::Get().FileExists(*Ready);Cancel->store(true);
    };
    const double Started=FPlatformTime::Seconds();const auto Cancelled=StudioHome4Authoring::Build(Request,Cancel);
    TestTrue(TEXT("Cancellation occurs only after a live owned child ignores SIGTERM"),Barrier&&ExactChild>0);
    TestTrue(TEXT("Bounded active-child cancellation produces no geometry or success"),Cancelled.bCancelled&&!Cancelled.IsValid()&&!Cancelled.Mesh&&Cancelled.Error.Contains(TEXT("cancelled"))&&FPlatformTime::Seconds()-Started<8);
    TestTrue(TEXT("Terminated child's exact private work is removed"),!PrivateWork.IsEmpty()&&!IFileManager::Get().DirectoryExists(*PrivateWork)&&CacheIsClean(OriginalCache));
    if(ExactChild>0){auto Process=FPlatformProcess::OpenProcess(ExactChild);TestFalse(TEXT("The exact owned CAD child is no longer running"),Process.IsValid());if(Process.IsValid())FPlatformProcess::CloseProc(Process);}
    // A process fixture may emit metadata but never a scientific result. It
    // proves the production reader rejects oversized metadata before allocation.
    const FString MetadataCode=TEXT("import sys\nfrom pathlib import Path\na=sys.argv\nout=Path(a[a.index('--output')+1])\nPath(str(out)+'.json').write_bytes(b'x'*65537)\n");
    if(!TestTrue(TEXT("Bounded-metadata process fixture writes"),FFileHelper::SaveStringToFile(MetadataCode,*Child,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)))return false;
    Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);Request.OnCADProcessStarted={};
    const auto Oversized=StudioHome4Authoring::Build(Request,Cancel);
    TestTrue(TEXT("Oversized child metadata rejects before exposing geometry"),!Oversized.IsValid()&&!Oversized.Mesh&&Oversized.Error.Contains(TEXT("bounded read")));
    TestTrue(TEXT("Metadata rejection cleans only owned private work"),CacheIsClean(OriginalCache));
    FString OriginalAfter;TestTrue(*Error,StudioAssets::HashFile(Input,Cancel,OriginalAfter,Error));TestEqual(TEXT("Lifecycle tests preserve original input bytes"),OriginalAfter,SourceHash);
    return !HasAnyErrors();
}
#endif

#endif
