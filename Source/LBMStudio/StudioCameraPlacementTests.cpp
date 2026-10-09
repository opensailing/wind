#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace StudioCameraPlacementTests
{
    FStudioCameraState Observer()
    {
        FStudioCameraState C;C.Position=FVector::ZeroVector;C.Orientation=FQuat::Identity;
        C.Focus=FVector(10,0,0);C.OrbitDistance=10;C.FieldOfView=90;C.OrthoWidth=20;return C;
    }
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPlacementProjection,"Studio.CameraPlacement.ProjectionClippingAndRays",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioPlacementProjection::RunTest(const FString&)
{
    const FVector2D Size(800,600);auto C=StudioCameraPlacementTests::Observer();
    for(const bool Ortho:{false,true})
    {
        C.bOrthographic=Ortho;FVector2D Pixel;
        TestTrue(TEXT("Project known meters"),StudioCameraPlacement::Project(C,Size,FVector(10,5,2.5),Pixel));
        TestTrue(TEXT("Horizontal FOV and top-left screen coordinates"),Pixel.Equals(FVector2D(600,200),1.e-8));
        StudioCameraPlacement::FRay Ray;
        TestTrue(TEXT("Unproject same pixel"),StudioCameraPlacement::Ray(C,Size,Pixel,Ray));
        const double T=(10-Ray.Origin.X)/Ray.Direction.X;
        TestTrue(TEXT("Ray passes through original point"),(Ray.Origin+Ray.Direction*T).Equals(FVector(10,5,2.5),1.e-8));
        TestFalse(TEXT("Point behind observer rejected"),StudioCameraPlacement::Project(C,Size,FVector(-1,0,0),Pixel));
        FVector2D A,B;
        TestTrue(TEXT("Line crossing near plane clips safely"),StudioCameraPlacement::ProjectLine(C,Size,FVector(-1,-1,0),FVector(2,1,0),A,B));
        TestTrue(TEXT("Clipped endpoints inside viewport"),A.X>=-1.e-6&&A.X<=800+1.e-6&&A.Y>=0&&A.Y<=600&&B.X>=0&&B.X<=800&&B.Y>=0&&B.Y<=600);
        TestFalse(TEXT("Fully behind line omitted"),StudioCameraPlacement::ProjectLine(C,Size,FVector(-3,0,0),FVector(-1,0,0),A,B));
        C.bDepthClipping=true;C.NearClipMeters=2;C.FarClipMeters=8;
        TestFalse(TEXT("Far depth clips points"),StudioCameraPlacement::Project(C,Size,FVector(10,0,0),Pixel));
        TestFalse(TEXT("Near depth clips points"),StudioCameraPlacement::Project(C,Size,FVector(1,0,0),Pixel));
        TestFalse(TEXT("Entire line beyond far is absent"),StudioCameraPlacement::ProjectLine(C,Size,FVector(9,0,0),FVector(12,0,0),A,B));
        C.bDepthClipping=false;
        // Render targets are clamped and resize in steps. The Slate image can
        // stretch a 16:9 capture into this 4:3 panel; the camera keeps its FOV.
        TestTrue(TEXT("Project through the captured texture aspect"),StudioCameraPlacement::Project(C,Size,FVector(10,5,2.5),Pixel,16./9.));
        TestTrue(TEXT("Image stretch retains captured projection"),Pixel.Equals(FVector2D(600,500./3.),1.e-8));
        TestTrue(TEXT("Unproject stretched image"),StudioCameraPlacement::Ray(C,Size,Pixel,Ray,16./9.));
        const double StretchedT=(10-Ray.Origin.X)/Ray.Direction.X;
        TestTrue(TEXT("Stretched pixel still identifies original point"),(Ray.Origin+Ray.Direction*StretchedT).Equals(FVector(10,5,2.5),1.e-8));
        TestTrue(TEXT("Captured top edge clips stretched line"),StudioCameraPlacement::ProjectLine(C,Size,FVector(10,0,0),FVector(10,0,10),A,B,16./9.));
        TestTrue(TEXT("Clipping reaches actual image top"),A.Equals(FVector2D(400,300),1.e-8)&&B.Equals(FVector2D(400,0),1.e-8));
        TestFalse(TEXT("Invalid capture aspect rejected"),StudioCameraPlacement::Project(C,Size,FVector(10,0,0),Pixel,-1));
    }
    C=StudioCameraPlacementTests::Observer();C.Position=FVector(3,-2,7);C.Orientation=FRotator(67,42,119).Quaternion();
    const FVector P=C.Position+C.Orientation.RotateVector(FVector(10,5,2.5));FVector2D Screen;
    TestTrue(TEXT("Arbitrary observer pose projects"),StudioCameraPlacement::Project(C,Size,P,Screen));
    TestTrue(TEXT("Projection is invariant under rigid scene pose"),Screen.Equals(FVector2D(600,200),1.e-8));
    TestFalse(TEXT("Zero viewport rejected"),StudioCameraPlacement::Project(C,FVector2D::ZeroVector,P,Screen));
    TestFalse(TEXT("Nonfinite world coordinate rejected"),StudioCameraPlacement::Project(C,Size,FVector(std::numeric_limits<double>::infinity(),0,0),Screen));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPlacementGestures,"Studio.CameraPlacement.AxisTranslationAndRotation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioPlacementGestures::RunTest(const FString&)
{
    auto Observer=StudioCameraPlacementTests::Observer();auto Camera=Observer;
    Camera.Position=FVector(10,0,0);Camera.Focus=FVector(20,0,0);const FVector2D Size(800,600);
    for(bool Ortho:{false,true})
    {
        Observer.bOrthographic=Ortho;StudioCameraPlacement::FDrag Start;FStudioCameraState Changed;
        const auto Handles=StudioCameraPlacement::Handles(Camera,Observer,Size,EStudioCameraPlacementTool::Move);
        TestEqual(TEXT("Axis hit chooses Y"),StudioCameraPlacement::HitHandle(Handles,Observer,Size,FVector2D(440,300)),1);
        TestEqual(TEXT("Empty space leaves observer navigation available"),StudioCameraPlacement::HitHandle(Handles,Observer,Size,FVector2D(20,20)),INDEX_NONE);
        TestTrue(TEXT("Begin meter translation"),StudioCameraPlacement::BeginDrag(Camera,Observer,Size,FVector2D(440,300),EStudioCameraPlacementTool::Move,1,Start));
        TestTrue(TEXT("Drag along axis"),StudioCameraPlacement::Drag(Start,FVector2D(480,300),Changed));
        TestTrue(TEXT("40 pixels gives one meter here"),Changed.Position.Equals(FVector(10,1,0),1.e-8));
        TestTrue(TEXT("Focus translates by same displacement"),Changed.Focus.Equals(FVector(20,1,0),1.e-8));
        TestTrue(TEXT("Translation preserves orientation"),Changed.Orientation.Equals(Camera.Orientation));
        TestFalse(TEXT("End-on move axis is rejected"),StudioCameraPlacement::BeginDrag(Camera,Observer,Size,FVector2D(400,300),EStudioCameraPlacementTool::Move,0,Start));
        TestTrue(TEXT("Begin world X rotation ring"),StudioCameraPlacement::BeginDrag(Camera,Observer,Size,FVector2D(457.6,300),EStudioCameraPlacementTool::Rotate,0,Start));
        TestTrue(TEXT("Quarter-turn rotation"),StudioCameraPlacement::Drag(Start,FVector2D(400,242.4),Changed));
        TestTrue(TEXT("Quarter turn rotates Y toward Z"),Changed.Orientation.GetRightVector().Equals(FVector::UpVector,1.e-8));
        TestTrue(TEXT("Rotation retains camera position"),Changed.Position.Equals(Camera.Position));
        TestTrue(TEXT("Rotation retains normalized quaternion"),FMath::IsNearlyEqual(Changed.Orientation.SizeSquared(),1.,1.e-10));
        TestFalse(TEXT("Edge-on rotation plane is rejected"),StudioCameraPlacement::BeginDrag(Camera,Observer,Size,FVector2D(400,300),EStudioCameraPlacementTool::Rotate,1,Start));
        FVector2D A,B;const double Scale=StudioCameraPlacement::HandleScale(Observer,Size,Camera.Position);
        TestTrue(TEXT("Fixed-size handle is visible"),StudioCameraPlacement::ProjectLine(Observer,Size,Camera.Position,Camera.Position+FVector::RightVector*Scale,A,B));
        TestTrue(TEXT("Perpendicular axis remains 72 pixels"),FMath::IsNearlyEqual((B-A).Size(),72.,1.e-8));
        TestTrue(TEXT("Begin translation on stretched image"),StudioCameraPlacement::BeginDrag(Camera,Observer,Size,FVector2D(400,260),EStudioCameraPlacementTool::Move,2,Start,16./9.));
        TestTrue(TEXT("Vertical drag follows captured projection"),StudioCameraPlacement::Drag(Start,FVector2D(400,220),Changed));
        TestTrue(TEXT("Forty pixels gives three quarters of a meter at this aspect"),Changed.Position.Equals(FVector(10,0,.75),1.e-8));
    }
    TestEqual(TEXT("Perspective cone and center line"),StudioCameraPlacement::Frustum(Camera,4./3.).Num(),9);
    Camera.bOrthographic=true;
    TestEqual(TEXT("Orthographic cone adds parallel front rectangle"),StudioCameraPlacement::Frustum(Camera,4./3.).Num(),13);
    Camera.bDepthClipping=true;Camera.NearClipMeters=.25;Camera.FarClipMeters=7.5;
    for(bool Ortho:{false,true})
    {
        Camera.bOrthographic=Ortho;const auto Lines=StudioCameraPlacement::Frustum(Camera,4./3.);
        if(!TestEqual(TEXT("Clipped frustum has both planes and four edges"),Lines.Num(),13))return false;
        for(int32 I=0;I<4;++I)
        {
            const auto Near=Camera.Orientation.UnrotateVector(Lines[I].A-Camera.Position),Far=Camera.Orientation.UnrotateVector(Lines[I].B-Camera.Position);
            TestEqual(TEXT("Near rectangle uses actual near depth"),Near.X,.25);
            TestEqual(TEXT("Far rectangle uses actual far depth"),Far.X,7.5);
            TestTrue(TEXT("Near plane projection width"),FMath::IsNearlyEqual(FMath::Abs(Near.Y),Ortho?Camera.OrthoWidth*.5:.25,1.e-8));
            TestTrue(TEXT("Far plane projection width"),FMath::IsNearlyEqual(FMath::Abs(Far.Y),Ortho?Camera.OrthoWidth*.5:7.5,1.e-8));
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPlacementIsolation,"Studio.CameraPlacement.DraftApplyCancelAndIsolation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioPlacementIsolation::RunTest(const FString&)
{
    FStudioModel M(FPaths::ProjectSavedDir()/TEXT("Automation/CameraPlacement")/FGuid::NewGuid().ToString());
    const auto Original=M.Project.Camera;
    TestTrue(TEXT("Save camera to place"),M.AddCamera(TEXT("Wing camera"),Original));const auto Id=M.Project.Cameras[0].Id;
    M.Run();M.Tick(.2);M.Scrub(.6);M.AcceptLoadedView();
    const auto View=M.InspectionState();const auto Serial=StudioProjectIO::Serialize(M.SnapshotProject());
    const int32 Frame=M.SelectedFrame,Playback=M.PlaybackFrame,FieldRevision=M.Revision,CameraRevision=M.CameraRevision;
    const uint64 Intent=M.RenderIntentRevision;const auto* Solver=M.Solver.Get();
    const auto Case=StudioCaseIO::Serialize(M.Project.Draft);
    TestTrue(TEXT("Begin separate draft"),M.BeginCameraPlacement(Id));
    auto Draft=Original;Draft.Position=FVector(2.123456789012345,-3,4);Draft.Orientation=FRotator(40,70,120).Quaternion();
    Draft.Focus=Draft.Position+Draft.Orientation.GetForwardVector()*Draft.OrbitDistance;
    Draft.bOrthographic=true;Draft.OrthoWidth=3.456789012345;Draft.bDepthClipping=true;Draft.NearClipMeters=.002;Draft.FarClipMeters=14.56789012345;
    TestTrue(TEXT("Change exact draft"),M.EditCameraPlacement(Draft));M.SetCameraPlacementTool(EStudioCameraPlacementTool::Rotate);
    TestEqual(TEXT("Draft is not serialized"),StudioProjectIO::Serialize(M.SnapshotProject()),Serial);
    TestFalse(TEXT("Unapplied placement is not a document edit"),M.HasUnsavedChanges());
    TestTrue(TEXT("Original camera retained before apply"),StudioView::CameraEquals(M.FindCamera(Id)->Camera,Original));
    TestTrue(TEXT("Apply draft"),M.ApplyCameraPlacement());TestNull(TEXT("Apply closes transient draft"),M.CameraPlacement());
    TestTrue(TEXT("Exact transform and clipping stored"),StudioView::CameraEquals(M.FindCamera(Id)->Camera,Draft));
    TestEqual(TEXT("Full coordinate precision retained"),M.FindCamera(Id)->Camera.Position.X,Draft.Position.X);
    TestTrue(TEXT("One collection undo restores initial pose"),M.UndoSavedCameras());
    TestTrue(TEXT("Undo restores saved camera"),StudioView::CameraEquals(M.FindCamera(Id)->Camera,Original));
    TestTrue(TEXT("Redo placement"),M.RedoSavedCameras());
    TestTrue(TEXT("Second placement starts"),M.BeginCameraPlacement(Id));
    TestTrue(TEXT("Second draft changes"),M.EditCameraPlacement(Original));M.CancelCameraPlacement();
    TestTrue(TEXT("Cancel retains committed pose"),StudioView::CameraEquals(M.FindCamera(Id)->Camera,Draft));
    TestTrue(TEXT("All placement commands retain observing view"),M.InspectionState().Equals(View));
    TestFalse(TEXT("Placement creates no view-history entries"),M.CanUndoView());
    TestEqual(TEXT("Source frame retained"),M.SelectedFrame,Frame);TestEqual(TEXT("Playback cursor retained"),M.PlaybackFrame,Playback);
    TestTrue(TEXT("Replay continues in same review state"),M.State==EStudioRunState::Running&&M.bReviewing);
    TestEqual(TEXT("Field geometry revision retained"),M.Revision,FieldRevision);TestEqual(TEXT("Active camera revision retained"),M.CameraRevision,CameraRevision);
    TestEqual(TEXT("Render intent retained"),M.RenderIntentRevision,Intent);TestTrue(TEXT("Source object retained"),M.Solver.Get()==Solver);
    TestEqual(TEXT("Case parameters retained"),StudioCaseIO::Serialize(M.Project.Draft),Case);
    FString Error;FStudioProject Reloaded;
    TestTrue(TEXT("Placed camera round trips project serialization"),StudioProjectIO::Parse(StudioProjectIO::Serialize(M.SnapshotProject()),Reloaded,Error));
    TestTrue(TEXT("Reopened camera keeps full pose"),Reloaded.Cameras.Num()==1&&StudioView::CameraEquals(Reloaded.Cameras[0].Camera,Draft));
    TestTrue(TEXT("Re-enter placement for explicit preview"),M.BeginCameraPlacement(Id));
    auto Preview=Draft;Preview.Position.Z+=.125;Preview.Focus.Z+=.125;
    TestTrue(TEXT("Preview can use an unapplied pose"),M.EditCameraPlacement(Preview));
    const int32 Collection=M.CameraCollectionRevision,Placement=M.CameraPlacementRevision;
    TestTrue(TEXT("Explicit preview succeeds"),M.PreviewCameraPlacement());
    TestTrue(TEXT("Preview changes observing camera to draft"),StudioView::CameraEquals(M.Project.Camera,Preview));
    TestTrue(TEXT("Preview leaves saved camera unchanged"),StudioView::CameraEquals(M.FindCamera(Id)->Camera,Draft));
    TestTrue(TEXT("Preview retains editable draft"),M.IsCameraPlacementCurrent()&&StudioView::CameraEquals(M.CameraPlacement()->Camera,Preview));
    TestEqual(TEXT("Preview does not edit saved collection"),M.CameraCollectionRevision,Collection);
    TestEqual(TEXT("Preview does not rewrite draft"),M.CameraPlacementRevision,Placement);
    TestEqual(TEXT("Preview leaves field geometry intact"),M.Revision,FieldRevision);
    TestEqual(TEXT("Preview retains selected frame"),M.SelectedFrame,Frame);TestEqual(TEXT("Preview retains playback cursor"),M.PlaybackFrame,Playback);
    TestTrue(TEXT("Preview retains running review playback"),M.State==EStudioRunState::Running&&M.bReviewing);
    TestEqual(TEXT("Preview retains case"),StudioCaseIO::Serialize(M.Project.Draft),Case);
    const int32 PreviewRevision=M.CameraRevision;
    TestTrue(TEXT("Repeated preview is harmless"),M.PreviewCameraPlacement());
    TestEqual(TEXT("Repeated preview does not request another camera change"),M.CameraRevision,PreviewRevision);
    TestTrue(TEXT("No-op preview reports the current view"),M.CameraPlacementNotice.StartsWith(TEXT("Already viewing")));
    TestTrue(TEXT("Preview can be undone independently"),M.UndoView());
    TestTrue(TEXT("Undo preview restores exact previous inspection"),M.InspectionState().Equals(View));
    TestTrue(TEXT("Undo preview retains the draft"),M.IsCameraPlacementCurrent()&&StudioView::CameraEquals(M.CameraPlacement()->Camera,Preview));
    M.CancelCameraPlacement();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPlacementLifetime,"Studio.CameraPlacement.StaleDraftAndProjectLifetime",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioPlacementLifetime::RunTest(const FString&)
{
    FStudioModel M(FPaths::ProjectSavedDir()/TEXT("Automation/CameraPlacement")/FGuid::NewGuid().ToString());
    TestFalse(TEXT("Missing camera cannot be placed"),M.BeginCameraPlacement(FGuid::NewGuid()));
    TestTrue(TEXT("Save camera"),M.AddCamera(TEXT("Wake"),M.Project.Camera));const auto Id=M.Project.Cameras[0].Id;
    TestTrue(TEXT("Begin"),M.BeginCameraPlacement(Id));TestFalse(TEXT("Unapplied draft cannot be silently replaced"),M.BeginCameraPlacement(Id));
    const auto Original=M.CameraPlacement()->Camera;auto Bad=Original;Bad.Position.X=std::numeric_limits<double>::quiet_NaN();
    TestFalse(TEXT("Nonfinite draft rejected"),M.EditCameraPlacement(Bad));
    TestTrue(TEXT("Bad input keeps last valid draft"),StudioView::CameraEquals(M.CameraPlacement()->Camera,Original));
    Bad=Original;Bad.NearClipMeters=20;Bad.FarClipMeters=1;
    TestFalse(TEXT("Invalid clipping rejected even in draft"),M.EditCameraPlacement(Bad));
    TestTrue(TEXT("Collection can change independently"),M.RenameCamera(Id,TEXT("Renamed")));
    TestFalse(TEXT("Changed collection invalidates draft"),M.IsCameraPlacementCurrent());
    const auto Observer=M.Project.Camera;
    TestFalse(TEXT("Stale draft cannot move observing camera"),M.PreviewCameraPlacement());
    TestTrue(TEXT("Rejected preview keeps previous view"),StudioView::CameraEquals(M.Project.Camera,Observer));
    TestFalse(TEXT("Stale draft cannot overwrite saved camera"),M.ApplyCameraPlacement());
    TestNotNull(TEXT("Stale draft remains visible for cancellation"),M.CameraPlacement());
    M.CancelCameraPlacement();TestTrue(TEXT("Fresh placement after cancellation"),M.BeginCameraPlacement(Id));
    TestTrue(TEXT("Delete target independently"),M.DeleteCamera(Id));TestFalse(TEXT("Deleted target cannot be recreated by apply"),M.ApplyCameraPlacement());
    TestTrue(TEXT("Deleted target remains absent"),M.Project.Cameras.IsEmpty());M.CancelCameraPlacement();
    TestTrue(TEXT("Restore target"),M.UndoSavedCameras());TestTrue(TEXT("Start before replacing project"),M.BeginCameraPlacement(Id));
    M.NewProject(TEXT("Replacement"));TestNull(TEXT("Project replacement clears transient draft"),M.CameraPlacement());
    TestFalse(TEXT("Old draft cannot apply to new project"),M.ApplyCameraPlacement());
    return true;
}
#endif
