#include "StudioModel.h"
#include "StudioScene.h"
#include "StudioWorkspace.h"
#include "StudioHeadlessSlate.h"
#include "StudioHome4Recipes.h"
#include "Engine/World.h"
#include "UObject/StrongObjectPtr.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto Home4WorkflowFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4RequestOnlyProject,"Studio.Home4.Projects.RequestOnlyCreationAndReopen",Home4WorkflowFlags)
bool FHome4RequestOnlyProject::RunTest(const FString&)
{
    const FString Root=FPaths::ProjectSavedDir()/TEXT("Automation")/(TEXT("Home4Empty_")+FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ON_SCOPE_EXIT{IFileManager::Get().DeleteDirectory(*Root,false,true);};
    FStudioModel M(Root);M.BeginHome4Authoring();
    TestEqual(TEXT("Fresh recipe chooser contains no unrelated CFD"),M.Solver->FrameCount(),0);
    TestTrue(TEXT("No source identity"),M.Project.Dataset.IsEmpty()&&M.Solver->Descriptor().Id.IsEmpty());
    TestFalse(TEXT("No fabricated field snapshot"),M.Solver->CaptureField(0)->IsValid());
    FStudioProject Parsed;FString Error;
    TestTrue(TEXT("Empty authoring document round trips"),StudioProjectIO::Parse(StudioProjectIO::Serialize(M.SnapshotProject()),Parsed,Error));
    const auto* Recipe=StudioHome4Recipes::Find(TEXT("th01-hull"));if(!TestNotNull(TEXT("Documented hull recipe"),Recipe))return false;
    auto Spec=Recipe->Template;Spec.LineageId=FGuid::NewGuid().ToString();
    const FString Path=Root/TEXT("hull.lbms");
    if(!TestTrue(M.Notice,M.CreateProject(Path,TEXT("Original hull request"),&Spec)))return false;
    TestTrue(TEXT("Request starts without a fake run"),M.Project.Runs.IsEmpty()&&M.Project.Recordings.IsEmpty());
    TestEqual(TEXT("Request has no source frames"),M.Solver->FrameCount(),0);
    M.Run();M.Step();TestEqual(TEXT("Playback cannot create data"),M.Solver->FrameCount(),0);
    M.NewProject(TEXT("Unrelated replay"));TestTrue(TEXT("Explicit replay remains available separately"),M.Solver->FrameCount()>0);
    TestTrue(TEXT("Asynchronous request-only project open"),M.RequestProjectOpen(Path));
    const double Deadline=FPlatformTime::Seconds()+5;
    while(M.IsProjectOpenPending()&&FPlatformTime::Seconds()<Deadline){M.Tick(0);FPlatformProcess::Sleep(.002f);}
    TestFalse(TEXT("Open worker completes"),M.IsProjectOpenPending());
    TestEqual(TEXT("Reopen does not inject the prior replay"),M.Solver->FrameCount(),0);
    TestTrue(TEXT("Typed recipe retained"),M.Project.Draft.Home4.IsSet()&&M.Project.Draft.Home4->RecipeId==Spec.RecipeId);
    TestFalse(TEXT("Reopened request is clean"),M.HasUnsavedChanges());
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHome4RequestOnlyWorkspace,"Studio.HeadlessUI.Home4.RequestOnlyWorkspaceJourney",Home4WorkflowFlags)
bool FHome4RequestOnlyWorkspace::RunTest(const FString&)
{
    const FString Root=FPaths::ProjectSavedDir()/TEXT("Automation")/(TEXT("Home4Journey_")+FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ON_SCOPE_EXIT{IFileManager::Get().DeleteDirectory(*Root,false,true);};
    auto Model=MakeShared<FStudioModel>(Root);
    const auto Spec=StudioHome4Recipes::Find(TEXT("th01-hull"))->Template;
    if(!TestTrue(Model->Notice,Model->CreateProject(Root/TEXT("request.lbms"),TEXT("Request only"),&Spec)))return false;
    TStrongObjectPtr<UWorld> World(UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,false));
    ON_SCOPE_EXIT{World->DestroyWorld(false);};
    auto* Scene=World->SpawnActor<AStudioScene>();if(!TestNotNull(TEXT("Native bindings scene"),Scene))return false;
    Scene->Model=Model;Scene->ApplyCamera(Model->Project.Camera);
    auto Workspace=SNew(SStudioWorkspace).Model(Model).Scene(Scene);
    FStudioHeadlessSlate UI(*this,Workspace,FVector2D(1440,1000));
    if(!UI.Inspect(TEXT("home4-request-empty-fields"),{TEXT("Home4EmptyFields"),TEXT("RunControl")}))return false;
    const auto Camera=Model->Project.Camera;const FString SpecBefore=StudioHome4Config::Serialize(*Model->Project.Draft.Home4);
    // Exercise actual sidebar button routing across every HOME4 destination.
    for(const auto Destination:{EStudioWorkspace::Dashboard,EStudioWorkspace::Projects,EStudioWorkspace::Geometry,EStudioWorkspace::Meshing,
        EStudioWorkspace::Materials,EStudioWorkspace::BoundaryConditions,EStudioWorkspace::Bodies,EStudioWorkspace::Run,
        EStudioWorkspace::Monitors,EStudioWorkspace::Solve,EStudioWorkspace::Validation,EStudioWorkspace::Reports,EStudioWorkspace::Settings})
    {
        const FName Tag(*FString::Printf(TEXT("Workspace%d"),int32(Destination)));
        if(!UI.Press(Tag))return false;UI.Layout();
        TestEqual(TEXT("Navigation never creates scientific frames"),Model->Solver->FrameCount(),0);
    }
    TestEqual(TEXT("Browsing preserves immutable request"),StudioHome4Config::Serialize(*Model->Project.Draft.Home4),SpecBefore);
    TestTrue(TEXT("Camera remains independent"),StudioView::CameraEquals(Model->Project.Camera,Camera));
    TestEqual(TEXT("Virtual journey needs no GPU captures"),Scene->GetCaptureCount(),uint64(0));
    return !HasAnyErrors();
}
#endif
