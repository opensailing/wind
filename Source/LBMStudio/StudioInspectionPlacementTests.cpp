#include "StudioInspectionPlacement.h"
#include "Math/RotationMatrix.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto PlacementFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
FStudioCameraState Observer(FVector Position,FVector Focus,bool Ortho=false)
{
    FStudioCameraState C;C.Position=Position;C.Focus=Focus;C.OrbitDistance=(Position-Focus).Size();
    C.Orientation=FRotationMatrix::MakeFromXZ((Focus-Position).GetSafeNormal(),FVector::UpVector).ToQuat().GetNormalized();
    C.bOrthographic=Ortho;C.OrthoWidth=5;return C;
}
const FStudioInspectionSource GeometrySource{TEXT("placement-geometry-only"),FString::ChrN(64,'a'),FString::ChrN(64,'b')};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPlacementFrozenPlane,"Studio.Inspection.PlacementFrozenPlaneAndIdentity",PlacementFlags)
bool FStudioPlacementFrozenPlane::RunTest(const FString&)
{
    const FGuid Project=FGuid::NewGuid(),Object=FGuid::NewGuid();const FVector2D Size(900,500);
    auto Camera=Observer(FVector(-4,0,0),FVector::ZeroVector);
    auto Draft=FStudioInspectionPlacement::Begin(Project,Object,GeometrySource,7,FVector::ZeroVector,3,3,FVector::ZeroVector,Camera);
    if(!TestTrue(TEXT("Begin three-point geometry-only draft"),Draft.IsSet()))return false;
    const FVector Origin=Draft->PlaneOrigin,Normal=Draft->PlaneNormal;
    const FVector Points[]={FVector(0,.4,.2),FVector(0,-.6,.4),FVector(0,-.6,-.4)};
    for(int32 I=0;I<3;++I)
    {
        if(I>0)Camera=Observer(FVector(-3,2,1),FVector::ZeroVector,I==2);
        FVector2D Pixel;
        TestTrue(TEXT("Known geometry point projects in changed observer"),StudioCameraPlacement::Project(Camera,Size,Points[I],Pixel,1.8));
        Draft->UpdatePreview(Camera,Size,Pixel,1.8);
        TestTrue(TEXT("Preview intersects the original frozen plane"),Draft->Preview.IsSet()&&Draft->Preview->Equals(Points[I],1.e-10));
        TestTrue(TEXT("A preview does not commit an endpoint"),Draft->Accepted.Num()==I);
        TestTrue(TEXT("Accept next endpoint"),Draft->AcceptPreview());
        TestTrue(TEXT("Camera movement keeps plane exact"),Draft->PlaneOrigin==Origin&&Draft->PlaneNormal==Normal);
    }
    TestTrue(TEXT("Three accepted endpoints finish the bounded gesture"),Draft->IsComplete());
    TestFalse(TEXT("Completed gesture cannot add a fourth point"),Draft->AcceptPreview());
    TestTrue(TEXT("Camera motion retained first accepted point"),Draft->Accepted[0].Equals(Points[0],1.e-10));
    TestTrue(TEXT("Same project/source/object/revision is current"),Draft->IsCurrent(Project,Object,GeometrySource,7));
    TestFalse(TEXT("Undo/object edit invalidates a draft"),Draft->IsCurrent(Project,Object,GeometrySource,8));
    TestFalse(TEXT("Reopen/replacement project invalidates a draft"),Draft->IsCurrent(FGuid::NewGuid(),Object,GeometrySource,7));
    TestFalse(TEXT("Selection change invalidates a draft"),Draft->IsCurrent(Project,FGuid::NewGuid(),GeometrySource,7));
    auto ChangedSource=GeometrySource;ChangedSource.MetadataSHA256=FString::ChrN(64,'c');
    TestFalse(TEXT("Same dataset title with different content invalidates a draft"),Draft->IsCurrent(Project,Object,ChangedSource,7));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioPlacementPlanar,"Studio.Inspection.PlacementSourcePlaneAndMisses",PlacementFlags)
bool FStudioPlacementPlanar::RunTest(const FString&)
{
    const FVector2D Size(800,600);const FVector Point(.15,.125,.4);
    auto Camera=Observer(FVector(0,4,1),Point);
    auto Draft=FStudioInspectionPlacement::Begin(FGuid::NewGuid(),FGuid::NewGuid(),GeometrySource,1,FVector(0,7,0),2,2,FVector(0,.125,0),Camera);
    if(!TestTrue(TEXT("Begin source-plane gesture"),Draft.IsSet()))return false;
    TestEqual(TEXT("Source offset fixes exact plane coordinate"),Draft->PlaneOrigin.Y,.125);
    FVector2D Pixel;TestTrue(TEXT("Point on shifted source plane projects"),StudioCameraPlacement::Project(Camera,Size,Point,Pixel));
    Draft->UpdatePreview(Camera,Size,Pixel,0);
    TestTrue(TEXT("2D placement has exact source Y and correct X/Z"),Draft->Preview.IsSet()&&Draft->Preview->Y==.125&&Draft->Preview->Equals(Point,1.e-10));
    TestTrue(TEXT("Moving outside view clears previous preview"),Draft->UpdatePreview(Camera,Size,FVector2D(-1,-1),0));
    TestFalse(TEXT("Missing preview cannot reuse a stale accepted coordinate"),Draft->AcceptPreview());
    Camera.bDepthClipping=true;Camera.NearClipMeters=.1;Camera.FarClipMeters=1;
    Draft->UpdatePreview(Camera,Size,Pixel,0);
    TestFalse(TEXT("Far-clipped plane cannot receive an invisible endpoint"),Draft->Preview.IsSet());
    Camera.FarClipMeters=10;Camera.NearClipMeters=5;Draft->UpdatePreview(Camera,Size,Pixel,0);
    TestFalse(TEXT("Near-clipped plane cannot receive an invisible endpoint"),Draft->Preview.IsSet());
    Camera=Observer(FVector(0,.125,0),FVector(1,.125,0));
    Draft->UpdatePreview(Camera,Size,Size*.5,0);
    TestFalse(TEXT("Parallel ray does not invent an intersection"),Draft->Preview.IsSet());
    Camera=Observer(FVector(0,4,0),FVector(0,5,0));Draft->UpdatePreview(Camera,Size,Size*.5,0);
    TestFalse(TEXT("Plane behind observer does not accept a point"),Draft->Preview.IsSet());
    TestFalse(TEXT("Unsupported spatial dimension rejected"),FStudioInspectionPlacement::Begin(FGuid::NewGuid(),FGuid::NewGuid(),GeometrySource,1,Point,2,1,FVector::ZeroVector,Camera).IsSet());
    return true;
}
#endif
