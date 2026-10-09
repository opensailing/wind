#include "StudioSegmentCoverage.h"
#include <limits>

bool StudioSegmentCoverage::Triangle(const FVector2D& P,const FVector2D& Q,const FVector2D& A,
    const FVector2D& B,const FVector2D& C,FVector2D& Out)
{
    const auto Cross=[](FVector2D X,FVector2D Y){return X.X*Y.Y-X.Y*Y.X;};
    const double Area=Cross(B-A,C-A);
    if(!FMath::IsFinite(Area)||Area==0)return false;
    // Barycentric coordinates vary linearly along a straight segment.
    const auto Weights=[&](FVector2D X)
    {const double V=Cross(X-A,C-A)/Area,W=Cross(B-A,X-A)/Area;return FVector(1.-V-W,V,W);};
    const FVector First=Weights(P),Last=Weights(Q);
    if(First.ContainsNaN()||Last.ContainsNaN())return false;
    double Enter=0,Exit=1;
    for(int32 I=0;I<3;++I)
    {
        const double D=Last[I]-First[I];
        if(D==0){if(First[I]<0)return false;continue;}
        const double T=-First[I]/D;
        if(D>0)Enter=FMath::Max(Enter,T);else Exit=FMath::Min(Exit,T);
        if(Enter>Exit)return false;
    }
    Out=FVector2D(Enter,Exit);return true;
}

bool StudioSegmentCoverage::Complete(TArray<FVector2D>& Intervals)
{
    if(Intervals.IsEmpty())return false;
    Intervals.Sort([](const auto& A,const auto& B){return A.X==B.X?A.Y<B.Y:A.X<B.X;});
    // Only roundoff at a shared triangle edge is merged. No sample-distance
    // tolerance is allowed to fill a geometrical hole.
    constexpr double Tolerance=64.*std::numeric_limits<double>::epsilon();
    double End=0;
    for(const auto& I:Intervals)
    {
        if(!FMath::IsFinite(I.X)||!FMath::IsFinite(I.Y)||I.X>End+Tolerance)return false;
        End=FMath::Max(End,I.Y);if(End>=1.-Tolerance)return true;
    }
    return false;
}
