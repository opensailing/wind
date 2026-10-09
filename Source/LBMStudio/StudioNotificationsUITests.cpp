#include "StudioNotifications.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "StudioHeadlessSlate.h"
#include "StudioModel.h"
#include "StudioScene.h"
#include "StudioWorkspace.h"
#include "SStudioNotifications.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

namespace StudioNotificationUITests
{
FString Session(){return FPaths::ProjectDir()/TEXT("tmp/debug/notification-sessions")/FGuid::NewGuid().ToString();}
bool Header(FAutomationTestBase& Test,FVector2D Size)
{
    const FString Root=Session();TSharedPtr<FStudioModel> M=MakeShared<FStudioModel>(Root);
    ON_SCOPE_EXIT{M.Reset();IFileManager::Get().DeleteDirectory(*Root,false,true);};
    TStrongObjectPtr<UWorld> World(UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,false));
    ON_SCOPE_EXIT{World->DestroyWorld(false);};
    auto* Scene=World->SpawnActor<AStudioScene>();if(!Test.TestNotNull(TEXT("Scene for real root bindings"),Scene))return false;
    Scene->Model=M;Scene->ApplyCamera(M->Project.Camera);
    auto Workspace=SNew(SStudioWorkspace).Model(M).Scene(Scene);
    FStudioHeadlessSlate UI(Test,Workspace,Size);
    const FString Prefix=FString::Printf(TEXT("notifications-header-%dx%d"),int32(Size.X),int32(Size.Y));
    M->AddLog(TEXT("UI automation fixture: local disk write failed; current project retained."),EStudioLogSeverity::Error);
    const uint64 Id=M->Notifications().LastSequence();
    if(!UI.Inspect(Prefix+TEXT("-closed"),{TEXT("HeaderNotifications"),TEXT("NotificationBadge"),TEXT("HeaderHelp"),TEXT("ExportMenu")}))return false;
    const FString Before=StudioProjectIO::Serialize(M->SnapshotProject());
    const auto Intent=M->RenderIntentRevision;const auto Captures=Scene->GetCaptureCount();
    if(!UI.Press(TEXT("HeaderNotifications")))return false;
    if(!UI.Inspect(Prefix+TEXT("-open"),{TEXT("NotificationsPanel"),TEXT("NotificationsClose"),TEXT("NotificationsMarkShown"),TEXT("NotificationsRefresh")}))return false;
    Test.TestEqual(TEXT("Opening does not mark read"),M->Notifications().UnreadCount(),1);
    UI.Key(EKeys::Escape);UI.Layout();
    Test.TestFalse(TEXT("Escape dismisses notification popup"),UI.Exists(TEXT("NotificationsPanel")));
    auto Button=UI.Find(TEXT("HeaderNotifications"));
    Test.TestTrue(TEXT("Escape returns header focus"),Button->HasKeyboardFocus()||Button->HasFocusedDescendants());
    if(!UI.Press(TEXT("HeaderNotifications"))||!UI.Press(FName(*FString::Printf(TEXT("NotificationOpen_%llu"),Id))))return false;
    Test.TestTrue(TEXT("Issue opens exact log owner"),M->Workspace==EStudioWorkspace::Solve&&M->bActivityLogExpanded);
    Test.TestTrue(TEXT("Exact log detail revealed"),UI.Text(TEXT("LogDetail")).Contains(TEXT("local disk write failed")));
    Test.TestEqual(TEXT("Successful reveal marks only its notification read"),M->Notifications().UnreadCount(),0);
    Test.TestEqual(TEXT("Log reveal preserves exact case/camera/project"),StudioProjectIO::Serialize(M->SnapshotProject()),Before);
    Test.TestEqual(TEXT("Notifications submit no render intent"),M->RenderIntentRevision,Intent);
    Test.TestEqual(TEXT("Notifications submit no captures"),Scene->GetCaptureCount(),Captures);
    if(!UI.Press(TEXT("HeaderNotifications"))||!UI.Press(TEXT("NotificationsClose")))return false;
    Test.TestTrue(TEXT("Close returns header focus"),Button->HasKeyboardFocus()||Button->HasFocusedDescendants());
    // A real accepted harness completion links the exact saved run in Results.
    M->SetControlHarness(true);M->Control(EStudioJobCommand::Submit);M->Tick(.1);M->SimulateJobEvent(EStudioJobState::Completed);
    const auto Run=M->Job().Run()->GetId();const uint64 Completion=M->Notifications().LastSequence();
    const FString CompletedBefore=StudioProjectIO::Serialize(M->SnapshotProject());
    if(!UI.Press(TEXT("HeaderNotifications"))||!UI.Press(FName(*FString::Printf(TEXT("NotificationOpen_%llu"),Completion))))return false;
    Test.TestTrue(TEXT("Completion opens Results metadata"),M->Workspace==EStudioWorkspace::Results);
    Test.TestTrue(TEXT("Exact run is visible"),UI.Text(TEXT("ResultsRunIdentity")).Contains(Run.ToString()));
    Test.TestEqual(TEXT("Run link does not mutate project/view"),StudioProjectIO::Serialize(M->SnapshotProject()),CompletedBefore);
    return !Test.HasAnyErrors();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioNotificationsPanelWorkflow,"Studio.HeadlessUI.Notifications.PanelWorkflow",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioNotificationsPanelWorkflow::RunTest(const FString&)
{
    const FString Root=StudioNotificationUITests::Session();TSharedPtr<FStudioModel> M=MakeShared<FStudioModel>(Root);
    ON_SCOPE_EXIT{M.Reset();IFileManager::Get().DeleteDirectory(*Root,false,true);};
    int32 Closed=0,Revealed=0;
    auto Panel=SNew(SStudioNotifications).Model(M).OnClose_Lambda([&]{++Closed;}).Reveal([&](uint64){++Revealed;return true;});
    FStudioHeadlessSlate UI(*this,Panel,FVector2D(500,500));
    if(!UI.Inspect(TEXT("notifications-empty"),{TEXT("NotificationsEmpty"),TEXT("NotificationsClose"),TEXT("NotificationsRefresh")}))return false;
    M->AddLog(TEXT("UI automation fixture: imported geometry needs review."),EStudioLogSeverity::Warning);
    const uint64 First=M->Notifications().LastSequence();UI.Layout();
    TestTrue(TEXT("New arrivals announced without rebuilding list"),UI.Text(TEXT("NotificationsSummary")).Contains(TEXT("1 new")));
    TestTrue(TEXT("Captured empty list stable until Refresh"),UI.Exists(TEXT("NotificationsEmpty")));
    if(!UI.Press(TEXT("NotificationsRefresh")))return false;
    const FName Read(*FString::Printf(TEXT("NotificationRead_%llu"),First));
    if(!UI.Inspect(TEXT("notifications-history"),{Read,TEXT("NotificationsSummary"),TEXT("NotificationsMarkShown")}))return false;
    TestEqual(TEXT("Refresh does not mark read"),M->Notifications().UnreadCount(),1);
    M->AddLog(TEXT("UI automation fixture: disk write failed."),EStudioLogSeverity::Error);
    if(!UI.Press(TEXT("NotificationsMarkShown")))return false;
    TestEqual(TEXT("Bulk read excludes new unseen event"),M->Notifications().UnreadCount(),1);
    if(!UI.Press(Read))return false;TestEqual(TEXT("Manual unread works"),M->Notifications().UnreadCount(),2);
    if(!UI.Press(Read)||!UI.Press(TEXT("NotificationsUnread")))return false;
    TestTrue(TEXT("Unread filter hides read captured item"),UI.Exists(TEXT("NotificationsEmpty")));
    if(!UI.Press(TEXT("NotificationsRefresh")))return false;
    const uint64 Second=M->Notifications().LastSequence();
    if(!UI.Inspect(TEXT("notifications-unread"),{FName(*FString::Printf(TEXT("NotificationRead_%llu"),Second)),TEXT("NotificationsRefresh")}))return false;
    if(!UI.Press(FName(*FString::Printf(TEXT("NotificationOpen_%llu"),Second))))return false;
    TestEqual(TEXT("Reveal delegates to owner"),Revealed,1);TestEqual(TEXT("Successful reveal acknowledged"),M->Notifications().UnreadCount(),0);
    UI.Key(EKeys::Escape);TestEqual(TEXT("Escape uses close owner"),Closed,1);
    return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioNotificationsHeaderCompact,"Studio.HeadlessUI.Notifications.HeaderCompact",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioNotificationsHeaderCompact::RunTest(const FString&){return StudioNotificationUITests::Header(*this,FVector2D(1280,720));}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioNotificationsHeaderWide,"Studio.HeadlessUI.Notifications.HeaderWide",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioNotificationsHeaderWide::RunTest(const FString&){return StudioNotificationUITests::Header(*this,FVector2D(1320,740));}
#endif
