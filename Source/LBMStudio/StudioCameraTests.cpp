#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioCameraCommands,"Studio.Cameras.CollectionCommandsAndIsolation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioCameraCommands::RunTest(const FString&)
{
    FStudioModel M(FPaths::ProjectSavedDir()/TEXT("Automation/Cameras")/FGuid::NewGuid().ToString());
    M.Run(); M.Tick(.2); M.Scrub(.6);
    const auto OriginalView=M.InspectionState();
    const int32 Selected=M.SelectedFrame,Playing=M.PlaybackFrame,FieldRevision=M.Revision,ViewRevision=M.CameraRevision;
    const FString Draft=StudioCaseIO::Serialize(M.Project.Draft),Dataset=M.Project.Dataset;
    auto Saved=OriginalView.Camera;
    Saved.Position=FVector(2.1234567890123,-4,6);
    Saved.Orientation=FRotator(114,23,88).Quaternion(); Saved.bFreeCamera=true;
    Saved.bOrthographic=true; Saved.OrthoWidth=2.75;
    TestTrue(TEXT("Save exact arbitrary camera"),M.AddCamera(TEXT("  Wake detail  "),Saved));
    if(M.Project.Cameras.Num()!=1) return false;
    const auto Id=M.Project.Cameras[0].Id;
    TestEqual(TEXT("Trimmed name"),M.FindCamera(Id)->Name,FString(TEXT("Wake detail")));
    TestTrue(TEXT("Rename camera by stable identity"),M.RenameCamera(Id,TEXT("Trailing edge")));
    TestTrue(TEXT("Duplicate saved pose"),M.DuplicateCamera(Id));
    if(M.Project.Cameras.Num()!=2) return false;
    const auto Copy=M.Project.Cameras[1];
    TestNotEqual(TEXT("Copy has independent identity"),Copy.Id,Id);
    TestEqual(TEXT("Copy name is unique"),Copy.Name,FString(TEXT("Trailing edge copy")));
    TestTrue(TEXT("Copy uses saved pose, independent of active view"),StudioView::CameraEquals(Copy.Camera,Saved));
    auto Updated=Saved; Updated.Position.Z=12.345678901234;
    TestTrue(TEXT("Replace saved pose from current values"),M.UpdateCamera(Id,Updated));
    TestEqual(TEXT("Full precision retained"),M.FindCamera(Id)->Camera.Position.Z,Updated.Position.Z);
    TestTrue(TEXT("Update leaves duplicate untouched"),StudioView::CameraEquals(M.FindCamera(Copy.Id)->Camera,Saved));
    TestTrue(TEXT("Delete a saved camera"),M.DeleteCamera(Copy.Id));
    TestTrue(TEXT("Deleted camera can be restored"),M.UndoSavedCameras());
    TestTrue(TEXT("Undo preserves duplicate identity"),M.FindCamera(Copy.Id)!=nullptr);
    TestTrue(TEXT("Camera collection edits leave active view untouched"),M.InspectionState().Equals(OriginalView));
    TestEqual(TEXT("Camera collection edits do not invalidate field"),M.Revision,FieldRevision);
    TestEqual(TEXT("Camera collection edits do not invalidate active camera"),M.CameraRevision,ViewRevision);
    TestFalse(TEXT("Camera collection edits create no active-view undo"),M.CanUndoView());
    TestTrue(TEXT("Activate saved camera"),M.RestoreSavedCamera(Id));
    TestTrue(TEXT("Activation restores pose and projection"),StudioView::CameraEquals(M.Project.Camera,Updated));
    TestTrue(TEXT("Activation belongs to active-view history"),M.UndoView());
    TestTrue(TEXT("View undo restores prior camera"),M.InspectionState().Equals(OriginalView));
    TestTrue(TEXT("View undo retains saved update"),StudioView::CameraEquals(M.FindCamera(Id)->Camera,Updated));
    TestTrue(TEXT("Independent collection redo still available"),M.RedoSavedCameras());
    TestTrue(TEXT("Redo removes only the duplicate"),M.FindCamera(Copy.Id)==nullptr&&M.FindCamera(Id)!=nullptr);
    TestEqual(TEXT("Selected source frame unchanged"),M.SelectedFrame,Selected);
    TestEqual(TEXT("Playback cursor unchanged"),M.PlaybackFrame,Playing);
    TestEqual(TEXT("Source identity unchanged"),M.Project.Dataset,Dataset);
    TestEqual(TEXT("Case draft unchanged"),StudioCaseIO::Serialize(M.Project.Draft),Draft);
    TestTrue(TEXT("Playback remains running in review"),M.State==EStudioRunState::Running&&M.bReviewing);
    M.BeginViewEdit(TEXT("Unfinished orbit"));
    auto Dragged=M.Project.Camera; Dragged.Position.X+=3.;
    TestTrue(TEXT("Camera drag begins before activation"),M.EditCamera(TEXT("Orbit"),Dragged));
    TestTrue(TEXT("Activate closes preceding gesture"),M.RestoreSavedCamera(Id));
    TestFalse(TEXT("Activation does not retain an open gesture"),M.IsViewEditActive());
    TestTrue(TEXT("Activation has its own undo entry"),M.UndoView());
    TestTrue(TEXT("Activation undo retains the preceding drag"),StudioView::CameraEquals(M.Project.Camera,Dragged));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioCameraValidation,"Studio.Cameras.ValidationAndBoundedHistory",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioCameraValidation::RunTest(const FString&)
{
    FStudioModel M(FPaths::ProjectSavedDir()/TEXT("Automation/Cameras")/FGuid::NewGuid().ToString());
    const auto Camera=M.Project.Camera;
    TestFalse(TEXT("Blank name rejected"),M.AddCamera(TEXT(" \t "),Camera));
    TestFalse(TEXT("Embedded newline rejected"),M.AddCamera(TEXT("Wake\ndetail"),Camera));
    TestFalse(TEXT("Long name rejected"),M.AddCamera(FString::ChrN(121,TCHAR('x')),Camera));
    auto Bad=Camera; Bad.Orientation=FQuat(0,0,0,0);
    TestFalse(TEXT("Invalid rotation cannot enter camera library"),M.AddCamera(TEXT("Invalid"),Bad));
    Bad=Camera; Bad.Position.X=std::numeric_limits<double>::quiet_NaN();
    TestFalse(TEXT("Nonfinite position rejected"),M.AddCamera(TEXT("Invalid"),Bad));
    TestFalse(TEXT("Rejected changes create no history"),M.CanUndoSavedCameras());
    TestTrue(TEXT("First camera accepted"),M.AddCamera(TEXT("Wake"),Camera));
    const FGuid Id=M.Project.Cameras[0].Id;
    TestFalse(TEXT("Case-insensitive duplicate rejected"),M.AddCamera(TEXT("wake"),Camera));
    TestTrue(TEXT("Second camera accepted"),M.AddCamera(TEXT("Wing"),Camera));
    const FString Before=StudioProjectIO::Serialize(M.SnapshotProject());
    TestFalse(TEXT("Rename cannot collide"),M.RenameCamera(Id,TEXT("WING")));
    TestFalse(TEXT("Invalid replacement pose rejected"),M.UpdateCamera(Id,Bad));
    const auto Missing=FGuid::NewGuid();
    TestFalse(TEXT("Missing rename rejected"),M.RenameCamera(Missing,TEXT("Missing")));
    TestFalse(TEXT("Missing update rejected"),M.UpdateCamera(Missing,Camera));
    TestFalse(TEXT("Missing duplicate rejected"),M.DuplicateCamera(Missing));
    TestFalse(TEXT("Missing delete rejected"),M.DeleteCamera(Missing));
    TestFalse(TEXT("Missing activate rejected"),M.RestoreSavedCamera(Missing));
    TestEqual(TEXT("Invalid commands preserve entire document"),StudioProjectIO::Serialize(M.SnapshotProject()),Before);
    TestTrue(TEXT("Undo second addition"),M.UndoSavedCameras());
    TestTrue(TEXT("Unchanged rename accepted"),M.RenameCamera(Id,TEXT("Wake")));
    TestTrue(TEXT("Unchanged update accepted"),M.UpdateCamera(Id,Camera));
    TestTrue(TEXT("No-op commands preserve redo"),M.CanRedoSavedCameras());
    TestTrue(TEXT("Changed rename accepted"),M.RenameCamera(Id,TEXT("Wake profile")));
    TestFalse(TEXT("New edit discards redo branch"),M.CanRedoSavedCameras());
    M.Project.Cameras[0].Name=TEXT("External change");
    TestFalse(TEXT("Stale undo refuses to overwrite external changes"),M.UndoSavedCameras());
    TestEqual(TEXT("External name preserved"),M.Project.Cameras[0].Name,FString(TEXT("External change")));
    M.Project.Cameras[0].Name=TEXT("Wake profile");
    TestTrue(TEXT("Matching history can still restore"),M.UndoSavedCameras());

    M.NewProject(TEXT("Camera capacity"));
    for(int32 I=0;I<128;++I) TestTrue(TEXT("Camera within capacity"),M.AddCamera(FString::Printf(TEXT("View %d"),I),Camera));
    TestFalse(TEXT("Capacity rejects new camera"),M.AddCamera(TEXT("Excess"),Camera));
    TestFalse(TEXT("Capacity rejects duplicate"),M.DuplicateCamera(M.Project.Cameras[0].Id));
    TestEqual(TEXT("Rejected operations retain camera capacity"),M.Project.Cameras.Num(),128);
    int32 Undos=0; while(M.CanUndoSavedCameras()){if(!M.UndoSavedCameras())break;++Undos;}
    TestEqual(TEXT("Collection history bounded to 64 changes"),Undos,64);
    TestEqual(TEXT("Oldest retained history starts at camera 64"),M.Project.Cameras.Num(),64);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioCameraPersistence,"Studio.Cameras.ProjectLifetimeAndPersistence",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioCameraPersistence::RunTest(const FString&)
{
    const FString Dir=FPaths::ProjectSavedDir()/TEXT("Automation/Cameras")/FGuid::NewGuid().ToString();
    FStudioModel M(Dir); const FString Path=Dir/TEXT("cameras.lbms");
    const FString LongName=FString::ChrN(120,TCHAR('w'));
    TestTrue(TEXT("Maximum name length accepted"),M.AddCamera(LongName,M.Project.Camera));
    const auto Id=M.Project.Cameras[0].Id;
    TestTrue(TEXT("Long-name duplication stays saveable"),M.DuplicateCamera(Id));
    TestTrue(TEXT("Repeated duplication generates another unique name"),M.DuplicateCamera(Id));
    TestEqual(TEXT("Duplicate name stays in bounds"),M.Project.Cameras[1].Name.Len(),120);
    TestNotEqual(TEXT("Repeated names unique"),M.Project.Cameras[1].Name,M.Project.Cameras[2].Name);
    auto Changed=M.Project.Camera; Changed.Position=FVector(7,8,-4); Changed.FieldOfView=111.25;
    TestTrue(TEXT("Update pose before save"),M.UpdateCamera(Id,Changed));
    TestTrue(TEXT("Save camera library"),M.SaveProject(Path));
    const FString Saved=StudioProjectIO::Serialize(M.SnapshotProject());
    TestTrue(TEXT("Save retains collection history"),M.UndoSavedCameras());
    TestTrue(TEXT("Undo after save is unsaved"),M.HasUnsavedChanges());
    TestTrue(TEXT("Redo restores saved state"),M.RedoSavedCameras());
    TestFalse(TEXT("Redo to saved state is clean"),M.HasUnsavedChanges());
    TestFalse(TEXT("Failed load cannot erase history"),M.LoadProject(Dir/TEXT("missing.lbms")));
    TestTrue(TEXT("Failed load retained collection history"),M.CanUndoSavedCameras());
    TestTrue(TEXT("Reopen camera library"),M.LoadProject(Path));
    TestEqual(TEXT("Full project round trip"),StudioProjectIO::Serialize(M.SnapshotProject()),Saved);
    TestTrue(TEXT("Exact camera survives reopening"),StudioView::CameraEquals(M.FindCamera(Id)->Camera,Changed));
    TestFalse(TEXT("Opening begins new collection history"),M.CanUndoSavedCameras()||M.CanRedoSavedCameras());
    TestTrue(TEXT("Rename reopened camera"),M.RenameCamera(Id,TEXT("Inspection")));
    TestTrue(TEXT("Duplicate document"),M.DuplicateProject(Dir/TEXT("copy.lbms"),TEXT("Copy")));
    TestFalse(TEXT("Collection history cannot cross project identity"),M.CanUndoSavedCameras()||M.CanRedoSavedCameras());
    TestTrue(TEXT("Camera identities retained in copied project"),M.FindCamera(Id)!=nullptr);
    TestTrue(TEXT("Delete camera in copy"),M.DeleteCamera(Id));
    M.NewProject(TEXT("Empty camera library"));
    TestTrue(TEXT("New project camera library empty"),M.Project.Cameras.IsEmpty());
    TestFalse(TEXT("New project cannot restore old cameras through undo"),M.CanUndoSavedCameras()||M.CanRedoSavedCameras());
    return true;
}
#endif
