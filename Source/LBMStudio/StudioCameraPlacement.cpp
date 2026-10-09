#include "StudioCameraPlacement.h"

namespace StudioCameraPlacementPrivate
{
    bool Valid(const FStudioCameraState& C,FVector2D S,double Aspect=0)
    {
        FStudioInspectionState State;State.Camera=C;
        return StudioView::IsValid(State)&&FMath::IsFinite(S.X)&&FMath::IsFinite(S.Y)&&S.X>0&&S.Y>0&&
            FMath::IsFinite(Aspect)&&Aspect>=0;
    }
    FVector Axis(int32 I) { return I==0?FVector::ForwardVector:I==1?FVector::RightVector:FVector::UpVector; }
    double Near(const FStudioCameraState& C) { return C.bDepthClipping?C.NearClipMeters:1.e-6; }
    FVector Local(const FStudioCameraState& C,const FVector& P) { return C.Orientation.UnrotateVector(P-C.Position); }
    double Aspect(FVector2D S,double A) { return A>0?A:S.X/S.Y; }
    FVector2D Pixel(const FStudioCameraState& C,FVector2D S,const FVector& P,double A)
    {
        const double Half=C.bOrthographic?C.OrthoWidth*.5:P.X*FMath::Tan(FMath::DegreesToRadians(C.FieldOfView*.5));
        return {S.X*.5+P.Y*S.X/(2*Half),S.Y*.5-P.Z*S.Y*Aspect(S,A)/(2*Half)};
    }
    bool AxisDistance(const StudioCameraPlacement::FRay& Ray,const FVector& Center,const FVector& A,double& Out)
    {
        const double K=FVector::DotProduct(Ray.Direction,A),Denominator=1-K*K;
        if(Denominator<1.e-6)return false;
        const FVector Offset=Ray.Origin-Center;
        Out=(FVector::DotProduct(A,Offset)-K*FVector::DotProduct(Ray.Direction,Offset))/Denominator;
        return FMath::IsFinite(Out);
    }
    bool RingDirection(const StudioCameraPlacement::FRay& Ray,const FVector& Center,const FVector& A,FVector& Out)
    {
        const double Denominator=FVector::DotProduct(A,Ray.Direction);
        if(FMath::Abs(Denominator)<1.e-6)return false;
        const double T=FVector::DotProduct(Center-Ray.Origin,A)/Denominator;
        if(T<0)return false;
        Out=(Ray.Origin+Ray.Direction*T-Center).GetSafeNormal();
        return !Out.IsNearlyZero();
    }
}
bool StudioCameraPlacement::Ray(const FStudioCameraState& C,FVector2D S,FVector2D P,FRay& Out,double A)
{
    if(!StudioCameraPlacementPrivate::Valid(C,S,A)||!FMath::IsFinite(P.X)||!FMath::IsFinite(P.Y))return false;
    const double Half=C.bOrthographic?C.OrthoWidth*.5:FMath::Tan(FMath::DegreesToRadians(C.FieldOfView*.5));
    const FVector Offset(0,(2*P.X/S.X-1)*Half,(1-2*P.Y/S.Y)*Half/StudioCameraPlacementPrivate::Aspect(S,A));
    Out.Origin=C.Position+(C.bOrthographic?C.Orientation.RotateVector(Offset):FVector::ZeroVector);
    Out.Direction=C.Orientation.RotateVector(C.bOrthographic?FVector::ForwardVector:(FVector::ForwardVector+Offset).GetSafeNormal());
    return true;
}
bool StudioCameraPlacement::Project(const FStudioCameraState& C,FVector2D S,const FVector& World,FVector2D& Out,double A)
{
    if(!StudioCameraPlacementPrivate::Valid(C,S,A)||World.ContainsNaN())return false;
    const auto P=StudioCameraPlacementPrivate::Local(C,World);
    if(P.X<StudioCameraPlacementPrivate::Near(C)||(C.bDepthClipping&&P.X>C.FarClipMeters))return false;
    Out=StudioCameraPlacementPrivate::Pixel(C,S,P,A);return FMath::IsFinite(Out.X)&&FMath::IsFinite(Out.Y);
}
bool StudioCameraPlacement::ProjectLine(const FStudioCameraState& C,FVector2D S,FVector A,FVector B,FVector2D& OutA,FVector2D& OutB,double Aspect)
{
    if(!StudioCameraPlacementPrivate::Valid(C,S,Aspect)||A.ContainsNaN()||B.ContainsNaN())return false;
    A=StudioCameraPlacementPrivate::Local(C,A);B=StudioCameraPlacementPrivate::Local(C,B);
    const double H=C.bOrthographic?C.OrthoWidth*.5:FMath::Tan(FMath::DegreesToRadians(C.FieldOfView*.5)),V=H/StudioCameraPlacementPrivate::Aspect(S,Aspect);
    double Lo=0,Hi=1;
    auto Clip=[&](double FA,double FB)
    {
        if(FA<0&&FB<0)return false;
        if(FA<0)Lo=FMath::Max(Lo,FA/(FA-FB));
        else if(FB<0)Hi=FMath::Min(Hi,FA/(FA-FB));
        return Lo<=Hi;
    };
    if(!Clip(A.X-StudioCameraPlacementPrivate::Near(C),B.X-StudioCameraPlacementPrivate::Near(C))||
        !Clip((C.bDepthClipping?C.FarClipMeters:1.e8)-A.X,(C.bDepthClipping?C.FarClipMeters:1.e8)-B.X))return false;
    const double AH=C.bOrthographic?H:H*A.X,BH=C.bOrthographic?H:H*B.X,AV=C.bOrthographic?V:V*A.X,BV=C.bOrthographic?V:V*B.X;
    if(!Clip(AH-A.Y,BH-B.Y)||!Clip(AH+A.Y,BH+B.Y)||!Clip(AV-A.Z,BV-B.Z)||!Clip(AV+A.Z,BV+B.Z))return false;
    const FVector Delta=B-A;
    OutA=StudioCameraPlacementPrivate::Pixel(C,S,A+Delta*Lo,Aspect);OutB=StudioCameraPlacementPrivate::Pixel(C,S,A+Delta*Hi,Aspect);
    return FMath::IsFinite(OutA.X)&&FMath::IsFinite(OutA.Y)&&FMath::IsFinite(OutB.X)&&FMath::IsFinite(OutB.Y);
}
double StudioCameraPlacement::HandleScale(const FStudioCameraState& C,FVector2D S,const FVector& P)
{
    if(!StudioCameraPlacementPrivate::Valid(C,S))return 0;
    const double Depth=StudioCameraPlacementPrivate::Local(C,P).X;
    if(Depth<StudioCameraPlacementPrivate::Near(C))return 0;
    return (C.bOrthographic?C.OrthoWidth:2*Depth*FMath::Tan(FMath::DegreesToRadians(C.FieldOfView*.5)))*72/S.X;
}
TArray<StudioCameraPlacement::FLine> StudioCameraPlacement::Handles(const FStudioCameraState& C,const FStudioCameraState& Observer,FVector2D Size,EStudioCameraPlacementTool Tool)
{
    TArray<FLine> Lines;const double Scale=HandleScale(Observer,Size,C.Position);if(Scale<=0)return Lines;
    for(int32 I=0;I<3;++I)
    {
        const FVector Axis=StudioCameraPlacementPrivate::Axis(I);
        if(Tool==EStudioCameraPlacementTool::Move)Lines.Add({C.Position,C.Position+Axis*Scale,I});
        else
        {
            const FVector U=StudioCameraPlacementPrivate::Axis((I+1)%3),V=StudioCameraPlacementPrivate::Axis((I+2)%3);
            for(int32 J=0;J<64;++J)
            {
                auto Point=[&](int32 K){const double Angle=2*PI*K/64;return C.Position+(U*FMath::Cos(Angle)+V*FMath::Sin(Angle))*Scale*.8;};
                Lines.Add({Point(J),Point(J+1),I});
            }
        }
    }
    return Lines;
}
TArray<StudioCameraPlacement::FLine> StudioCameraPlacement::Frustum(const FStudioCameraState& C,double Aspect)
{
    TArray<FLine> Lines;FStudioInspectionState Check;Check.Camera=C;
    if(!StudioView::IsValid(Check)||!FMath::IsFinite(Aspect)||Aspect<=0)return Lines;
    const double Near=C.bDepthClipping?C.NearClipMeters:0,Depth=C.bDepthClipping?C.FarClipMeters:C.OrbitDistance;
    const double Tan=FMath::Tan(FMath::DegreesToRadians(C.FieldOfView*.5));
    const double Half=C.bOrthographic?C.OrthoWidth*.5:Depth*Tan,NearHalf=C.bOrthographic?Half:Near*Tan;
    FVector Corners[4],Origins[4];
    for(int32 I=0;I<4;++I)
    {
        const double Y=I==0||I==3?-1:1,Z=I<2?1:-1;
        Corners[I]=C.Position+C.Orientation.RotateVector(FVector(Depth,Y*Half,Z*Half/Aspect));
        Origins[I]=C.Position+C.Orientation.RotateVector(FVector(Near,Y*NearHalf,Z*NearHalf/Aspect));
        Lines.Add({Origins[I],Corners[I]});
    }
    for(int32 I=0;I<4;++I){Lines.Add({Corners[I],Corners[(I+1)%4]});if(C.bOrthographic||C.bDepthClipping)Lines.Add({Origins[I],Origins[(I+1)%4]});}
    Lines.Add({C.Position,C.Position+C.Orientation.GetForwardVector()*Depth});return Lines;
}
int32 StudioCameraPlacement::HitHandle(const TArray<FLine>& Lines,const FStudioCameraState& C,FVector2D Size,FVector2D P,double Aspect)
{
    int32 Hit=INDEX_NONE;double Best=7*7;
    for(const auto& Line:Lines)
    {
        FVector2D A,B;if(!ProjectLine(C,Size,Line.A,Line.B,A,B,Aspect))continue;
        const auto D=B-A;const double Length=D.SizeSquared();if(Length<1.e-6)continue;
        const double T=FMath::Clamp(FVector2D::DotProduct(P-A,D)/Length,0.,1.);
        const double Distance=(P-(A+D*T)).SizeSquared();
        if(Distance<Best){Hit=Line.Axis;Best=Distance;}
    }
    return Hit;
}
bool StudioCameraPlacement::BeginDrag(const FStudioCameraState& C,const FStudioCameraState& Observer,FVector2D Size,FVector2D P,EStudioCameraPlacementTool Tool,int32 I,FDrag& Out,double Aspect)
{
    if(I<0||I>2)return false;FRay R;if(!Ray(Observer,Size,P,R,Aspect))return false;
    Out={};Out.Camera=C;Out.Observer=Observer;Out.Viewport=Size;Out.Tool=Tool;Out.Axis=I;Out.ProjectionAspect=Aspect;
    const auto Axis=StudioCameraPlacementPrivate::Axis(I);
    return Tool==EStudioCameraPlacementTool::Move?StudioCameraPlacementPrivate::AxisDistance(R,C.Position,Axis,Out.StartDistance):
        StudioCameraPlacementPrivate::RingDirection(R,C.Position,Axis,Out.StartDirection);
}
bool StudioCameraPlacement::Drag(const FDrag& Start,FVector2D P,FStudioCameraState& Out)
{
    if(Start.Axis<0||Start.Axis>2)return false;FRay R;if(!Ray(Start.Observer,Start.Viewport,P,R,Start.ProjectionAspect))return false;
    const auto A=StudioCameraPlacementPrivate::Axis(Start.Axis);Out=Start.Camera;
    if(Start.Tool==EStudioCameraPlacementTool::Move)
    {
        double Distance;if(!StudioCameraPlacementPrivate::AxisDistance(R,Out.Position,A,Distance))return false;
        const FVector Delta=A*(Distance-Start.StartDistance);Out.Position+=Delta;Out.Focus+=Delta;
    }
    else
    {
        FVector Direction;if(!StudioCameraPlacementPrivate::RingDirection(R,Out.Position,A,Direction))return false;
        const double Angle=FMath::Atan2(FVector::DotProduct(A,FVector::CrossProduct(Start.StartDirection,Direction)),FVector::DotProduct(Start.StartDirection,Direction));
        Out.Orientation=(FQuat(A,Angle)*Start.Camera.Orientation).GetNormalized();
        Out.Focus=Out.Position+Out.Orientation.GetForwardVector()*Out.OrbitDistance;
    }
    FStudioInspectionState Check;Check.Camera=Out;return StudioView::IsValid(Check);
}
