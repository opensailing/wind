#include "StudioHome4Authoring.h"
#include "StudioModel.h"
#include "Async/Async.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "Spatial/FastWinding.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace StudioHome4AuthoringLocal
{
using namespace UE::Geometry;
FQuat Rotation(const FVector& Degrees)
{
    return FQuat(FVector::UpVector,FMath::DegreesToRadians(Degrees.Z))*
        FQuat(FVector::YAxisVector,FMath::DegreesToRadians(Degrees.Y))*
        FQuat(FVector::XAxisVector,FMath::DegreesToRadians(Degrees.X));
}
TSharedPtr<FStudioImportedMesh,ESPMode::ThreadSafe> Primitive(const FString& Kind,const FVector& Size)
{
    auto M=MakeShared<FStudioImportedMesh,ESPMode::ThreadSafe>();
    auto V=[&](const FVector& P){return M->Positions.Add(P);};
    auto T=[&](int32 A,int32 B,int32 C){M->Indices.Append({A,B,C});};
    const FVector H=Size*.5;
    if(Kind==TEXT("box"))
    {
        for(int32 Z=0;Z<2;++Z)for(int32 Y=0;Y<2;++Y)for(int32 X=0;X<2;++X)V(FVector(X?H.X:-H.X,Y?H.Y:-H.Y,Z?H.Z:-H.Z));
        const int32 Faces[][4]={{0,2,3,1},{4,5,7,6},{0,1,5,4},{2,6,7,3},{0,4,6,2},{1,3,7,5}};
        for(const auto& F:Faces){T(F[0],F[1],F[2]);T(F[0],F[2],F[3]);}
    }
    else if(Kind==TEXT("cylinder"))
    {
        constexpr int32 N=96;
        for(int32 Z=0;Z<2;++Z)for(int32 I=0;I<N;++I){const double A=2*PI*I/N;V(FVector(H.X*FMath::Cos(A),H.Y*FMath::Sin(A),Z?H.Z:-H.Z));}
        const int32 Bottom=V(FVector(0,0,-H.Z)),Top=V(FVector(0,0,H.Z));
        for(int32 I=0;I<N;++I){const int32 J=(I+1)%N;T(I,J,J+N);T(I,J+N,I+N);T(Bottom,J,I);T(Top,I+N,J+N);}
    }
    else if(Kind==TEXT("sphere"))
    {
        constexpr int32 N=64,R=32;const int32 Top=V(FVector(0,0,H.Z)),Bottom=V(FVector(0,0,-H.Z));
        for(int32 J=1;J<R;++J)for(int32 I=0;I<N;++I){const double A=2*PI*I/N,B=PI*J/R;V(FVector(H.X*FMath::Sin(B)*FMath::Cos(A),H.Y*FMath::Sin(B)*FMath::Sin(A),H.Z*FMath::Cos(B)));}
        for(int32 I=0;I<N;++I){const int32 J=(I+1)%N;T(Top,2+I,2+J);T(Bottom,2+(R-2)*N+J,2+(R-2)*N+I);}
        for(int32 J=0;J<R-2;++J)for(int32 I=0;I<N;++I){const int32 A=2+J*N+I,B=2+J*N+(I+1)%N,C=B+N,D=A+N;T(A,D,C);T(A,C,B);}
    }
    else return nullptr;
    for(const FVector& P:M->Positions)M->Bounds+=P;
    return M;
}
bool ReadCAD(const FStudioHome4AuthoringRequest& R,const FStudioAssetCancellation& Cancel,FStudioMeshImportResult& Source,FString& Method)
{
    const auto& A=R.Spec.Authoring;FString Hash,Error;
    if(IFileManager::Get().FileSize(*R.Spec.Geometry.SourcePath)>64LL*1024*1024){Source.Error=TEXT("CAD source exceeds the bounded 64 MiB import contract.");return false;}
    if(!StudioAssets::HashFile(R.Spec.Geometry.SourcePath,Cancel,Hash,Error)){Source.Error=Error;return false;}
    if(!A.SourceSHA256.IsEmpty()&&A.SourceSHA256!=Hash){Source.Error=TEXT("Geometry source differs from the pinned SHA256; choose/re-pin the actual source explicitly.");return false;}
    FString Root=FPaths::ProjectContentDir()/TEXT("ThirdParty/Home4CAD");
    FString Python=A.TessellatorPython.IsEmpty()?Root/TEXT("bin/python"):A.TessellatorPython;
    FString Library=A.TessellatorLibrary.IsEmpty()?Root/TEXT("lib"):A.TessellatorLibrary;
    const FString Helper=Root/TEXT("home4_cad_prepare.py");
    if(!IFileManager::Get().FileExists(*Python)||!IFileManager::Get().FileExists(*Helper))
    {Source.Error=TEXT("The headless CAD runtime is missing. Package the verified FreeCAD dependency or select its explicit interpreter/library in Geometry.");return false;}
    if(!A.SurfaceTolerance){Source.Error=TEXT("Declare a positive CAD tessellation tolerance in original source units.");return false;}
    const FString Work=FPaths::ProjectSavedDir()/TEXT("Home4CAD")/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Work,true);
    const FString Output=Work/TEXT("prepared.stl");
    auto Quoted=[](FString S){S.ReplaceInline(TEXT("\\"),TEXT("\\\\"));S.ReplaceInline(TEXT("\""),TEXT("\\\""));return TEXT("\"")+S+TEXT("\"");};
    const FString Args=Quoted(Helper)+TEXT(" --input ")+Quoted(R.Spec.Geometry.SourcePath)+TEXT(" --output ")+Quoted(Output)+TEXT(" --library ")+Quoted(Library)+
        FString::Printf(TEXT(" --tolerance %.17g --expected-sha256 "),*A.SurfaceTolerance)+Hash;
    FProcHandle Process=FPlatformProcess::CreateProc(*Python,*Args,false,true,true,nullptr,0,nullptr,nullptr);
    if(!Process.IsValid()){Source.Error=TEXT("The configured CAD interpreter could not start.");IFileManager::Get().DeleteDirectory(*Work,false,true);return false;}
    const double Deadline=FPlatformTime::Seconds()+45;
    while(FPlatformProcess::IsProcRunning(Process))
    {
        if(Cancel->load()||FPlatformTime::Seconds()>Deadline){FPlatformProcess::TerminateProc(Process,true);Source.Error=Cancel->load()?TEXT("Geometry preparation cancelled."):TEXT("CAD tessellation exceeded its 45-second deadline.");break;}
        FPlatformProcess::Sleep(.02f);
    }
    int32 Code=1;FPlatformProcess::GetProcReturnCode(Process,&Code);FPlatformProcess::CloseProc(Process);
    if(Code==0&&Source.Error.IsEmpty())
    {
        FString JSON;TSharedPtr<FJsonObject> Manifest;FString Original,Prepared,Kernel;
        if(!FFileHelper::LoadFileToString(JSON,*(Output+TEXT(".json")))||JSON.Len()>65536||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JSON),Manifest)||
            !Manifest->TryGetStringField(TEXT("source_sha256"),Original)||Original!=Hash||!Manifest->TryGetStringField(TEXT("output_sha256"),Prepared)||!Manifest->TryGetStringField(TEXT("kernel_version"),Kernel))
            Source.Error=TEXT("CAD preparation returned invalid source/kernel provenance.");
        else
        {
            Source=StudioMeshImport::Read(Output,Cancel);
            if(Source.IsValid()&&Source.SHA256!=Prepared)Source.Error=TEXT("Prepared CAD mesh differs from its output SHA256.");
            if(Source.IsValid()){Source.SHA256=Hash;Source.Path=R.Spec.Geometry.SourcePath;Method=TEXT("FreeCAD/OpenCASCADE ")+Kernel+TEXT(" · source-pinned tessellation");}
        }
    }
    else if(Source.Error.IsEmpty())Source.Error=TEXT("CAD tessellation failed. Check the source shape, source-unit tolerance and explicit runtime paths.");
    IFileManager::Get().DeleteDirectory(*Work,false,true);return Source.IsValid();
}
void ZeroSurface(FStudioHome4AuthoringPreview& Out,const FStudioAssetCancellation& Cancel)
{
    TMap<FIntVector,int32> Lookup;for(int32 I=0;I<Out.Cells.Num();++I)Lookup.Add(Out.Cells[I].Index,I);
    const FIntVector Corners[]={{0,0,0},{1,0,0},{1,1,0},{0,1,0},{0,0,1},{1,0,1},{1,1,1},{0,1,1}};
    const int32 Tetrahedra[][4]={{0,5,1,6},{0,1,2,6},{0,2,3,6},{0,3,7,6},{0,7,4,6},{0,4,5,6}};
    auto Triangle=[&](const FVector& A,const FVector& B,const FVector& C)
    {
        if(FVector::CrossProduct(B-A,C-A).SizeSquared()<1e-20)return;
        const int32 Start=Out.SdfSurfacePositions.Num();Out.SdfSurfacePositions.Append({A,B,C});Out.SdfSurfaceIndices.Append({Start,Start+1,Start+2});
    };
    for(const auto& Cell:Out.Cells)
    {
        if(Cancel->load())return;
        const FStudioHome4PreviewCell* C[8];bool Complete=true;
        for(int32 I=0;I<8;++I){const int32* Index=Lookup.Find(Cell.Index+Corners[I]*Out.Stride);if(!Index){Complete=false;break;}C[I]=&Out.Cells[*Index];}
        if(!Complete)continue;
        for(const auto& T:Tetrahedra)
        {
            TArray<int32,TInlineAllocator<4>> Inside,Outside;
            for(int32 I:T)(C[I]->SignedDistance<=0?Inside:Outside).Add(I);
            if(Inside.IsEmpty()||Outside.IsEmpty())continue;
            auto Cross=[&](int32 A,int32 B){const double DA=C[A]->SignedDistance,DB=C[B]->SignedDistance;return FMath::Lerp(FVector(C[A]->Index),FVector(C[B]->Index),DA/(DA-DB));};
            if(Inside.Num()==1)Triangle(Cross(Inside[0],Outside[0]),Cross(Inside[0],Outside[1]),Cross(Inside[0],Outside[2]));
            else if(Inside.Num()==3)Triangle(Cross(Outside[0],Inside[0]),Cross(Outside[0],Inside[2]),Cross(Outside[0],Inside[1]));
            else{const FVector A=Cross(Inside[0],Outside[0]),B=Cross(Inside[0],Outside[1]),C0=Cross(Inside[1],Outside[0]),D=Cross(Inside[1],Outside[1]);Triangle(A,B,D);Triangle(A,D,C0);}
        }
    }
}

