#include "StudioBoundarySelection.h"

namespace
{
bool InsideBoundaryViewport(FVector2D Size,FVector2D Pixel)
{
    return FMath::IsFinite(Size.X)&&FMath::IsFinite(Size.Y)&&Size.X>0&&Size.Y>0&&
        FMath::IsFinite(Pixel.X)&&FMath::IsFinite(Pixel.Y)&&Pixel.X>=0&&Pixel.Y>=0&&Pixel.X<Size.X&&Pixel.Y<Size.Y;
}
bool BoundaryRayBox(const StudioCameraPlacement::FRay& Ray,const FBox& Box,double Maximum)
{
    if(!Box.IsValid)return false;
    double Minimum=0;
    for(int32 Axis=0;Axis<3;++Axis)
    {
        if(FMath::Abs(Ray.Direction[Axis])<1.e-15)
        {if(Ray.Origin[Axis]<Box.Min[Axis]||Ray.Origin[Axis]>Box.Max[Axis])return false;continue;}
        double A=(Box.Min[Axis]-Ray.Origin[Axis])/Ray.Direction[Axis];
        double B=(Box.Max[Axis]-Ray.Origin[Axis])/Ray.Direction[Axis];
        if(A>B)Swap(A,B);Minimum=FMath::Max(Minimum,A);Maximum=FMath::Min(Maximum,B);
        if(Minimum>Maximum)return false;
    }
    return true;
}
bool BoundaryRayTriangle(const StudioCameraPlacement::FRay& Ray,const FVector& A,const FVector& B,const FVector& C,double& Distance)
{
    if(A.ContainsNaN()||B.ContainsNaN()||C.ContainsNaN())return false;
    const FVector Edge1=B-A,Edge2=C-A,P=FVector::CrossProduct(Ray.Direction,Edge2);
    const double Scale=FMath::Sqrt(Edge1.SizeSquared()*Edge2.SizeSquared());
    const double Determinant=FVector::DotProduct(Edge1,P);
    if(!FMath::IsFinite(Scale)||Scale<=0||FMath::Abs(Determinant)<=Scale*1.e-12)return false;
    const FVector Offset=Ray.Origin-A,Q=FVector::CrossProduct(Offset,Edge1);
    const double U=FVector::DotProduct(Offset,P)/Determinant;
    const double V=FVector::DotProduct(Ray.Direction,Q)/Determinant;
    if(U<-1.e-10||V<-1.e-10||U+V>1.+1.e-10)return false;
    Distance=FVector::DotProduct(Edge2,Q)/Determinant;
    return FMath::IsFinite(Distance)&&Distance>=0;
}
}
TArray<StudioBoundarySelection::FFaceHandle> StudioBoundarySelection::FaceHandles(
    const FStudioDomain& Domain,const FStudioCameraState& Observer,FVector2D Size,double Aspect)
{
    TArray<FFaceHandle> Out;
    for(int32 Face=0;Face<6&&Domain.Faces.IsValidIndex(Face);++Face)
    {
        const FVector Position=StudioDomain::FaceCenter(Domain,Face);FVector2D Pixel;
        if(!Domain.Faces[Face].IsValid()||!StudioCameraPlacement::Project(Observer,Size,Position,Pixel,Aspect)||
            Pixel.X<10||Pixel.Y<10||Pixel.X>Size.X-28||Pixel.Y>Size.Y-20)continue;
        const double Depth=Observer.Orientation.UnrotateVector(Position-Observer.Position).X;
        Out.Add({Domain.Faces[Face],Face,Position,Pixel,Depth});
    }
    Out.Sort([](const auto& A,const auto& B){return A.Depth<B.Depth;});
    TArray<FFaceHandle> Visible;
    for(const auto& Handle:Out)
        if(!Visible.ContainsByPredicate([&](const auto& Nearer){return (Nearer.Pixel-Handle.Pixel).SizeSquared()<144.;}))Visible.Add(Handle);
    return Visible;
}
FGuid StudioBoundarySelection::HitFaceHandle(const TArray<FFaceHandle>& Handles,FVector2D Pixel)
{
    FGuid Target;double Best=100.,Depth=TNumericLimits<double>::Max();
    for(const auto& Handle:Handles)
    {
        const double Distance=(Pixel-Handle.Pixel).SizeSquared();
        if(Distance<=100.&&(Distance<Best||(FMath::IsNearlyEqual(Distance,Best,1.e-8)&&Handle.Depth<Depth)))
        {Target=Handle.Target;Best=Distance;Depth=Handle.Depth;}
    }
    return Target;
}
bool StudioBoundarySelection::PickPatch(const FStudioDomainGeometry& Geometry,const FStudioCameraState& Observer,
    FVector2D Size,FVector2D Pixel,FPatchHit& Out,double Aspect)
{
    if(!InsideBoundaryViewport(Size,Pixel)||Geometry.bCancelled||!Geometry.Preview)return false;
    const auto& Mesh=*Geometry.Preview;
    if(Mesh.Indices.Num()%3||Mesh.Indices.Num()/3>StudioDomain::MaximumPreviewTriangles||
        Mesh.Positions.Num()>StudioDomain::MaximumPreviewVertices||Geometry.TriangleTargets.Num()!=Mesh.Indices.Num()/3)return false;
    StudioCameraPlacement::FRay Ray;if(!StudioCameraPlacement::Ray(Observer,Size,Pixel,Ray,Aspect))return false;
    double Best=TNumericLimits<double>::Max();FPatchHit Hit;TMap<FGuid,bool> Intersections;
    if(!BoundaryRayBox(Ray,Mesh.Bounds,Best))return false;
    for(int32 Triangle=0;Triangle<Geometry.TriangleTargets.Num();++Triangle)
    {
        const FGuid Target=Geometry.TriangleTargets[Triangle];if(!Target.IsValid())return false;
        const bool* Intersects=Intersections.Find(Target);
        if(!Intersects)
        {
            const auto* Bounds=Geometry.PatchBounds.Find(Target);if(!Bounds||!Bounds->IsValid)return false;
            Intersects=&Intersections.Add(Target,BoundaryRayBox(Ray,*Bounds,Best));
        }
        if(!*Intersects)continue;
        const int32 A=Mesh.Indices[Triangle*3],B=Mesh.Indices[Triangle*3+1],C=Mesh.Indices[Triangle*3+2];
        if(!Mesh.Positions.IsValidIndex(A)||!Mesh.Positions.IsValidIndex(B)||!Mesh.Positions.IsValidIndex(C))return false;
        double Distance=0;
        if(!BoundaryRayTriangle(Ray,Mesh.Positions[A],Mesh.Positions[B],Mesh.Positions[C],Distance)||Distance>=Best)continue;
        const FVector Position=Ray.Origin+Distance*Ray.Direction;FVector2D Projected;
        // Project applies physical near/far clipping to the actual hit point.
        if(!StudioCameraPlacement::Project(Observer,Size,Position,Projected,Aspect))continue;
        Best=Distance;Hit={Target,Position,Triangle,Distance};
    }
    if(!Hit.Target.IsValid())return false;
    Out=Hit;return true;
}
