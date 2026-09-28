#include "StudioInspectionObjects.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
constexpr auto InspectionFlags=EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter;
// Geometry/serialization fixtures only. These contain no CFD values or claims
// about a published recording; scientific sampling uses real-source tests.
FStudioInspectionObjects Objects()
{
    FStudioInspectionObjects O;
    const FStudioInspectionSource Source{TEXT("geometry-only-fixture"),FString::ChrN(64,'a'),FString::ChrN(64,'b')};
    FStudioSliceObject S;S.Name=TEXT("Oblique section");S.Source=Source;
    S.Origin=FVector(.12345678901234567,-.00000000123456789,2.345678901234567);
    S.Normal=FVector(1,2,3).GetSafeNormal();S.Opacity=.41234567890123456;O.Slices.Add(S);
    S.Id=FGuid::NewGuid();S.Name=TEXT("Cross section");S.Normal=FVector::ForwardVector;S.bVisible=false;O.Slices.Add(S);
    FStudioProbeObject P;P.Name=TEXT("Point sample");P.Source=Source;P.A=S.Origin;P.Field=TEXT("pressure");O.Probes.Add(P);
    P.Id=FGuid::NewGuid();P.Name=TEXT("Line sample");P.Kind=EStudioProbeKind::Line;P.B=FVector(.4,.5,.6);P.Samples=37;O.Probes.Add(P);
    P.Id=FGuid::NewGuid();P.Name=TEXT("Original point");P.Kind=EStudioProbeKind::Point;P.Method=EStudioProbeMethod::OriginalPoint;
    P.PointId=MAX_int64;O.Probes.Add(P);
    FStudioRulerObject R;R.Name=TEXT("Span");R.Source=Source;R.A=S.Origin;R.B=FVector(2,3,4);R.Unit=TEXT("mm");O.Rulers.Add(R);
    R.Id=FGuid::NewGuid();R.Name=TEXT("Angle");R.Kind=EStudioRulerKind::Angle;R.C=FVector(6,3,7);O.Rulers.Add(R);
    return O;
}
TSharedPtr<FJsonObject> ThroughText(const FStudioInspectionObjects& O)
{
    FString Text;FJsonSerializer::Serialize(StudioInspectionObjects::ToJSON(O),TJsonWriterFactory<>::Create(&Text));
    TSharedPtr<FJsonObject> JSON;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),JSON);return JSON;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioInspectionRoundTrip,"Studio.Inspection.ExactObjectRoundTrip",InspectionFlags)
