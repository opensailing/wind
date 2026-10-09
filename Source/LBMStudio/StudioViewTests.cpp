#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioViewIsolation,"Studio.View.GesturesAndIndependentState",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioViewIsolation::RunTest(const FString&)
{
    const FString Dir=FPaths::ProjectDir()/TEXT("tmp/debug/view-tests")/FGuid::NewGuid().ToString();
    FStudioModel M(Dir); const auto Initial=M.InspectionState(); const int32 InitialRevision=M.Revision;
    TestTrue(TEXT("Case edit has its own history"),M.EditCase(TEXT("Reference length"),[](auto& D){D.Setup.ReferenceLength=2.;}));
    const FString Case=StudioCaseIO::Serialize(M.Project.Draft);
    M.Run(); M.Tick(.1); M.Scrub(.7);
    const int32 Selected=M.SelectedFrame,Playing=M.PlaybackFrame,FieldRevision=M.Revision;
    M.BeginViewEdit(TEXT("Orbit camera"));
    for(int32 I=1;I<=600;++I)
    {
        auto C=Initial.Camera; C.Position=FVector(I*.01,3,4);
        C.Orientation=StudioView::Turn(Initial.Camera.Orientation,0,I,.25);
        M.EditCamera(TEXT("Orbit camera"),C);
    }
    M.EndViewEdit(); const auto Moved=M.InspectionState();
    TestEqual(TEXT("Camera edits do not rebuild the field"),M.Revision,FieldRevision);
    TestTrue(TEXT("Camera revision changes independently"),M.CameraRevision>0);
    TestTrue(TEXT("A complete drag is undoable"),M.UndoView());
    TestTrue(TEXT("One undo restores before all 600 moves"),M.InspectionState().Equals(Initial));
    TestFalse(TEXT("Drag created only one history entry"),M.CanUndoView());
    TestTrue(TEXT("Redo restores the final gesture"),M.RedoView());
    TestTrue(TEXT("Final camera restored"),M.InspectionState().Equals(Moved));
    TestEqual(TEXT("View history leaves case untouched"),StudioCaseIO::Serialize(M.Project.Draft),Case);
    TestEqual(TEXT("View history leaves selected frame untouched"),M.SelectedFrame,Selected);
    TestEqual(TEXT("View history leaves replay cursor untouched"),M.PlaybackFrame,Playing);
    TestTrue(TEXT("Playback continues during view history"),M.State==EStudioRunState::Running&&M.bReviewing);
    M.BeginViewEdit(TEXT("Slice position"));
    for(int32 I=0;I<100;++I) M.EditView(TEXT("Slice position"),[I](auto& S){S.Display.SlicePosition=I*.005;});
    M.EndViewEdit(); M.PlaybackRate=4.; M.bLoopPlayback=true;
    TestTrue(TEXT("Display edit invalidates field geometry"),M.Revision>FieldRevision&&M.Revision>InitialRevision);
    TestTrue(TEXT("Display gesture is one undo"),M.UndoView());
    TestEqual(TEXT("Slice returns to original position"),M.SlicePosition,Initial.Display.SlicePosition);
    TestEqual(TEXT("Undo retains current replay speed"),M.PlaybackRate,4.);
    TestTrue(TEXT("Undo retains current loop preference"),M.bLoopPlayback);
    TestTrue(TEXT("Display redo available"),M.RedoView());
    TestTrue(TEXT("Independent case undo remains available"),M.UndoCase());
    TestFalse(TEXT("Case reference value removed by case undo only"),M.Project.Draft.Setup.ReferenceLength.IsSet());
    TestTrue(TEXT("Case undo did not change camera"),StudioView::CameraEquals(M.Project.Camera,Moved.Camera));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioViewBoundaries,"Studio.View.BoundsBranchesAndInvalidEdits",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioViewBoundaries::RunTest(const FString&)
{
    FStudioViewHistory H; FStudioInspectionState State;
    for(int32 I=1;I<=100;++I) {auto Next=State;Next.Camera.Position.X=I;H.Record(TEXT("Move"),State,Next);State=Next;}
    int32 Count=0; FString Label; FStudioInspectionState Out;
    while(H.Restore(false,State,Out,Label)) {State=Out;++Count;}
    TestEqual(TEXT("History retains at most 64 edits"),Count,64);
    TestEqual(TEXT("Oldest retained starting position"),State.Camera.Position.X,36.);
    TestTrue(TEXT("Redo exists after bounded undo"),H.CanRedo());
    H.Begin(TEXT("Empty drag"),State); H.End();
    TestTrue(TEXT("Empty gesture preserves redo"),H.CanRedo());
    H.Begin(TEXT("Out and back"),State); auto Moved=State;Moved.Camera.Position.X+=1.;
    H.Record(TEXT("Move"),State,Moved); H.Record(TEXT("Move"),Moved,State); H.End();
    TestTrue(TEXT("Returned-to-start gesture preserves redo"),H.CanRedo());
    H.Record(TEXT("Branch"),State,Moved); State=Moved;
    TestFalse(TEXT("A real new edit clears redo"),H.CanRedo());
    auto ChangedOutside=State;ChangedOutside.Display.bVectors=!ChangedOutside.Display.bVectors;
    TestFalse(TEXT("History refuses to overwrite an unrelated external edit"),H.Restore(false,ChangedOutside,Out,Label));
    TestTrue(TEXT("Failed restore keeps valid history"),H.Restore(false,State,Out,Label));

    const FString Dir=FPaths::ProjectDir()/TEXT("tmp/debug/view-tests")/FGuid::NewGuid().ToString();
    FStudioModel M(Dir);const auto Before=M.InspectionState();
    TestFalse(TEXT("Nonfinite position rejected"),M.EditView(TEXT("Invalid"),[](auto& S){S.Camera.Position.X=std::numeric_limits<double>::quiet_NaN();}));
    TestFalse(TEXT("Degenerate quaternion rejected"),M.EditView(TEXT("Invalid"),[](auto& S){S.Camera.Orientation=FQuat(0,0,0,0);}));
    TestFalse(TEXT("Bad display range rejected"),M.EditView(TEXT("Invalid"),[](auto& S){S.Display.VolumeOpacity=2.;}));
    TestTrue(TEXT("Invalid edits preserve the view"),M.InspectionState().Equals(Before));
    TestFalse(TEXT("Invalid edits create no history"),M.CanUndoView());
    auto Antipodal=Before.Camera;Antipodal.Orientation=Antipodal.Orientation*-1.;
    TestTrue(TEXT("Equivalent quaternion accepted"),M.EditCamera(TEXT("Equivalent"),Antipodal));
    TestFalse(TEXT("Equivalent quaternion creates no history"),M.CanUndoView());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioViewPersistence,"Studio.View.ProjectLifetimeAndPersistence",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioViewPersistence::RunTest(const FString&)
{
    const FString Dir=FPaths::ProjectDir()/TEXT("tmp/debug/view-tests")/FGuid::NewGuid().ToString();
    FStudioModel M(Dir); const FString Path=Dir/TEXT("view.lbms");
    TestTrue(TEXT("Initial view saved"),M.SaveProject(Path)); const auto Initial=M.InspectionState();
    M.BeginViewEdit(TEXT("Camera gesture"));
    M.EditView(TEXT("Camera"),[](auto& S){S.Camera.Position=FVector(7,8,9);S.Camera.Orientation=FRotator(90,143,72).Quaternion();S.Camera.bOrthographic=true;});
    TestTrue(TEXT("Save commits the current gesture"),M.SaveProject(Path));
    TestFalse(TEXT("Save closes gesture"),M.IsViewEditActive());
    TestTrue(TEXT("Save preserves in-session undo"),M.UndoView());
    TestTrue(TEXT("Undo is dirty relative to saved view"),M.HasUnsavedChanges());
    TestTrue(TEXT("Undo restores original view"),M.InspectionState().Equals(Initial));
    TestTrue(TEXT("Redo returns to saved view"),M.RedoView());
    TestFalse(TEXT("Redo to saved view is clean"),M.HasUnsavedChanges());
    const auto Saved=M.InspectionState();
    TestFalse(TEXT("Failed open preserves view and history"),M.LoadProject(Dir/TEXT("missing.lbms")));
    TestTrue(TEXT("Failed open kept view undo"),M.CanUndoView());
    TestTrue(TEXT("Reopen succeeds"),M.LoadProject(Path));
    TestTrue(TEXT("Reopen restores arbitrary camera/projection"),M.InspectionState().Equals(Saved));
    TestFalse(TEXT("Reopen starts a new history"),M.CanUndoView()||M.CanRedoView());
    M.EditView(TEXT("Grid"),[](auto& S){S.Display.bMesh=true;});
    TestTrue(TEXT("Duplicate succeeds"),M.DuplicateProject(Dir/TEXT("copy.lbms"),TEXT("Copy")));
    TestFalse(TEXT("History never crosses project identity"),M.CanUndoView()||M.CanRedoView());
    M.EditView(TEXT("Vectors"),[](auto& S){S.Display.bVectors=false;}); M.NewProject(TEXT("Fresh"));
    TestFalse(TEXT("New project clears view history"),M.CanUndoView()||M.CanRedoView());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioCameraPoles,"Studio.View.ContinuousCameraOrientation",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioCameraPoles::RunTest(const FString&)
{
    FQuat Q=FQuat::Identity;
    for(int32 I=0;I<720;++I) Q=StudioView::Turn(Q,0,1,.25);
    TestTrue(TEXT("Drag passes the pole and reaches backward"),Q.GetForwardVector().Equals(-FVector::ForwardVector,1.e-8));
    TestTrue(TEXT("Camera is upside down beyond pole"),Q.GetUpVector().Equals(-FVector::UpVector,1.e-8));
    for(int32 I=0;I<720;++I) Q=StudioView::Turn(Q,0,1,.25);
    TestTrue(TEXT("Full pitch turn returns continuously"),Q.Equals(FQuat::Identity,1.e-8));
    const FQuat Rolled=FRotator(0,0,90).Quaternion();
    const auto Turned=StudioView::Turn(Rolled,360,0,.25);
    TestTrue(TEXT("Yaw around rolled local up preserves that axis"),Turned.GetUpVector().Equals(Rolled.GetUpVector(),1.e-8));
    TestTrue(TEXT("Repeated rotation stays normalized"),FMath::IsNearlyEqual(Turned.SizeSquared(),1.,1.e-10));
    return true;
}
#endif

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioFitRecordingBounds,"Studio.View.FitTranslatedRecordingBounds",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioFitRecordingBounds::RunTest(const FString&)
{
    const FBox Bounds(FVector(8.5,1.2,4.2),FVector(12.9,2.8,5.8));
    for(double Aspect:{.65,2.25})for(bool Ortho:{false,true})
    {
        FStudioCameraState C;C.Orientation=FRotator(38,67,29).Quaternion();C.bOrthographic=Ortho;C.FieldOfView=51;
        const auto Fitted=StudioView::FitBounds(C,Bounds,Aspect);
        TestEqual(TEXT("Fit targets the actual recording center"),Fitted.Focus,Bounds.GetCenter());
        TestEqual(TEXT("Fit retains arbitrary roll/orientation"),Fitted.Orientation,C.Orientation);
        TestEqual(TEXT("Fit retains projection"),Fitted.bOrthographic,Ortho);
        for(int32 I=0;I<8;++I)
        {
            const FVector P=FVector(I&1?Bounds.Max.X:Bounds.Min.X,I&2?Bounds.Max.Y:Bounds.Min.Y,I&4?Bounds.Max.Z:Bounds.Min.Z);
            const FVector Local=Fitted.Orientation.UnrotateVector(P-Fitted.Position);
            const double HalfW=Ortho?Fitted.OrthoWidth*.5:Local.X*FMath::Tan(FMath::DegreesToRadians(C.FieldOfView*.5));
            TestTrue(TEXT("All corners in front of camera"),Local.X>0);
            TestTrue(TEXT("All corners inside horizontal view"),FMath::Abs(Local.Y)<HalfW);
            TestTrue(TEXT("All corners inside vertical view"),FMath::Abs(Local.Z)<HalfW/Aspect);
        }
        FStudioInspectionState State;State.Camera=Fitted;State.Display.SlicePosition=10.7;
        TestTrue(TEXT("Translated slice is a valid view edit"),StudioView::IsValid(State));
        FStudioProject Project;Project.Camera=Fitted;Project.View=State.Display;FStudioProject Loaded;FString Error;
        TestTrue(TEXT("Translated inspection persists"),StudioProjectIO::Parse(StudioProjectIO::Serialize(Project),Loaded,Error));
        TestEqual(TEXT("Slice position retained in source coordinates"),Loaded.View.SlicePosition,10.7);
    }
    for(double Scale:{1.e-5,1.,1.e5})for(double FOV:{5.,51.,160.})for(bool Ortho:{false,true})
    {
        const FBox Box(-FVector(Scale),FVector(Scale));
        FStudioCameraState C;C.Orientation=FVector(1,1,1).Rotation().Quaternion();C.FieldOfView=FOV;C.bOrthographic=Ortho;
        constexpr double NearPlane=.1,Aspect=2.25;
        const auto Fitted=StudioView::FitBounds(C,Box,Aspect,NearPlane);
        for(int32 I=0;I<8;++I)
        {
            const FVector P=FVector(I&1?Box.Max.X:Box.Min.X,I&2?Box.Max.Y:Box.Min.Y,I&4?Box.Max.Z:Box.Min.Z);
            const FVector Local=C.Orientation.UnrotateVector(P-Fitted.Position);
            const double HalfW=Ortho?Fitted.OrthoWidth*.5:Local.X*FMath::Tan(FMath::DegreesToRadians(FOV*.5));
            TestTrue(TEXT("Diagonal fit has positive actual near-plane clearance at every scale/FOV"),Local.X>NearPlane);
            TestTrue(TEXT("Wide-FOV and small-bounds fit retains horizontal coverage"),FMath::Abs(Local.Y)<HalfW);
            TestTrue(TEXT("Wide-FOV and small-bounds fit retains vertical coverage"),FMath::Abs(Local.Z)<HalfW/Aspect);
        }
        TestEqual(TEXT("Near-plane clearance retains orientation"),Fitted.Orientation,C.Orientation);
    }
    return true;
}
#endif
