#include "StudioHome4Body.h"
#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStudioHome4ActualMass,"Studio.Home4.Authoring.ClosedTriangleMassPropertiesAndBoundGeometryIdentity",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FStudioHome4ActualMass::RunTest(const FString&)
{
    FStudioHome4AuthoringRequest R;R.Spec.Authoring.Primitive=TEXT("box");R.Spec.Authoring.PrimitiveSizeCells=FVector(2,3,4);R.Spec.Lattice.Extents=FIntVector(16);R.Spec.Geometry.InitialPositionCells=FVector(8);const auto P=StudioHome4Authoring::Build(R,MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false));TestTrue(*P.Error,P.IsValid());FStudioHome4GeometricMassProperties M;FString Error;TestTrue(*Error,StudioHome4Body::MassProperties(P,2,M,Error));TestTrue(TEXT("Geometric uniform body density integrates actual closed mesh volume and CoG"),FMath::IsNearlyEqual(M.Volume,24.,1e-12)&&FMath::IsNearlyEqual(M.Mass,48.,1e-12)&&M.CenterOfGravity.Equals(FVector(8),1e-12));
    TestTrue(TEXT("Full inertia tensor matches analytic cuboid"),M.InertiaDiagonal.Equals(FVector(100,80,52),1e-10)&&M.InertiaProducts.IsNearlyZero(1e-10));FStudioHome4Spec Adopted;TestTrue(*Error,StudioHome4Body::AdoptMassProperties(P,2,Adopted,Error));TestEqual(TEXT("Calculated inertia explicitly identifies source XYZ frame"),Adopted.Geometry.InertiaFrame,FString(TEXT("source-xyz")));TestTrue(TEXT("Calculated mass request retains exact geometric source"),Adopted.Geometry.MassPropertySource.Contains(P.RequestSHA256));
    FStudioGeometryAsset A;A.SourcePath=TEXT("actual.stl");A.SourceSHA256=FString::ChrN(64,'a');A.MetersPerSourceUnit=.001;TestTrue(*Error,StudioHome4Body::Bind(A,R.Spec,Adopted,Error));TestTrue(TEXT("Bound geometry request uses verified original identity and source units"),Adopted.Authoring.GeometryAssetId==A.Id.ToString()&&Adopted.Authoring.SourceSHA256==A.SourceSHA256&&Adopted.Geometry.SourcePath==A.SourcePath&&Adopted.Authoring.MetersPerSourceUnit==.001&&Adopted.Authoring.Primitive.IsEmpty());
    const auto Before=StudioHome4Config::Serialize(Adopted);A.SourceSHA256.Empty();TestFalse(TEXT("Unverified asset does not bind"),StudioHome4Body::Bind(A,R.Spec,Adopted,Error));TestEqual(TEXT("Failed binding is transactional"),StudioHome4Config::Serialize(Adopted),Before);
    R.Spec.Geometry.InertiaDiagonal=FVector(1,1,3);TestFalse(TEXT("A tensor violating physical principal-moment bounds is rejected"),StudioHome4Config::Validate(R.Spec,Error));R.Spec.Geometry.InertiaDiagonal=FVector(1);R.Spec.Geometry.InertiaProducts=FVector(2,0,0);TestFalse(TEXT("Indefinite inertia tensor is rejected"),StudioHome4Config::Validate(R.Spec,Error));
    return !HasAnyErrors();
}
#endif