struct FVolume {double V=0;FVector Moment=FVector::ZeroVector;};
FVolume Volume(const FStudioImportedMesh& M,double Waterline,const FTransform& Transform,bool Clip,const FStudioAssetCancellation& Cancel)
{
    FVolume Out;const FVector O(0,0,Waterline);
    for(int32 I=0;I<M.Indices.Num();I+=3)
    {
        if((I&255)==0&&Cancel->load())return {};
        TArray<FVector,TInlineAllocator<5>> Polygon;
        for(int32 J=0;J<3;++J)Polygon.Add(Transform.TransformPosition(M.Positions[M.Indices[I+J]]));
        if(Clip)
        {
            TArray<FVector,TInlineAllocator<5>> Clipped;
            for(int32 J=0;J<Polygon.Num();++J)
            {
                const FVector A=Polygon[J],B=Polygon[(J+1)%Polygon.Num()];const bool IA=A.Z<=Waterline,IB=B.Z<=Waterline;
                if(IA)Clipped.Add(A);
                if(IA!=IB){const double T=(Waterline-A.Z)/(B.Z-A.Z);Clipped.Add(A+(B-A)*T);}
            }
            Polygon=MoveTemp(Clipped);
        }
        for(int32 J=1;J+1<Polygon.Num();++J)
        {
            const FVector A=Polygon[0],B=Polygon[J],C=Polygon[J+1];const double V=FVector::DotProduct(A-O,FVector::CrossProduct(B-O,C-O))/6.;
            Out.V+=V;Out.Moment+=(O+A+B+C)*(V*.25);
        }
    }
    if(Out.V<0){Out.V=-Out.V;Out.Moment=-Out.Moment;}return Out;
}
}

