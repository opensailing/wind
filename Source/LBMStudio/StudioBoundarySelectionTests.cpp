#include "StudioBoundarySelection.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr EAutomationTestFlags BoundarySelectionFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter;
FStudioCameraState BoundaryObserver()
{
    FStudioCameraState Camera;Camera.Position=FVector::ZeroVector;Camera.Focus=FVector(2,0,0);
    Camera.Orientation=FQuat::Identity;Camera.OrbitDistance=2;Camera.FieldOfView=60;Camera.OrthoWidth=4;
    Camera.bDepthClipping=true;Camera.NearClipMeters=.1;Camera.FarClipMeters=10;return Camera;
}
FStudioDomainGeometry BoundaryTriangles(const FGuid& Near,const FGuid& Far)
{
    // Explicit geometric test fixture, not CFD or a product sample.
    FStudioDomainGeometry Geometry;auto Mesh=MakeShared<FStudioImportedMesh,ESPMode::ThreadSafe>();
    Mesh->Positions={FVector(4,-1,-1),FVector(4,1,-1),FVector(4,0,1),FVector(2,-1,-1),FVector(2,1,-1),FVector(2,0,1)};
    Mesh->Indices={0,1,2,3,4,5};Mesh->Bounds=FBox(FVector(2,-1,-1),FVector(4,1,1));
    Geometry.Preview=Mesh;Geometry.Bounds=Mesh->Bounds;Geometry.TriangleTargets={Far,Near};
    Geometry.PatchBounds.Add(Far,FBox(FVector(4,-1,-1),FVector(4,1,1)));
    Geometry.PatchBounds.Add(Near,FBox(FVector(2,-1,-1),FVector(2,1,1)));return Geometry;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioBoundaryPickTest,"Studio.Boundaries.OriginalTriangleOcclusionAndClipping",BoundarySelectionFlags)
bool FStudioBoundaryPickTest::RunTest(const FString&)
{
    const FGuid Near=FGuid::NewGuid(),Far=FGuid::NewGuid();const auto Geometry=BoundaryTriangles(Near,Far);
    auto Camera=BoundaryObserver();const FVector2D Size(800,600),Center(400,300);StudioBoundarySelection::FPatchHit Hit;
    TestTrue(TEXT("Perspective picks original surface"),StudioBoundarySelection::PickPatch(Geometry,Camera,Size,Center,Hit));
    TestEqual(TEXT("Near triangle occludes farther triangle regardless of source order"),Hit.Target,Near);
    TestEqual(TEXT("Physical hit position in meters"),Hit.Position,FVector(2,0,0));TestEqual(TEXT("Original triangle index retained"),Hit.Triangle,1);
    Camera.NearClipMeters=3;
    TestTrue(TEXT("Clipped near surface exposes original far surface"),StudioBoundarySelection::PickPatch(Geometry,Camera,Size,Center,Hit));TestEqual(TEXT("Correct surface behind near plane"),Hit.Target,Far);
    Camera.NearClipMeters=.1;Camera.FarClipMeters=3;
    TestTrue(TEXT("Far clip keeps closer surface selectable"),StudioBoundarySelection::PickPatch(Geometry,Camera,Size,Center,Hit));TestEqual(TEXT("Far-clipped surface never intercepts"),Hit.Target,Near);
    Camera.NearClipMeters=2.1;
    TestFalse(TEXT("No hits between clipped surfaces"),StudioBoundarySelection::PickPatch(Geometry,Camera,Size,Center,Hit));TestEqual(TEXT("No-hit result cannot replace selection"),Hit.Target,Near);
    Camera=BoundaryObserver();Camera.bOrthographic=true;
    const FVector Expected(2,.25,.125);FVector2D Pixel;
    TestTrue(TEXT("Project through actual captured aspect"),StudioCameraPlacement::Project(Camera,Size,Expected,Pixel,2.));
    TestTrue(TEXT("Orthographic original-surface pick with stretched viewport"),StudioBoundarySelection::PickPatch(Geometry,Camera,Size,Pixel,Hit,2.));
    TestTrue(TEXT("Ortho intersection honors captured aspect"),Hit.Position.Equals(Expected,1.e-10));
    Camera.bOrthographic=false;
    StudioCameraPlacement::Project(Camera,Size,Expected,Pixel,2.);
    TestTrue(TEXT("Perspective original-surface pick with captured aspect"),StudioBoundarySelection::PickPatch(Geometry,Camera,Size,Pixel,Hit,2.));
    TestTrue(TEXT("Perspective intersection matches visible pixel"),Hit.Position.Equals(Expected,1.e-10));
    Camera.Position=FVector(3,0,0);Camera.Focus=FVector(5,0,0);
    TestTrue(TEXT("Observer can be inside the original surfaces"),StudioBoundarySelection::PickPatch(Geometry,Camera,Size,Center,Hit));TestEqual(TEXT("Behind-camera surface is excluded"),Hit.Target,Far);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioBoundaryPickFailureTest,"Studio.Boundaries.FaceHandlesAndInvalidSelection",BoundarySelectionFlags)
