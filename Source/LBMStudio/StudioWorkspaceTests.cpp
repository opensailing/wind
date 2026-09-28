#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
    bool WaitForCatalog(FStudioModel& Model)
    {
        const double Deadline=FPlatformTime::Seconds()+5;
        while(Model.bCatalogLoading && FPlatformTime::Seconds()<Deadline)
        { Model.Tick(0); FPlatformProcess::Sleep(.002f); }
        return !Model.bCatalogLoading;
    }
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioWorkspaceRoutes,"Studio.Workspace.NavigationKeepsWorkingState",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioWorkspaceRoutes::RunTest(const FString&)
{
    const FString Dir=FPaths::ProjectDir()/TEXT("tmp/debug/workspace-tests")/FGuid::NewGuid().ToString();
    FStudioModel M(Dir); M.Run(); M.Scrub(.6); M.Project.Camera.Position=FVector(9,8,7);
    const FString Before=StudioProjectIO::Serialize(M.SnapshotProject()); const int32 Revision=M.Revision;
    TestTrue(TEXT("Projects is a working route"),M.Navigate(EStudioWorkspace::Projects));
    TestTrue(TEXT("Dashboard is a working route"),M.Navigate(EStudioWorkspace::Dashboard));
    TestFalse(TEXT("Unimplemented routes cannot become active"),M.Navigate(EStudioWorkspace::Domain));
    TestTrue(TEXT("Rejected destination keeps Dashboard active"),M.Workspace==EStudioWorkspace::Dashboard);
    TestEqual(TEXT("Navigation cannot change the project document"),StudioProjectIO::Serialize(M.SnapshotProject()),Before);
    TestEqual(TEXT("Navigation cannot rebuild the CFD field"),M.Revision,Revision);
    M.Tick(.05);
    TestTrue(TEXT("Playback advances behind Dashboard"),M.PlaybackFrame>0);
    TestEqual(TEXT("Pinned review remains at its frame"),M.SelectedFrame,360);
    TestTrue(TEXT("Returning to Solve uses shared routing"),M.Navigate(EStudioWorkspace::Solve));
    TestEqual(TEXT("Return to Solve keeps camera"),M.Project.Camera.Position,FVector(9,8,7));
    TestTrue(TEXT("Return to Solve keeps playback"),M.State==EStudioRunState::Running);
    TestTrue(TEXT("Return to Solve keeps review mode"),M.bReviewing);
    M.bSidebarCollapsed=true; M.SaveSession();
    FStudioModel Reopened(Dir); Reopened.OpenSession();
    TestTrue(TEXT("Sidebar preference persists separately from document"),Reopened.bSidebarCollapsed);
    TestTrue(TEXT("Startup still opens Solve first"),Reopened.Workspace==EStudioWorkspace::Solve);
    TestTrue(TEXT("Background metadata completes"),WaitForCatalog(M));
    TestTrue(TEXT("Reopened metadata completes"),WaitForCatalog(Reopened));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioProjectCreateCopy,"Studio.Workspace.CreateAndDuplicateAreTransactional",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioProjectCreateCopy::RunTest(const FString&)
{
    const FString Dir=FPaths::ProjectDir()/TEXT("tmp/debug/workspace-tests")/FGuid::NewGuid().ToString();
    IFileManager::Get().MakeDirectory(*Dir,true);
    FStudioModel M(Dir); M.Project.Name=TEXT("Unsaved work"); M.Scrub(.4);
    const FString Before=StudioProjectIO::Serialize(M.SnapshotProject());
    TestFalse(TEXT("Unwritable new-project target fails"),M.CreateProject(Dir,TEXT("New project")));
    TestEqual(TEXT("Failed create preserves unsaved document"),StudioProjectIO::Serialize(M.SnapshotProject()),Before);
    const FString OriginalPath=Dir/TEXT("original.lbms");
    TestTrue(TEXT("Create commits a new project"),M.CreateProject(OriginalPath,TEXT("Original")));
    const FGuid OriginalId=M.Project.Id, OriginalCase=M.Project.Draft.Id;
    M.Scrub(.7); M.Project.Camera.Position=FVector(2,3,4);
    TestTrue(TEXT("Duplicate includes unsaved camera bookmark"),M.AddCamera(TEXT("Study"),M.Project.Camera));
    const FString SourceState=StudioProjectIO::Serialize(M.SnapshotProject());
    TestFalse(TEXT("Duplicate cannot replace original destination"),M.DuplicateProject(OriginalPath,TEXT("Same")));
    TestEqual(TEXT("Rejected duplicate preserves unsaved work"),StudioProjectIO::Serialize(M.SnapshotProject()),SourceState);
    const FString CopyPath=Dir/TEXT("copy.lbms");
    TestTrue(TEXT("Duplicate commits and opens independent copy"),M.DuplicateProject(CopyPath,TEXT("Copy")));
    TestNotEqual(TEXT("Copy has independent project identity"),M.Project.Id,OriginalId);
    TestNotEqual(TEXT("Copy has independent case identity"),M.Project.Draft.Id,OriginalCase);
    TestEqual(TEXT("Copy retains unsaved selected frame"),M.SelectedFrame,420);
    TestEqual(TEXT("Copy retains camera"),M.Project.Camera.Position,FVector(2,3,4));
    TestEqual(TEXT("Copy retains camera bookmarks"),M.Project.Cameras.Num(),1);
    TestFalse(TEXT("Committed copy starts clean"),M.HasUnsavedChanges());
    FStudioProject Original; FString Error;
    TestTrue(TEXT("Original file remains readable"),StudioProjectIO::Load(OriginalPath,Original,Error));
    TestEqual(TEXT("Original identity unchanged"),Original.Id,OriginalId);
    TestEqual(TEXT("Original saved frame unchanged"),Original.SelectedFrame,0);
    TestEqual(TEXT("Original saved bookmarks unchanged"),Original.Cameras.Num(),0);
    TestTrue(TEXT("Catalog completes"),WaitForCatalog(M));
    TestEqual(TEXT("Both independent files are recent"),M.ProjectCatalog.Num(),2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioProjectCatalog,"Studio.Workspace.RecentFilesFavoritesAndFailures",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioProjectCatalog::RunTest(const FString&)
{
    const FString Dir=FPaths::ProjectDir()/TEXT("tmp/debug/workspace-tests")/FGuid::NewGuid().ToString();
    FStudioModel M(Dir); const FString First=Dir/TEXT("first.lbms"),Second=Dir/TEXT("second.lbms"),Missing=Dir/TEXT("missing.lbms");
    TestTrue(TEXT("First project created"),M.CreateProject(First,TEXT("First")));
    TestTrue(TEXT("Second project created"),M.CreateProject(Second,TEXT("Second")));
    M.RecentProjects.Add(Missing); M.RefreshProjectCatalog();
    TestTrue(TEXT("Favorite another file without opening it"),M.SetProjectFavorite(First,true));
    TestEqual(TEXT("Favorite leaves current project selected"),M.Project.Name,FString(TEXT("Second")));
    TestFalse(TEXT("Other-file favorite leaves current project clean"),M.HasUnsavedChanges());
    TestTrue(TEXT("Catalog refresh after in-flight edits completes"),WaitForCatalog(M));
    TestEqual(TEXT("Catalog includes missing files explicitly"),M.ProjectCatalog.Num(),3);
    const auto* Favorite=M.ProjectCatalog.FindByPredicate([&First](const auto& P){return FPaths::IsSamePath(P.Path,First);});
    const auto* Absent=M.ProjectCatalog.FindByPredicate([&Missing](const auto& P){return FPaths::IsSamePath(P.Path,Missing);});
    TestTrue(TEXT("Latest favorite wins over stale in-flight read"),Favorite&&Favorite->bFavorite);
    TestTrue(TEXT("Missing file has a visible error"),Absent&&!Absent->Error.IsEmpty());
    TestTrue(TEXT("Current favorite marks current document dirty"),M.SetProjectFavorite(Second,true));
    TestTrue(TEXT("Current favorite awaits explicit save"),M.HasUnsavedChanges());
    M.ForgetRecentProject(First); TestTrue(TEXT("Removal refresh completes"),WaitForCatalog(M));
    TestFalse(TEXT("Forgotten project is no longer in catalog"),M.ProjectCatalog.ContainsByPredicate([&First](const auto& P){return FPaths::IsSamePath(P.Path,First);}));
    TestTrue(TEXT("Remove from recents keeps actual file"),IFileManager::Get().FileExists(*First));
    M.RecentProjects={Missing}; M.RefreshProjectCatalog(); M.RecentProjects={Second}; M.RefreshProjectCatalog();
    TestTrue(TEXT("Refresh invalidation completes"),WaitForCatalog(M));
    TestEqual(TEXT("Stale refresh does not restore removed entries"),M.ProjectCatalog.Num(),1);
    if(M.ProjectCatalog.Num()==1) TestTrue(TEXT("Most recent request wins"),FPaths::IsSamePath(M.ProjectCatalog[0].Path,Second));
    return true;
}
#endif