bool FStudioInspectionRoundTrip::RunTest(const FString&)
{
    auto Original=Objects();FString Error;FStudioInspectionObjects Loaded;
    if(!TestTrue(TEXT("Valid geometry-only collection"),StudioInspectionObjects::IsValid(Original,Error))||
        !TestTrue(*Error,StudioInspectionObjects::FromJSON(ThroughText(Original),Loaded,Error)))return false;
    TestTrue(TEXT("All stable IDs, source hashes, coordinates, normals, visibility and settings round trip exactly"),Loaded==Original);
    TestEqual(TEXT("Point ID above JSON's exact integer range retains all 64 bits"),Loaded.Probes[2].PointId.GetValue(),MAX_int64);
    Original.Probes[2].PointId=MIN_int64;
    TestTrue(TEXT("Negative original point ID round trips"),StudioInspectionObjects::FromJSON(ThroughText(Original),Loaded,Error));
    TestEqual(TEXT("Minimum signed point ID is exact"),Loaded.Probes[2].PointId.GetValue(),MIN_int64);
    auto Changed=Original;Changed.Slices[0].Source.MetadataSHA256=FString::ChrN(64,'c');
    TestFalse(TEXT("Same dataset name with changed metadata is a different source"),Changed.Slices[0].Source==Original.Slices[0].Source);
    Changed=Original;Changed.Slices[0].Source.PayloadSHA256=FString::ChrN(64,'c');
    TestFalse(TEXT("Legacy payload identity also matters"),Changed.Slices[0].Source==Original.Slices[0].Source);
    Changed=Original;Changed.Slices[0].Source.PayloadSHA256.Empty();
    TestTrue(TEXT("Point descriptors may pin arrays without a legacy payload hash"),StudioInspectionObjects::IsValid(Changed,Error));
    TestTrue(TEXT("Empty collections are valid"),StudioInspectionObjects::FromJSON(ThroughText({}),Loaded,Error)&&Loaded.Slices.IsEmpty()&&Loaded.Probes.IsEmpty()&&Loaded.Rulers.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioInspectionValidation,"Studio.Inspection.AtomicValidation",InspectionFlags)
bool FStudioInspectionValidation::RunTest(const FString&)
{
    const auto Original=Objects();FStudioInspectionObjects Out=Original;FString Error;
    auto Reject=[&](const TCHAR* Label,TFunctionRef<void(FStudioInspectionObjects&)> Edit)
    {
        auto Invalid=Original;Edit(Invalid);
        TestFalse(Label,StudioInspectionObjects::IsValid(Invalid,Error));
        TestFalse(TEXT("Corrupt collection also rejected at persistence boundary"),
            StudioInspectionObjects::FromJSON(StudioInspectionObjects::ToJSON(Invalid),Out,Error));
        TestTrue(TEXT("Rejected parse leaves every existing object intact"),Out==Original);
        TestFalse(TEXT("Rejected parse explains the error"),Error.IsEmpty());
    };
    Reject(TEXT("Cross-kind ID collision"),[](auto& O){O.Rulers[0].Id=O.Slices[0].Id;});
    Reject(TEXT("Invalid stable ID"),[](auto& O){O.Slices[0].Id.Invalidate();});
    Reject(TEXT("Case-insensitive name collision"),[](auto& O){O.Probes[0].Name=O.Slices[0].Name.ToUpper();});
    Reject(TEXT("Blank name"),[](auto& O){O.Slices[0].Name=TEXT(" ");});
    Reject(TEXT("Control character name"),[](auto& O){O.Slices[0].Name=TEXT("Bad\nname");});
    Reject(TEXT("Unpinned source"),[](auto& O){O.Slices[0].Source.MetadataSHA256.Empty();});
    Reject(TEXT("Nonhex source hash"),[](auto& O){O.Slices[0].Source.MetadataSHA256[0]='z';});
    Reject(TEXT("Zero plane normal"),[](auto& O){O.Slices[0].Normal=FVector::ZeroVector;});
    Reject(TEXT("Unnormalized plane normal"),[](auto& O){O.Slices[0].Normal*=2;});
    Reject(TEXT("Nonfinite coordinate"),[](auto& O){O.Slices[0].Origin.X=std::numeric_limits<double>::quiet_NaN();});
    Reject(TEXT("Opacity out of range"),[](auto& O){O.Slices[0].Opacity=1.1;});
    Reject(TEXT("Unknown probe kind"),[](auto& O){O.Probes[0].Kind=EStudioProbeKind(255);});
    Reject(TEXT("Unknown sample method"),[](auto& O){O.Probes[0].Method=EStudioProbeMethod(255);});
    Reject(TEXT("Unbounded line count"),[](auto& O){O.Probes[1].Samples=MAX_int32;});
    Reject(TEXT("Original point must identify its source row"),[](auto& O){O.Probes[2].PointId.Reset();});
    Reject(TEXT("Line is not an original point"),[](auto& O){O.Probes[2].Kind=EStudioProbeKind::Line;});
    Reject(TEXT("Interpolated query cannot retain a misleading original ID"),[](auto& O){O.Probes[0].PointId=3;});
    Reject(TEXT("Unknown ruler kind"),[](auto& O){O.Rulers[0].Kind=EStudioRulerKind(255);});
    Reject(TEXT("Unknown distance unit"),[](auto& O){O.Rulers[0].Unit=TEXT("pixels");});
    auto JSON=StudioInspectionObjects::ToJSON(Original);
    JSON->GetArrayField(TEXT("probes"))[2]->AsObject()->SetStringField(TEXT("pointId"),TEXT("9223372036854775808"));
    TestFalse(TEXT("Signed point-ID overflow rejected"),StudioInspectionObjects::FromJSON(JSON,Out,Error));
    JSON=StudioInspectionObjects::ToJSON(Original);
    JSON->GetArrayField(TEXT("probes"))[1]->AsObject()->SetNumberField(TEXT("samples"),3.5);
    TestFalse(TEXT("Fractional line count rejected"),StudioInspectionObjects::FromJSON(JSON,Out,Error));
    JSON=StudioInspectionObjects::ToJSON(Original);JSON->SetNumberField(TEXT("version"),StudioInspectionObjects::CurrentVersion+1);
    TestFalse(TEXT("Future object format rejected"),StudioInspectionObjects::FromJSON(JSON,Out,Error));
    JSON=StudioInspectionObjects::ToJSON(Original);JSON->RemoveField(TEXT("slices"));
    TestFalse(TEXT("Missing collection rejected"),StudioInspectionObjects::FromJSON(JSON,Out,Error));
    TestTrue(TEXT("Every malformed document preserves destination"),Out==Original);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioInspectionSlice,"Studio.Inspection.ArbitrarySliceGeometry",InspectionFlags)
bool FStudioInspectionSlice::RunTest(const FString&)
{
    FStudioSliceObject S;S.Normal=FVector::UpVector;
    const FBox Bounds(FVector(-1),FVector(1));
    auto Polygon=StudioInspectionObjects::SlicePolygon(S,Bounds);
    TestEqual(TEXT("Central axis slice is a quadrilateral"),Polygon.Num(),4);
    double Area=0;
    for(int32 I=0;I<Polygon.Num();++I)
    {
        TestEqual(TEXT("Every vertex lies exactly on the plane"),Polygon[I].Z,0.);
        TestTrue(TEXT("Every vertex stays in the physical domain"),Bounds.IsInsideOrOn(Polygon[I]));
        Area+=FVector::CrossProduct(Polygon[I],Polygon[(I+1)%Polygon.Num()]).Z*.5;
    }
    TestEqual(TEXT("Winding faces the normal and square area is four square meters"),Area,4.);
    S.Normal=FVector(1,1,1).GetSafeNormal();S.Origin=FVector(1./3);
    Polygon=StudioInspectionObjects::SlicePolygon(S,FBox(FVector::ZeroVector,FVector(1)));
    TestEqual(TEXT("Oblique cut through three cube corners deduplicates edge hits"),Polygon.Num(),3);
    for(const auto& Expected:{FVector(1,0,0),FVector(0,1,0),FVector(0,0,1)})
        TestTrue(TEXT("Oblique vertices match analytic intersections"),Polygon.ContainsByPredicate([&](const FVector& P){return P.Equals(Expected,1.e-12);}));
    S.Normal=FVector::UpVector;S.Origin=FVector(0,0,1);
    TestEqual(TEXT("Plane on a box face is retained"),StudioInspectionObjects::SlicePolygon(S,Bounds).Num(),4);
    S.Origin.Z=2;TestTrue(TEXT("Outside plane produces no polygon"),StudioInspectionObjects::SlicePolygon(S,Bounds).IsEmpty());
    S.Normal=FVector(1,1,1).GetSafeNormal();S.Origin=FVector(1);
    TestTrue(TEXT("Single-corner contact is not a slice"),StudioInspectionObjects::SlicePolygon(S,Bounds).IsEmpty());
    S.Normal=FVector(1,1,0).GetSafeNormal();
    TestTrue(TEXT("Single-edge contact is not a slice"),StudioInspectionObjects::SlicePolygon(S,Bounds).IsEmpty());
    S.Normal=FVector::RightVector;S.Origin=FVector(3,.125,5);
    TestTrue(TEXT("Physical plane offset is editable"),StudioInspectionObjects::MoveSlice(S,.75));
    TestEqual(TEXT("Offset preserves tangential position"),S.Origin,FVector(3,.75,5));
    double Minimum=123,Maximum=456;
    TestTrue(TEXT("Axis slider uses physical bounds"),StudioInspectionObjects::SliceRange(S.Normal,Bounds,Minimum,Maximum));
    TestEqual(TEXT("Minimum physical offset"),Minimum,-1.);TestEqual(TEXT("Maximum physical offset"),Maximum,1.);
    TestFalse(TEXT("Invalid normal is rejected"),StudioInspectionObjects::SliceRange(FVector::ZeroVector,Bounds,Minimum,Maximum));
    TestEqual(TEXT("Failed range query preserves output"),Minimum,-1.);
    S.Normal=FVector::UpVector;S.Origin=FVector(1000000,1000000,1000000);
    Polygon=StudioInspectionObjects::SlicePolygon(S,FBox(S.Origin-FVector(.001),S.Origin+FVector(.001)));
    TestEqual(TEXT("Small geometry far from the world origin remains sliceable"),Polygon.Num(),4);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioInspectionRay,"Studio.Inspection.ForwardPlanePicking",InspectionFlags)
bool FStudioInspectionRay::RunTest(const FString&)
{
    FStudioSliceObject S;S.Normal=FVector(1,2,3).GetSafeNormal();S.Origin=FVector(.1,.2,.3);
    FVector Hit(99);
    TestTrue(TEXT("Oblique forward ray hits the plane"),StudioInspectionObjects::IntersectSlice(S,S.Origin-S.Normal*5,S.Normal*2,Hit));
    TestTrue(TEXT("Ray need not be unit length"),Hit.Equals(S.Origin,1.e-12));
    const FVector Before=Hit;
    TestFalse(TEXT("Plane behind the camera cannot be picked"),StudioInspectionObjects::IntersectSlice(S,S.Origin-S.Normal,S.Normal*-1,Hit));
    TestFalse(TEXT("Parallel ray has no invented hit"),StudioInspectionObjects::IntersectSlice(S,S.Origin-S.Normal,FVector(2,-1,0),Hit));
    TestFalse(TEXT("Zero direction rejected"),StudioInspectionObjects::IntersectSlice(S,FVector::ZeroVector,FVector::ZeroVector,Hit));
    TestFalse(TEXT("Nonfinite ray rejected"),StudioInspectionObjects::IntersectSlice(S,FVector(std::numeric_limits<double>::infinity()),S.Normal,Hit));
    TestEqual(TEXT("All misses retain the previous output"),Hit,Before);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioInspectionMeasurements,"Studio.Inspection.MeasurementsAndLineLocations",InspectionFlags)
bool FStudioInspectionMeasurements::RunTest(const FString&)
{
    FStudioRulerObject R;R.A=FVector(1,2,3);R.B=FVector(4,6,3);
    TestEqual(TEXT("Known 3-4-5 distance in meters"),StudioInspectionObjects::Measurement(R).GetValue(),5.);
    R.Unit=TEXT("mm");TestEqual(TEXT("Distance in millimeters"),StudioInspectionObjects::Measurement(R).GetValue(),5000.);
    R.Unit=TEXT("in");R.B=R.A+FVector(.0254,0,0);
    TestTrue(TEXT("International inch is exactly 0.0254 meters"),FMath::IsNearlyEqual(StudioInspectionObjects::Measurement(R).GetValue(),1.,1.e-12));
    R.Kind=EStudioRulerKind::Angle;R.A=FVector(1,0,0);R.B=FVector::ZeroVector;R.C=FVector(0,1,0);
    TestEqual(TEXT("Angle uses B as its vertex, independently of distance units"),StudioInspectionObjects::Measurement(R).GetValue(),90.);
    R.C=FVector(-1,0,0);TestEqual(TEXT("Straight angle"),StudioInspectionObjects::Measurement(R).GetValue(),180.);
    R.C=R.B;TestFalse(TEXT("Zero-length angle arm is unavailable"),StudioInspectionObjects::Measurement(R).IsSet());
    FStudioProbeObject P;P.A=FVector(.12345678901234567,2,3);P.B=FVector(4,6,8);P.Kind=EStudioProbeKind::Line;P.Samples=3;
    auto Locations=StudioInspectionObjects::ProbeLocations(P);
    if(!TestEqual(TEXT("Line sample count includes both endpoints"),Locations.Num(),3))return false;
    TestEqual(TEXT("First endpoint exact"),Locations[0],P.A);TestEqual(TEXT("Last endpoint exact"),Locations[2],P.B);
    TestTrue(TEXT("Interior location is evenly spaced"),Locations[1].Equals((P.A+P.B)*.5,1.e-14));
    P.Samples=StudioInspectionObjects::MaxLineSamples;
    TestEqual(TEXT("Bounded maximum sample count"),StudioInspectionObjects::ProbeLocations(P).Num(),StudioInspectionObjects::MaxLineSamples);
    P.Samples=MAX_int32;TestTrue(TEXT("Invalid count cannot allocate unbounded samples"),StudioInspectionObjects::ProbeLocations(P).IsEmpty());
    P.Samples=3;P.Kind=EStudioProbeKind::Point;
    TestEqual(TEXT("Point probe has one position"),StudioInspectionObjects::ProbeLocations(P).Num(),1);
    P.Method=EStudioProbeMethod::OriginalPoint;P.PointId=9;
    TestTrue(TEXT("An original-point probe must resolve its ID in the actual source"),StudioInspectionObjects::ProbeLocations(P).IsEmpty());
    return true;
}
#endif
