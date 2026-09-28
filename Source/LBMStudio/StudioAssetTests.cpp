#include "StudioAssetPaths.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
    FString AssetTestDirectory()
    {
#if WITH_EDITOR
        const FString Parent=FPaths::ProjectDir()/TEXT("tmp/debug/asset-tests");
#else
        const FString Parent=FPaths::ProjectSavedDir()/TEXT("Automation/AssetTests");
#endif
        const FString Root=FPaths::ConvertRelativePathToFull(Parent/FGuid::NewGuid().ToString());
        IFileManager::Get().MakeDirectory(*Root,true); return Root;
    }
    FStudioGeometryAsset AssetAt(const FString& Path)
    {
        FStudioGeometryAsset Asset; Asset.Name=TEXT("Wing source"); Asset.SourcePath=Path;
        Asset.SourceSHA256=FString::ChrN(64,TEXT('a'));
        FStudioSurfacePatch Patch; Patch.Name=TEXT("Wall"); Asset.Patches.Add(Patch); return Asset;
    }
    bool ReadStoredProject(const FString& Path,FStudioProject& Out)
    {
        FString Text,Error;
        return FFileHelper::LoadFileToString(Text,*Path)&&StudioProjectIO::Parse(Text,Out,Error);
    }
    bool DrainAssets(FStudioModel& Model)
    {
        const double Deadline=FPlatformTime::Seconds()+10.;
        Model.Tick(0);
        while(Model.IsCheckingAssets()&&FPlatformTime::Seconds()<Deadline)
        {FPlatformProcess::Sleep(.001f);Model.Tick(0);}
        return !Model.IsCheckingAssets();
    }
    const FString ABCSHA=TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    void WriteABC(const FString& Path)
    {IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);FFileHelper::SaveStringToFile(TEXT("abc"),*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioAssetPortability,"Studio.Assets.SaveAsDuplicateAndUndo",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioAssetPortability::RunTest(const FString&)
{
    const FString Root=AssetTestDirectory(), Original=Root/TEXT("original/case.lbms"), Copy=Root/TEXT("copy/case.lbms");
    const FString Source=Root/TEXT("original/assets/wing α.stl");
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Source),true);
    FFileHelper::SaveStringToFile(TEXT("source identity fixture"),*Source);
    FStudioModel M(Root/TEXT("session"));
    TestTrue(TEXT("Create original project"),M.CreateProject(Original,TEXT("Original")));
    TestTrue(TEXT("Relative authoring path has an explicit owning project"),M.EditCase(TEXT("Add wing"),[](auto& D)
    { D.Geometry.Add(AssetAt(TEXT("assets/wing α.stl"))); D.Setup.BackendId=TEXT("control-harness"); }));
    TestEqual(TEXT("Live reference resolves on entry before history capture"),M.Project.Draft.Geometry[0].SourcePath,Source);
    M.Project.Runs.Add(FStudioRunRecord::Capture(TEXT("Frozen"),M.Project.Draft,EStudioRunOrigin::ControlHarness));
    const auto Run=M.Project.Runs.Last(); const FString Frozen=StudioCaseIO::Serialize(*Run.GetConfiguration());
    const FGuid AssetId=M.Project.Draft.Geometry[0].Id, PatchId=M.Project.Draft.Geometry[0].Patches[0].Id;
    TestTrue(TEXT("Authoring change for history"),M.EditCase(TEXT("Translate wing"),[](auto& D){D.Geometry[0].Translation.X=2.;}));
    M.Scrub(.5); M.Project.Camera.Position=FVector(6,7,8);
    TestTrue(TEXT("Save As to different parent succeeds"),M.SaveProject(Copy));
    TestFalse(TEXT("Save As leaves a clean project"),M.HasUnsavedChanges());
    FStudioProject Stored,Loaded; FString Error;
    TestTrue(TEXT("Stored document parses without location effects"),ReadStoredProject(Copy,Stored));
    TestEqual(TEXT("On-disk path rebased to original geometry"),Stored.Draft.Geometry[0].SourcePath,FString(TEXT("../original/assets/wing α.stl")));
    TestEqual(TEXT("Frozen storage reference also rebased"),Stored.Runs.Last().GetConfiguration()->Geometry[0].SourcePath,Stored.Draft.Geometry[0].SourcePath);
    TestTrue(TEXT("Saved project opens with resolved references"),StudioProjectIO::Load(Copy,Loaded,Error));
    TestEqual(TEXT("Loaded asset still refers to original file"),Loaded.Draft.Geometry[0].SourcePath,Source);
    TestEqual(TEXT("Frozen configuration unchanged semantically"),StudioCaseIO::Serialize(*Loaded.Runs.Last().GetConfiguration()),Frozen);
    TestTrue(TEXT("Undo remains usable after moving document"),M.UndoCase());
    TestEqual(TEXT("Undo restores translation"),M.Project.Draft.Geometry[0].Translation.X,0.);
    TestEqual(TEXT("Undo keeps resolved asset"),M.Project.Draft.Geometry[0].SourcePath,Source);
    TestTrue(TEXT("Redo remains usable"),M.RedoCase());
    TestEqual(TEXT("Redo restores saved authored transform"),M.Project.Draft.Geometry[0].Translation.X,2.);
    // Case revisions are monotonic; the saved snapshot includes that bookkeeping.
    TestTrue(TEXT("Resave after history traversal"),M.SaveProject(Copy));
    const FString Before=StudioProjectIO::Serialize(M.SnapshotProject());
    TestFalse(TEXT("Failed Save As does not replace live path/state"),M.SaveProject(Root));
    TestEqual(TEXT("Failed Save As preserves project"),StudioProjectIO::Serialize(M.SnapshotProject()),Before);
    TestEqual(TEXT("Failed Save As preserves location"),M.ProjectPath,Copy);
    TestTrue(TEXT("Failed save preserves undo"),M.CanUndoCase());
    const FGuid ProjectId=M.Project.Id;
    TestTrue(TEXT("Duplicate into third folder"),M.DuplicateProject(Root/TEXT("duplicate/study.lbms"),TEXT("Copy")));
    TestNotEqual(TEXT("Duplicate has independent project identity"),M.Project.Id,ProjectId);
    TestEqual(TEXT("Duplicate keeps asset identity"),M.Project.Draft.Geometry[0].Id,AssetId);
    TestEqual(TEXT("Duplicate keeps patch mappings"),M.Project.Draft.Geometry[0].Patches[0].Id,PatchId);
    TestEqual(TEXT("Run identity unchanged"),M.Project.Runs.Last().GetId(),Run.GetId());
    TestEqual(TEXT("Frozen run values unchanged"),StudioCaseIO::Serialize(*M.Project.Runs.Last().GetConfiguration()),Frozen);
    TestEqual(TEXT("Camera remains independent"),M.Project.Camera.Position,FVector(6,7,8));
    TestEqual(TEXT("Selected recording frame remains independent"),M.SelectedFrame,300);
    TestTrue(TEXT("Duplicate reopens"),M.LoadProject(M.ProjectPath));
    TestEqual(TEXT("Reopened duplicate resolves original source"),M.Project.Draft.Geometry[0].SourcePath,Source);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioAssetRecovery,"Studio.Assets.BackupRecoveryAndMovedFolder",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioAssetRecovery::RunTest(const FString&)
{
    const FString Root=AssetTestDirectory(), Path=Root/TEXT("project/case.lbms"), Session=Root/TEXT("session");
    const FString Source=Root/TEXT("project/assets/wing.obj");
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Source),true);
    FFileHelper::SaveStringToFile(TEXT("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"),*Source);
    FStudioProject P; P.Draft.Geometry.Add(AssetAt(Source)); P.Draft.Setup.BackendId=TEXT("harness");
    P.Runs.Add(FStudioRunRecord::Capture(TEXT("Frozen"),P.Draft,EStudioRunOrigin::ControlHarness));
    FString Error; TestTrue(TEXT("Initial file saved"),StudioProjectIO::Save(Path,P,Error));
    P.Name=TEXT("Second revision"); TestTrue(TEXT("Replacement makes backup"),StudioProjectIO::Save(Path,P,Error));
    FStudioProject Backup;
    TestTrue(TEXT("Backup is independently loadable"),StudioProjectIO::Load(StudioProjectIO::BackupPath(Path),Backup,Error));
    TestEqual(TEXT("Backup asset is based on original owner, not backup directory"),Backup.Draft.Geometry[0].SourcePath,Source);
    TestEqual(TEXT("Backup run resolves correctly"),Backup.Runs.Last().GetConfiguration()->Geometry[0].SourcePath,Source);
    FStudioModel M(Session); TestTrue(TEXT("Open project"),M.LoadProject(Path));
    M.EditCase(TEXT("Move"),[](auto& D){D.Geometry[0].Translation.Y=3;}); M.WriteRecovery();
    FStudioModel Recovery(Session); Recovery.OpenSession();
    TestTrue(TEXT("Recovery accepted"),Recovery.RestoreRecovery());
    TestEqual(TEXT("Recovery keeps asset location"),Recovery.Project.Draft.Geometry[0].SourcePath,Source);
    TestEqual(TEXT("Recovery restores original document path"),Recovery.ProjectPath,Path);
    TestTrue(TEXT("Recovered edit can save elsewhere"),Recovery.SaveProject(Root/TEXT("recovered/again.lbms")));
    TestTrue(TEXT("Recovered project reopens"),Recovery.LoadProject(Recovery.ProjectPath));
    TestEqual(TEXT("Recovery Save As leaves source stable"),Recovery.Project.Draft.Geometry[0].SourcePath,Source);
    // Simulate relocating a project and its asset directory together.
    const FString Moved=Root/TEXT("moved/case.lbms"); FString Bytes;
    FFileHelper::LoadFileToString(Bytes,*Path); StudioProjectIO::WriteAtomic(Moved,Bytes,Error);
    FFileHelper::LoadFileToString(Bytes,*Source);
    StudioProjectIO::WriteAtomic(Root/TEXT("moved/assets/wing.obj"),Bytes,Error);
    FStudioProject Relocated;
    TestTrue(TEXT("Copied project uses new owner for stored relative paths"),StudioProjectIO::Load(Moved,Relocated,Error));
    TestEqual(TEXT("Folder relocation follows geometry beside project"),Relocated.Draft.Geometry[0].SourcePath,Root/TEXT("moved/assets/wing.obj"));
    FString MovedBytes;
    TestTrue(TEXT("Relocated reference opens its source"),FFileHelper::LoadFileToString(MovedBytes,*Relocated.Draft.Geometry[0].SourcePath));
    TestEqual(TEXT("Relocated source content preserved"),MovedBytes,Bytes);
    TestEqual(TEXT("Frozen references follow the same folder relocation"),Relocated.Runs.Last().GetConfiguration()->Geometry[0].SourcePath,Relocated.Draft.Geometry[0].SourcePath);
    // Migration from older recovery JSON whose references were never rebased.
    FStudioProject Legacy; ReadStoredProject(Path,Legacy); Legacy.RecoverySource=Path;
    const FString LegacyPath=Root/TEXT("old-recovery/recovery.lbms");
    StudioProjectIO::WriteAtomic(LegacyPath,StudioProjectIO::Serialize(Legacy),Error);
    TestTrue(TEXT("Legacy relative recovery loads"),StudioProjectIO::Load(LegacyPath,Relocated,Error));
    TestEqual(TEXT("Legacy recovery uses its original project folder"),Relocated.Draft.Geometry[0].SourcePath,Source);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioAssetAmbiguity,"Studio.Assets.RejectAmbiguousLocationsTransactionally",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioAssetAmbiguity::RunTest(const FString&)
{
    const FString Root=AssetTestDirectory(); FString Error;
    FStudioProject P; P.Draft.Geometry.Add(AssetAt(TEXT("assets/wing.stl")));
    const FString Before=StudioProjectIO::Serialize(P);
    TestFalse(TEXT("Unsaved relative source must not resolve against cwd or destination"),StudioProjectIO::Save(Root/TEXT("case.lbms"),P,Error));
    TestFalse(TEXT("Rejected save creates no document"),IFileManager::Get().FileExists(*(Root/TEXT("case.lbms"))));
    TestFalse(TEXT("Relative base also rejected"),StudioAssetPaths::Resolve(P,TEXT("relative-owner"),Error));
    TestEqual(TEXT("Rejected resolution preserves destination"),StudioProjectIO::Serialize(P),Before);
    P.AssetBaseDirectory=Root/TEXT("original");
    TestTrue(TEXT("Explicit original owner permits storage elsewhere"),StudioProjectIO::Save(Root/TEXT("copy.lbms"),P,Error));
    TestEqual(TEXT("Save never mutates caller's references"),StudioProjectIO::Serialize(P),Before);
    FStudioModel M(Root/TEXT("session"));
    TestFalse(TEXT("Unsaved authoring edit cannot smuggle ambiguous reference into undo"),M.EditCase(TEXT("Ambiguous"),[](auto& D){D.Geometry.Add(AssetAt(TEXT("assets/wing.stl")));}));
    TestFalse(TEXT("Invalid edit adds no undo entry"),M.CanUndoCase());
    TestTrue(TEXT("Absolute source accepted before first save"),M.EditCase(TEXT("Import"),[&Root](auto& D){D.Geometry.Add(AssetAt(Root/TEXT("wing.stl")));}));
    M.WriteRecovery(); FStudioModel Recovered(Root/TEXT("session")); Recovered.OpenSession();
    TestTrue(TEXT("Never-saved project recovery accepted"),Recovered.RestoreRecovery());
    TestEqual(TEXT("Never-saved recovery preserves absolute location"),Recovered.Project.Draft.Geometry[0].SourcePath,Root/TEXT("wing.stl"));
    TestTrue(TEXT("Recovered new project has no invented owner"),Recovered.Project.AssetBaseDirectory.IsEmpty());
    P.Draft.Geometry.Add(AssetAt(TEXT("https://invalid/wing.stl")));
    TestFalse(TEXT("Unsupported remote URI rejected without reinterpretation"),StudioAssetPaths::Resolve(P,Root,Error));
    TestEqual(TEXT("Earlier references not partially changed by failed resolution"),P.Draft.Geometry[0].SourcePath,FString(TEXT("assets/wing.stl")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioAssetVerification,"Studio.Assets.StreamedChecksumsAndInventory",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioAssetVerification::RunTest(const FString&)
{
    const auto Cancel=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    const FString Root=AssetTestDirectory(),Path=Root/TEXT("original.stl");WriteABC(Path);
    FString Hash,Error;
    TestTrue(TEXT("Hash original bytes"),StudioAssets::HashFile(Path,Cancel,Hash,Error));
    TestEqual(TEXT("SHA256 matches independent standard vector"),Hash,ABCSHA);
    FString Large;for(int32 I=0;I<100000;++I) Large+=TEXT("abc");
    FFileHelper::SaveStringToFile(Large,*(Root/TEXT("large.stl")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    TestTrue(TEXT("Hash across multiple bounded reads"),StudioAssets::HashFile(Root/TEXT("large.stl"),Cancel,Hash,Error));
    TestEqual(TEXT("Streaming checksum matches independently computed value"),Hash,FString(TEXT("a77aedfe2e4a7232ea628a71745a966224c4521d93134b993cde5b65ea2f6e3c")));
    FStudioProject P;auto Asset=AssetAt(Path);Asset.SourceSHA256=ABCSHA;P.Draft.Geometry.Add(Asset);P.Draft.Setup.BackendId=TEXT("harness");
    P.Runs.Add(FStudioRunRecord::Capture(TEXT("Run"),P.Draft,EStudioRunOrigin::ControlHarness));
    auto References=StudioAssets::References(P);
    TestEqual(TEXT("Shared draft/run source is checked once"),References.Num(),1);
    TestTrue(TEXT("Inventory shows both usages"),References[0].bDraft&&References[0].RunCount==1);
    auto Checked=StudioAssets::Check(References,Cancel);
    TestTrue(TEXT("Matching content verified"),Checked.References[0].State==EStudioAssetState::Verified);
    References[0].SHA256=FString::ChrN(64,TEXT('a'));Checked=StudioAssets::Check(References,Cancel);
    TestTrue(TEXT("Changed content detected"),Checked.References[0].State==EStudioAssetState::Changed);
    References[0].Path=Root/TEXT("missing.stl");Checked=StudioAssets::Check(References,Cancel);
    TestTrue(TEXT("Missing file disclosed"),Checked.References[0].State==EStudioAssetState::Missing);
    *Cancel=true;TestFalse(TEXT("Cancellation prevents hashing"),StudioAssets::HashFile(Path,Cancel,Hash,Error));
    TestTrue(TEXT("Cancellation clears stale result hash"),Hash.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioAssetRelink,"Studio.Assets.VerifiedRelinkPreservesRunsAndUndo",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioAssetRelink::RunTest(const FString&)
{
    const FString Root=AssetTestDirectory(),Old=Root/TEXT("missing/wing.stl"),New=Root/TEXT("found/wing-renamed.mesh");WriteABC(New);
    FStudioModel M(Root/TEXT("session"));auto Asset=AssetAt(Old);Asset.SourceSHA256=ABCSHA;
    TestTrue(TEXT("Add geometry reference"),M.EditCase(TEXT("Import"),[&Asset](auto& D){D.Geometry.Add(Asset);D.Setup.BackendId=TEXT("harness");}));
    M.Project.Runs.Add(FStudioRunRecord::Capture(TEXT("Frozen"),M.Project.Draft,EStudioRunOrigin::ControlHarness));
    const auto Frozen=M.Project.Runs.Last();const auto FrozenConfig=*Frozen.GetConfiguration();
    M.EditCase(TEXT("Move"),[](auto& D){D.Geometry[0].Translation.X=2;});M.UndoCase();
    TestTrue(TEXT("Missing refs finish background scan"),DrainAssets(M));
    TestTrue(TEXT("Source marked missing"),M.AssetReferences[0].State==EStudioAssetState::Missing);
    const auto Source=M.AssetReferences[0];const auto Camera=M.Project.Camera;M.Run();M.Scrub(.5);
    const int32 Frame=M.SelectedFrame,Revision=M.Revision;
    TestTrue(TEXT("Locate starts background verification"),M.LocateAsset(Source,New));
    TestTrue(TEXT("References unchanged before polling acknowledgement"),M.Project.Draft.Geometry[0].SourcePath==Old);
    TestTrue(TEXT("Verified relocation completes"),DrainAssets(M));
    TestEqual(TEXT("Draft now uses located file"),M.Project.Draft.Geometry[0].SourcePath,New);
    TestEqual(TEXT("Frozen run now locates same bytes"),M.Project.Runs.Last().GetConfiguration()->Geometry[0].SourcePath,New);
    TestEqual(TEXT("Frozen run identity retained"),M.Project.Runs.Last().GetId(),Frozen.GetId());
    auto Expected=FrozenConfig;Expected.Geometry[0].SourcePath=New;
    TestEqual(TEXT("Only frozen file location changes"),StudioCaseIO::Serialize(*M.Project.Runs.Last().GetConfiguration()),StudioCaseIO::Serialize(Expected));
    TestTrue(TEXT("Redo survived verified relocation"),M.RedoCase());
    TestEqual(TEXT("Redo cannot resurrect missing location"),M.Project.Draft.Geometry[0].SourcePath,New);
    TestTrue(TEXT("Undo survived verified relocation"),M.UndoCase());
    TestEqual(TEXT("Undo cannot resurrect missing location"),M.Project.Draft.Geometry[0].SourcePath,New);
    TestEqual(TEXT("Renderer state independent"),M.Revision,Revision);
    TestEqual(TEXT("Frame independent"),M.SelectedFrame,Frame);
    TestEqual(TEXT("Camera independent"),M.Project.Camera.Position,Camera.Position);
    TestTrue(TEXT("Playback stays running"),M.State==EStudioRunState::Running);
    TestTrue(TEXT("Located project saves"),M.SaveProject(Root/TEXT("case.lbms")));
    TestTrue(TEXT("Located project reopens"),M.LoadProject(Root/TEXT("case.lbms")));
    TestEqual(TEXT("Location persists through reopen"),M.Project.Draft.Geometry[0].SourcePath,New);
    M.EditCase(TEXT("Remove draft geometry"),[](auto& D){D.Geometry.Reset();});TestTrue(TEXT("Run-only inventory scan"),DrainAssets(M));
    TestEqual(TEXT("Run-only reference remains findable"),M.AssetReferences.Num(),1);
    TestFalse(TEXT("Run-only usage correctly labelled"),M.AssetReferences[0].bDraft);
    const FString Again=Root/TEXT("archive/wing.stl");WriteABC(Again);
    TestTrue(TEXT("Run-only reference can be located"),M.LocateAsset(M.AssetReferences[0],Again));TestTrue(TEXT("Run-only location completes"),DrainAssets(M));
    TestTrue(TEXT("Undo removed geometry"),M.UndoCase());
    TestEqual(TEXT("Undo-only draft ref is relocated too"),M.Project.Draft.Geometry[0].SourcePath,Again);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioAssetRelinkFailure,"Studio.Assets.RejectedCancelledAndStaleRelink",EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioAssetRelinkFailure::RunTest(const FString&)
{
    const FString Root=AssetTestDirectory(),New=Root/TEXT("found.stl"),Wrong=Root/TEXT("wrong.stl");WriteABC(New);
    FFileHelper::SaveStringToFile(TEXT("changed"),*Wrong);
    FStudioModel M(Root/TEXT("session"));auto Asset=AssetAt(Root/TEXT("missing.stl"));Asset.SourceSHA256=ABCSHA;
    M.EditCase(TEXT("Import"),[&Asset](auto& D){D.Geometry.Add(Asset);});TestTrue(TEXT("Initial scan completes"),DrainAssets(M));
    const auto Ref=M.AssetReferences[0];const FString Before=StudioProjectIO::Serialize(M.SnapshotProject());
    TestTrue(TEXT("Different file begins verification"),M.LocateAsset(Ref,Wrong));TestTrue(TEXT("Mismatch completes"),DrainAssets(M));
    TestEqual(TEXT("Different bytes cannot replace geometry"),StudioProjectIO::Serialize(M.SnapshotProject()),Before);
    TestTrue(TEXT("Different bytes give actionable error"),M.Notice.Contains(TEXT("differ")));
    TestTrue(TEXT("Start cancellable locate"),M.LocateAsset(Ref,New));M.CancelAssetCheck();TestTrue(TEXT("Cancelled worker drains"),DrainAssets(M));
    TestEqual(TEXT("Cancellation leaves document unchanged"),StudioProjectIO::Serialize(M.SnapshotProject()),Before);
    TestTrue(TEXT("Start locate before case mutation"),M.LocateAsset(Ref,New));M.EditCase(TEXT("Rename"),[](auto& D){D.Name=TEXT("New case revision");});
    TestTrue(TEXT("Stale work drained and inventory refreshed"),DrainAssets(M));
    TestEqual(TEXT("Stale result cannot move reference"),M.Project.Draft.Geometry[0].SourcePath,Ref.Path);
    TestTrue(TEXT("Start locate before project replacement"),M.LocateAsset(Ref,New));M.NewProject(TEXT("Different project"));
    const FGuid Id=M.Project.Id;TestTrue(TEXT("Old worker drains"),DrainAssets(M));
    TestEqual(TEXT("Replacement identity remains"),M.Project.Id,Id);
    TestTrue(TEXT("Old references cannot leak into replacement"),M.Project.Draft.Geometry.IsEmpty()&&M.AssetReferences.IsEmpty());
    TestFalse(TEXT("Old Locate control cannot target new project"),M.LocateAsset(Ref,New));
    return true;
}
#endif
