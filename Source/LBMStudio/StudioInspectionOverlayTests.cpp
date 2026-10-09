#include "StudioInspectionOverlay.h"
#include "StudioProbeMarkers.h"
#include "StudioStreamlines.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto OverlayFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
const FStudioInspectionSource OverlaySource{TEXT("annotation-geometry-only"),FString::ChrN(64,'a'),FString::ChrN(64,'b')};
FStudioCameraState OverlayCamera(bool Orthographic=false)
{
    FStudioCameraState C;C.Position=FVector(-4,0,0);C.Focus=FVector::ZeroVector;
    C.Orientation=FQuat::Identity;C.OrbitDistance=4;C.bOrthographic=Orthographic;C.OrthoWidth=4;
    return C;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioOverlayPicking,"Studio.Inspection.AnnotationPickingAndClipping",OverlayFlags)
bool FStudioOverlayPicking::RunTest(const FString&)
{
    const FGuid Project=FGuid::NewGuid();const FVector2D Size(800,600);const FBox Bounds(FVector(-1),FVector(1));
    FStudioInspectionObjects Objects;
    FStudioSliceObject Slice;Slice.Source=OverlaySource;Slice.Normal=FVector::ForwardVector;Slice.Name=TEXT("Slice");Objects.Slices.Add(Slice);
    FStudioProbeObject Point;Point.Source=OverlaySource;Point.A=FVector(0,-.5,.5);Objects.Probes.Add(Point);
    FStudioProbeObject Line;Line.Source=OverlaySource;Line.Kind=EStudioProbeKind::Line;Line.A=FVector(0,-.7,-.5);Line.B=FVector(0,-.2,-.5);Objects.Probes.Add(Line);
    FStudioRulerObject Angle;Angle.Source=OverlaySource;Angle.Kind=EStudioRulerKind::Angle;
    Angle.A=FVector(0,.2,-.5);Angle.B=FVector(0,.6,-.5);Angle.C=FVector(0,.6,.1);Objects.Rulers.Add(Angle);
    const auto Geometry=StudioInspectionOverlay::Build(Objects,OverlaySource,Project,Angle.Id,Bounds,nullptr);
    for(bool Ortho:{false,true})
    {
        auto Camera=OverlayCamera(Ortho);
        auto PickAt=[&](const FVector& World)
        {FVector2D Pixel;TestTrue(TEXT("Known geometric target projects"),StudioCameraPlacement::Project(Camera,Size,World,Pixel,1.9));return StudioInspectionOverlay::Pick(Geometry,Camera,Size,Pixel,FGuid(),1.9);};
        TestTrue(TEXT("Slice edge is selectable"),PickAt(FVector(0,1,.3))==Slice.Id);
        TestTrue(TEXT("Slice origin is selectable"),PickAt(FVector::ZeroVector)==Slice.Id);
        TestFalse(TEXT("Slice interior leaves the camera gesture available"),PickAt(FVector(0,.3,.3)).IsValid());
        TestTrue(TEXT("Point marker is selectable"),PickAt(Point.A)==Point.Id);
        TestTrue(TEXT("Line probe interior is selectable"),PickAt((Line.A+Line.B)*.5)==Line.Id);
        TestTrue(TEXT("First angle arm selectable"),PickAt((Angle.A+Angle.B)*.5)==Angle.Id);
        TestTrue(TEXT("Second angle arm selectable"),PickAt((Angle.B+Angle.C)*.5)==Angle.Id);
        FVector2D Pixel;StudioCameraPlacement::Project(Camera,Size,Point.A,Pixel,1.9);
        TestTrue(TEXT("Six-unit marker tolerance"),StudioInspectionOverlay::Pick(Geometry,Camera,Size,Pixel+FVector2D(6,0),FGuid(),1.9)==Point.Id);
        TestFalse(TEXT("Ten units misses a marker"),StudioInspectionOverlay::Pick(Geometry,Camera,Size,Pixel+FVector2D(10,0),FGuid(),1.9).IsValid());
        Camera.bDepthClipping=true;Camera.NearClipMeters=.01;Camera.FarClipMeters=3;
        TestFalse(TEXT("Far-clipped annotations cannot be selected"),StudioInspectionOverlay::Pick(Geometry,Camera,Size,Pixel,FGuid(),1.9).IsValid());
        Camera.NearClipMeters=5;Camera.FarClipMeters=8;
        TestFalse(TEXT("Near-clipped annotations cannot be selected"),StudioInspectionOverlay::Pick(Geometry,Camera,Size,Pixel,FGuid(),1.9).IsValid());
    }
    FStudioInspectionObjects SeedObjects;FStudioSeedObject Seed;Seed.Source=OverlaySource;Seed.Count=7;Seed.Name=TEXT("Inlet");SeedObjects.Seeds.Add(Seed);
    const FBox Subdomain(FVector(-.5,-.25,-.75),FVector(.75,.5,.8));
    for(int32 Dimensions:{2,3})
    {
        TArray<FVector> Expected;FString Error;
        TestTrue(TEXT("Seed reference positions valid"),StudioStreamlines::Seeds(Seed,Subdomain,Dimensions,.125,Expected,Error));
        const auto Seeds=StudioInspectionOverlay::Build(SeedObjects,OverlaySource,Project,Seed.Id,Bounds,nullptr,Dimensions,.125,Subdomain);
        TestEqual(TEXT("One annotation per exact seed"),Seeds.Markers.Num(),Expected.Num());
        for(int32 I=0;I<FMath::Min(Seeds.Markers.Num(),Expected.Num());++I)
            TestTrue(TEXT("Seed markers follow tracing domain and source plane"),Seeds.Markers[I].Position==Expected[I]);
    }
    TestFalse(TEXT("Outside panel cannot pick"),StudioInspectionOverlay::Pick(Geometry,OverlayCamera(),Size,FVector2D(-1,300),FGuid()).IsValid());
    StudioInspectionOverlay::FGeometry Crossing;const FGuid Edge=FGuid::NewGuid();
    Crossing.Lines.Add({Edge,FVector(-5,0,0),FVector(1,0,0)});
    TestTrue(TEXT("Visible part of a near-plane crossing is selectable"),StudioInspectionOverlay::Pick(Crossing,OverlayCamera(),Size,Size*.5,FGuid())==Edge);
    Crossing.Lines[0].B=FVector(-4.5,0,0);
    TestFalse(TEXT("Wholly behind camera is excluded"),StudioInspectionOverlay::Pick(Crossing,OverlayCamera(),Size,Size*.5,FGuid()).IsValid());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioOverlayIdentity,"Studio.Inspection.AnnotationSourceAndOriginalIDIdentity",OverlayFlags)
