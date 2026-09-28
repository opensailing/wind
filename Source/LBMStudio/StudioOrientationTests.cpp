#include "StudioOrientation.h"
#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioOrientationDirections,"Studio.Orientation.DirectionsProjectionAndPicking",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioOrientationDirections::RunTest(const FString&)
{
    const auto Directions=StudioOrientation::Directions();TSet<FIntVector> Unique;
    TestEqual(TEXT("Six faces, twelve edges and eight corners"),Directions.Num(),26);
    FStudioCameraState Original;Original.Focus=FVector(2,3,4);Original.OrbitDistance=7.25;
    Original.FieldOfView=67;Original.OrthoWidth=3.75;Original.bFreeCamera=true;Original.bOrthographic=true;
    for(const auto& D:Directions)
    {
        Unique.Add(D);const auto C=StudioOrientation::Align(Original,D);
        TestTrue(TEXT("Camera lies on requested side of pivot"),(C.Position-C.Focus).GetSafeNormal().Equals(FVector(D).GetSafeNormal(),1.e-10));
        const FVector ExpectedForward=(C.Focus-C.Position).GetSafeNormal(),ActualForward=C.Orientation.GetForwardVector();
        // UE's platform matrix/quaternion conversion measured up to 2e-10
        // direction error. Use the existing view-pose tolerance, while exact
        // saved focus, radius, projection and persistence remain separate checks.
        TestTrue(*FString::Printf(TEXT("Camera looks at focus for %s (direction error %.17g)"),*D.ToString(),
            (ActualForward-ExpectedForward).Size()),ActualForward.Equals(ExpectedForward,1.e-8));
        TestEqual(TEXT("Pivot retained"),C.Focus,Original.Focus);
        TestTrue(TEXT("Orbit radius retained"),FMath::IsNearlyEqual((C.Position-C.Focus).Size(),Original.OrbitDistance,1.e-10));
        TestEqual(TEXT("Projection retained"),C.bOrthographic,Original.bOrthographic);
        TestEqual(TEXT("Lens retained"),C.FieldOfView,Original.FieldOfView);
        TestEqual(TEXT("Orthographic width retained"),C.OrthoWidth,Original.OrthoWidth);
        TestEqual(TEXT("Free-flight mode retained"),C.bFreeCamera,Original.bFreeCamera);
        TestTrue(TEXT("Normalized finite pose"),!C.Orientation.ContainsNaN()&&FMath::IsNearlyEqual(C.Orientation.SizeSquared(),1.,1.e-10));
        TestTrue(TEXT("Repeated direction is stable"),StudioView::CameraEquals(StudioOrientation::Align(C,D),C));
        for(const auto& Size:{FVector2D(100,90),FVector2D(200,180)})
        {
            const auto Regions=StudioOrientation::Regions(C.Orientation,Size);
            TestTrue(TEXT("One to three visible faces"),Regions.Num()>=9&&Regions.Num()<=27);
            for(const auto& R:Regions)
            {
                TestTrue(TEXT("Projected region has a valid direction"),StudioOrientation::IsDirection(R.Direction));
                const int32 Hit=StudioOrientation::Hit(Regions,R.Center);
                if(TestTrue(TEXT("Region center is hittable"),Regions.IsValidIndex(Hit)))
                    TestEqual(TEXT("Hit direction matches visible region"),Regions[Hit].Direction,R.Direction);
                for(const auto& P:R.Polygon)TestTrue(TEXT("Cube fits its reserved box"),P.X>0&&P.X<Size.X&&P.Y>0&&P.Y<Size.Y);
            }
            TestEqual(TEXT("Outside click has no direction"),StudioOrientation::Hit(Regions,FVector2D(-10,-10)),INDEX_NONE);
        }
    }
    TestEqual(TEXT("Every direction is distinct"),Unique.Num(),26);
    for(const auto& D:{FIntVector::ZeroValue,FIntVector(2,0,0),FIntVector(MIN_int32,0,0)})
    {
        TestFalse(TEXT("Invalid direction rejected"),StudioOrientation::IsDirection(D));
        TestTrue(TEXT("Invalid direction leaves camera untouched"),StudioView::CameraEquals(StudioOrientation::Align(Original,D),Original));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioOrientationHistory,"Studio.Orientation.HistoryAndProjectPersistence",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioOrientationHistory::RunTest(const FString&)
{
    FStudioModel M(FPaths::ProjectSavedDir()/TEXT("Automation/OrientationModel"));
    M.Run();M.Tick(.1);M.Scrub(.4);const auto Before=M.InspectionState();
    const auto Case=StudioCaseIO::Serialize(M.Project.Draft);const auto Selected=M.SelectedFrame,Playing=M.PlaybackFrame,Revision=M.Revision;
    const auto Intent=M.RenderIntentRevision;
    for(const auto& D:StudioOrientation::Directions())
    {
        const auto Camera=StudioOrientation::Align(M.Project.Camera,D);
        TestTrue(TEXT("Direction is an ordinary camera edit"),M.EditCamera(TEXT("Standard view"),Camera));
        TestEqual(TEXT("No case changes"),StudioCaseIO::Serialize(M.Project.Draft),Case);
        TestEqual(TEXT("No selected frame changes"),M.SelectedFrame,Selected);
        TestEqual(TEXT("No playback cursor changes"),M.PlaybackFrame,Playing);
        TestEqual(TEXT("No geometry rebuild"),M.Revision,Revision);
        TestEqual(TEXT("No field cancellation"),M.RenderIntentRevision,Intent);
        TestTrue(TEXT("Playback stays running"),M.State==EStudioRunState::Running);
    }
    const auto Final=M.InspectionState();FStudioProject Reloaded;FString Error;
    TestTrue(TEXT("Direction persists in current project format"),StudioProjectIO::Parse(StudioProjectIO::Serialize(M.SnapshotProject()),Reloaded,Error));
    TestTrue(TEXT("Reopened orientation/pose is exact"),StudioView::CameraEquals(Reloaded.Camera,Final.Camera));
    int32 Undos=0;while(M.CanUndoView()){if(!M.UndoView())break;++Undos;}
    TestEqual(TEXT("Each direction is one undo step"),Undos,26);
    TestTrue(TEXT("Undo restores arbitrary initial pose"),M.InspectionState().Equals(Before));
    int32 Redos=0;while(M.CanRedoView()){if(!M.RedoView())break;++Redos;}
    TestEqual(TEXT("Every direction can be redone"),Redos,26);
    TestTrue(TEXT("Redo restores final pose"),M.InspectionState().Equals(Final));
    return true;
}
#endif