FString StudioHome4Authoring::Fingerprint(const FStudioHome4Spec& Spec)
{
    const FTCHARToUTF8 Bytes(*StudioHome4Config::Serialize(Spec));uint8 Hash[32];SHA256(reinterpret_cast<const unsigned char*>(Bytes.Get()),Bytes.Length(),Hash);return BytesToHex(Hash,32).ToLower();
}
FTransform StudioHome4Authoring::Motion(const FStudioHome4Spec& S,double Step)
{
    using namespace StudioHome4AuthoringLocal;const auto& G=S.Geometry;
    FVector Translation=FVector::ZeroVector,Angles=FVector::ZeroVector;
    const double Phase=2*PI*G.MotionFrequencyCyclesPerStep.Get(0)*Step+FMath::DegreesToRadians(G.MotionPhaseDegrees.Get(0));
    if(G.BodyMotion==TEXT("forced-heave"))Translation.Z=G.HeaveAmplitudeCells.Get(0)*FMath::Sin(Phase);
    if(G.BodyMotion==TEXT("forced-roll"))Angles.X=G.RollAmplitudeDegrees.Get(0)*FMath::Sin(Phase);
    if(G.BodyMotion==TEXT("forced-pitch"))Angles.Y=G.PitchAmplitudeDegrees.Get(0)*FMath::Sin(Phase);
    if(G.BodyMotion==TEXT("forced-spin"))
    {double Integrated=Step;if(G.SpinRampSteps&&*G.SpinRampSteps>0){const double T=*G.SpinRampSteps,N=FMath::Max(0.,Step);Integrated=N<T?N*N*N/(T*T)-.5*N*N*N*N/(T*T*T):N-.5*T;}Angles.Z=FMath::RadiansToDegrees(G.SpinRadiansPerStep.Get(0)*Integrated);}
    const FVector Pivot=G.CenterOfGravity.Get(G.InitialPositionCells.Get(FVector::ZeroVector));FQuat Q=Rotation(Angles);
    if(G.BodyMotion==TEXT("free"))
    {Translation=G.InitialVelocityCellsPerStep.Get(FVector::ZeroVector)*Step;const FVector W=G.InitialAngularVelocityRadiansPerStep.Get(FVector::ZeroVector);if(!W.IsNearlyZero())Q=FQuat(W.GetSafeNormal(),W.Size()*Step);}

    return FTransform(Q,Pivot-Q.RotateVector(Pivot)+Translation);
}
bool StudioHome4Authoring::AcceptEquilibrium(const FStudioHome4AuthoringPreview& Preview,FStudioHome4Spec& Out,FString& Error)
{
    using namespace StudioHome4AuthoringLocal;
    if(!Preview.IsValid()||!Preview.Hydrostatics||!Preview.Hydrostatics->bConverged||!Preview.Hydrostatics->bEquilibrated||
        !Preview.Spec.Geometry.CenterOfGravity||Fingerprint(Preview.Spec)!=Preview.RequestSHA256)
    {Error=TEXT("Accept only a current converged geometric flotation response with its declared CoG.");return false;}
    FStudioHome4Spec S=Preview.Spec;const auto& H=*Preview.Hydrostatics;
    const FVector Pivot=*S.Geometry.CenterOfGravity;
    const FQuat Delta=Rotation(FVector(0,H.TrimDegrees,0));
    const FQuat Base=Rotation(S.Geometry.InitialAttitudeDegrees.Get(FVector::ZeroVector)+FVector(S.Geometry.HeelDegrees.Get(0),S.Geometry.TrimDegrees.Get(0),S.Geometry.YawDegrees.Get(0)));
    const FQuat Q=(Delta*Base).GetNormalized();
    const FVector X=Q.RotateVector(FVector::XAxisVector),Y=Q.RotateVector(FVector::YAxisVector),Z=Q.RotateVector(FVector::ZAxisVector);
    const double Pitch=FMath::Asin(FMath::Clamp(-X.Z,-1.,1.));
    const bool Regular=FMath::Abs(FMath::Cos(Pitch))>1e-10;
    const FVector Radians(Regular?FMath::Atan2(Y.Z,Z.Z):0,Pitch,Regular?FMath::Atan2(X.Y,X.X):FMath::Atan2(-Y.X,Y.Y));
    FVector Translation=S.Geometry.InitialPositionCells.Get(FVector::ZeroVector);Translation.Z-=S.Geometry.SinkCells.Get(0);
    S.Geometry.InitialPositionCells=Pivot-Delta.RotateVector(Pivot)+Delta.RotateVector(Translation)+FVector(0,0,H.Heave);
    S.Geometry.InitialAttitudeDegrees=FVector(FMath::RadiansToDegrees(Radians.X),FMath::RadiansToDegrees(Radians.Y),FMath::RadiansToDegrees(Radians.Z));S.Geometry.HeelDegrees=0;S.Geometry.TrimDegrees=0;S.Geometry.YawDegrees=0;S.Geometry.SinkCells=0;
    S.Geometry.CenterOfGravity=Pivot+FVector(0,0,H.Heave);S.Geometry.Float=false;S.Geometry.NoEquilibrate=true;
    if(!StudioHome4Config::Validate(S,Error))return false;
    Out=MoveTemp(S);Error.Empty();return true;
}
double StudioHome4Authoring::ZoneWeight(const FStudioHome4PreviewRegion& R,const FVector& Position,int32 Level)
{
    if(!R.Bounds.IsInsideOrOn(Position))return 0;
    const int32 Axis=R.Axis==TEXT("y")?1:R.Axis==TEXT("z")?2:0;
    const double Width=R.Bounds.Max[Axis]-R.Bounds.Min[Axis];if(Width<=0)return 0;
    const double T=FMath::Clamp((Position[Axis]-R.Bounds.Min[Axis])/Width,0.,1.);
    const double X=R.Profile.EndsWith(TEXT("-reverse"))?1-T:T;
    const double Profile=R.Profile==TEXT("constant")?1:(R.Profile==TEXT("linear")||R.Profile==TEXT("linear-reverse"))?X:X*X*X;
    return FMath::Clamp(R.Strength*Profile*FMath::Pow(2.,-R.LevelExponent*Level),0.,1.);
}
bool StudioHome4Authoring::Hydrostatics(const FStudioImportedMesh& M,double Waterline,double RH,double RL,double Mass,const FVector& CoG,double Gravity,bool Equilibrate,FStudioHome4Hydrostatics& Out,FString& Error,const FStudioAssetCancellation& Cancel)
{
    using namespace StudioHome4AuthoringLocal;
    if(!FMath::IsFinite(Waterline)||RH<=RL||RL<0||Mass<=0||Gravity<=0||M.Indices.IsEmpty())
    {Error=TEXT("Hydrostatic preparation requires a closed body, ρH>ρL≥0, positive mass/gravity and a declared waterline.");return false;}
    FStudioHome4Hydrostatics Result;const auto Total=Volume(M,Waterline,FTransform::Identity,false,Cancel);Result.TotalVolume=Total.V;
    if(Total.V<=1e-12){Error=TEXT("The closed triangle mesh has no positive bounded volume.");return false;}
    if(Equilibrate&&(Mass<=RL*Total.V||Mass>=RH*Total.V))
    {Error=TEXT("This body mass has no partially submerged equilibrium between the declared phase densities.");return false;}
    const double Extent=FMath::Max(1.,M.Bounds.GetSize().GetMax()),DH=FMath::Max(1e-5,Extent*1e-5),DT=1e-5;
    auto Evaluate=[&](double H,double Trim,FStudioHome4Hydrostatics* Output=nullptr)
    {
        const FQuat Q=Rotation(FVector(0,FMath::RadiansToDegrees(Trim),0));const FVector Translation=CoG-Q.RotateVector(CoG)+FVector(0,0,H);
        const auto Sub=Volume(M,Waterline,FTransform(Q,Translation),true,Cancel);
        const FVector TotalMoment=Q.RotateVector(Total.Moment)+(Translation*Total.V);
        const FVector DisplacementMoment=Sub.Moment*(RH-RL)+TotalMoment*RL;
        const double Displaced=RL*Total.V+(RH-RL)*Sub.V;
        const FVector NewCoG=CoG+FVector(0,0,H);const double Fz=(Displaced-Mass)*Gravity;
        const double Pitch=Displaced>0?-Gravity*(DisplacementMoment.X-NewCoG.X*Displaced):0;
        if(Output){Output->SubmergedVolume=Sub.V;Output->SubmergedCentroid=Sub.V>1e-12?Sub.Moment/Sub.V:NewCoG;Output->DisplacedMass=Displaced;Output->CenterOfGravity=NewCoG;Output->VerticalResidual=Fz;Output->PitchMomentResidual=Pitch;}
        return FVector2D(Fz,Pitch);
    };
    double H=0,Trim=0;bool Converged=!Equilibrate;
    for(int32 I=0;Equilibrate&&I<40;++I)
    {
        if(Cancel->load()){Error=TEXT("Hydrostatic preparation cancelled.");return false;}
        const FVector2D V=Evaluate(H,Trim);Result.Iterations=I+1;
        if(FMath::Abs(V.X)<Mass*Gravity*1e-6&&FMath::Abs(V.Y)<Mass*Gravity*Extent*1e-6){Converged=true;break;}
        const auto DX=(Evaluate(H+DH,Trim)-Evaluate(H-DH,Trim))/(2*DH),DY=(Evaluate(H,Trim+DT)-Evaluate(H,Trim-DT))/(2*DT);
        const double Det=DX.X*DY.Y-DY.X*DX.Y;
        if(FMath::Abs(Det)<1e-16){Error=TEXT("Hydrostatic heave/trim Jacobian is singular; adjust the initial waterline/attitude.");return false;}
        const double StepH=(-V.X*DY.Y+DY.X*V.Y)/Det,StepT=(-DX.X*V.Y+V.X*DX.Y)/Det;
        H+=FMath::Clamp(StepH,-Extent*.2,Extent*.2);Trim+=FMath::Clamp(StepT,-.1,.1);
    }
    Evaluate(H,Trim,&Result);const auto DX=(Evaluate(H+DH,Trim)-Evaluate(H-DH,Trim))/(2*DH),DY=(Evaluate(H,Trim+DT)-Evaluate(H,Trim-DT))/(2*DT);
    Result.bEquilibrated=Equilibrate;Result.Heave=H;Result.TrimDegrees=FMath::RadiansToDegrees(Trim);Result.K33=-DX.X;Result.K53=-DX.Y;Result.K35=-DY.X;Result.K55=-DY.Y;Result.bConverged=Converged;
    if(!Converged){Error=TEXT("Geometric heave/trim equilibrium did not converge in 40 iterations; no equilibrium was applied.");return false;}
    Out=Result;Error.Empty();return true;
}