bool FStudioOverlayIdentity::RunTest(const FString&)
{
    const FGuid Project=FGuid::NewGuid();const FBox Bounds(FVector(-1),FVector(1));
    FStudioInspectionObjects Objects;
    FStudioProbeObject Probe;Probe.Source=OverlaySource;Probe.Method=EStudioProbeMethod::OriginalPoint;Probe.PointId=MAX_int64;
    Probe.A=FVector(100,100,100);Objects.Probes.Add(Probe);
    auto Build=[&](const FStudioProbeMarkerResult* Markers)
    {return StudioInspectionOverlay::Build(Objects,OverlaySource,Project,Probe.Id,Bounds,Markers);};
    TestTrue(TEXT("Unresolved ID never uses interpolated coordinates"),Build(nullptr).Markers.IsEmpty());
    FStudioProbeMarkerResult Result;Result.Project=Project;Result.Source=OverlaySource;Result.Queries.Add({Probe.Id,MAX_int64});
    Result.Positions.Add(Probe.Id,FVector(0,.25,.5));
    auto Geometry=Build(&Result);
    if(!TestEqual(TEXT("One resolved original marker"),Geometry.Markers.Num(),1))return false;
    TestTrue(TEXT("Picking and painting share resolved original coordinates"),Geometry.Markers[0].Position==FVector(0,.25,.5));
    Result.Project=FGuid::NewGuid();TestTrue(TEXT("Prior project marker excluded"),Build(&Result).Markers.IsEmpty());Result.Project=Project;
    Result.Source.PayloadSHA256=FString::ChrN(64,'c');TestTrue(TEXT("Changed source marker excluded"),Build(&Result).Markers.IsEmpty());Result.Source=OverlaySource;
    Result.Queries[0].PointId=17;TestTrue(TEXT("Stale ID excluded"),Build(&Result).Markers.IsEmpty());Result.Queries[0].PointId=MAX_int64;
    Result.bCancelled=true;TestTrue(TEXT("Cancelled marker excluded"),Build(&Result).Markers.IsEmpty());Result.bCancelled=false;
    Objects.Probes[0].bVisible=false;TestTrue(TEXT("Hidden original marker excluded"),Build(&Result).Markers.IsEmpty());Objects.Probes[0].bVisible=true;
    Objects.Probes[0].Source.Dataset=TEXT("another recording");TestTrue(TEXT("Foreign recording object excluded"),Build(&Result).Markers.IsEmpty());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioOverlayOverlap,"Studio.Inspection.AnnotationOverlapAndHiddenObjects",OverlayFlags)
bool FStudioOverlayOverlap::RunTest(const FString&)
{
    FStudioInspectionObjects Objects;FStudioRulerObject R;R.Source=OverlaySource;R.A=FVector(0,-.5,0);R.B=FVector(0,.5,0);Objects.Rulers.Add(R);
    auto Copy=R;Copy.Id=FGuid::NewGuid();Objects.Rulers.Add(Copy);
    FStudioProbeObject Hidden;Hidden.Source=OverlaySource;Hidden.A=FVector::ZeroVector;Hidden.bVisible=false;Objects.Probes.Add(Hidden);
    const FVector2D Size(800,600);const FGuid Project=FGuid::NewGuid();
    auto Geometry=StudioInspectionOverlay::Build(Objects,OverlaySource,Project,R.Id,FBox(FVector(-1),FVector(1)),nullptr);
    TestEqual(TEXT("Hidden marker absent from common geometry"),Geometry.Markers.Num(),4);
    TestTrue(TEXT("Exact overlap keeps current selection"),StudioInspectionOverlay::Pick(Geometry,OverlayCamera(),Size,Size*.5,R.Id)==R.Id);
    TestTrue(TEXT("Unselected overlap follows stable paint order"),StudioInspectionOverlay::Pick(Geometry,OverlayCamera(),Size,Size*.5,FGuid())==Copy.Id);
    Objects.Rulers[1].bVisible=false;Geometry=StudioInspectionOverlay::Build(Objects,OverlaySource,Project,Copy.Id,FBox(FVector(-1),FVector(1)),nullptr);
    TestTrue(TEXT("Hidden selected ruler cannot steal a hit"),StudioInspectionOverlay::Pick(Geometry,OverlayCamera(),Size,Size*.5,Copy.Id)==R.Id);
    return true;
}
#endif
