#include "StudioLattice.h"
#include "Spatial/MeshAABBTree3.h"

namespace
{
using namespace UE::Geometry;
/** Immutable adapter over original vertices and triangle IDs. No mesh copying,
 * welding or retriangulation is needed for GeometryCore's spatial index. */
struct FLatticeOriginalMesh
{
    const FStudioImportedMesh& Source;
    uint64 GetChangeStamp() const { return 1; }
    int32 MaxTriangleID() const { return Source.Indices.Num()/3; }
    int32 TriangleCount() const { return MaxTriangleID(); }
    bool IsTriangle(int32 Index) const { return Index>=0&&Index<MaxTriangleID(); }
    FIndex3i GetTriangle(int32 Index) const { return {Source.Indices[Index*3],Source.Indices[Index*3+1],Source.Indices[Index*3+2]}; }
    FVector3d GetVertex(int32 Index) const { return Source.Positions[Index]; }
    void GetTriVertices(int32 Index,FVector3d& A,FVector3d& B,FVector3d& C) const
    {const auto T=GetTriangle(Index);A=GetVertex(T.A);B=GetVertex(T.B);C=GetVertex(T.C);}
};
using FLatticeTree=TMeshAABBTree3<FLatticeOriginalMesh>;
struct FLatticeObject
{
    FBox Bounds=FBox(ForceInit);
    bool bClosed=false,bPresent=false;
};
bool LatticeBoxOverlap(const FBox& A,const FBox& B)
{
    return A.IsValid&&B.IsValid&&A.Min.X<=B.Max.X&&A.Max.X>=B.Min.X&&
        A.Min.Y<=B.Max.Y&&A.Max.Y>=B.Min.Y&&A.Min.Z<=B.Max.Z&&A.Max.Z>=B.Min.Z;
}
bool LatticeTriangleBox(const FVector& A,const FVector& B,const FVector& C,const FBox& Box)
{
    const FVector Center=Box.GetCenter(),Extent=Box.GetExtent(),V[3]={A-Center,B-Center,C-Center};
    const FVector Edges[3]={V[1]-V[0],V[2]-V[1],V[0]-V[2]};
    auto Separated=[&](const FVector& Axis)
    {
        const double P[3]={FVector::DotProduct(V[0],Axis),FVector::DotProduct(V[1],Axis),FVector::DotProduct(V[2],Axis)};
        const double Radius=FVector::DotProduct(Extent,Axis.GetAbs());
        return FMath::Min3(P[0],P[1],P[2])>Radius||FMath::Max3(P[0],P[1],P[2])<-Radius;
    };
    const FVector Axes[3]={FVector::ForwardVector,FVector::RightVector,FVector::UpVector};
    for(const auto& Axis:Axes)if(Separated(Axis))return false;
    if(Separated(FVector::CrossProduct(Edges[0],Edges[1])))return false;
    for(const auto& Edge:Edges)for(const auto& Axis:Axes)if(Separated(FVector::CrossProduct(Edge,Axis)))return false;
    return true;
}
bool LatticeRayBox(const FVector& Origin,const FVector& Direction,const FAxisAlignedBox3d& Box)
{
    double Low=0,High=TNumericLimits<double>::Max();
    for(int32 Axis=0;Axis<3;++Axis)
    {
        if(FMath::Abs(Direction[Axis])<1.e-15){if(Origin[Axis]<Box.Min[Axis]||Origin[Axis]>Box.Max[Axis])return false;continue;}
        double A=(Box.Min[Axis]-Origin[Axis])/Direction[Axis],B=(Box.Max[Axis]-Origin[Axis])/Direction[Axis];
        if(A>B)Swap(A,B);Low=FMath::Max(Low,A);High=FMath::Min(High,B);if(Low>High)return false;
    }
    return true;
}
bool LatticeRayTriangle(const FVector& Origin,const FVector& Direction,const FVector& A,const FVector& B,const FVector& C,bool& Ambiguous)
{
    const FVector E1=B-A,E2=C-A,P=FVector::CrossProduct(Direction,E2);
    const double Scale=FMath::Sqrt(E1.SizeSquared()*E2.SizeSquared()),D=FVector::DotProduct(E1,P);
    if(Scale<=0||FMath::Abs(D)<=Scale*1.e-12)return false;
    const FVector T=Origin-A,Q=FVector::CrossProduct(T,E1);
    const double U=FVector::DotProduct(T,P)/D,V=FVector::DotProduct(Direction,Q)/D,Distance=FVector::DotProduct(E2,Q)/D;
    if(U<-1.e-10||V<-1.e-10||U+V>1.+1.e-10||Distance<0)return false;
    Ambiguous=U<1.e-10||V<1.e-10||1.-U-V<1.e-10;return true;
}
}
FStudioLatticePreview StudioLattice::Preview(const FStudioCaseDraft& Case,const TSharedPtr<const FStudioDomainGeometry,ESPMode::ThreadSafe>& Geometry,
    const FStudioLatticePreviewSettings& Settings,const FStudioAssetCancellation& Cancel,const TSharedRef<FStudioLatticePreviewProgress,ESPMode::ThreadSafe>& Progress)
{
    FStudioLatticePreview Result;FStudioLatticeLayout Grid;
    Progress->Stage=0;Progress->Completed=0;Progress->Total=0;
    auto Cancelled=[&]()
    {
        if(!Cancel->load())return false;
        Result.bCancelled=true;Result.Samples.Reset();Result.Outside=Result.Inside=Result.Surface=Result.Unknown=0;Progress->Stage=3;return true;
    };
    if(Cancelled())return Result;
    Result.Key=PreviewKey(Case,Settings);
    if(!Layout(Case.Domain,Case.Setup.LatticeResolution,Grid,Result.Error)||!SamplePlan(Grid,Settings,Result.Plan,Result.Error))return Result;
    if((!Geometry&&!Case.Geometry.IsEmpty())||(Geometry&&(Geometry->bCancelled||Geometry->Key!=StudioDomain::GeometryKey(Case))))
    {Result.Error=TEXT("Check the current original geometry before previewing lattice occupancy.");return Result;}
    Progress->Total=Result.Plan.Samples;Progress->Stage=1;
    TArray<FLatticeObject> Objects;TMap<FGuid,int32> ObjectByPatch;bool UncertainEverywhere=false;
    for(const auto& Asset:Case.Geometry)
    {
        const auto* Verified=Geometry->Objects.FindByPredicate([&](const auto& Object){return Object.Id==Asset.Id;});
        FLatticeObject Object;
        if(!Verified||!Verified->Error.IsEmpty()||!Verified->Bounds.IsValid)UncertainEverywhere=true;
        else {Object.Bounds=Verified->Bounds;Object.bClosed=Verified->bClosedSurface;Object.bPresent=Verified->bInPreview&&Geometry->Preview&&!Geometry->Preview->Indices.IsEmpty();}
        const int32 Index=Objects.Add(Object);for(const auto& Patch:Asset.Patches)ObjectByPatch.Add(Patch.Id,Index);
    }
    const FStudioImportedMesh EmptyMesh;
    const auto* Source=Geometry&&Geometry->Preview?Geometry->Preview.Get():&EmptyMesh;
    if(Source->Indices.Num()%3||Source->Indices.Num()/3>StudioDomain::MaximumPreviewTriangles||Source->Positions.Num()>StudioDomain::MaximumPreviewVertices||
        (Geometry&&Geometry->TriangleTargets.Num()!=Source->Indices.Num()/3))
    {Result.Error=TEXT("The verified preview has an invalid original-triangle mapping. Check geometry again.");return Result;}
    for(int32 Index:Source->Indices)if(!Source->Positions.IsValidIndex(Index))
    {Result.Error=TEXT("The verified preview contains an invalid source vertex reference.");return Result;}
    for(const auto& Position:Source->Positions)if(Position.ContainsNaN()||Position.GetAbsMax()>1.e8)
    {Result.Error=TEXT("The verified preview contains an invalid physical source coordinate.");return Result;}
    TArray<int32> TriangleObjects;TriangleObjects.Reserve(Source->Indices.Num()/3);
    for(int32 Triangle=0;Triangle<Source->Indices.Num()/3;++Triangle)
    {
        if((Triangle&1023)==0&&Cancelled())return Result;
        const auto* Object=ObjectByPatch.Find(Geometry->TriangleTargets[Triangle]);
        if(!Object){Result.Error=TEXT("A preview triangle no longer maps to a current case surface.");return Result;}
        TriangleObjects.Add(*Object);
    }
    const FLatticeOriginalMesh Mesh{*Source};FLatticeTree Tree(&Mesh,false);
    if(Mesh.TriangleCount())Tree.Build(); // Bounded original preview, worker-only; cancellation drains this index build.
    if(Cancelled())return Result;
    Progress->Stage=2;Result.Samples.Reserve(Result.Plan.Samples);
    TArray<int32> RayCounts;RayCounts.Init(0,Objects.Num());TArray<uint8> Pending,Ambiguous;Pending.Init(0,Objects.Num());Ambiguous.Init(0,Objects.Num());
    for(int32 I=0;I<Result.Plan.Samples;++I)
    {
        if(Cancelled())return Result;
        FStudioLatticeCellSample Sample;Sample.Index=Result.Plan.Index(I);FVector Center;FBox Cell;
        if(!Grid.Cell(Sample.Index,Center,Cell)){Result.Error=TEXT("The preview requested a cell outside the applied lattice.");Result.Samples.Reset();return Result;}
        bool Surface=false,Inside=false,Uncertain=UncertainEverywhere;
        if(Mesh.TriangleCount())
        {
            FLatticeTree::FTreeTraversal Visit;
            Visit.NextBoxF=[&](const FAxisAlignedBox3d& Bounds,int){return !Surface&&!Cancel->load()&&LatticeBoxOverlap(Cell,FBox(Bounds.Min,Bounds.Max));};
            Visit.NextTriangleF=[&](int Triangle)
            {
                if(Surface)return;FVector3d A,B,C;Mesh.GetTriVertices(Triangle,A,B,C);
                if(LatticeTriangleBox(A,B,C,Cell)){Surface=true;Sample.SurfacePatch=Geometry->TriangleTargets[Triangle];}
            };
            Tree.DoTraversal(Visit);
        }
        if(!Surface)
        {
            bool NeedsRay=false;
            for(int32 Object=0;Object<Objects.Num();++Object)
            {
                const auto& Info=Objects[Object];Pending[Object]=0;
                if(!Info.Bounds.IsValid)continue;
                if(!Info.bPresent&&LatticeBoxOverlap(Cell,Info.Bounds))Uncertain=true;
                if(Info.Bounds.IsInsideOrOn(Center))
                {
                    if(Info.bClosed&&Info.bPresent){Pending[Object]=1;NeedsRay=true;}
                    else Uncertain=true;
                }
            }
            const FVector Directions[3]={FVector(1,.41421356237,.73205080757).GetSafeNormal(),FVector(.61803398875,1,.27182818285).GetSafeNormal(),FVector(.14142135623,.57721566490,1).GetSafeNormal()};
            for(int32 Ray=0;NeedsRay&&!Inside&&Ray<3;++Ray)
            {
                for(int32 Object=0;Object<Objects.Num();++Object){RayCounts[Object]=0;Ambiguous[Object]=0;}
                FLatticeTree::FTreeTraversal Visit;
                Visit.NextBoxF=[&](const FAxisAlignedBox3d& Bounds,int){return !Cancel->load()&&LatticeRayBox(Center,Directions[Ray],Bounds);};
                Visit.NextTriangleF=[&](int Triangle)
                {
                    const int32 Object=TriangleObjects[Triangle];if(!Pending[Object])return;
                    FVector3d A,B,C;Mesh.GetTriVertices(Triangle,A,B,C);bool Edge=false;
                    if(LatticeRayTriangle(Center,Directions[Ray],A,B,C,Edge)){++RayCounts[Object];if(Edge)Ambiguous[Object]=1;}
                };
                Tree.DoTraversal(Visit);NeedsRay=false;
                for(int32 Object=0;Object<Objects.Num();++Object)if(Pending[Object])
                {
                    if(Ambiguous[Object])NeedsRay=true;
                    else {Inside|=(RayCounts[Object]%2)!=0;Pending[Object]=0;}
                }
            }
            Uncertain|=NeedsRay;
        }
        if(Surface){Sample.Kind=EStudioLatticeCell::Surface;++Result.Surface;}
        else if(Inside){Sample.Kind=EStudioLatticeCell::InsideClosedGeometry;++Result.Inside;}
        else if(Uncertain){Sample.Kind=EStudioLatticeCell::Unknown;++Result.Unknown;}
        else {Sample.Kind=EStudioLatticeCell::OutsideGeometry;++Result.Outside;}
        Result.Samples.Add(Sample);Progress->Completed=I+1;
    }
    if(Cancelled())return Result;Progress->Stage=3;return Result;
}
