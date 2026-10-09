#include "StudioOrientation.h"
#include "Math/RotationMatrix.h"

bool StudioOrientation::IsDirection(const FIntVector& D)
{return D!=FIntVector::ZeroValue&&D.X>=-1&&D.X<=1&&D.Y>=-1&&D.Y<=1&&D.Z>=-1&&D.Z<=1;}
TArray<FIntVector> StudioOrientation::Directions()
{
    TArray<FIntVector> Out;
    // Faces, edges and corners, in stable keyboard/menu order.
    for(int32 Count=1;Count<=3;++Count)for(int32 Z=-1;Z<=1;++Z)for(int32 Y=-1;Y<=1;++Y)for(int32 X=-1;X<=1;++X)
        if(int32(X!=0)+int32(Y!=0)+int32(Z!=0)==Count)Out.Add(FIntVector(X,Y,Z));
    return Out;
}
FString StudioOrientation::Label(const FIntVector& D)
{
    FString Out;
    for(int32 I=0;I<3;++I)if(D[I])
    {if(!Out.IsEmpty())Out+=TEXT(" ");Out+=FString(D[I]>0?TEXT("+"):TEXT("−"))+FString::Chr(TEXT("XYZ")[I]);}
    return Out;
}
FStudioCameraState StudioOrientation::Align(const FStudioCameraState& Camera,const FIntVector& D)
{
    if(!IsDirection(D))return Camera;
    auto Out=Camera;const FVector TowardCamera=FVector(D).GetSafeNormal(),Forward=-TowardCamera;
    const FVector Up=FMath::Abs(Forward.Z)>.999?FVector::RightVector:FVector::UpVector;
    Out.Orientation=FRotationMatrix::MakeFromXZ(Forward,Up).ToQuat().GetNormalized();
    Out.Position=Out.Focus+TowardCamera*Out.OrbitDistance;
    return Out;
}
TArray<StudioOrientation::FRegion> StudioOrientation::Regions(const FQuat& Q,const FVector2D& Size)
{
    TArray<FRegion> Out;
    const double Scale=FMath::Min(Size.X,Size.Y)*.23;
    auto Project=[&](const FVector& P){const FVector V=Q.UnrotateVector(P);return Size*.5+FVector2D(V.Y,-V.Z)*Scale;};
    const double Stops[]={-1,-.5,.5,1};
    for(int32 Axis=0;Axis<3;++Axis)for(int32 Sign:{-1,1})
    {
        FVector Normal=FVector::ZeroVector;Normal[Axis]=Sign;
        if(Q.UnrotateVector(Normal).X>=-1.e-6)continue;
        const int32 U=(Axis+1)%3,V=(Axis+2)%3;
        for(int32 J=0;J<3;++J)for(int32 I=0;I<3;++I)
        {
            FRegion R;R.Direction=FIntVector::ZeroValue;R.Direction[Axis]=Sign;
            R.Direction[U]=I-1;R.Direction[V]=J-1;R.bFaceCenter=I==1&&J==1;
            FVector Center=Normal;Center[U]=(Stops[I]+Stops[I+1])*.5;Center[V]=(Stops[J]+Stops[J+1])*.5;
            R.Center=Project(Center);R.Depth=Q.UnrotateVector(Center).X;
            for(const auto& Pair:{FIntPoint(I,J),FIntPoint(I+1,J),FIntPoint(I+1,J+1),FIntPoint(I,J+1)})
            {FVector P=Normal;P[U]=Stops[Pair.X];P[V]=Stops[Pair.Y];R.Polygon.Add(Project(P));}
            if(R.bFaceCenter)for(const auto& Corner:{FIntPoint(-1,-1),FIntPoint(1,-1),FIntPoint(1,1),FIntPoint(-1,1)})
            {FVector P=Normal;P[U]=Corner.X;P[V]=Corner.Y;R.FaceOutline.Add(Project(P));}
            Out.Add(MoveTemp(R));
        }
    }
    Out.Sort([](const FRegion& A,const FRegion& B){return A.Depth>B.Depth;});
    return Out;
}
int32 StudioOrientation::Hit(const TArray<FRegion>& Regions,const FVector2D& P)
{
    for(int32 I=Regions.Num()-1;I>=0;--I)
    {
        const auto& V=Regions[I].Polygon;bool Positive=false,Negative=false;
        for(int32 J=0;J<V.Num();++J)
        {
            const FVector2D A=V[(J+1)%V.Num()]-V[J],B=P-V[J];const double Cross=A.X*B.Y-A.Y*B.X;
            Positive|=Cross>1.e-7;Negative|=Cross<-1.e-7;
        }
        if(!(Positive&&Negative)&&(Positive||Negative))return I;
    }
    return INDEX_NONE;
}
