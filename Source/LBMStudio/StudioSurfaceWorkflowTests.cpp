#include "StudioModel.h"
#include "StudioSurfaceReconstruction.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto SurfaceWorkflowFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
FString SurfaceSourcePath()
{return FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_ReaderFixture/recording.json"));}
FString SurfaceFixturePath()
{return FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()/TEXT("Samples/NACA0018_SurfaceFixture/reconstruction.json"));}
struct FSurfaceWorkflowFiles
{
    FString Root=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/SurfaceWorkflows")/FGuid::NewGuid().ToString());
    ~FSurfaceWorkflowFiles(){IFileManager::Get().DeleteDirectory(*Root,false,true);}
    FString CopySurface(const TCHAR* Name) const
    {
        const FString Folder=Root/Name;
        if(!IFileManager::Get().MakeDirectory(*Folder,true))return {};
        for(const TCHAR* File:{TEXT("reconstruction.json"),TEXT("triangles.u32"),TEXT("solid-boundary.u32")})
            if(IFileManager::Get().Copy(*(Folder/File),*(FPaths::GetPath(SurfaceFixturePath())/File))!=COPY_OK)return {};
        return Folder/TEXT("reconstruction.json");
    }
};
bool FinishSurfaceWork(FStudioModel& Model)
{
    const double Deadline=FPlatformTime::Seconds()+30;
    while((Model.IsProjectOpenPending()||Model.IsRecordingLoadPending())&&FPlatformTime::Seconds()<Deadline)
    {Model.Tick(0);FPlatformProcess::Sleep(.001f);}
    return !Model.IsProjectOpenPending()&&!Model.IsRecordingLoadPending();
}
FStudioRecordingLoadResult BoundSurfaceSource()
{
    auto Source=StudioRecordings::Import(SurfaceSourcePath(),0,{});
    if(!Source.Reference.IsSet())return Source;
    return StudioRecordings::ImportReconstruction(*Source.Reference,SurfaceFixturePath(),0,{});
}
FStudioProject SurfaceProject(const FStudioRecordingReference& Reference)
{
    FStudioProject P;
    P.Name=TEXT("Wing with explicit reconstruction");P.Dataset=Reference.Id;P.Recordings={Reference};
    P.SelectedFrame=1;P.Camera.Position=FVector(.42,-.63,.27);
    P.Runs={FStudioRunRecord::Recording(P.Name,P.Dataset,false)};
    return P;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioSurfaceCommandsTest,"Studio.Surface.AsyncImportRelinkRemove",SurfaceWorkflowFlags)
