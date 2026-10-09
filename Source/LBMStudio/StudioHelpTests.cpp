#include "StudioHelp.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "StudioHeadlessSlate.h"
#include "StudioModel.h"
#include "StudioScene.h"
#include "StudioWorkspace.h"
#include "SStudioHelpPanel.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"

namespace StudioHelpTests
{
TSharedPtr<FJsonObject> Read(FAutomationTestBase& Test,const FString& Text)
{
    TSharedPtr<FJsonObject> JSON;
    Test.TestTrue(TEXT("Diagnostics is valid JSON"),FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),JSON)&&JSON.IsValid());
    return JSON;
}
FString Session()
{
    return FPaths::ProjectDir()/TEXT("tmp/debug/help-sessions")/FGuid::NewGuid().ToString();
}
bool Header(FAutomationTestBase& Test,FVector2D Size)
{
    const FString Root=Session();TSharedPtr<FStudioModel> Model=MakeShared<FStudioModel>(Root);
    ON_SCOPE_EXIT{Model.Reset();IFileManager::Get().DeleteDirectory(*Root,false,true);};
    // A real scene actor supplies the production workspace's read-only view
    // bindings. It is never initialized/ticked and submits no GPU captures.
    TStrongObjectPtr<UWorld> World(UWorld::CreateWorld(EWorldType::Game,false,NAME_None,nullptr,false));
    ON_SCOPE_EXIT{World->DestroyWorld(false);};
    auto* Scene=World->SpawnActor<AStudioScene>();
    if(!Test.TestNotNull(TEXT("Virtual workspace scene"),Scene))return false;
    Scene->Model=Model;Scene->ApplyCamera(Model->Project.Camera);
    auto Workspace=SNew(SStudioWorkspace).Tag(TEXT("HelpRoot")).Model(Model).Scene(Scene);
    FStudioHeadlessSlate UI(Test,Workspace,Size);
    const FString Prefix=FString::Printf(TEXT("help-header-%dx%d"),int32(Size.X),int32(Size.Y));
    if(!UI.Inspect(Prefix+TEXT("-closed"),{TEXT("HeaderHelp"),TEXT("ExportMenu"),TEXT("RunControl")}))return false;
    // Opening the workspace can complete its initial fit to the actual flow
    // viewport. Help must preserve the established view after that first layout.
    const FString Before=StudioProjectIO::Serialize(Model->SnapshotProject());
    const auto Intent=Model->RenderIntentRevision;const auto State=Model->State;
    const auto Frame=Model->SelectedFrame;const auto Captures=Scene->GetCaptureCount();
    if(!UI.Focus(TEXT("HeaderHelp")))return false;
    UI.Key(EKeys::F1);
    if(!UI.Inspect(Prefix+TEXT("-open"),{TEXT("HelpPanel"),TEXT("HelpClose"),TEXT("HelpPage0"),TEXT("HelpPage3")}))return false;
    if(!UI.Press(TEXT("HelpPage2"))||!UI.Focus(TEXT("HelpDiagnostics")))return false;
    UI.Key(EKeys::F1);
    Test.TestTrue(TEXT("F1 tunnels from focused diagnostics"),UI.Exists(TEXT("HelpPanel")));
    if(!UI.Press(TEXT("HelpClose")))return false;
    Test.TestFalse(TEXT("Close dismisses help"),UI.Exists(TEXT("HelpPanel")));
    const auto HeaderHelp=UI.Find(TEXT("HeaderHelp"));
    Test.TestTrue(TEXT("Close restores header keyboard target"),HeaderHelp->HasKeyboardFocus()||HeaderHelp->HasFocusedDescendants());
    if(!UI.Press(TEXT("HeaderHelp")))return false;
    UI.Key(EKeys::Escape);UI.Layout();
    Test.TestFalse(TEXT("Escape dismisses help"),UI.Exists(TEXT("HelpPanel")));
    Test.TestTrue(TEXT("Escape restores header keyboard target"),HeaderHelp->HasKeyboardFocus()||HeaderHelp->HasFocusedDescendants());
    Test.TestTrue(TEXT("Help retains exact project, case and camera"),StudioProjectIO::Serialize(Model->SnapshotProject())==Before);
    Test.TestEqual(TEXT("Help retains requested frame"),Model->SelectedFrame,Frame);
    Test.TestTrue(TEXT("Help retains playback state"),Model->State==State);
    Test.TestEqual(TEXT("Help submits no field intent"),Model->RenderIntentRevision,Intent);
    Test.TestEqual(TEXT("Help submits no render capture"),Scene->GetCaptureCount(),Captures);
    return !Test.HasAnyErrors();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHelpDiagnostics,"Studio.Help.ReadOnlyOriginalDiagnostics",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHelpDiagnostics::RunTest(const FString&)
{
    const FString Root=StudioHelpTests::Session();FStudioModel M(Root);
    ON_SCOPE_EXIT{IFileManager::Get().DeleteDirectory(*Root,false,true);};
    if(!TestTrue(TEXT("Published default recording loaded"),M.Solver&&M.Solver->FrameCount()>1))return false;
    M.SelectedFrame=M.Solver->FrameCount()-1;
    const FString Before=StudioProjectIO::Serialize(M.SnapshotProject());
    const auto Intent=M.RenderIntentRevision;const auto Revision=M.Revision;const auto State=M.State;
    const auto JSON=StudioHelpTests::Read(*this,StudioHelp::Diagnostics(M));if(!JSON)return false;
    const auto& D=M.Solver->Descriptor();const auto Recording=JSON->GetObjectField(TEXT("recording"));
    TestEqual(TEXT("Playback lifecycle is distinct from job lifecycle"),JSON->GetStringField(TEXT("playback_state")),M.StatusText());
    TestEqual(TEXT("Playback cursor is distinct from requested ordinal"),int32(JSON->GetNumberField(TEXT("playback_ordinal"))),M.PlaybackFrame);
    TestEqual(TEXT("Original-frame review mode is explicit"),JSON->GetBoolField(TEXT("reviewing_original_frame")),M.bReviewing);
    TestEqual(TEXT("Identity retains original dataset"),Recording->GetStringField(TEXT("dataset")),D.Id);
    TestEqual(TEXT("Identity retains original URL"),Recording->GetStringField(TEXT("source_url")),D.SourceURL);
    TestEqual(TEXT("Identity retains exact metadata hash"),Recording->GetStringField(TEXT("metadata_sha256")),D.MetadataSHA256);
    TestEqual(TEXT("Identity retains exact payload hash"),Recording->GetStringField(TEXT("payload_sha256")),D.PayloadSHA256);
    const auto F=M.Solver->EvaluateFrame(M.SelectedFrame);
    TestEqual(TEXT("Ordinal remains separate from original step"),int32(Recording->GetNumberField(TEXT("requested_ordinal"))),M.SelectedFrame);
    TestEqual(TEXT("Exact original step"),int32(Recording->GetNumberField(TEXT("requested_step"))),F.Index);
    TestEqual(TEXT("Exact original physical time"),Recording->GetNumberField(TEXT("requested_time_seconds")),F.Time);
    TestFalse(TEXT("Missing renderer is explicit"),JSON->GetObjectField(TEXT("renderer"))->GetBoolField(TEXT("available")));
    TestEqual(TEXT("Read-only diagnostics preserves exact document"),StudioProjectIO::Serialize(M.SnapshotProject()),Before);
    TestEqual(TEXT("Read-only diagnostics preserves intent"),M.RenderIntentRevision,Intent);
    TestEqual(TEXT("Read-only diagnostics preserves revision"),M.Revision,Revision);
    TestTrue(TEXT("Read-only diagnostics preserves state"),M.State==State);
    M.Project.bControlHarness=true;
    const auto Harness=StudioHelpTests::Read(*this,StudioHelp::Diagnostics(M));
    if(Harness)TestTrue(TEXT("Harness cannot claim computed fields"),Harness->GetStringField(TEXT("control_mode")).Contains(TEXT("no CFD computed")));
    M.Solver.Reset();const auto Missing=StudioHelpTests::Read(*this,StudioHelp::Diagnostics(M));
    if(Missing)
    {
        TestTrue(TEXT("Missing recording stays null"),Missing->Values[TEXT("recording")]->IsNull());
        TestEqual(TEXT("Missing recording cannot report ready playback"),Missing->GetStringField(TEXT("playback_state")),FString(TEXT("Data unavailable")));
    }
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHelpGuidance,"Studio.Help.WorkspaceGuidance",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHelpGuidance::RunTest(const FString&)
{
    for(int32 I=0;I<=int32(EStudioWorkspace::Setup);++I)
    {
        TestFalse(TEXT("Every workspace has a concrete name"),StudioHelp::WorkspaceName(EStudioWorkspace(I)).IsEmpty());
        TestFalse(TEXT("Every workspace has guidance"),StudioHelp::Guidance(EStudioWorkspace(I),false).IsEmpty());
    }
    TestTrue(TEXT("Replay guidance names original snapshots"),StudioHelp::Guidance(EStudioWorkspace::Solve,false).Contains(TEXT("original snapshots")));
    TestTrue(TEXT("Harness guidance discloses no CFD"),StudioHelp::Guidance(EStudioWorkspace::Solve,true).Contains(TEXT("computes no CFD")));
    TestTrue(TEXT("Provenance belongs to Results"),StudioHelp::Guidance(EStudioWorkspace::Results,false).Contains(TEXT("source provenance")));
    TestTrue(TEXT("Settings disclose source map ownership"),StudioHelp::Guidance(EStudioWorkspace::Settings,false).Contains(TEXT("source unit map")));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHelpPanelWorkflow,"Studio.HeadlessUI.Help.PanelWorkflow",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHelpPanelWorkflow::RunTest(const FString&)
{
    const FString Root=StudioHelpTests::Session();TSharedPtr<FStudioModel> M=MakeShared<FStudioModel>(Root);
    ON_SCOPE_EXIT{M.Reset();IFileManager::Get().DeleteDirectory(*Root,false,true);};
    const FString Before=StudioProjectIO::Serialize(M->SnapshotProject());
    const auto Intent=M->RenderIntentRevision;const auto Frame=M->SelectedFrame;const auto State=M->State;
    int32 Visits=0,Closed=0;FString Copied;
    auto Panel=SNew(SStudioHelpPanel).Model(M).Scene(nullptr)
        .OnResults_Lambda([&]{++Visits;}).OnClose_Lambda([&]{++Closed;})
        .CopyText([&](const FString& Value){Copied=Value;});
    FStudioHeadlessSlate UI(*this,Panel,FVector2D(540,500));
    if(!UI.Inspect(TEXT("help-workspace"),{TEXT("HelpGuidance"),TEXT("HelpResults"),TEXT("HelpClose")}))return false;
    TestEqual(TEXT("Guidance follows workspace"),UI.Text(TEXT("HelpWorkspaceTitle")),FString(TEXT("Fields")));
    if(!UI.Press(TEXT("HelpResults")))return false;
    TestEqual(TEXT("Provenance delegates to existing owner"),Visits,1);
    M->Workspace=EStudioWorkspace::Geometry;
    TestEqual(TEXT("Help context follows current workspace"),UI.Text(TEXT("HelpWorkspaceTitle")),FString(TEXT("Geometry")));
    M->Workspace=EStudioWorkspace::Solve;
    if(!UI.Press(TEXT("HelpPage1"))||!UI.Inspect(TEXT("help-shortcuts"),{TEXT("HelpShortcutScope"),TEXT("HelpPage3")}))return false;
    if(!UI.Press(TEXT("HelpPage2")))return false;
    const FString Frozen=UI.Text(TEXT("HelpDiagnostics"));
    if(!StudioHelpTests::Read(*this,Frozen))return false;
    M->Notice=TEXT("Playback notice after snapshot");UI.Layout();
    TestEqual(TEXT("Diagnostics is frozen during external changes"),UI.Text(TEXT("HelpDiagnostics")),Frozen);
    if(!UI.Type(TEXT("HelpDiagnostics"),TEXT("Overwrite attempt")))return false;
    TestEqual(TEXT("Read-only snapshot cannot be edited"),UI.Text(TEXT("HelpDiagnostics")),Frozen);
    if(!UI.Press(TEXT("HelpCopy")))return false;
    TestEqual(TEXT("Copy uses exactly displayed snapshot"),Copied,Frozen);
    TestTrue(TEXT("Copy status visible"),UI.Text(TEXT("HelpCopyStatus")).Contains(TEXT("Copied")));
    if(!UI.Inspect(TEXT("help-diagnostics"),{TEXT("HelpDiagnostics"),TEXT("HelpRefresh"),TEXT("HelpCopy")}))return false;
    if(!UI.Press(TEXT("HelpPage3"))||!UI.Inspect(TEXT("help-about"),{TEXT("HelpAbout"),TEXT("HelpVersion"),TEXT("HelpEngineVersion"),TEXT("HelpAboutData")}))return false;
    TestTrue(TEXT("About uses configured application version"),UI.Text(TEXT("HelpVersion")).EndsWith(StudioHelp::ApplicationVersion()));
    if(!UI.Press(TEXT("HelpPage2")))return false;
    TestEqual(TEXT("Category return retains displayed snapshot"),UI.Text(TEXT("HelpDiagnostics")),Frozen);
    if(!UI.Press(TEXT("HelpRefresh")))return false;
    const FString Refreshed=UI.Text(TEXT("HelpDiagnostics"));const auto JSON=StudioHelpTests::Read(*this,Refreshed);
    if(JSON)TestEqual(TEXT("Refresh observes current notice"),JSON->GetStringField(TEXT("notice")),M->Notice);
    TestEqual(TEXT("Refresh clears copy acknowledgement"),UI.Text(TEXT("HelpCopyStatus")),FString());
    if(!UI.Press(TEXT("HelpCopy")))return false;
    TestEqual(TEXT("Copy after refresh uses refreshed snapshot"),Copied,Refreshed);
    if(!UI.Press(TEXT("HelpClose")))return false;
    TestEqual(TEXT("Close uses owner callback"),Closed,1);
    TestEqual(TEXT("Help retains case, view and project"),StudioProjectIO::Serialize(M->SnapshotProject()),Before);
    TestEqual(TEXT("Help retains field frame"),M->SelectedFrame,Frame);
    TestTrue(TEXT("Help retains playback state"),M->State==State);
    TestEqual(TEXT("Help submits no field render intent"),M->RenderIntentRevision,Intent);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHelpHeaderCompact,"Studio.HeadlessUI.Help.HeaderCompact",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHelpHeaderCompact::RunTest(const FString&){return StudioHelpTests::Header(*this,FVector2D(1280,720));}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHelpHeaderWide,"Studio.HeadlessUI.Help.HeaderWide",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHelpHeaderWide::RunTest(const FString&){return StudioHelpTests::Header(*this,FVector2D(1320,740));}
#endif
