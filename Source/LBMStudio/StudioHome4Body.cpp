#include "StudioHome4Body.h"
bool StudioHome4Body::Bind(const FStudioGeometryAsset& Asset,const FStudioHome4Spec& Base,FStudioHome4Spec& Out,FString& Error)
{
    if(Asset.SourcePath.IsEmpty()||Asset.SourceSHA256.Len()!=64||!FMath::IsFinite(Asset.MetersPerSourceUnit)||Asset.MetersPerSourceUnit<=0){Error=TEXT("Bind a verified source asset with its SHA256 and source-unit length.");return false;}
    FStudioHome4Spec S=Base;S.Authoring.GeometryAssetId=Asset.Id.ToString();S.Geometry.SourcePath=Asset.SourcePath;S.Authoring.SourceSHA256=Asset.SourceSHA256;S.Authoring.MetersPerSourceUnit=Asset.MetersPerSourceUnit;S.Authoring.Primitive.Empty();S.Authoring.PreparationMethod.Empty();
    if(!StudioHome4Config::Validate(S,Error))return false;Out=MoveTemp(S);return true;
}
bool StudioHome4Body::MassProperties(const FStudioHome4AuthoringPreview& P,double Density,FStudioHome4GeometricMassProperties& Out,FString& Error)
{
    if(!P.IsValid()||!P.bClosed||!P.Mesh||!FMath::IsFinite(Density)||Density<=0){Error=TEXT("Mass properties need verified closed triangle geometry and an explicitly supplied uniform body density.");return false;}
    // Integrate signed tetrahedra about a nearby origin to avoid cancellation
    // from remote source coordinates. Closed orientation is verified by Prepare.
    const FVector O=P.Body.GetCenter();double Volume=0;FVector First=FVector::ZeroVector,Second=FVector::ZeroVector,Products=FVector::ZeroVector;
    for(int32 I=0;I<P.Mesh->Indices.Num();I+=3)
    {
        const FVector A=P.Mesh->Positions[P.Mesh->Indices[I]]-O,B=P.Mesh->Positions[P.Mesh->Indices[I+1]]-O,C=P.Mesh->Positions[P.Mesh->Indices[I+2]]-O;
        const double V=FVector::DotProduct(A,FVector::CrossProduct(B,C))/6.;Volume+=V;First+=(A+B+C)*(V/4.);
        for(int32 Axis=0;Axis<3;++Axis)Second[Axis]+=V*(A[Axis]*A[Axis]+B[Axis]*B[Axis]+C[Axis]*C[Axis]+A[Axis]*B[Axis]+A[Axis]*C[Axis]+B[Axis]*C[Axis])/10.;
        const int32 X[]={0,0,1},Y[]={1,2,2};for(int32 K=0;K<3;++K)Products[K]+=V*((A[X[K]]+B[X[K]]+C[X[K]])*(A[Y[K]]+B[Y[K]]+C[Y[K]])+A[X[K]]*A[Y[K]]+B[X[K]]*B[Y[K]]+C[X[K]]*C[Y[K]])/20.;
    }
    if(Volume<0){Volume=-Volume;First=-First;Second=-Second;Products=-Products;}
    if(Volume<=1e-12||!FMath::IsFinite(Volume)){Error=TEXT("The prepared body has no finite positive volume.");return false;}
    const FVector Center=First/Volume;const FVector S=Second-Volume*Center*Center;const FVector Cross=Products-Volume*FVector(Center.X*Center.Y,Center.X*Center.Z,Center.Y*Center.Z);
    FStudioHome4GeometricMassProperties R;R.Volume=Volume;R.Mass=Density*Volume;R.CenterOfGravity=O+Center;R.InertiaDiagonal=Density*FVector(S.Y+S.Z,S.X+S.Z,S.X+S.Y);R.InertiaProducts=Cross*Density;R.Frame=TEXT("source-XYZ axes through geometric CoG; uniform declared density; full prepared triangle mesh");
    if(!FMath::IsFinite(R.Mass)||R.InertiaDiagonal.ContainsNaN()||R.InertiaDiagonal.GetMin()<=0||R.CenterOfGravity.ContainsNaN()){Error=TEXT("Geometric mass-property integration exceeded finite bounds.");return false;}Out=R;Error.Empty();return true;
}
bool StudioHome4Body::AdoptMassProperties(const FStudioHome4AuthoringPreview& P,double Density,FStudioHome4Spec& Out,FString& Error)
{
    FStudioHome4GeometricMassProperties M;if(!MassProperties(P,Density,M,Error))return false;FStudioHome4Spec S=P.Spec;if(P.Hydrostatics&&P.Hydrostatics->bEquilibrated&&!StudioHome4Authoring::AcceptEquilibrium(P,S,Error))return false;S.Geometry.BodyMass=M.Mass;S.Geometry.CenterOfGravity=M.CenterOfGravity;S.Geometry.InertiaDiagonal=M.InertiaDiagonal;S.Geometry.InertiaProducts=M.InertiaProducts;S.Geometry.InertiaFrame=TEXT("source-xyz");S.Geometry.MassPropertySource=M.Frame+TEXT(" · request ")+P.RequestSHA256+TEXT(" · original source ")+P.SourceSHA256;
    if(!StudioHome4Config::Validate(S,Error))return false;Out=MoveTemp(S);return true;
}
bool StudioHome4Body::AdoptHydrostaticStiffness(const FStudioHome4AuthoringPreview& P,FStudioHome4Spec& Out,FString& Error)
{
    if(!P.IsValid()||!P.Hydrostatics||!P.Hydrostatics->bConverged){Error=TEXT("Prepare a valid geometric heave/pitch stiffness response first.");return false;}FStudioHome4Spec S=P.Spec;if(P.Hydrostatics&&P.Hydrostatics->bEquilibrated&&!StudioHome4Authoring::AcceptEquilibrium(P,S,Error))return false;if(S.Geometry.Stiffness.IsEmpty())S.Geometry.Stiffness.Init(0,36);
    const auto& H=*P.Hydrostatics;S.Geometry.Stiffness[2*6+2]=H.K33;S.Geometry.Stiffness[2*6+4]=H.K35;S.Geometry.Stiffness[4*6+2]=H.K53;S.Geometry.Stiffness[4*6+4]=H.K55;
    if(!StudioHome4Config::Validate(S,Error))return false;Out=MoveTemp(S);return true;
}