bool FStudioSurfaceCommandsTest::RunTest(const FString&)
{
    FSurfaceWorkflowFiles Files;FStudioModel M(Files.Root/TEXT("session"));
    TestFalse(TEXT("Installed mesh recording cannot receive point reconstruction"),M.RequestReconstruction(SurfaceFixturePath()));
    if(!TestTrue(TEXT("Import original source"),M.RequestExternalRecording(SurfaceSourcePath())&&FinishSurfaceWork(M)))return false;
    if(!TestEqual(TEXT("Point source selected"),M.Solver->Descriptor().Id,FString(TEXT("NACA0018_ReaderFixture_3OriginalFrames"))))return false;
    M.Scrub(.5);M.Run();
    auto Camera=M.Project.Camera;Camera.Position=FVector(3,4,5);
    TestTrue(TEXT("Independent camera edit"),M.EditCamera(TEXT("Test camera"),Camera));
    const auto Source=M.Solver;const auto State=M.State;
    const int32 Frame=M.SelectedFrame,RunCount=M.Project.Runs.Num();
    const auto Original=M.Solver->CaptureField(Frame);
    TestTrue(TEXT("Import surface starts"),M.RequestReconstruction(SurfaceFixturePath()));
    TestTrue(TEXT("Existing source remains published while reading"),M.Solver==Source);
    TestFalse(TEXT("Second surface worker is refused"),M.RequestReconstruction(SurfaceFixturePath()));
    TestTrue(TEXT("Surface preparation finishes"),FinishSurfaceWork(M));
    if(!TestTrue(TEXT("Verified surface published"),M.Solver->Reconstruction().IsValid()))return false;
    TestEqual(TEXT("Original selected frame retained"),M.SelectedFrame,Frame);
    TestEqual(TEXT("Playback state retained"),M.State,State);
    TestEqual(TEXT("Exact camera retained"),M.Project.Camera.Position,Camera.Position);
    TestEqual(TEXT("No new scientific run created"),M.Project.Runs.Num(),RunCount);
    TestTrue(TEXT("Previously captured original field remains valid"),Original->IsValid());
    const auto Bound=M.Solver;const auto Ref=M.Project.Recordings[0];
    TestEqual(TEXT("Saved reference pins loaded metadata"),Ref.Reconstruction->MetadataSHA256,Bound->Reconstruction()->MetadataSHA256);

    const FString Copy=Files.CopySurface(TEXT("relocated"));
    if(!TestFalse(TEXT("Exact replacement fixture prepared"),Copy.IsEmpty()))return false;
    TestTrue(TEXT("Locate surface starts"),M.RequestReconstruction(Copy,true));
    TestTrue(TEXT("Locate surface completes"),FinishSurfaceWork(M));
    TestEqual(TEXT("Location updated"),M.Project.Recordings[0].Reconstruction->Path,Copy);
    TestEqual(TEXT("Relocation retains content hash"),M.Project.Recordings[0].Reconstruction->MetadataSHA256,Ref.Reconstruction->MetadataSHA256);
    const auto Located=M.Solver;
    FString Metadata;FFileHelper::LoadFileToString(Metadata,*Copy);
    FFileHelper::SaveStringToFile(Metadata+TEXT("\n "),*Copy);
    TestTrue(TEXT("Changed location is checked"),M.RequestReconstruction(Copy,true)&&FinishSurfaceWork(M));
    TestTrue(TEXT("Changed interpretation cannot replace current source"),M.Solver==Located);
    TestEqual(TEXT("Failed relocation retains saved hash"),M.Project.Recordings[0].Reconstruction->MetadataSHA256,Ref.Reconstruction->MetadataSHA256);

    TestTrue(TEXT("Removal can be cancelled"),M.RemoveReconstruction());M.CancelRecording();
    TestTrue(TEXT("Cancelled worker drains"),FinishSurfaceWork(M));
    TestTrue(TEXT("Cancelled removal retains surface and source"),M.Solver==Located&&M.Project.Recordings[0].Reconstruction.IsSet());
    TestTrue(TEXT("Removal starts and completes"),M.RemoveReconstruction()&&FinishSurfaceWork(M));
    TestFalse(TEXT("Explicit removal drops optional reference"),M.Project.Recordings[0].Reconstruction.IsSet());
    TestFalse(TEXT("Original-point solver no longer holds derived surface"),M.Solver->Reconstruction().IsValid());
    TestEqual(TEXT("Removal retains source identity"),M.Project.Recordings[0].MetadataSHA256,Ref.MetadataSHA256);
    TestEqual(TEXT("Removal retains frame"),M.SelectedFrame,Frame);
    TestEqual(TEXT("Removal retains playback state"),M.State,State);
    TestFalse(TEXT("No surface remains to relocate"),M.RequestReconstruction(Copy,true));
    TestTrue(TEXT("Import can start again"),M.RequestReconstruction(SurfaceFixturePath()));
    M.NewProject(TEXT("New document wins"));const auto NewId=M.Project.Id;
    TestTrue(TEXT("Abandoned document load drains"),FinishSurfaceWork(M));
    TestEqual(TEXT("Stale surface cannot replace a new project"),M.Project.Id,NewId);
    TestFalse(TEXT("New project's source gains no old surface"),M.Solver->Reconstruction().IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioSurfaceRepairTest,"Studio.Surface.ProjectSourceAndSurfaceRepair",SurfaceWorkflowFlags)
bool FStudioSurfaceRepairTest::RunTest(const FString&)
{
    FSurfaceWorkflowFiles Files;
    auto Bound=BoundSurfaceSource();
    if(!TestTrue(*Bound.Error,Bound.Source.IsValid()&&Bound.Reference.IsSet()))return false;
    const auto Original=*Bound.Reference;auto Missing=Original;
    Missing.Path=Files.Root/TEXT("missing-source/recording.json");
    Missing.Reconstruction->Path=Files.Root/TEXT("missing-surface/reconstruction.json");
    const auto Target=SurfaceProject(Missing);const FString Path=Files.Root/TEXT("project.lbms");FString Error;
    if(!TestTrue(*Error,StudioProjectIO::Save(Path,Target,Error)))return false;
    FStudioModel M(Files.Root/TEXT("session"));M.Scrub(.5);
    const auto Current=M.Solver;const auto CurrentId=M.Project.Id;const auto CurrentCamera=M.Project.Camera;
    TestTrue(TEXT("Missing source project checked"),M.RequestProjectOpen(Path)&&FinishSurfaceWork(M));
    if(!TestTrue(TEXT("Source repair offered first"),M.RecordingRepair.IsSet()&&!M.RecordingRepair->bReconstruction))return false;
    TestFalse(TEXT("Source failure cannot be bypassed by dropping topology"),M.RetryWithoutReconstruction());
    TestTrue(TEXT("Source repair checked"),M.RetryRecordingRepair(SurfaceSourcePath())&&FinishSurfaceWork(M));
    if(!TestTrue(TEXT("Surface failure identified separately"),M.RecordingRepair.IsSet()&&M.RecordingRepair->bReconstruction))return false;
    TestEqual(TEXT("Source location survives multi-stage repair"),M.RecordingRepair->SourcePath,SurfaceSourcePath());
    TestTrue(TEXT("Incomplete candidate never replaces current source"),M.Solver==Current);
    TestEqual(TEXT("Incomplete candidate never replaces camera"),M.Project.Camera.Position,CurrentCamera.Position);

    const FString Changed=Files.CopySurface(TEXT("changed"));
    if(!TestFalse(TEXT("Changed-copy fixture prepared"),Changed.IsEmpty()))return false;
    FString Metadata;FFileHelper::LoadFileToString(Metadata,*Changed);FFileHelper::SaveStringToFile(Metadata+TEXT("\n "),*Changed);
    TestTrue(TEXT("Wrong reconstruction checked"),M.RetryRecordingRepair(Changed)&&FinishSurfaceWork(M));
    TestEqual(TEXT("Wrong interpretation retains current document"),M.Project.Id,CurrentId);
    if(!TestTrue(TEXT("Repair remains available after wrong choice"),M.RecordingRepair.IsSet()&&M.RecordingRepair->bReconstruction))return false;
    TestEqual(TEXT("Verified source retained after wrong surface"),M.RecordingRepair->SourcePath,SurfaceSourcePath());
    TestTrue(TEXT("Matching surface repair checked"),M.RetryRecordingRepair(SurfaceFixturePath())&&FinishSurfaceWork(M));
    if(!TestEqual(TEXT("Complete candidate published"),M.Project.Id,Target.Id)||
        !TestTrue(TEXT("Surface is available after repair"),M.Solver->Reconstruction().IsValid()))return false;
    TestEqual(TEXT("Exact saved frame restored"),M.SelectedFrame,Target.SelectedFrame);
    TestEqual(TEXT("Exact saved camera restored"),M.Project.Camera.Position,Target.Camera.Position);
    TestEqual(TEXT("Source checksum retained"),M.Project.Recordings[0].MetadataSHA256,Original.MetadataSHA256);
    TestEqual(TEXT("Reconstruction checksum retained"),M.Project.Recordings[0].Reconstruction->MetadataSHA256,Original.Reconstruction->MetadataSHA256);
    TestTrue(TEXT("Repaired paths need saving"),M.HasUnsavedChanges());
    TestFalse(TEXT("Repair banner clears after success"),M.RecordingRepair.IsSet());
    TestTrue(TEXT("Save repaired paths"),M.SaveProject(Files.Root/TEXT("repaired.lbms")));
    TestTrue(TEXT("Repaired project reopens without repair"),M.RequestProjectOpen(M.ProjectPath)&&FinishSurfaceWork(M));
    TestFalse(TEXT("Saved repaired project reopens clean"),M.HasUnsavedChanges());

    TestTrue(TEXT("Surface location can precede source repair"),M.RequestProjectOpen(Path,FString(),false,
        FString(),SurfaceFixturePath())&&FinishSurfaceWork(M));
    if(!TestTrue(TEXT("Source failure keeps requested surface path"),M.RecordingRepair.IsSet()&&!M.RecordingRepair->bReconstruction))return false;
    TestEqual(TEXT("Requested reconstruction survives source failure"),M.RecordingRepair->ReconstructionPath,SurfaceFixturePath());
    TestTrue(TEXT("Repair source using retained surface choice"),M.RetryRecordingRepair(SurfaceSourcePath())&&FinishSurfaceWork(M));
    if(!TestFalse(TEXT("Both repairs resolved in either order"),M.RecordingRepair.IsSet()))return false;
    TestTrue(TEXT("Retained reconstruction is loaded"),M.Solver->Reconstruction().IsValid());

    TestTrue(TEXT("Explicit surface removal survives a missing source"),M.RequestProjectOpen(Path,FString(),false,
        FString(),FString(),true)&&FinishSurfaceWork(M));
    if(!TestTrue(TEXT("Source repair remembers explicit removal"),M.RecordingRepair.IsSet()&&M.RecordingRepair->bRemoveReconstruction))return false;
    TestTrue(TEXT("Repair source without reintroducing removed surface"),M.RetryRecordingRepair(SurfaceSourcePath())&&FinishSurfaceWork(M));
    if(!TestFalse(TEXT("Removal and source repair both resolved"),M.RecordingRepair.IsSet()))return false;
    TestFalse(TEXT("Removed reconstruction remains absent"),M.Project.Recordings[0].Reconstruction.IsSet());

    TestTrue(TEXT("Original missing project remains unchanged"),M.RequestProjectOpen(Path)&&FinishSurfaceWork(M));
    if(!TestTrue(TEXT("Original still needs source repair"),M.RecordingRepair.IsSet()&&!M.RecordingRepair->bReconstruction))return false;
    TestTrue(TEXT("Source repaired a second time"),M.RetryRecordingRepair(SurfaceSourcePath())&&FinishSurfaceWork(M));
    if(!TestTrue(TEXT("Optional surface can now be omitted"),M.RecordingRepair.IsSet()&&M.RecordingRepair->bReconstruction))return false;
    TestTrue(TEXT("Open original points explicitly"),M.RetryWithoutReconstruction()&&FinishSurfaceWork(M));
    TestEqual(TEXT("Original project opened"),M.Project.Id,Target.Id);
    TestFalse(TEXT("No failed surface silently retained"),M.Project.Recordings[0].Reconstruction.IsSet());
    TestFalse(TEXT("Original-point capability restored"),M.Solver->Reconstruction().IsValid());
    TestEqual(TEXT("Dropping topology never changes source bytes"),M.Project.Recordings[0].MetadataSHA256,Original.MetadataSHA256);
    TestEqual(TEXT("Dropping topology retains saved camera"),M.Project.Camera.Position,Target.Camera.Position);
    TestTrue(TEXT("Explicit removal needs saving"),M.HasUnsavedChanges());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioSurfaceRecoveryTest,"Studio.Surface.RepairCancellationRecoveryAndConflict",SurfaceWorkflowFlags)
bool FStudioSurfaceRecoveryTest::RunTest(const FString&)
{
    FSurfaceWorkflowFiles Files;auto Bound=BoundSurfaceSource();
    if(!TestTrue(*Bound.Error,Bound.Source.IsValid()&&Bound.Reference.IsSet()))return false;
    auto Ref=*Bound.Reference;Ref.Reconstruction->Path=Files.Root/TEXT("missing/reconstruction.json");
    auto Target=SurfaceProject(Ref);const FString Path=Files.Root/TEXT("target.lbms");FString Error;
    if(!TestTrue(*Error,StudioProjectIO::Save(Path,Target,Error)))return false;
    FStudioModel M(Files.Root/TEXT("session"));const auto PreviousId=M.Project.Id;
    TestTrue(TEXT("Surface failure prepared"),M.RequestProjectOpen(Path)&&FinishSurfaceWork(M));
    if(!TestTrue(TEXT("Repair is available"),M.RecordingRepair.IsSet()&&M.RecordingRepair->bReconstruction))return false;
    TestTrue(TEXT("Repair may be cancelled"),M.RetryRecordingRepair(SurfaceFixturePath()));M.CancelProjectOpen();
    TestTrue(TEXT("Cancelled repair drains"),FinishSurfaceWork(M));
    TestEqual(TEXT("Cancelled repair preserves current project"),M.Project.Id,PreviousId);
    TestTrue(TEXT("Surface failure can be reopened"),M.RequestProjectOpen(Path)&&FinishSurfaceWork(M));
    TestTrue(TEXT("Original-point retry begins"),M.RetryWithoutReconstruction());
    TestTrue(TEXT("Authoring can continue during repair"),M.RenameProject(TEXT("Keep this newer edit")));
    TestTrue(TEXT("Stale retry drains"),FinishSurfaceWork(M));
    TestEqual(TEXT("Stale repair does not overwrite newer content"),M.Project.Name,FString(TEXT("Keep this newer edit")));
    TestEqual(TEXT("Stale repair retains document identity"),M.Project.Id,PreviousId);

    // Recovery references are absolute and preserve the original save target.
    const FString OriginalPath=Files.Root/TEXT("original.lbms"),Recovery=Files.Root/TEXT("Recovery/unsaved.lbms");
    Target.RecoverySource=OriginalPath;Target.Name=TEXT("Recovered surface inspection");
    if(!TestTrue(*Error,StudioProjectIO::Save(Recovery,Target,Error)))return false;
    M.PendingRecovery=Recovery;
    TestTrue(TEXT("Recovery checked off-thread"),M.RequestRecoveryOpen()&&FinishSurfaceWork(M));
    if(!TestTrue(TEXT("Recovery surface repair retains recovery intent"),M.RecordingRepair.IsSet()&&M.RecordingRepair->bRecovery&&M.RecordingRepair->bReconstruction))return false;
    TestTrue(TEXT("Recovery opens with original points on explicit request"),M.RetryWithoutReconstruction()&&FinishSurfaceWork(M));
    if(!TestEqual(TEXT("Recovered project published"),M.Project.Id,Target.Id))return false;
    TestEqual(TEXT("Original recovery save target retained"),M.ProjectPath,OriginalPath);
    TestEqual(TEXT("Recovered unsaved name retained"),M.Project.Name,Target.Name);
    TestEqual(TEXT("Recovered camera retained"),M.Project.Camera.Position,Target.Camera.Position);
    TestEqual(TEXT("Recovered frame retained"),M.SelectedFrame,Target.SelectedFrame);
    TestTrue(TEXT("Recovery still needs saving"),M.HasUnsavedChanges());
    TestTrue(TEXT("Recovery prompt resolved"),M.PendingRecovery.IsEmpty());
    TestFalse(TEXT("Explicitly removed sidecar stays absent"),M.Project.Recordings[0].Reconstruction.IsSet());
    return true;
}
#endif