bool FStudioBoundaryPickFailureTest::RunTest(const FString&)
{
    FStudioDomain Domain;auto Camera=BoundaryObserver();Camera.Position=FVector(-5,0,0);Camera.Focus=FVector::ZeroVector;Camera.OrbitDistance=5;
    const FVector2D Size(800,600),Center(400,300);
    auto Handles=StudioBoundarySelection::FaceHandles(Domain,Camera,Size);
    TestEqual(TEXT("Nearest overlapping face handle wins"),StudioBoundarySelection::HitFaceHandle(Handles,Center),Domain.Faces[0]);
    TestFalse(TEXT("Occluded coincident face handle is not drawn with a competing label"),Handles.ContainsByPredicate([](const auto& H){return H.Face==1;}));
    TestFalse(TEXT("Blank space does not select the enclosing domain box"),StudioBoundarySelection::HitFaceHandle(Handles,FVector2D(3,3)).IsValid());
    Camera.NearClipMeters=4.5;Handles=StudioBoundarySelection::FaceHandles(Domain,Camera,Size);
    TestEqual(TEXT("Clipped face handle cannot hide the farther face"),StudioBoundarySelection::HitFaceHandle(Handles,Center),Domain.Faces[1]);
    Camera.FarClipMeters=5.5;Handles=StudioBoundarySelection::FaceHandles(Domain,Camera,Size);
    TestFalse(TEXT("Both centered handles outside clipping are unavailable"),StudioBoundarySelection::HitFaceHandle(Handles,Center).IsValid());
    const FGuid Near=FGuid::NewGuid(),Far=FGuid::NewGuid();auto Geometry=BoundaryTriangles(Near,Far);Camera=BoundaryObserver();
    StudioBoundarySelection::FPatchHit Hit;Hit.Target=FGuid::NewGuid();const FGuid Kept=Hit.Target;
    for(const FVector2D Pixel:{FVector2D(-1,300),FVector2D(800,300),FVector2D(400,600),FVector2D(400,-1)})
        TestFalse(TEXT("Clicks outside displayed viewport rejected"),StudioBoundarySelection::PickPatch(Geometry,Camera,Size,Pixel,Hit));
    TestFalse(TEXT("Empty viewport rejected"),StudioBoundarySelection::PickPatch(Geometry,Camera,FVector2D::ZeroVector,Center,Hit));
    Geometry.bCancelled=true;TestFalse(TEXT("Cancelled source cannot be selected"),StudioBoundarySelection::PickPatch(Geometry,Camera,Size,Center,Hit));Geometry.bCancelled=false;
    Geometry.TriangleTargets.Pop();TestFalse(TEXT("Incomplete original patch mapping rejected"),StudioBoundarySelection::PickPatch(Geometry,Camera,Size,Center,Hit));
    Geometry=BoundaryTriangles(Near,Far);Geometry.PatchBounds.Remove(Near);
    TestFalse(TEXT("Incomplete verified patch bounds rejected"),StudioBoundarySelection::PickPatch(Geometry,Camera,Size,Center,Hit));
    TestEqual(TEXT("Every rejected selection retains output"),Hit.Target,Kept);
    return true;
}
#endif
