#include "StudioModel.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
FString ClippingDocument(const FStudioProject& P,TFunction<void(TSharedPtr<FJsonObject>)> Change)
{
    TSharedPtr<FJsonObject> Object;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StudioProjectIO::Serialize(P)),Object);
    Change(Object);FString Text;FJsonSerializer::Serialize(Object,TJsonWriterFactory<>::Create(&Text));return Text;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioClippingProjection,"Studio.CameraClipping.ProjectionDepthAndFraming",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioClippingProjection::RunTest(const FString&)
{
    for(bool Ortho:{false,true})for(FIntPoint Size:{FIntPoint(1320,740),FIntPoint(720,1280)})
        for(double Scale:{.001,1.,1000.})
    {
        FStudioCameraState C;C.bDepthClipping=true;C.bOrthographic=Ortho;
        C.NearClipMeters=.2*Scale;C.FarClipMeters=7.3*Scale;C.FieldOfView=63.;C.OrthoWidth=2.6*Scale;
        FMatrix P;
        if(!TestTrue(TEXT("Valid camera projection builds"),StudioView::BuildClippedProjection(C,Size,P)))return false;
        const auto Depth=[&](double Meters)
        {const FVector4 V=P.TransformFVector4(FVector4(0,0,Meters*100.,1));return V.Z/V.W;};
        TestTrue(TEXT("Near plane maps to reversed depth one"),FMath::IsNearlyEqual(Depth(C.NearClipMeters),1.,1.e-10));
        TestTrue(TEXT("Far plane maps to reversed depth zero"),FMath::IsNearlyZero(Depth(C.FarClipMeters),1.e-10));
        TestTrue(TEXT("Before near lies outside clip interval"),Depth(C.NearClipMeters*.5)>1.);
        TestTrue(TEXT("After far lies outside clip interval"),Depth(C.FarClipMeters*1.5)<0.);
        const double D=(C.NearClipMeters+C.FarClipMeters)*.5;
        TestTrue(TEXT("Middle depth is visible"),Depth(D)>0&&Depth(D)<1.);
        const double HalfWidth=Ortho?C.OrthoWidth*.5:D*FMath::Tan(FMath::DegreesToRadians(C.FieldOfView*.5));
        const double HalfHeight=HalfWidth*Size.Y/Size.X;
        const auto Edge=P.TransformFVector4(FVector4(HalfWidth*100.,HalfHeight*100.,D*100.,1.));
        TestTrue(TEXT("Horizontal framing stays correct"),FMath::IsNearlyEqual(Edge.X/Edge.W,1.,1.e-10));
        TestTrue(TEXT("Viewport aspect preserves vertical framing"),FMath::IsNearlyEqual(Edge.Y/Edge.W,1.,1.e-10));
    }
    FStudioCameraState Bad;FMatrix Untouched=FMatrix::Identity;
    TestFalse(TEXT("Zero viewport rejected"),StudioView::BuildClippedProjection(Bad,FIntPoint(0,720),Untouched));
    Bad.NearClipMeters=Bad.FarClipMeters;
    TestFalse(TEXT("Coincident planes rejected"),StudioView::BuildClippedProjection(Bad,FIntPoint(1280,720),Untouched));
    TestTrue(TEXT("Failed projection leaves output intact"),Untouched.Equals(FMatrix::Identity));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioClippingHistory,"Studio.CameraClipping.HistoryBookmarksAndIsolation",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioClippingHistory::RunTest(const FString&)
{
    FStudioModel M(FPaths::ProjectSavedDir()/TEXT("Automation/Clipping")/FGuid::NewGuid().ToString());
    M.Run();M.Tick(.3);M.Scrub(.5);
    const auto Before=M.InspectionState();const FString Case=StudioCaseIO::Serialize(M.Project.Draft),Dataset=M.Project.Dataset;
    const int32 Frame=M.SelectedFrame,Playback=M.PlaybackFrame,Revision=M.Revision;
    auto C=Before.Camera;C.bDepthClipping=true;C.NearClipMeters=.000123456789;C.FarClipMeters=12.345678901234;
    TestTrue(TEXT("Edit camera clipping"),M.EditCamera(TEXT("Clip depth"),C));
    TestEqual(TEXT("Clipping does not rebuild CFD geometry"),M.Revision,Revision);
    TestEqual(TEXT("Clipping leaves selected frame"),M.SelectedFrame,Frame);
    TestEqual(TEXT("Clipping leaves replay cursor"),M.PlaybackFrame,Playback);
    TestEqual(TEXT("Clipping leaves source"),M.Project.Dataset,Dataset);
    TestEqual(TEXT("Clipping leaves computational case"),StudioCaseIO::Serialize(M.Project.Draft),Case);
    TestTrue(TEXT("Replay remains running"),M.State==EStudioRunState::Running);
    TestTrue(TEXT("Clipped camera can be saved"),M.AddCamera(TEXT("Wake depth"),C));
    if(!TestEqual(TEXT("One camera bookmark"),M.Project.Cameras.Num(),1))return false;
    const auto Id=M.Project.Cameras[0].Id;
    TestTrue(TEXT("Clipping undo available"),M.UndoView());
    TestTrue(TEXT("Undo restores automatic planes and exact view"),M.InspectionState().Equals(Before));
    TestTrue(TEXT("Clipping redo available"),M.RedoView());
    TestTrue(TEXT("Redo restores exact clipping"),StudioView::CameraEquals(M.Project.Camera,C));
    auto Other=C;Other.bDepthClipping=false;
    TestTrue(TEXT("Disable retains saved distances"),M.EditCamera(TEXT("Disable clipping"),Other));
    TestTrue(TEXT("Saved camera restores clipping"),M.RestoreSavedCamera(Id));
    TestEqual(TEXT("Near precision retained"),M.Project.Camera.NearClipMeters,C.NearClipMeters);
    TestEqual(TEXT("Far precision retained"),M.Project.Camera.FarClipMeters,C.FarClipMeters);
    const auto Saved=M.InspectionState();
    for(const auto Pair:{FVector2D(0,1),FVector2D(2,1),FVector2D(1,1+1.e-8),
                        FVector2D(.01,1.e9),FVector2D(std::numeric_limits<double>::quiet_NaN(),1)})
    {
        auto Invalid=C;Invalid.NearClipMeters=Pair.X;Invalid.FarClipMeters=Pair.Y;
        TestFalse(TEXT("Invalid range rejected without mutation"),M.EditCamera(TEXT("Invalid clipping"),Invalid));
        TestFalse(TEXT("Invalid saved-camera update rejected"),M.UpdateCamera(Id,Invalid));
        TestTrue(TEXT("Rejected clipping leaves inspection intact"),M.InspectionState().Equals(Saved));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioClippingPersistence,"Studio.CameraClipping.Version11AndMigration",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioClippingPersistence::RunTest(const FString&)
{
    FStudioProject P;P.Camera.bDepthClipping=true;P.Camera.NearClipMeters=.01234567890123;P.Camera.FarClipMeters=23.45678901234;
    FStudioCameraBookmark B;B.Name=TEXT("Saved clip");B.Camera=P.Camera;P.Cameras.Add(B);
    FStudioProject Read;FString Error;
    TestTrue(TEXT("Version 11 clipping document round trips"),StudioProjectIO::Parse(StudioProjectIO::Serialize(P),Read,Error));
    TestTrue(TEXT("Active clipping retained exactly"),StudioView::CameraEquals(P.Camera,Read.Camera));
    if(!TestEqual(TEXT("Saved clipping camera retained"),Read.Cameras.Num(),1))return false;
    TestTrue(TEXT("Saved clipping retained exactly"),StudioView::CameraEquals(P.Cameras[0].Camera,Read.Cameras[0].Camera));
    const FString Legacy=ClippingDocument(P,[](auto O)
    {
        O->SetNumberField(TEXT("version"),10);
        TArray<TSharedPtr<FJsonObject>> Cameras{O->GetObjectField(TEXT("camera"))};
        for(const auto& Item:O->GetArrayField(TEXT("cameras")))Cameras.Add(Item->AsObject()->GetObjectField(TEXT("camera")));
        for(const auto& C:Cameras)for(const TCHAR* Key:{TEXT("depthClipping"),TEXT("nearClipMeters"),TEXT("farClipMeters")})C->RemoveField(Key);
    });
    TestTrue(TEXT("Version 10 remains readable"),StudioProjectIO::Parse(Legacy,Read,Error));
    TestFalse(TEXT("Legacy active camera keeps automatic clipping"),Read.Camera.bDepthClipping);
    TestFalse(TEXT("Legacy saved camera keeps automatic clipping"),Read.Cameras[0].Camera.bDepthClipping);
    const FString Retained=StudioProjectIO::Serialize(Read);
    for(const TCHAR* Key:{TEXT("depthClipping"),TEXT("nearClipMeters"),TEXT("farClipMeters")})
    {
        const FString Missing=ClippingDocument(P,[Key](auto O){O->GetObjectField(TEXT("camera"))->RemoveField(Key);});
        TestFalse(TEXT("Version 11 requires every clipping property"),StudioProjectIO::Parse(Missing,Read,Error));
        TestEqual(TEXT("Failed parse preserves preceding document"),StudioProjectIO::Serialize(Read),Retained);
    }
    const FString BadBookmark=ClippingDocument(P,[](auto O)
    {O->GetArrayField(TEXT("cameras"))[0]->AsObject()->GetObjectField(TEXT("camera"))->SetNumberField(TEXT("farClipMeters"),0);});
    TestFalse(TEXT("Invalid bookmark clipping rejects entire replacement"),StudioProjectIO::Parse(BadBookmark,Read,Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioClippingFit,"Studio.CameraClipping.FitIncludesEntireDepth",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FStudioClippingFit::RunTest(const FString&)
{
    const FBox Bounds(FVector(-3,-1,.2),FVector(5,2,4));
    for(bool Ortho:{false,true})for(double Aspect:{.6,2.2})
    {
        FStudioCameraState C;C.bDepthClipping=true;C.NearClipMeters=.1;C.FarClipMeters=.2;
        C.bOrthographic=Ortho;C.Orientation=FRotator(39,73,21).Quaternion();
        const auto Fit=StudioView::FitBounds(C,Bounds,Aspect);
        TestEqual(TEXT("Fit retains near plane"),Fit.NearClipMeters,C.NearClipMeters);
        TestTrue(TEXT("Fit extends too-short far plane"),Fit.FarClipMeters>C.FarClipMeters);
        TestTrue(TEXT("Fit camera remains valid"),StudioView::IsValidClipping(Fit));
        for(int32 I=0;I<8;++I)
        {
            const FVector P(I&1?Bounds.Max.X:Bounds.Min.X,I&2?Bounds.Max.Y:Bounds.Min.Y,I&4?Bounds.Max.Z:Bounds.Min.Z);
            const double Depth=FVector::DotProduct(P-Fit.Position,Fit.Orientation.GetForwardVector());
            TestTrue(TEXT("Every corner between clipping planes"),Depth>Fit.NearClipMeters&&Depth<Fit.FarClipMeters);
        }
    }
    return true;
}
#endif
