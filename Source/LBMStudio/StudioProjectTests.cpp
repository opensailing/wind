#include "StudioProject.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioProjectRoundTrip,"Studio.Project.DocumentRoundTrip",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioProjectRoundTrip::RunTest(const FString&)
{
    FStudioProject Original; Original.Name=TEXT("Wing α / camera study"); Original.bFavorite=true;
    Original.SelectedFrame=417; Original.View.bMesh=true; Original.View.PlaybackRate=.5;
    Original.Camera.Position=FVector(-3.6,8.2,1.9);
    Original.Camera.Orientation=FRotator(90,170,-87).Quaternion();
    Original.Camera.bOrthographic=true; Original.Camera.OrthoWidth=4.125;
    Original.Camera.FieldOfView=67; Original.Camera.bFreeCamera=true;
    FStudioCameraBookmark Bookmark; Bookmark.Name=TEXT("Trailing edge"); Bookmark.Camera=Original.Camera;
    Original.Cameras.Add(Bookmark);
    FStudioProject Loaded; FString Error;
    TestTrue(TEXT("Full document parses"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Original),Loaded,Error));
    TestEqual(TEXT("Stable project identity"),Loaded.Id,Original.Id);
    TestEqual(TEXT("Unicode name"),Loaded.Name,Original.Name);
    TestEqual(TEXT("Source frame restored"),Loaded.SelectedFrame,417);
    TestTrue(TEXT("Exact arbitrary orientation retained"),Loaded.Camera.Orientation.Equals(Original.Camera.Orientation,1.e-12));
    TestEqual(TEXT("Position retained in meters"),Loaded.Camera.Position,Original.Camera.Position);
    TestTrue(TEXT("Projection and mode retained"),Loaded.Camera.bOrthographic&&Loaded.Camera.bFreeCamera);
    TestEqual(TEXT("Ortho width retained"),Loaded.Camera.OrthoWidth,4.125);
    TestEqual(TEXT("Favorite retained"),Loaded.bFavorite,true);
    TestEqual(TEXT("Bookmark count"),Loaded.Cameras.Num(),1);
    if(Loaded.Cameras.Num()==1) TestEqual(TEXT("Stable camera identity"),Loaded.Cameras[0].Id,Bookmark.Id);
    TestEqual(TEXT("Playback speed is separate view state"),Loaded.View.PlaybackRate,.5);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioProjectPersistence,"Studio.Project.AtomicSaveAndRejectedReplacement",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioProjectPersistence::RunTest(const FString&)
{
    const FString Dir=FPaths::ProjectDir()/TEXT("tmp/debug/project-tests");
    IFileManager::Get().MakeDirectory(*Dir,true);
    const FString Path=Dir/TEXT("roundtrip.lbms");
    FStudioProject P; P.Name=TEXT("Original"); P.SelectedFrame=30;
    FString Error;
    TestTrue(TEXT("Initial durable save"),StudioProjectIO::Save(Path,P,Error));
    P.Name=TEXT("Revised"); P.SelectedFrame=400;
    TestTrue(TEXT("Atomic replacement succeeds"),StudioProjectIO::Save(Path,P,Error));
    FStudioProject Saved,Backup;
    TestTrue(TEXT("Latest document readable"),StudioProjectIO::Load(Path,Saved,Error));
    TestTrue(TEXT("Previous document readable"),StudioProjectIO::Load(StudioProjectIO::BackupPath(Path),Backup,Error));
    TestEqual(TEXT("Latest data"),Saved.Name,FString(TEXT("Revised")));
    TestEqual(TEXT("Backup has previous data"),Backup.Name,FString(TEXT("Original")));
    // A process interrupted before rename leaves a temporary file, never a partial document.
    FFileHelper::SaveStringToFile(TEXT("{partial"),*(Path+TEXT(".interrupted.tmp")));
    TestTrue(TEXT("Interrupted write cannot replace committed document"),StudioProjectIO::Load(Path,Saved,Error));
    TestEqual(TEXT("Committed frame retained"),Saved.SelectedFrame,400);
    P.Camera.Orientation=FQuat(0,0,0,0);
    TestFalse(TEXT("Invalid rotation cannot be saved"),StudioProjectIO::Save(Path,P,Error));
    TestTrue(TEXT("Failed save retains valid original"),StudioProjectIO::Load(Path,Saved,Error));
    TestEqual(TEXT("Original file preserved"),Saved.Name,FString(TEXT("Revised")));
    TestFalse(TEXT("Rename onto a directory fails safely"),StudioProjectIO::WriteAtomic(Dir,TEXT("invalid target"),Error));
    TestTrue(TEXT("Project inside directory survives failed atomic replace"),StudioProjectIO::Load(Path,Saved,Error));

    FStudioModel M(Dir/TEXT("session")); M.Scrub(.5); M.Project.Camera.Position=FVector(4,5,6);
    const FString Before=StudioProjectIO::Serialize(M.SnapshotProject());
    const FString Corrupt=Dir/TEXT("corrupt.lbms"); FFileHelper::SaveStringToFile(TEXT("{partial"),*Corrupt);
    TestFalse(TEXT("Corrupt open rejected"),M.LoadProject(Corrupt));
    TestEqual(TEXT("Rejected open leaves live state untouched"),StudioProjectIO::Serialize(M.SnapshotProject()),Before);
    Saved.SelectedFrame=99999; StudioProjectIO::Save(Corrupt,Saved,Error);
    TestFalse(TEXT("Out of recording frame rejected"),M.LoadProject(Corrupt));
    TestEqual(TEXT("Rejected frame leaves live state untouched"),StudioProjectIO::Serialize(M.SnapshotProject()),Before);
    TestTrue(TEXT("Valid project opens"),M.LoadProject(Path));
    TestEqual(TEXT("Open does not reset selected frame"),M.SelectedFrame,400);
    TestTrue(TEXT("Opened recording waits for user"),M.State==EStudioRunState::Paused);
    TestFalse(TEXT("Opened project starts clean"),M.HasUnsavedChanges());
    M.Project.Camera.Position.X+=1;
    TestTrue(TEXT("Camera edits mark document dirty"),M.HasUnsavedChanges());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioProjectMigration,"Studio.Project.MigrationAndSchemaValidation",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioProjectMigration::RunTest(const FString&)
{
    const FString Legacy=TEXT(R"({"version":2,"sample":"MeshGraphNets_Airfoil_test009","sliceAxis":2,"slicePosition":0.4,"streamlineDensity":0.7,"vectorScale":1.5,"volumeOpacity":0.2,"playbackRate":2,"loopPlayback":true,"streamlines":true,"vectors":false,"cutPlane":true,"volume":true,"mesh":false})");
    FStudioProject P; FString Error;
    TestTrue(TEXT("Legacy preference migration"),StudioProjectIO::Parse(Legacy,P,Error));
    TestEqual(TEXT("Legacy slice preserved"),P.View.SliceAxis,2);
    TestEqual(TEXT("Legacy playback preserved"),P.View.PlaybackRate,2.);
    TestTrue(TEXT("Migrated document has stable identity"),P.Id.IsValid());
    TestFalse(TEXT("Unsupported version rejected"),StudioProjectIO::Parse(TEXT("{\"version\":99}"),P,Error));
    FStudioProject Wrong; Wrong.Dataset=TEXT("nonexistent-recording");
    TestTrue(TEXT("Document preserves unavailable source identity for relinking"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Wrong),P,Error));
    const FString MissingPath=FPaths::ProjectDir()/TEXT("tmp/debug/project-tests/missing-recording.lbms");
    TestTrue(TEXT("Document can retain missing asset reference"),StudioProjectIO::Save(MissingPath,Wrong,Error));
    FStudioModel Live(FPaths::ProjectDir()/TEXT("tmp/debug/missing-recording-session"));
    const FString Before=StudioProjectIO::Serialize(Live.SnapshotProject());
    TestFalse(TEXT("Open refuses unavailable source without substitution"),Live.LoadProject(MissingPath));
    TestEqual(TEXT("Failed open preserves active document"),StudioProjectIO::Serialize(Live.SnapshotProject()),Before);
    Wrong=FStudioProject(); FStudioCameraBookmark B; B.Name=TEXT("Same"); Wrong.Cameras={B,B};
    TestFalse(TEXT("Duplicate bookmark identity rejected"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Wrong),P,Error));
    Wrong=FStudioProject(); Wrong.View.SliceAxis=-1;
    TestFalse(TEXT("Invalid display enum rejected"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Wrong),P,Error));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioProjectRecovery,"Studio.Project.RecoveryAndIndependentProjects",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioProjectRecovery::RunTest(const FString&)
{
    const FString Dir=FPaths::ProjectDir()/TEXT("tmp/debug/project-tests")/FGuid::NewGuid().ToString();
    FStudioModel First(Dir); First.NewProject(TEXT("Project one"));
    const FString Path=Dir/TEXT("one.lbms");
    TestTrue(TEXT("First project saves"),First.SaveProject(Path));
    const FGuid FirstId=First.Project.Id;
    First.Scrub(.75); First.Project.Camera.Position=FVector(9,8,7);
    TestTrue(TEXT("Named camera added"),First.AddCamera(TEXT("Wake detail"),First.Project.Camera));
    TestFalse(TEXT("Duplicate names rejected"),First.AddCamera(TEXT("wake DETAIL"),First.Project.Camera));
    First.WriteRecovery();
    FStudioModel Reopened(Dir); Reopened.OpenSession();
    TestEqual(TEXT("Normal reopen reads last saved source frame"),Reopened.SelectedFrame,0);
    TestFalse(TEXT("Recovery offered separately"),Reopened.PendingRecovery.IsEmpty());
    TestTrue(TEXT("Restore unsaved work"),Reopened.RestoreRecovery());
    TestEqual(TEXT("Recovered source frame"),Reopened.SelectedFrame,450);
    TestEqual(TEXT("Recovered arbitrary camera"),Reopened.Project.Camera.Position,FVector(9,8,7));
    TestEqual(TEXT("Recovered camera bookmark"),Reopened.Project.Cameras.Num(),1);
    TestEqual(TEXT("Recovery retains project identity"),Reopened.Project.Id,FirstId);
    TestEqual(TEXT("Recovery saves back to original destination"),Reopened.ProjectPath,FPaths::ConvertRelativePathToFull(Path));
    TestTrue(TEXT("Recovery remains unsaved until explicitly saved"),Reopened.HasUnsavedChanges());
    TestTrue(TEXT("Recovered project can be committed"),Reopened.SaveProject(Path));
    TestFalse(TEXT("Successful save clears recovery"),IFileManager::Get().FileExists(*(Dir/TEXT("Recovery/StudioRecovery.lbms"))));
    Reopened.NewProject(TEXT("Project two"));
    TestNotEqual(TEXT("New document has a new identity"),Reopened.Project.Id,FirstId);
    TestEqual(TEXT("New document has its own bookmarks"),Reopened.Project.Cameras.Num(),0);
    TestTrue(TEXT("Second project saves independently"),Reopened.SaveProject(Dir/TEXT("two.lbms")));
    TestTrue(TEXT("First document remains available"),Reopened.LoadProject(Path));
    TestEqual(TEXT("Switching back retains first document frame"),Reopened.SelectedFrame,450);
    TestEqual(TEXT("Switching back retains first identity"),Reopened.Project.Id,FirstId);
    TestEqual(TEXT("Recent projects bounded list contains both"),Reopened.RecentProjects.Num(),2);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioVectorPersistence,"Studio.Project.VectorSettingsMigrationAndHistory",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioVectorPersistence::RunTest(const FString&)
{
    FStudioProject P;P.View.VectorCount=777;P.View.VectorScale=1.625;P.View.bUniformVectors=true;
    FStudioProject Loaded;FString Error;
    TestTrue(TEXT("Vector settings parse"),StudioProjectIO::Parse(StudioProjectIO::Serialize(P),Loaded,Error));
    TestTrue(TEXT("Vector settings retain exact values"),Loaded.View.VectorCount==777&&Loaded.View.VectorScale==1.625&&Loaded.View.bUniformVectors);
    TSharedPtr<FJsonObject> JSON;
    if(!TestTrue(TEXT("Decode project for migration"),FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StudioProjectIO::Serialize(P)),JSON)))return false;
    const auto Encode=[&]{FString Text;FJsonSerializer::Serialize(JSON.ToSharedRef(),TJsonWriterFactory<>::Create(&Text));return Text;};
    const auto View=JSON->GetObjectField(TEXT("view"));
    View->RemoveField(TEXT("vectorCount"));View->RemoveField(TEXT("uniformVectors"));
    TestFalse(TEXT("Current schema requires complete vector state"),StudioProjectIO::Parse(Encode(),Loaded,Error));
    JSON->SetNumberField(TEXT("version"),13);
    TestTrue(TEXT("Previous schema migrates"),StudioProjectIO::Parse(Encode(),Loaded,Error));
    TestTrue(TEXT("Migration supplies bounded proportional arrows and keeps old scale"),Loaded.View.VectorCount==384&&!Loaded.View.bUniformVectors&&Loaded.View.VectorScale==1.625);
    JSON->SetNumberField(TEXT("version"),FStudioProject::CurrentVersion);View->SetBoolField(TEXT("uniformVectors"),true);
    for(double Count:{0.,4097.,1.5})
    {
        View->SetNumberField(TEXT("vectorCount"),Count);
        TestFalse(TEXT("Invalid count refused"),StudioProjectIO::Parse(Encode(),Loaded,Error));
        TestEqual(TEXT("Rejected count preserves prior project"),Loaded.View.VectorCount,384);
    }
    FStudioModel M(FPaths::ProjectSavedDir()/TEXT("Automation/VectorHistory")/FGuid::NewGuid().ToString());
    const auto Before=M.SnapshotProject();
    TestTrue(TEXT("Vector settings enter view history"),M.EditView(TEXT("Vector display"),[](auto& S){S.Display.VectorCount=777;S.Display.VectorScale=1.625;S.Display.bUniformVectors=true;}));
    TestTrue(TEXT("Vector edit invalidates geometry"),!StudioView::RenderEquals(M.SnapshotProject().View,Before.View));
    TestTrue(TEXT("Undo restores vector settings together"),M.UndoView()&&M.VectorCount==Before.View.VectorCount&&M.bUniformVectors==Before.View.bUniformVectors&&M.VectorScale==Before.View.VectorScale);
    TestTrue(TEXT("Redo restores exact vector controls"),M.RedoView()&&M.VectorCount==777&&M.VectorScale==1.625&&M.bUniformVectors);
    TestTrue(TEXT("Vector edits keep the source and physical frame"),M.Project.Dataset==Before.Dataset&&M.SelectedFrame==Before.SelectedFrame);
    return true;
}
#endif
