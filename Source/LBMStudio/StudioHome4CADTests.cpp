#include "StudioHome4Authoring.h"
#include "StudioHome4Body.h"
#include "StudioFileDialog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

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
    const FString Script=TEXT("import sys;from pathlib import Path;sys.path.insert(0,sys.argv[1]);import FreeCAD,Part;assert sys.flags.isolated==1;assert Path(FreeCAD.__file__).resolve().is_relative_to(Path(sys.argv[1]).resolve());shape=Part.makeBox(2,3,4);assert abs(shape.Volume-24)<1e-12;shape.exportStep(sys.argv[2]);shape.exportIges(sys.argv[3])");
    if(!TestTrue(*Error,RunBoundedPython(Python,TEXT("-I -c ")+Quote(Script)+TEXT(" ")+Quote(Library)+TEXT(" ")+Quote(STEP)+TEXT(" ")+Quote(IGES),Error)))return false;
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
    }
    return !HasAnyErrors();
}
#endif