FStudioHome4AuthoringPreview StudioHome4Authoring::Build(const FStudioHome4AuthoringRequest& R,const FStudioAssetCancellation& Cancel)
{
    using namespace StudioHome4AuthoringLocal;
    FStudioHome4AuthoringPreview Out;Out.ProjectId=R.ProjectId;Out.CaseId=R.CaseId;Out.Spec=R.Spec;Out.RequestSHA256=Fingerprint(R.Spec);
    const auto& S=R.Spec;const auto& A=S.Authoring;
    if(!StudioHome4Config::Validate(S,Out.Error))return Out;
    if(R.SampleBudget<1||R.SampleBudget>32768){Out.Error=TEXT("Preview sample budget must be 1–32768 original nodes.");return Out;}
    if(!S.Lattice.Extents){Out.Error=TEXT("Declare the actual tank lattice extents before preparing a grid preview.");return Out;}
    Out.Tank=FBox(FVector::ZeroVector,FVector(*S.Lattice.Extents));
    auto BuildRegions=[&]()
    {
    double ZoneScale=1;
    if(!A.Zones.IsEmpty())
    {
        if(A.ZoneUnits==TEXT("body-lengths")&&S.Reference.LengthCells)ZoneScale=*S.Reference.LengthCells;
        else if(A.ZoneUnits==TEXT("physical-metres")&&S.Units.DxMeters)ZoneScale=1/ *S.Units.DxMeters;
        else if(A.ZoneUnits!=TEXT("root-cells")){Out.Error=TEXT("Declared zones need their coordinate unit map before preview.");return false;}
    }
    for(const auto& Z:A.Zones){FStudioHome4PreviewRegion V;V.Id=Z.Id;V.Kind=Z.Kind;V.Profile=Z.Profile;V.Axis=Z.Axis;V.Bounds=FBox(Z.Minimum*ZoneScale,Z.Maximum*ZoneScale);V.Strength=Z.Strength;V.LevelExponent=Z.LevelExponent;Out.Regions.Add(V);}
    for(const auto& P:A.Patches)
    {
        FStudioHome4PreviewRegion V;V.Id=P.Id;V.Kind=TEXT("multidomain");V.Level=P.Level;V.bFollowBody=P.bFollowBody;
        const double Spacing=FMath::Pow(2.,-P.Level);V.Bounds=FBox(P.Origin,P.Origin+FVector(P.Extents)*Spacing);Out.Regions.Add(V);
    }
        return true;
    };
    if(A.Primitive.IsEmpty()&&S.Geometry.SourcePath.IsEmpty())
    {
        Out.Method=TEXT("Declared tank, boundary, wave and region requests; no body geometry supplied");
        if(S.Geometry.Float.Get(false)){Out.Error=TEXT("Flotation requires an actual closed body geometry.");return Out;}
        if(!BuildRegions())return Out;
        return Out;
    }
    TSharedPtr<FStudioImportedMesh,ESPMode::ThreadSafe> Mesh;
    if(!A.Primitive.IsEmpty())
    {
        if(!A.PrimitiveSizeCells){Out.Error=TEXT("Declare primitive XYZ dimensions in root cells.");return Out;}
        if((A.Primitive==TEXT("cylinder")&&!FMath::IsNearlyEqual(A.PrimitiveSizeCells->X,A.PrimitiveSizeCells->Y))||
            (A.Primitive==TEXT("sphere")&&(!FMath::IsNearlyEqual(A.PrimitiveSizeCells->X,A.PrimitiveSizeCells->Y)||!FMath::IsNearlyEqual(A.PrimitiveSizeCells->X,A.PrimitiveSizeCells->Z))))
        {Out.Error=TEXT("Cylinder X/Y diameters must agree; sphere X/Y/Z diameters must agree.");return Out;}
        Mesh=Primitive(A.Primitive,*A.PrimitiveSizeCells);Out.Method=TEXT("Declared ")+A.Primitive+TEXT(" · triangle geometry");Out.SourceSHA256.Empty();
    }
    else
    {
        if(S.Geometry.SourcePath.IsEmpty()||!A.MetersPerSourceUnit||!S.Units.DxMeters)
        {Out.Error=TEXT("Choose a mesh/CAD source and explicitly declare metres per source unit and metres per root cell.");return Out;}
        Out.SourcePath=S.Geometry.SourcePath;FStudioMeshImportResult Source;
        const FString Extension=FPaths::GetExtension(Out.SourcePath).ToLower();
        if(Extension==TEXT("step")||Extension==TEXT("stp")||Extension==TEXT("iges")||Extension==TEXT("igs"))ReadCAD(R,Cancel,Source,Out.Method);
        else{Source=StudioMeshImport::Read(Out.SourcePath,Cancel);Out.Method=TEXT("Original ")+Extension.ToUpper()+TEXT(" triangle mesh");}
        if(!Source.IsValid()){Out.Error=Source.Error;Out.bCancelled=Source.bCancelled||Cancel->load();return Out;}
        if(!A.SourceSHA256.IsEmpty()&&A.SourceSHA256.ToLower()!=Source.SHA256.ToLower())
        {Out.Error=TEXT("Original geometry SHA256 differs from the pinned request; reselect and repin the source.");return Out;}
        Out.SourceSHA256=Source.SHA256;Mesh=MakeShared<FStudioImportedMesh,ESPMode::ThreadSafe>(*Source.Mesh);
        const double Scale=*A.MetersPerSourceUnit/ *S.Units.DxMeters;const FQuat Q=StudioMeshImport::AxisRotation(A.SourceUpAxis,A.SourceForwardAxis);
        for(auto& P:Mesh->Positions)P=Q.RotateVector(P)*Scale;
    }
    if(!Mesh){Out.Error=TEXT("Unsupported declared primitive.");return Out;}
    const FQuat Pose=Rotation(S.Geometry.InitialAttitudeDegrees.Get(FVector::ZeroVector)+FVector(S.Geometry.HeelDegrees.Get(0),S.Geometry.TrimDegrees.Get(0),S.Geometry.YawDegrees.Get(0)));
    FVector Translation=S.Geometry.InitialPositionCells.Get(FVector::ZeroVector);Translation.Z-=S.Geometry.SinkCells.Get(0);
    // Attitude acts about the explicitly chosen source/body origin. No hidden centering or axis swap.
    Mesh->Bounds=FBox(ForceInit);for(auto& P:Mesh->Positions){P=Pose.RotateVector(P)+Translation;Mesh->Bounds+=P;}
    FDynamicMesh3 Dynamic;TMap<FVector,int32> Welded;TArray<int32> Remap;Remap.Reserve(Mesh->Positions.Num());
    for(const FVector& P:Mesh->Positions){if(const int32* Id=Welded.Find(P))Remap.Add(*Id);else{const int32 AddedId=Dynamic.AppendVertex(FVector3d(P));Welded.Add(P,AddedId);Remap.Add(AddedId);}}
    for(int32 I=0;I<Mesh->Indices.Num();I+=3)
    {
        if((I&255)==0&&Cancel->load()){Out.bCancelled=true;return Out;}
        if(Dynamic.AppendTriangle(Remap[Mesh->Indices[I]],Remap[Mesh->Indices[I+1]],Remap[Mesh->Indices[I+2]])<0)
        {Out.Error=TEXT("The mesh contains duplicate, degenerate or nonmanifold triangles; repair topology before building signed distance.");return Out;}
    }
    Out.bClosed=Dynamic.IsClosed()&&Mesh->InconsistentEdges==0&&Mesh->DuplicateFaces==0;
    if(!Out.bClosed){Out.Error=TEXT("Signed distance, voxel classification and flotation require a consistently oriented closed triangle mesh.");return Out;}
    const auto D=StudioHome4Config::Derive(S);
    if(A.WaterlineCells&&D.RhoHeavy&&D.RhoLight&&S.Geometry.BodyMass&&D.Gravity&&S.Geometry.CenterOfGravity)
    {
        FStudioHome4Hydrostatics H;const bool Float=S.Geometry.Float.Get(false)&&!S.Geometry.NoEquilibrate.Get(false);
        if(Hydrostatics(*Mesh,*A.WaterlineCells,*D.RhoHeavy,*D.RhoLight,*S.Geometry.BodyMass,*S.Geometry.CenterOfGravity,*D.Gravity,Float,H,Out.HydrostaticError,Cancel))
        {
            Out.Hydrostatics=H;
            if(Float)
            {
                const FQuat Q=Rotation(FVector(0,H.TrimDegrees,0));const FVector Pivot=*S.Geometry.CenterOfGravity;
                Mesh->Bounds=FBox(ForceInit);
                for(auto& P:Mesh->Positions){P=Pivot+Q.RotateVector(P-Pivot)+FVector(0,0,H.Heave);Mesh->Bounds+=P;}
                for(const auto& Pair:Welded){const FVector P=Pivot+Q.RotateVector(Pair.Key-Pivot)+FVector(0,0,H.Heave);Dynamic.SetVertex(Pair.Value,FVector3d(P));}
            }
        }
    }
    else if(S.Geometry.Float.Get(false))Out.HydrostaticError=TEXT("Float requires the waterline, mass, CoG, phase densities and gravity; the fixed geometric preview remains available.");
    const double GeometricStart=FPlatformTime::Seconds();
    Out.Mesh=Mesh;Out.Body=Mesh->Bounds;FDynamicMeshAABBTree3 Tree(&Dynamic,true);TFastWindingTree<FDynamicMesh3> Winding(&Tree,true);
    const double Band=S.Geometry.BandCells.Get(4);const FBox Region=Out.Body.ExpandBy(Band).Overlap(Out.Tank);
    if(!Region.IsValid){Out.Error=TEXT("The posed body/SDF band does not intersect the declared tank.");Out.Mesh.Reset();return Out;}
    FIntVector Minimum,Maximum;
    for(int32 Axis=0;Axis<3;++Axis){Minimum[Axis]=FMath::Clamp(FMath::FloorToInt(Region.Min[Axis]),0,(*S.Lattice.Extents)[Axis]-1);Maximum[Axis]=FMath::Clamp(FMath::CeilToInt(Region.Max[Axis]),0,(*S.Lattice.Extents)[Axis]-1);}
    const FIntVector Counts=Maximum-Minimum+FIntVector(1);Out.RequestedCells=int64(Counts.X)*Counts.Y*Counts.Z;
    int32 Stride=1;auto Samples=[&](int32 Str){return int64((Counts.X+Str-1)/Str)*((Counts.Y+Str-1)/Str)*((Counts.Z+Str-1)/Str);};
    while(Samples(Stride)>R.SampleBudget)++Stride;
    Out.Stride=Stride;
    for(int32 Z=Minimum.Z;Z<=Maximum.Z;Z+=Stride)for(int32 Y=Minimum.Y;Y<=Maximum.Y;Y+=Stride)for(int32 X=Minimum.X;X<=Maximum.X;X+=Stride)
    {
        if(Cancel->load()){Out.bCancelled=true;Out.Cells.Reset();Out.Links.Reset();return Out;}
        const FIntVector Index(X,Y,Z);const FVector P(Index);double Dist2=0;
        if(Tree.FindNearestTriangle(FVector3d(P),Dist2)<0){Out.Error=TEXT("Signed-distance nearest-triangle query failed.");return Out;}
        const bool Inside=FMath::Abs(Winding.FastWindingNumber(FVector3d(P)))>.5;const double Distance=FMath::Sqrt(FMath::Max(0.,Dist2));
        Out.Cells.Add({Index,Inside?-Distance:Distance,Inside});
        if(Inside||Distance>FMath::Sqrt(3.)+1e-9)continue;
        for(int32 DZ=-1;DZ<=1;++DZ)for(int32 DY=-1;DY<=1;++DY)for(int32 DX=-1;DX<=1;++DX)
        {
            if(DX==0&&DY==0&&DZ==0)continue;
            const FIntVector Direction(DX,DY,DZ),Neighbor=Index+Direction;
            bool InTank=true;for(int32 Axis=0;Axis<3;++Axis)InTank&=Neighbor[Axis]>=0&&Neighbor[Axis]<(*S.Lattice.Extents)[Axis];if(!InTank)continue;
            const FVector Delta(Direction);const double Length=Delta.Size();double T=0;int32 Triangle=INDEX_NONE;
            if(Tree.FindNearestHitTriangle(FRay3d(FVector3d(P),FVector3d(Delta/Length)),T,Triangle)&&T>1e-9&&T<=Length)
                Out.Links.Add({Index,Direction,P+Delta*(T/Length),T/Length});
        }
    }
    ZeroSurface(Out,Cancel);
    if(Cancel->load()){Out.bCancelled=true;Out.Cells.Reset();Out.Links.Reset();Out.SdfSurfacePositions.Reset();Out.SdfSurfaceIndices.Reset();return Out;}
    if(!BuildRegions())return Out;
    Out.GeometricPreparationSeconds=FPlatformTime::Seconds()-GeometricStart;
    return Out;
}
FStudioHome4AuthoringSession::~FStudioHome4AuthoringSession(){Cancel();}
void FStudioHome4AuthoringSession::Cancel(){Cancellation->store(true);}
bool FStudioHome4AuthoringSession::Request()
{
    if(Pending.IsValid()){Status=TEXT("A geometric preparation is already running; cancel it before starting another request.");return false;}
    const auto M=Session?Session->Owner():nullptr;FStudioHome4Spec Spec;FString Error;
    if(!M||!Session->Build(Spec,Error)){Status=Error;return false;}
    if(!Spec.Authoring.GeometryAssetId.IsEmpty())
    {FGuid Id;if(!FGuid::Parse(Spec.Authoring.GeometryAssetId,Id)){Status=TEXT("Body geometry asset ID is not a project asset UUID.");return false;}const auto* Asset=M->Project.Draft.Geometry.FindByPredicate([&](const auto& G){return G.Id==Id;});if(!Asset||Asset->SourceSHA256!=Spec.Authoring.SourceSHA256||!FPaths::IsSamePath(Asset->SourcePath,Spec.Geometry.SourcePath)||!Spec.Authoring.MetersPerSourceUnit||Asset->MetersPerSourceUnit!=*Spec.Authoring.MetersPerSourceUnit){Status=TEXT("Bound body geometry differs from the verified project asset identity or source-unit scale. Rebind it explicitly.");return false;}}
    ProjectId=M->Project.Id;CaseId=M->Project.Draft.Id;RequestedSHA=StudioHome4Authoring::Fingerprint(Spec);
    if(Response&&Response->ProjectId==ProjectId&&Response->CaseId==CaseId&&Response->IsValid()&&Response->RequestSHA256==RequestedSHA&&!Spec.Authoring.Primitive.IsEmpty())
    {auto Copy=MakeShared<FStudioHome4AuthoringPreview,ESPMode::ThreadSafe>(*Response);Copy->bCacheHit=true;Response=Copy;Status=TEXT("Reused the exact immutable primitive/SDF request.");return true;}
    Cancellation=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);const auto Token=Cancellation;
    FStudioHome4AuthoringRequest R;R.ProjectId=ProjectId;R.CaseId=CaseId;R.Spec=Spec;
    const auto Cached=Response;
    Pending=Async(EAsyncExecution::ThreadPool,[R,Token,Cached]
    {
        if(Cached&&Cached->IsValid()&&Cached->ProjectId==R.ProjectId&&Cached->CaseId==R.CaseId&&Cached->RequestSHA256==StudioHome4Authoring::Fingerprint(R.Spec)&&!R.Spec.Geometry.SourcePath.IsEmpty())
        {FString Hash,Error;const int64 Size=IFileManager::Get().FileSize(*R.Spec.Geometry.SourcePath);if(Size>=0&&Size<=64LL*1024*1024&&StudioAssets::HashFile(R.Spec.Geometry.SourcePath,Token,Hash,Error)&&Hash==Cached->SourceSHA256){auto Copy=*Cached;Copy.bCacheHit=true;return Copy;}}
        return StudioHome4Authoring::Build(R,Token);
    });Status=TEXT("Preparing source-pinned geometry, signed distance and original-index cut links…");return true;
}
void FStudioHome4AuthoringSession::Poll()
{
    const auto Owner=Session?Session->Owner():nullptr;FStudioHome4Spec Draft;FString ScopeError;
    if(Response&&(!Owner||Owner->Project.Id!=Response->ProjectId||Owner->Project.Draft.Id!=Response->CaseId||!Session->Build(Draft,ScopeError)||StudioHome4Authoring::Fingerprint(Draft)!=Response->RequestSHA256))
    {Response.Reset();Status=TEXT("The draft changed; prepare its current geometric request.");}
    if(!Pending.IsValid()||!Pending.IsReady())return;
    auto Result=Pending.Get();Pending=TFuture<FStudioHome4AuthoringPreview>();const auto M=Session?Session->Owner():nullptr;FStudioHome4Spec Current;FString Error;
    if(!M||M->Project.Id!=ProjectId||M->Project.Draft.Id!=CaseId||!Session->Build(Current,Error)||StudioHome4Authoring::Fingerprint(Current)!=RequestedSHA)
    {Status=TEXT("The project, case or draft changed; the stale geometric response was discarded.");return;}
    if(Result.bCancelled){Status=TEXT("Geometric preparation cancelled; the previous response was retained.");return;}
    if(!Result.IsValid()){Response.Reset();Status=Result.Error;return;}
    Response=MakeShared<FStudioHome4AuthoringPreview,ESPMode::ThreadSafe>(MoveTemp(Result));Status=TEXT("Geometric preparation ready. This response contains no CFD or validation measurements.");
}
